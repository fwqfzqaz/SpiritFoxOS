"""Live kernel handle quotas without revoking existing IPC authority."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy'
        disk=ROOT/'build'/('cap-limit-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'cap-limit-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes CAP-LIMIT-PRESERVED','write: OK');before=disk.read_bytes()
            vm.command('cap-limit 1','cap-limit: pid=1 limit=32')
            vm.command('cap-limit 1 0','cap-limit: failed code=7')
            vm.command('cap-limit 1','cap-limit: pid=1 limit=32')
            for command in ['cap-limit','cap-limit 0','cap-limit 1 33','cap-limit 4294967297 3',
                            'cap-limit 2147483648 3','cap-limit -1 3','cap-limit 1 -1',
                            'cap-limit 1 2 extra','cap-limit 1x','cap-limit "unterminated']:
                vm.command(command,'cap-limit: failed code=1')
            vm.command('cap-limit 2147483647','cap-limit: failed code=16')
            for _ in range(4):
                start=vm.send('cap-limit-test');vm.expect('CAP LIMIT APP PASSED',start);vm.expect('CAP LIMIT TEST PASSED',start)
            vm.command('cap-limit 1','cap-limit: pid=1 limit=32')
            vm.command('run probe.sfk','CAPABILITY WIDTH APP PASSED')
            vm.command('priority-test','PRIORITY SCHEDULER TEST PASSED')
            vm.command('runat /tmp fileclient.sfk','FILE PREAD APP PASSED')
            vm.command('run hello.sfk','sfexec: exit 0');vm.command('echo','echo: ping')
            vm.command('cat /persist/notes','CAP-LIMIT-PRESERVED')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'Kernel handle limits changed persistent bytes'
        finally:
            vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('CAP LIMIT QEMU PASS: 2 boots; root query/configure, busy lowering preserved, create/derive refusal without endpoint leak, close/lower and existing two-cap file IPC, ordinary denial, 4 staged process reuses per transport and persistent bytes preserved',flush=True)


if __name__=='__main__':run()
