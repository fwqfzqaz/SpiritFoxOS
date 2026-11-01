"""Root-controlled priorities, timer preemption and low-priority progress."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy'
        disk=ROOT/'build'/('scheduler-priority-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'scheduler-priority-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes PRIORITY-PRESERVED','write: OK');before=disk.read_bytes()
            vm.command('priority 1','priority: pid=1 level=1')
            vm.command('priority 1 3','priority: pid=1 level=3')
            vm.command('priority 1 1','priority: pid=1 level=1')
            for command in ['priority','priority 0','priority 1 4','priority 4294967297 3','priority 2147483648 3',
                            'priority -1 3','priority 1 -1','priority 1 1 extra','priority 1x','priority "unterminated']:
                vm.command(command,'priority: failed code=1')
            vm.command('priority 2147483647','priority: failed code=16')
            for _ in range(4):
                vm.command('priority-test','PRIORITY SCHEDULER TEST PASSED')
                vm.command('priority 1','priority: pid=1 level=1')
            vm.command('run probe.sfk','CAPABILITY WIDTH APP PASSED')
            vm.command('runat /tmp fileclient.sfk','FILE PREAD APP PASSED')
            vm.command('run hello.sfk','sfexec: exit 0');vm.command('run pieprobe.sfk','sfexec: exit 0')
            vm.command('cat /persist/notes','PRIORITY-PRESERVED');vm.command('echo','echo: ping')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'Priority control or task cleanup changed persistent bytes'
        finally:
            vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('PRIORITY QEMU PASS: 2 boots; root query/set and canonical CLI, unprivileged denial, 4 pairs of noncooperating high-priority tasks per transport with lowest-priority manager progress, restore/reap and apps/disk unchanged',flush=True)


if __name__=='__main__':run()
