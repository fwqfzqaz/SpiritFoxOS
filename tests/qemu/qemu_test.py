"""Real UEFI/ring-3 end-to-end tests. Only this workspace's VM is controlled."""
import argparse
import hashlib
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/package'))
# QEMU's vvfat driver creates temporary files through the host temp directory.
# Keep them inside the workspace so a read-only /var/tmp cannot break boot.
(ROOT / '.cache').mkdir(exist_ok=True)
os.environ['TMPDIR'] = os.environ['TEMP'] = os.environ['TMP'] = str(ROOT / '.cache')
LINUX = hasattr(os, 'uname') and os.uname().sysname == 'Linux'
if LINUX:
    import shutil
    QEMU = shutil.which('qemu-system-x86_64')
    FIRMWARE = VARS_SRC = None
    # OVMF naming differs across distributions: Arch uses OVMF_CODE.4m.fd,
    # Debian/Ubuntu use OVMF_CODE_4M.fd, and older packages use OVMF_CODE.fd.
    for base in ('/usr/share/ovmf/x64', '/usr/share/OVMF', '/usr/share/edk2/x64'):
        for code_name, vars_name in (('OVMF_CODE.4m.fd', 'OVMF_VARS.4m.fd'),
                                     ('OVMF_CODE_4M.fd', 'OVMF_VARS_4M.fd'),
                                     ('OVMF_CODE.fd', 'OVMF_VARS.fd')):
            code, vars_ = Path(base) / code_name, Path(base) / vars_name
            if code.exists() and vars_.exists():
                FIRMWARE, VARS_SRC = code, vars_
                break
        if FIRMWARE:
            break
else:
    QEMU = str(ROOT / '.tools/qemu/qemu-system-x86_64.exe')
    FIRMWARE = ROOT / '.tools/qemu/share/edk2-x86_64-code.fd'
    VARS_SRC = ROOT / '.tools/qemu/share/edk2-i386-vars.fd'


