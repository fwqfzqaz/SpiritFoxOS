"""Persistence over real virtio OUT/FLUSH, reboots, interrupted commits and EIO."""
import hashlib
import json
from pathlib import Path
import queue
import shutil
import socket
import struct
import subprocess
import threading
import time
import qemu_test

ROOT = qemu_test.ROOT


class VM:
    def __init__(self, disk, name, extra=(), debug=None, stop_on_error=False, iommu=False, writable=True, memory=256, iommu_options=None, legacy=False):
        print('Persistence VM: '+name,flush=True)
        shutil.copyfile(ROOT/'.tools/qemu/share/edk2-i386-vars.fd', ROOT/'build/OVMF_VARS.fd')
        args = qemu_test.command(memory=memory,disk=disk,writable=writable,iommu=iommu)
        if iommu_options:
            args[args.index(next(x for x in args if x.startswith('intel-iommu,')))]=iommu_options
        if legacy:
            args[args.index(next(x for x in args if x.startswith('virtio-blk-pci,')))]='virtio-blk-pci,drive=sf-storage,disable-modern=on,disable-legacy=off,vectors=0,addr=0x5'
        if debug:
            index = args.index('if=none,id=sf-storage,format=raw,readonly=off,file='+str(disk))
            args[index] = 'if=none,id=sf-storage,format=raw,file=blkdebug:build/'+debug+':build/'+disk.name
        if stop_on_error:
            args[args.index(next(x for x in args if x.startswith('virtio-blk-pci,')))] += ',werror=stop'
            with socket.socket() as port:
                port.bind(('127.0.0.1',0))
                self.port = port.getsockname()[1]
            args += ['-qmp',f'tcp:127.0.0.1:{self.port},server=on,wait=off']
        args += list(extra)
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0, cwd=ROOT)
        self.queue = queue.Queue()
        self.output = bytearray()
        self.name = name
        def read():
            while data := self.process.stdout.read(1):
                self.queue.put(data)
        threading.Thread(target=read, daemon=True).start()
        self.qmp = None
        if stop_on_error:
            until = time.monotonic()+5
            while True:
                try:
                    self.qmp = socket.create_connection(('127.0.0.1',self.port),timeout=2).makefile('rwb',buffering=0)
                    break
                except OSError:
                    if time.monotonic()>until:
                        self.close(False)
                        raise
                    time.sleep(.05)
            json.loads(self.qmp.readline())
            self.query('qmp_capabilities')

    def query(self, name):
        self.qmp.write((json.dumps({'execute':name})+'\n').encode())
        while True:
            message = json.loads(self.qmp.readline())
            if 'error' in message:
                raise RuntimeError(message)
            if 'return' in message:
                return message['return']

    def expect(self, text, start=0, timeout=45):
        end = time.monotonic()+timeout
        while text.encode() not in self.output[start:]:
            if time.monotonic()>end or (self.process.poll() is not None and self.queue.empty()):
                raise RuntimeError(self.name+': missing '+text+'\n'+self.output[-2500:].decode(errors='replace'))
            try:
                self.output.extend(self.queue.get(timeout=.1))
            except queue.Empty:
                continue
            if any(x in self.output for x in (b'[FAIL]',b'[PANIC]',b'[EFISTUB] rejected')):
                raise RuntimeError(self.name+': guest failure')

    def send(self, command):
        start = len(self.output)
        for b in command.encode()+b'\n':
            self.process.stdin.write(bytes([b]))
            self.process.stdin.flush()
            time.sleep(.004)
        return start

    def command(self, command, marker):
        self.expect(marker, self.send(command))

    def boot(self, marker='PERSIST READY'):
        self.expect('BOOT TESTS PASSED')
        if marker.encode() not in self.output:
            raise RuntimeError(self.name+': missing boot status '+marker)

    def close(self, graceful=True):
        try:
            if self.process.poll() is None:
                if graceful:
                    self.command('shutdown','SHUTDOWN OK')
                    if self.process.wait(timeout=10)!=1:
                        raise RuntimeError('Unexpected shutdown status')
                else:
                    self.process.kill()
                    self.process.wait(timeout=5)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=5)
            while not self.queue.empty():
                self.output.extend(self.queue.get())
            (ROOT/'build'/('qemu-persist-'+self.name+'.log')).write_bytes(self.output)
            if self.qmp:
                self.qmp.close()


def generation(disk):
    data = disk.read_bytes()
    valid = []
    for slot in (0,1):
        sector = data[(2049+slot)*512:(2050+slot)*512]
        if sector[:8] == b'SFCMT1\0\0' and hashlib.sha256(sector[:480]).digest()==sector[480:]:
            valid.append(struct.unpack_from('<Q',sector,8)[0])
    return max(valid)


