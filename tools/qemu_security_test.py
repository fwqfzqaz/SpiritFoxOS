"""Fail closed after a driver dies, retain committed data and recover on reset."""
import shutil
from qemu_persist_test import VM, ROOT, generation


def run():
    for iommu in (False, True):
        disk = ROOT/'build'/('security-'+('modern' if iommu else 'legacy')+'.img')
        shutil.copyfile(ROOT/'build/persistent.img', disk)
        vm = VM(disk, 'security-'+('modern' if iommu else 'legacy'), iommu=iommu)
        try:
            vm.boot()
            if iommu and b'MMIO BUFFER TESTS PASSED' not in vm.output:
                raise RuntimeError('Missing real MMIO syscall buffer tests')
            vm.command('write /persist/notes failure-safe', 'write: OK')
            vm.command('install /disk/bin/diskhello.sfk', 'install: COMMITTED')
            vm.command('run installed', 'sfexec: exit 0')
            vm.command('persist', 'persist: ACTIVE generation=5')
            committed = disk.read_bytes()
            vm.command('disk-crash', 'STORAGE CRASH TEST PASSED')
            vm.command('persist', 'persist: UNAVAILABLE generation=5')
            vm.command('cat /persist/notes', 'cat: error')
            vm.command('write /persist/notes forbidden', 'write: error')
            vm.command('run installed', 'sfexec: failed code=10')
            vm.command('install /disk/bin/diskupdate.sfk', 'install: failed code=10')
            assert disk.read_bytes() == committed, 'Offline access modified disk'
            vm.command('echo', 'echo: ping')
            vm.command('run hello.sfk', 'sfexec: exit 0')
            vm.command('unmount /disk', 'disk: UNMOUNTED')
            vm.command('stop disk', 'disk: STOPPED')
            vm.command('start disk', 'disk: ACTIVE')
            vm.command('mount /disk', 'disk: MOUNTED')
            vm.command('cat /persist/notes', 'failure-safe')
            vm.command('run installed', 'sfexec: exit 0')
            assert generation(disk) == 5
            assert disk.read_bytes() == committed
            if iommu:
                vm.command('dma-test write', 'DMA PROBE PASSED: guard intact, device quarantined')
                vm.command('persist', 'persist: UNAVAILABLE generation=5')
                vm.command('cat /persist/notes', 'cat: error')
                vm.command('run installed', 'sfexec: failed code=10')
                vm.command('echo', 'echo: ping')
                assert disk.read_bytes() == committed
        finally:
            vm.close()
    print('SECURITY QEMU PASS: MMIO buffers, endpoint churn, exit-code identity, driver death, cache refusal, dirty release, reset recovery and persistent DMA quarantine', flush=True)


if __name__ == '__main__':
    run()
