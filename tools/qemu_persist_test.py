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
from mkreadfs import create as create_readfs

ROOT = qemu_test.ROOT


class VM:
    def __init__(self, disk, name, extra=(), debug=None, stop_on_error=False, iommu=False, writable=True, memory=256, iommu_options=None, legacy=False):
        print('Persistence VM: '+name,flush=True)
        shutil.copyfile(qemu_test.VARS_SRC, ROOT/'build/OVMF_VARS.fd')
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


def snapshot(data):
    records = []
    for slot in (0,1):
        start = (2049+slot)*512
        record = data[start:start+512]
        if record[:8]==b'SFCMT1\0\0' and hashlib.sha256(record[:480]).digest()==record[480:]:
            records.append((struct.unpack_from('<Q',record,8)[0],slot))
    gen,slot = max(records)
    bank = (2051+slot*256)*512
    files = {}
    for i in range(5):
        entry = bank+512+i*128
        name = data[entry:entry+64].split(b'\0',1)[0].decode()
        if name:
            size = struct.unpack_from('<I',data,entry+64)[0]
            files[name] = bytes(data[bank+1152+i*16384:bank+1152+i*16384+size])
    return gen,slot,files


def rewrite(data, changes):
    """Offline corruption fixture: repair filesystem hashes, preserving generation."""
    data = bytearray(data)
    _,slot,_ = snapshot(data)
    bank = (2051+slot*256)*512
    for i in range(5):
        entry = bank+512+i*128
        name = data[entry:entry+64].split(b'\0',1)[0].decode()
        if name in changes:
            content = changes[name]
            assert len(content)<=16384
            start = bank+1152+i*16384
            data[start:start+16384] = content+bytes(16384-len(content))
            struct.pack_into('<I',data,entry+64,len(content))
            data[entry+96:entry+128] = hashlib.sha256(content).digest()
    record = (2049+slot)*512
    data[record+40:record+72] = hashlib.sha256(data[bank:bank+256*512]).digest()
    data[record+480:record+512] = hashlib.sha256(data[record:record+480]).digest()
    return bytes(data)


def replace_base(data, changes):
    """Create an offline signed-policy fixture; leave its transaction tail intact."""
    files = {}
    for i in range(struct.unpack_from('<I',data,24)[0]):
        entry = 512+i*128
        name = data[entry:entry+64].split(b'\0',1)[0].decode()
        offset,size = struct.unpack_from('<QQ',data,entry+64)
        files[name] = data[offset:offset+size]
    files.update(changes)
    return create_readfs(files,len(data))[:2048*512]+data[2048*512:]


