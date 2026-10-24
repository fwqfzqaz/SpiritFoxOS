"""Per-instance echo replies survive cold concurrent reads and 40 generations."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('echo-channel-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'echo-channel-'+label,iommu=iommu)
        try:
            vm.boot();vm.expect('old and new service generations coexist')
            vm.command('echo-capacity-test','ECHO CAPACITY TEST PASSED')
            vm.command('write /persist/notes ECHO-PRESERVED','write: OK');before=disk.read_bytes()
            for _ in range(40):
                start=vm.send('echo-channel-test');vm.expect('ECHO CHANNEL TEST PASSED',start)
                assert b'echo-channels: failed' not in vm.output[start:]
                vm.command('stop echo','echo: STOPPED');vm.command('start echo','echo: ACTIVE')
                vm.command('echo','echo: ping')
                vm.command('unmount /disk','disk: UNMOUNTED');vm.command('mount /disk','disk: MOUNTED')
                assert disk.read_bytes()==before,'Echo/read/remount changed persistent bytes'
            vm.command('cat /persist/notes','ECHO-PRESERVED')
            vm.command('run hello.sfk','sfexec: exit 0')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('ECHO CHANNEL QEMU PASS: 2 boots; per-instance replies, concurrent echo plus cold directory reads, 40 stop/start generations per transport, old/new coexistence, actual allocation-failure rollback, endpoint/task/namespace cleanup and persistent bytes unchanged',flush=True)


if __name__=='__main__':run()
