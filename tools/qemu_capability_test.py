"""Canonical syscall capability values under both storage transports."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('capability-width-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'capability-width-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes CAP-PRESERVED','write: OK');before=disk.read_bytes()
            for _ in range(40):
                start=vm.send('run probe.sfk');vm.expect('CAPABILITY WIDTH APP PASSED',start);vm.expect('sfexec: exit 0',start)
            vm.command('run hello.sfk','sfexec: exit 0')
            start=vm.send('run pieprobe.sfk');vm.expect('PIE PASS base=',start);vm.expect('sfexec: exit 0',start)
            vm.command('runenv MODE=cap -- argprobe.sfk --require-mode','sfexec: exit 0')
            vm.command('runat /disk/etc readprobe.sfk','OPENAT APP PASSED');vm.command('runat /tmp fileprobe.sfk','OPENAT APP PASSED')
            vm.command('stat /persist/notes','stat: type=5 size=13 offset=0 mode=1 flags=6');vm.command('cat /persist/notes','CAP-PRESERVED')
            vm.command('echo','echo: ping');vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'Capability rejection changed persistent bytes'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('CAPABILITY WIDTH QEMU PASS: 2 boots; high-bit SEND/RECV/CLOSE/DERIVE/REVOKE rejected, valid IPC and last-reference cleanup, 40 task reuses, normal native/file RPC and persistent bytes preserved',flush=True)


if __name__=='__main__':run()
