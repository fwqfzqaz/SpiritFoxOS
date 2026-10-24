"""Cold concurrent reads across driver-instance reply channel generations."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('block-channel-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'block-channel-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes CHANNEL-PRESERVED','write: OK');before=disk.read_bytes()
            for _ in range(8):
                start=vm.send('channel-test');vm.expect('RPC CHANNEL TEST PASSED',start)
                assert b'rpc-channels: failed' not in vm.output[start:]
                vm.command('echo','echo: ping')
                vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
                vm.command('start disk','disk: ACTIVE');vm.command('mount /disk','disk: MOUNTED')
                assert disk.read_bytes()==before,'Read/restart channel test changed persistent state'
            vm.command('cat /persist/notes','CHANNEL-PRESERVED')
            vm.command('run /disk/bin/diskhello.sfk','sfexec: exit 0')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('BLOCK CHANNEL QEMU PASS: 2 boots; cold concurrent directory reads, 8 driver restarts per transport, private reply generations, file/directory/task/endpoint cleanup and persistent bytes unchanged',flush=True)


if __name__=='__main__':run()