def version_policy(machine,disk,committed):
    disk.write_bytes(committed)
    vm = machine('version-reject')
    try:
        vm.boot('PERSIST READY generation=8')
        vm.command('persist','update: confirmed-version=0.2.0 policy=monotonic')
        vm.command('install /disk/bin/diskhello.sfk','install: failed code=21')
        vm.command('install /disk/bin/diskupdate.sfk','install: UNCHANGED')
        vm.command('persist','persist: ACTIVE generation=8')
    finally:
        vm.close()
    assert disk.read_bytes()==committed, 'Rejected or identical install wrote the disk'
    vm = machine('version-confirm')
    try:
        vm.boot()
        vm.command('install /disk/bin/diskfault.sfk','install: COMMITTED')
        vm.command('run installed','sfexec: failed code=114')
        vm.command('persist','update: confirmed-version=0.2.0 policy=monotonic')
        vm.command('install /disk/bin/disknext.sfk','install: COMMITTED')
        vm.command('run installed','sfexec: exit 0')
        vm.command('persist','update: confirmed-version=0.3.0 policy=monotonic')
        vm.command('install /disk/bin/disknext.sfk','install: UNCHANGED')
        vm.command('install /disk/bin/diskupdate.sfk','install: failed code=21')
        vm.command('install /disk/bin/diskfault.sfk','install: failed code=21')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    assert generation(disk)==14
    confirmed = disk.read_bytes()
    vm = machine('version-reboot')
    try:
        vm.boot('PERSIST READY generation=14')
        vm.command('persist','update: confirmed-version=0.3.0 policy=monotonic')
        vm.command('run installed','sfexec: exit 0')
        vm.command('install /disk/bin/diskupdate.sfk','install: failed code=21')
    finally:
        vm.close()
    assert disk.read_bytes()==confirmed
    malformed = replace_base(committed,{
        '/bin/'+name+'.sfk':(ROOT/'build'/('version-'+label+'.sfk')).read_bytes()
        for name,label in [('diskfault','short'),('diskexit','leading'),('diskspin','overflow'),('disknext','suffix')]})
    disk.write_bytes(malformed)
    vm = machine('version-signed-malformed')
    try:
        vm.boot()
        for name in ['diskfault','diskexit','diskspin','disknext']:
            vm.command('install /disk/bin/'+name+'.sfk','install: failed code=3')
        vm.command('persist','update: confirmed-version=0.2.0 policy=monotonic')
    finally:
        vm.close()
    assert disk.read_bytes()==malformed
    # Old implementations could stage these records. Recheck at actual launch,
    # after TRYING is durable, and roll back without executing the candidate.
    for name,base,package,code in [
        ('downgrade',committed,(ROOT/'build/diskhello.sfk').read_bytes(),21),
        ('same-version',confirmed,(ROOT/'build/diskfault.sfk').read_bytes(),21),
        ('malformed',committed,(ROOT/'build/version-suffix.sfk').read_bytes(),3)]:
        _,_,files = snapshot(base)
        selector = bytearray(files['/boot/current'])
        slot = ord('a') if selector[9]==ord('b') else ord('b')
        selector[8]=1;selector[10]=slot
        selector[64:96]=hashlib.sha256(package).digest()
        disk.write_bytes(rewrite(base,{'/boot/current':selector,'/slots/'+chr(slot)+'.sfk':package}))
        vm = machine('version-pending-'+name)
        try:
            vm.boot()
            start = vm.send('run installed')
            vm.expect('sfexec: failed code='+str(code),start)
            assert b'update: ROLLED BACK' in vm.output[start:]
            assert b'Hello from diskhello.sfk!' not in vm.output[start:]
            assert b'update: CONFIRMED' not in vm.output[start:]
            vm.command('persist','update: confirmed-version='+('0.3.0' if name=='same-version' else '0.2.0')+' policy=monotonic')
            vm.command('run installed','sfexec: exit 0')
        finally:
            vm.close()
    disk.write_bytes(committed)


