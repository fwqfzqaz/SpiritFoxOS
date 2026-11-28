# SFRWFS1 端到端：空白可写盘上 mkfs、读写、卸载，重启后自动挂载并读回。
import hashlib
import shutil
import sys
from pathlib import Path
import qemu_test
from qemu_persist_test import VM

ROOT = qemu_test.ROOT
PRISTINE = ROOT / 'build' / 'storage-rw.img'
DISK = ROOT / 'build' / 'sfrw-test.img'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def boot(vm):
    vm.expect('BOOT TESTS PASSED')
    vm.expect('SpiritFoxOS ready')


def run():
    if not PRISTINE.exists():
        raise RuntimeError('missing '+str(PRISTINE))
    shutil.copyfile(PRISTINE, DISK)
    before = digest(DISK)
    vm = VM(DISK, 'sfrwfs', writable=True, memory=256)
    try:
        boot(vm)
        vm.command('mkfs /disk', 'mkfs: OK')
        vm.command('cat /disk/etc/readme.txt', 'SpiritFoxOS SFRWFS1 volume')
        vm.command('write /disk/etc/readme.txt hello-sfrw', 'write: OK')
        vm.command('cat /disk/etc/readme.txt', 'hello-sfrw')
        vm.command('unmount /disk', 'disk: UNMOUNTED')
    finally:
        vm.close(True)
    if digest(DISK) == before:
        raise RuntimeError('mkfs/write did not change the disk image')
    reboot = VM(DISK, 'sfrwfs-reboot', writable=True, memory=256)
    try:
        boot(reboot)
        reboot.command('cat /disk/etc/readme.txt', 'hello-sfrw')
    finally:
        reboot.close(True)
    print('SFRWFS PASS: mkfs, write and reboot persistence', flush=True)


if __name__ == '__main__':
    try:
        run()
    except Exception as error:
        print('SFRWFS FAIL:', error, flush=True)
        raise
