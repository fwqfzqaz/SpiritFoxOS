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
        # 目录创建、目录内建文件、改名与非法删除都要经过真实磁盘。
        vm.command('mkdir /disk/docs', 'mkdir: OK')
        vm.command('write /disk/docs/note.txt hello-dir', 'write: OK')
        vm.command('cat /disk/docs/note.txt', 'hello-dir')
        vm.command('mkdir /disk/docs/sub', 'mkdir: OK')
        vm.command('mv /disk/docs/note.txt /disk/docs/renamed.txt', 'mv: OK')
        vm.command('cat /disk/docs/renamed.txt', 'hello-dir')
        vm.command('mv /disk/docs /disk/docs/sub/deep', 'failed code=1')
        vm.command('rmdir /disk/docs', 'failed code=7')
        vm.command('unmount /disk', 'disk: UNMOUNTED')
    finally:
        vm.close(True)
    if digest(DISK) == before:
        raise RuntimeError('mkfs/write did not change the disk image')
    reboot = VM(DISK, 'sfrwfs-reboot', writable=True, memory=256)
    try:
        boot(reboot)
        reboot.command('cat /disk/etc/readme.txt', 'hello-sfrw')
        reboot.command('cat /disk/docs/renamed.txt', 'hello-dir')
        reboot.command('mv /disk/docs/renamed.txt /disk/docs/moved.txt', 'mv: OK')
        reboot.command('unmount /disk', 'disk: UNMOUNTED')
    finally:
        reboot.close(True)
    cleanup = VM(DISK, 'sfrwfs-cleanup', writable=True, memory=256)
    try:
        boot(cleanup)
        cleanup.command('cat /disk/docs/moved.txt', 'hello-dir')
        cleanup.command('rm /disk/docs/moved.txt', 'rm: OK')
        cleanup.command('rmdir /disk/docs/sub', 'rmdir: OK')
        cleanup.command('rmdir /disk/docs', 'rmdir: OK')
        cleanup.command('unmount /disk', 'disk: UNMOUNTED')
    finally:
        cleanup.close(True)
    final = VM(DISK, 'sfrwfs-final', writable=True, memory=256)
    try:
        boot(final)
        final.command('stat /disk/docs', 'failed code=16')
        final.command('cat /disk/etc/readme.txt', 'hello-sfrw')
    finally:
        final.close(True)
    print('SFRWFS PASS: mkfs, directories, rename and reboot persistence', flush=True)


if __name__ == '__main__':
    try:
        run()
    except Exception as error:
        print('SFRWFS FAIL:', error, flush=True)
        raise