def update_health(machine,disk,pristine,committed):
    disk.write_bytes(pristine)
    vm = machine('update-first-failure')
    try:
        vm.boot()
        vm.command('install /disk/bin/diskfault.sfk','install: COMMITTED')
        vm.command('run installed','sfexec: failed code=114')
        assert b'update: ROLLED BACK' in vm.output
        vm.command('persist','update: STABLE good=- candidate=-')
        vm.command('run installed','sfexec: failed code=16')
        vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
        vm.command('run installed','sfexec: exit 0')
    finally:
        vm.close()
    disk.write_bytes(committed)
    vm = machine('update-health-failures')
    try:
        vm.boot()
        vm.command('install /disk/bin/diskother.sfk','install: failed code=6')
        for name,code in [('diskfault',114),('diskexit',42),('diskspin',9)]:
            vm.command('install /disk/bin/'+name+'.sfk','install: COMMITTED')
            vm.command('install /disk/bin/diskhello.sfk','install: failed code=7')
            start = vm.send('run installed')
            vm.expect('sfexec: failed code='+str(code),start)
            assert b'update: ROLLED BACK' in vm.output[start:]
            vm.command('persist','update: STABLE good=b candidate=-')
            vm.command('persist','update: confirmed-version=0.2.0 policy=monotonic')
            vm.command('run installed','sfexec: exit 0')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    assert generation(disk)==17
    # A pending update survives a reboot, but an attempted, unconfirmed update
    # never retries automatically after a process interruption.
    disk.write_bytes(committed)
    vm = machine('update-pending-stage')
    try:
        vm.boot()
        vm.command('install /disk/bin/diskspin.sfk','install: COMMITTED')
    finally:
        vm.close()
    pending = disk.read_bytes()
    conf = ROOT/'build/update-begin-eio.conf'
    conf.write_text('[inject-error]\nevent = "write_aio"\nsector = "2050"\nerrno = "5"\nonce = "on"\n')
    vm = machine('update-begin-error',debug=conf.name)
    try:
        vm.boot('PERSIST READY generation=9')
        start = vm.send('run installed')
        vm.expect('sfexec: failed code=20',start)
        assert b'update: TRIAL started' not in vm.output[start:]
        assert b'update: CONFIRMED' not in vm.output[start:]
        vm.command('persist','persist: UNAVAILABLE')
    finally:
        vm.close()
    vm = machine('update-begin-recovery')
    try:
        vm.boot('PERSIST READY generation=9')
        vm.command('persist','update: PENDING good=b candidate=a')
    finally:
        vm.close()
    vm = machine('update-interrupted-trial')
    try:
        vm.boot('PERSIST READY generation=9')
        vm.command('persist','update: PENDING good=b candidate=a')
        vm.expect('update: TRIAL started',vm.send('run /installed.sfk'))
    finally:
        vm.close(False)
    assert generation(disk)==10
    conf = ROOT/'build/update-recovery-eio.conf'
    conf.write_text('[inject-error]\nevent = "write_aio"\nsector = "2049"\nerrno = "5"\nonce = "on"\n')
    vm = machine('update-recovery-error',debug=conf.name)
    try:
        vm.boot('PERSIST UNAVAILABLE code=20')
        vm.command('run installed','sfexec: failed code=10')
        vm.command('persist','persist: UNAVAILABLE')
    finally:
        vm.close()
    vm = machine('update-trial-recovery')
    try:
        vm.boot('PERSIST READY generation=11')
        assert b'update: RECOVERED interrupted trial' in vm.output
        vm.command('persist','update: STABLE good=b candidate=-')
        vm.command('run installed','sfexec: exit 0')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    # A different, correctly signed package cannot replace the staged digest.
    disk.write_bytes(rewrite(pending,{'/slots/a.sfk':(ROOT/'build/diskfault.sfk').read_bytes()}))
    vm = machine('update-substituted-candidate')
    try:
        vm.boot()
        vm.command('run installed','sfexec: failed code=4')
        assert b'update: ROLLED BACK' in vm.output
        vm.command('run installed','sfexec: exit 0')
    finally:
        vm.close()
    # Successful execution alone must not acknowledge a failed health commit.
    disk.write_bytes(committed)
    vm = machine('update-confirm-stage')
    try:
        vm.boot()
        vm.command('install /disk/bin/disknext.sfk','install: COMMITTED')
    finally:
        vm.close()
    conf = ROOT/'build/update-confirm-eio.conf'
    conf.write_text('[inject-error]\nevent = "write_aio"\nsector = "2049"\nerrno = "5"\nonce = "on"\n')
    vm = machine('update-confirm-error',debug=conf.name)
    try:
        vm.boot('PERSIST READY generation=9')
        start = vm.send('run installed')
        vm.expect('sfexec: failed code=20',start)
        assert b'Hello from diskhello.sfk!' in vm.output[start:]
        assert b'update: CONFIRMED' not in vm.output[start:]
        assert b'sfexec: exit 0' not in vm.output[start:]
        vm.command('persist','persist: UNAVAILABLE')
    finally:
        vm.close()
    vm = machine('update-confirm-recovery')
    try:
        vm.boot('PERSIST READY generation=11')
        assert b'update: RECOVERED interrupted trial' in vm.output
        vm.command('persist','update: STABLE good=b candidate=-')
        vm.command('run installed','sfexec: exit 0')
    finally:
        vm.close()
    # Compatibility with the original authenticated, single-byte selector.
    disk.write_bytes(rewrite(committed,{'/boot/current':b'b'}))
    vm = machine('update-legacy-selector')
    try:
        vm.boot()
        vm.command('run installed','sfexec: exit 0')
        vm.command('install /disk/bin/disknext.sfk','install: COMMITTED')
        vm.command('run installed','sfexec: exit 0')
        vm.command('persist','update: STABLE good=a candidate=-')
    finally:
        vm.close()
    assert snapshot(disk.read_bytes())[2]['/boot/current'].startswith(b'SFUPD1\0\0')
    _,_,files = snapshot(committed)
    selector = bytearray(files['/boot/current']);selector[11] = 1
    disk.write_bytes(rewrite(committed,{'/boot/current':selector}))
    vm = machine('update-invalid-state')
    try:
        vm.boot('PERSIST UNAVAILABLE code=3')
        vm.command('run installed','sfexec: failed code=10')
        vm.command('persist','persist: UNAVAILABLE')
    finally:
        vm.close()


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
        vm.command('persist','update: PENDING good=- candidate=a')
        vm.command('install /disk/bin/diskhello.sfk','install: failed code=7')
        vm.command('run installed','sfexec: exit 0')
        assert b'update: CONFIRMED' in vm.output
        vm.command('install /disk/bin/diskupdate.sfk','install: COMMITTED')
        vm.command('run /installed.sfk','sfexec: exit 0')
        vm.command('persist','persist: ACTIVE generation=8')
        vm.command('persist','update: STABLE good=b candidate=-')
        vm.command('unmount /disk','disk: UNMOUNTED')
        vm.command('stop disk','disk: STOPPED')
        vm.command('start disk','disk: ACTIVE')
        vm.command('mount /disk','disk: MOUNTED')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    assert generation(disk)==8
    committed = disk.read_bytes()
    vm = machine('reboot')
    try:
        vm.boot('PERSIST READY generation=8')
        vm.command('cat /persist/notes','durable-note')
        vm.command('run installed','sfexec: exit 0')
        assert b'Hello from diskhello.sfk!' in vm.output
    finally:
        vm.close()
    assert disk.read_bytes()==committed, 'Read-only reboot changed disk'
    assert committed[:2048*512]==pristine[:2048*512], 'Writes escaped transaction arena'
    # Repair storage hashes after tampering with the active package signature.
    # The volume is structurally valid, but each launch must still authenticate.
    _,_,files = snapshot(committed)
    package = bytearray(files['/slots/b.sfk']);package[-1] ^= 1
    selector = bytearray(files['/boot/current'])
    selector[32:64] = hashlib.sha256(package).digest()
    disk.write_bytes(rewrite(committed,{'/slots/b.sfk':package,'/boot/current':selector}))
    vm = machine('untrusted-slot')
    try:
        vm.boot('PERSIST READY generation=8')
        vm.command('run installed','sfexec: failed code=4')
        vm.command('run hello.sfk','sfexec: exit 0')
    finally:
        vm.close()
    # Damage the newest snapshot: select previous complete generation.
    corrupted = bytearray(committed)
    corrupted[(2051+256)*512+1152] ^= 1
    disk.write_bytes(corrupted)
    vm = machine('fallback')
    try:
        vm.boot('PERSIST READY generation=8')
        assert b'update: RECOVERED interrupted trial' in vm.output
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
        vm.boot('PERSIST READY generation=8')
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
        vm.boot('PERSIST READY generation=8')
        vm.command('cat /persist/notes','durable-note')
    finally:
        vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==pristine
    update_health(machine,disk,pristine,committed)
    version_policy(machine,disk,committed)
    assert (ROOT/'build/persistent.img').read_bytes()==pristine
    assert disk.read_bytes()[:2048*512]==pristine[:2048*512]
    print('PERSIST QEMU PASS: durable file, trial confirmation, fault/nonzero/timeout rollback, interrupted trial, identity/digest/signature/version, idempotent reinstall, pending policy recheck, legacy selector, invalid state, write/flush/health EIO and interrupted commit',flush=True)


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iommu',action='store_true')
    run(parser.parse_args().iommu)