def run(iommu=False):
    disk = ROOT/'build/persist-test.img'
    pristine = (ROOT/'build/persistent.img').read_bytes()
    disk.write_bytes(pristine)
    def machine(name,**kwargs):
        return VM(disk,('iommu-' if iommu else '')+name,iommu=iommu,**kwargs)
    vm = machine('write')
    try:
        vm.boot()
        vm.command('run installed','sfexec: failed code=16')
        vm.command('write /persist/notes durable-note','write: OK')
        vm.command('write /persist/untracked forbidden','write: error')
        vm.command('cat /persist/notes','durable-note')
        vm.command('install /disk/modules/disk-echo.sfmod','install: failed code=6')
        vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
        vm.command('run installed','sfexec: exit 0')
        vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
        vm.command('persist','persist: ACTIVE generation=4')
        vm.command('unmount /disk','disk: UNMOUNTED')
        vm.command('stop disk','disk: STOPPED')
        vm.command('start disk','disk: ACTIVE')
        vm.command('mount /disk','disk: MOUNTED')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    assert generation(disk)==4
    committed = disk.read_bytes()
    vm = machine('reboot')
    try:
        vm.boot('PERSIST READY generation=4')
        vm.command('cat /persist/notes','durable-note')
        vm.command('run installed','sfexec: exit 0')
        assert b'Hello from diskhello.sfk!' in vm.output
    finally:
        vm.close()
    assert disk.read_bytes()==committed, 'Read-only reboot changed disk'
    assert committed[:2048*512]==pristine[:2048*512], 'Writes escaped transaction arena'
    # Repair storage hashes after tampering with the active package signature.
    # The volume is structurally valid, but each launch must still authenticate.
    tampered = bytearray(committed)
    bank_start = (2051+256)*512
    for i in range(4):
        entry = bank_start+512+i*128
        if tampered[entry:entry+64].split(b'\0',1)[0] == b'/slots/b.sfk':
            size = struct.unpack_from('<I',tampered,entry+64)[0]
            start = bank_start+1024+i*16384
            tampered[start+size-1] ^= 1
            tampered[entry+96:entry+128] = hashlib.sha256(tampered[start:start+size]).digest()
            break
    else:
        raise RuntimeError('Active B package missing')
    record = 2050*512
    tampered[record+40:record+72] = hashlib.sha256(tampered[bank_start:bank_start+256*512]).digest()
    tampered[record+480:record+512] = hashlib.sha256(tampered[record:record+480]).digest()
    disk.write_bytes(tampered)
    vm = machine('untrusted-slot')
    try:
        vm.boot('PERSIST READY generation=4')
        vm.command('run installed','sfexec: failed code=4')
        vm.command('run hello.sfk','sfexec: exit 0')
    finally:
        vm.close()
    # Damage the newest snapshot: select previous complete generation.
    corrupted = bytearray(committed)
    corrupted[(2051+256)*512+1024] ^= 1
    disk.write_bytes(corrupted)
    vm = machine('fallback')
    try:
        vm.boot('PERSIST READY generation=3')
        vm.command('cat /persist/notes','durable-note')
        vm.command('run installed','sfexec: exit 0')
    finally:
        vm.close()
    # Real backend error at the commit-record write. The guest must not ACK it.
    disk.write_bytes(committed)
    conf = ROOT/'build/persist-eio.conf'
    conf.write_text('[inject-error]\nevent = "write_aio"\nsector = "2049"\nerrno = "5"\nonce = "on"\n')
    vm = machine('write-error',debug=conf.name)
    try:
        vm.boot()
        vm.command('write /persist/notes forbidden-new','write: error')
        vm.command('persist','persist: UNAVAILABLE')
        assert b'write: OK' not in vm.output
    finally:
        vm.close()
    # Stop at a failed write halfway through the inactive bank, then kill QEMU.
    # This is a process-interruption test, not a physical power-loss guarantee.
    disk.write_bytes(committed)
    conf.write_text('[inject-error]\nevent = "write_aio"\nsector = "2180"\nerrno = "5"\nonce = "on"\n')
    vm = machine('interrupted',debug=conf.name,stop_on_error=True)
    try:
        vm.boot()
        vm.send('write /persist/notes interrupted!')
        end = time.monotonic()+15
        while vm.query('query-status')['status']!='io-error':
            if time.monotonic()>end:
                raise RuntimeError('VM did not suspend at injected mid-bank write error')
            time.sleep(.05)
        assert b'write: OK' not in vm.output
    finally:
        vm.close(False)
    assert disk.read_bytes()[2049*512:2050*512]==bytes(512), 'Target record not invalidated before bank reuse'
    vm = machine('interrupted-recovery')
    try:
        vm.boot('PERSIST READY generation=4')
        vm.command('cat /persist/notes','durable-note')
        vm.command('run installed','sfexec: exit 0')
    finally:
        vm.close()
    disk.write_bytes(committed)
    conf.write_text('[inject-error]\nevent = "flush_to_disk"\niotype = "flush"\nerrno = "5"\nonce = "on"\n')
    vm = machine('flush-error',debug=conf.name)
    try:
        vm.boot()
        vm.command('write /persist/notes flush-error!','write: error')
        vm.command('persist','persist: UNAVAILABLE')
    finally:
        vm.close()
    vm = machine('error-recovery')
    try:
        vm.boot('PERSIST READY generation=4')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==pristine
    print('PERSIST QEMU PASS: durable file, signed A/B activation, reboot, untrusted slot, fallback, write/flush EIO, interrupted commit',flush=True)


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iommu',action='store_true')
    run(parser.parse_args().iommu)
