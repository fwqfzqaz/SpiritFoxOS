"""Fixed-width root IPC copy counters on both production transports."""
from qemu_persist_test import VM,ROOT
import re

def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy'
        disk=ROOT/'build'/('ipc-stats-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'ipc-stats-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes IPC-STATS-PRESERVED','write: OK');before=disk.read_bytes()
            start=vm.send('ipc-stats');marker='ipc-stats: send-bytes='
            vm.expect(marker,start);field=vm.output.index(marker.encode(),start)+len(marker);vm.expect('\n',field)
            fields=re.search(rb'ipc-stats: send-bytes=(\d+) receive-bytes=(\d+)\r?\n',vm.output[start:])
            assert fields and all(0<int(n)<2**64 and int(n)%4168==0 for n in fields.groups())
            for _ in range(4):vm.command('ipc-stats-test','IPC COPY TEST PASSED')
            vm.command('run probe.sfk','CAPABILITY WIDTH APP PASSED')
            vm.command('page-quota-test','PAGE QUOTA TEST PASSED');vm.command('ipc-cancel-test','IPC CANCEL TEST PASSED')
            vm.command('runat /tmp fileclient.sfk','FILE PREAD APP PASSED')
            vm.command('run hello.sfk','sfexec: exit 0');vm.command('echo','echo: ping')
            vm.command('cat /persist/notes','IPC-STATS-PRESERVED')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'IPC observation changed persistent bytes'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('IPC STATS QEMU PASS: 2 boots; actual copy counters, root size/reserved/read-only guards, four private round trips, ordinary denial, quota/withdrawal/file IPC and persistent bytes preserved',flush=True)

if __name__=='__main__':run()
