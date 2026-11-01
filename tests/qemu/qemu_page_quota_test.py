"""Private pool-page budgets and limited construction on production kernels."""
from qemu_persist_test import VM,ROOT
import re

def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy'
        disk=ROOT/'build'/('page-quota-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'page-quota-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes PAGE-QUOTA-PRESERVED','write: OK');before=disk.read_bytes()
            start=vm.send('page-limit 1');marker='page-limit: pid=1 limit=4096 used='
            vm.expect(marker,start);field=vm.output.index(marker.encode(),start)+len(marker);vm.expect('\n',field)
            usage=re.search(rb'page-limit: pid=1 limit=4096 used=(\d+)\r?\n',vm.output[start:])
            assert usage and 0<int(usage.group(1))<=4096
            vm.command('page-limit 1 0','page-limit: failed code=7')
            vm.command('page-limit 1','page-limit: pid=1 limit=4096 used=')
            for command in ['page-limit','page-limit 0','page-limit 1 4097','page-limit 4294967297 3',
                            'page-limit 2147483648 3','page-limit -1 3','page-limit 1 -1',
                            'page-limit 1 2 extra','page-limit 1x','page-limit "unterminated']:
                vm.command(command,'page-limit: failed code=1')
            vm.command('page-limit 2147483647','page-limit: failed code=16')
            for _ in range(4):vm.command('page-quota-test','PAGE QUOTA TEST PASSED')
            vm.command('cap-limit-test','CAP LIMIT TEST PASSED')
            vm.command('run probe.sfk','CAPABILITY WIDTH APP PASSED')
            vm.command('runat /tmp fileclient.sfk','FILE PREAD APP PASSED')
            vm.command('run hello.sfk','sfexec: exit 0');vm.command('echo','echo: ping')
            vm.command('cat /persist/notes','PAGE-QUOTA-PRESERVED')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'Pool-page policy changed persistent bytes'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('PAGE QUOTA QEMU PASS: 2 boots; limited spawn rollback, root live usage/limit/busy, exact-use lowering, uncooperative child preemption, death/default reuse, ordinary denial, file IPC and disk bytes preserved',flush=True)

if __name__=='__main__':run()
