"""Authenticated sender withdrawal while a manager deliberately keeps its queue."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy'
        disk=ROOT/'build'/('ipc-cancel-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'ipc-cancel-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes CANCEL-PRESERVED','write: OK');before=disk.read_bytes()
            for _ in range(8):
                start=vm.send('ipc-cancel-test');vm.expect('IPC CANCEL APP PASSED',start);vm.expect('IPC CANCEL TEST PASSED',start)
            vm.command('run probe.sfk','CAPABILITY WIDTH APP PASSED')
            vm.command('cap-limit-test','CAP LIMIT TEST PASSED')
            vm.command('priority-test','PRIORITY SCHEDULER TEST PASSED')
            vm.command('runat /tmp fileclient.sfk','FILE PREAD APP PASSED')
            vm.command('run hello.sfk','sfexec: exit 0');vm.command('echo','echo: ping')
            vm.command('cat /persist/notes','CANCEL-PRESERVED')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'IPC withdrawal changed persistent bytes'
        finally:
            vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('IPC CANCEL QEMU PASS: 2 boots; 8 gated processes per transport, sender isolation on shared manager endpoint, full 64-bit IDs/zero/duplicates/wrapped FIFO, delivered no-op and quota/priority/file IPC cleanup with persistent bytes preserved',flush=True)


if __name__=='__main__':run()
