"""Directory authority in real isolated apps, with mount and failure cleanup."""
import hashlib
from qemu_persist_test import VM, ROOT


def run():
    disk = ROOT/'build/storage.img'
    original = hashlib.sha256(disk.read_bytes()).digest()
    for iommu in (False, True):
        vm = VM(disk, 'openat-'+('modern' if iommu else 'legacy'), iommu=iommu, writable=False)
        try:
            vm.boot('STORAGE READY')
            vm.command('run fileprobe.sfk', 'sfexec: failed code=2')
            start = vm.send('runat /tmp fileprobe.sfk')
            vm.expect('OPENAT APP PASSED', start)
            vm.expect('sfexec: exit 0', start)
            for directory, code in [('/missing',16),('/tmp/notes',1),('/tmp/',1)]:
                vm.command('runat '+directory+' fileprobe.sfk', 'sfexec: failed code='+str(code))
            # Signature/requested capability policy fails before a child exists.
            vm.command('runat /disk/etc hello.sfk', 'sfexec: failed code=2')
            vm.command('runat /disk/etc missing.sfk', 'sfexec: failed code=16')
            vm.command('runat /disk/etc echo.sfmod', 'sfexec: failed code=2')
            vm.command('echo', 'echo: ping')
            # Each reader intentionally leaves its file description open. More
            # than 32 runs expose leaks in description or directory cleanup.
            for _ in range(40):
                start = vm.send('runat /disk/etc readprobe.sfk')
                vm.expect('OPENAT APP PASSED', start)
                vm.expect('sfexec: exit 0', start)
            vm.command('runat /disk/etc fileprobe.sfk', 'sfexec: failed code=9')
            vm.command('runat /disk/etc fault.sfk', 'sfexec: failed code=114')
            vm.command('runat /disk/etc spinner.sfk', 'sfexec: failed code=9')
            vm.command('echo', 'echo: ping')
            vm.command('unmount /disk', 'disk: UNMOUNTED')
            vm.command('stop disk', 'disk: STOPPED')
            vm.command('start disk', 'disk: ACTIVE')
            vm.command('mount /disk', 'disk: MOUNTED')
            start = vm.send('runat /disk/etc readprobe.sfk')
            vm.expect('OPENAT APP PASSED', start)
            vm.expect('sfexec: exit 0', start)
            vm.command('unmount /disk', 'disk: UNMOUNTED')
        finally:
            vm.close()
        assert hashlib.sha256(disk.read_bytes()).digest() == original
    print('OPENAT QEMU PASS: explicit directory authority, no ambient root, read-only attenuation, traversal/forgery rejection, requested capability policy, 40-run cleanup, fault/timeout and mount recovery', flush=True)


if __name__ == '__main__':
    run()