def command(memory=256, disk=None, writable=False, ioerror=False, iommu=False):
    args = [str(QEMU), '-machine', 'q35', '-accel', 'tcg', '-cpu', 'qemu64,+nx', '-m', str(memory), '-smp', '1', '-drive', 'if=pflash,format=raw,readonly=on,file='+str(FIRMWARE), '-drive', 'if=pflash,format=raw,file='+str(ROOT / 'build/OVMF_VARS.fd'), '-drive', 'format=raw,file=fat:rw:'+str(ROOT / 'build/esp'), '-net', 'none', '-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
    if iommu:
        args += ['-device','intel-iommu,intremap=off,caching-mode=on,device-iotlb=off,aw-bits=39']
    if disk:
        image = 'blkdebug:build/storage-blkdebug.conf:build/storage.img' if ioerror else str(disk)
        args += ['-drive', 'if=none,id=sf-storage,format=raw,readonly='+('off' if writable else 'on')+',file='+image, '-device', 'virtio-blk-pci,drive=sf-storage,disable-modern=on,disable-legacy=off,vectors=0,addr=0x5']
        if iommu:
            args[-1]='virtio-blk-pci,drive=sf-storage,disable-modern=off,disable-legacy=on,iommu_platform=on,vectors=0,addr=0x5'
    return args


def run(memory=256, interactive=False, storage='normal', persistent=False, iommu=False):
    if not QEMU or not FIRMWARE:
        raise RuntimeError('Linux needs qemu-system-x86_64 and OVMF (ovmf package) installed')
    os.chdir(ROOT)
    os.environ['TMPDIR'] = os.environ['TEMP'] = os.environ['TMP'] = str(ROOT / '.cache')
    shutil.copyfile(VARS_SRC, ROOT / 'build/OVMF_VARS.fd')
    if persistent:
        if not interactive:
            raise ValueError('Use --interactive with --persistent')
        disk = ROOT/'build/user-persistent.img'
        if not disk.exists():
            shutil.copyfile(ROOT/'build/persistent.img', disk)
        return subprocess.call(command(memory, disk, writable=True, iommu=iommu))
    disk = None if storage == 'missing' else ROOT / 'build/storage.img'
    if storage in ('badheader', 'badhash', 'badsignature'):
        data = bytearray(disk.read_bytes())
        if storage == 'badheader':
            data[0] ^= 1
        elif storage == 'badhash':
            data[struct.unpack_from('<Q', data, 512+128+64)[0]] ^= 1
        else:
            from mkreadfs import create
            files = {}
            for i in range(struct.unpack_from('<I',data,24)[0]):
                entry = 512+i*128
                name = bytes(data[entry:entry+64]).split(b'\0',1)[0].decode('ascii')
                off,n = struct.unpack_from('<QQ',data,entry+64)
                file = bytearray(data[off:off+n])
                if name == '/bin/diskhello.sfk':
                    file[-1] ^= 1
                files[name] = bytes(file)
            data = create(files)
        disk = ROOT / ('build/storage-'+storage+'.img')
        disk.write_bytes(data)
    if storage == 'ioerror':
        (ROOT / 'build/storage-blkdebug.conf').write_text('[inject-error]\nevent = "read_aio"\nerrno = "5"\n')
    before = hashlib.sha256(disk.read_bytes()).hexdigest() if disk else None
    if interactive:
        return subprocess.call(command(memory, disk, storage=='writable', storage=='ioerror', iommu))
    process = subprocess.Popen(command(memory, disk, storage=='writable', storage=='ioerror', iommu), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    chunks = queue.Queue()
    def read():
        while True:
            data = process.stdout.read(1)
            if not data:
                break
            chunks.put(data)
    threading.Thread(target=read, daemon=True).start()
    output = bytearray()
    log = ROOT / 'build' / ('qemu-'+str(memory)+('' if storage=='normal' else '-'+storage)+('-iommu' if iommu else '')+'.log')
    def expect(text, timeout=60):
        until = time.monotonic()+timeout
        while text.encode() not in output:
            if time.monotonic() > until:
                raise RuntimeError('Timed out waiting for '+text)
            if process.poll() is not None and chunks.empty():
                raise RuntimeError('QEMU exited before '+text)
            try:
                data = chunks.get(timeout=.1)
            except queue.Empty:
                continue
            output.extend(data)
            if output.endswith(b'\n'):
                line = output.splitlines()[-1].decode(errors='replace')
                print(line, flush=True)
                if '[FAIL]' in line or '[PANIC]' in line or '[EFISTUB] rejected' in line:
                    raise RuntimeError(line)
    try:
        expect('BOOT TESTS PASSED', 60)
        expect('DEPENDENCY GENERATION TESTS PASSED')
        if iommu and b'[IOMMU] VT-d active' not in output:
            raise RuntimeError('VT-d not active')
        if iommu and storage == 'normal' and b'MMIO BUFFER TESTS PASSED' not in output:
            raise RuntimeError('MMIO syscall buffer isolation not exercised')
        if storage == 'normal':
            if b'STORAGE READY' not in output or b'signed disk application runs through native runtime' not in output:
                raise RuntimeError('Disk-backed module/application path not exercised')
        elif storage == 'badsignature':
            if b'DISK PACKAGE REJECTED app code=4' not in output or b'Hello from diskhello.sfk!' in output:
                raise RuntimeError('Untrusted disk application executed or rejection missing')
        elif b'STORAGE UNAVAILABLE' not in output:
            raise RuntimeError('Unavailable storage was not diagnosed')
        tests = [('help', 'help | ls'), ('modules', 'sf-init ACTIVE pinned'), ('echo', 'echo: ping'), ('run hello.sfk', 'sfexec: exit 0'), ('stop echo', 'echo: STOPPED'), ('start echo', 'echo: ACTIVE'), ('write /tmp/check persistent-in-ram', 'write: OK'), ('cat /tmp/check', 'persistent-in-ram'), ('ls /tmp', 'notes'), ('selftest', 'SELFTEST OK'), ('shutdown', 'SHUTDOWN OK')]
        if storage == 'normal':
            tests[-1:-1] = [('cat /disk/etc/readme.txt', 'SpiritFoxOS read-only disk mounted'), ('run /disk/bin/diskhello.sfk', 'sfexec: exit 0'), ('write /disk/etc/readme.txt forbidden', 'write: error'), ('stop disk', 'disk: stop refused'), ('unmount /disk', 'disk: UNMOUNTED'), ('stop disk', 'disk: STOPPED'), ('start disk', 'disk: ACTIVE'), ('mount /disk', 'disk: MOUNTED'), ('run /disk/bin/diskhello.sfk', 'sfexec: exit 0')]
        elif storage == 'badsignature':
            tests[-1:-1] = [('run /disk/bin/diskhello.sfk', 'sfexec: failed code=4'), ('run hello.sfk', 'sfexec: exit 0')]
        for cmd, marker in tests:
            # A UART has a finite FIFO; emulate a human/baud-paced sender.
            for byte in cmd.encode()+b'\n':
                process.stdin.write(bytes([byte]))
                process.stdin.flush()
                time.sleep(.004)
            # Search only output produced after this command.
            old = bytes(output)
            output.clear()
            try:
                expect(marker, 10)
            finally:
                output[:0] = old
        code = process.wait(timeout=10)
        if code != 1:
            raise RuntimeError('Unexpected QEMU exit code '+str(code))
        print('QEMU PASS ('+str(memory)+' MiB, '+storage+')', flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        while not chunks.empty():
            output.extend(chunks.get())
        log.write_bytes(output)
        if disk and hashlib.sha256(disk.read_bytes()).hexdigest() != before:
            raise RuntimeError('Read-only test changed the storage image')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--memory', type=int, default=256, choices=[128, 256, 512])
    parser.add_argument('--interactive', action='store_true')
    parser.add_argument('--persistent', action='store_true', help='Interactive writable copy, build/user-persistent.img; kept across runs')
    parser.add_argument('--iommu', action='store_true', help='Q35 Intel VT-d, 39-bit legacy remapping')
    parser.add_argument('--storage', choices=['normal', 'missing', 'badheader', 'badhash', 'badsignature', 'writable', 'ioerror'], default='normal')
    args = parser.parse_args()
    run(args.memory, args.interactive, args.storage, args.persistent, args.iommu)
