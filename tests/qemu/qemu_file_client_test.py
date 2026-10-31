"""Production userspace file client over private capability IPC."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('file-client-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'file-client-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes SDK-PRESERVED','write: OK');before=disk.read_bytes()
            start=vm.send('run fileclient.sfk');vm.expect('FILE SDK CONSOLE',start);vm.expect('FILE SDK NO ROOT',start);vm.expect('sfexec: exit 0',start)
            for _ in range(20):
                for command in ('runat /tmp fileclient.sfk','runat /disk/etc fileclient.sfk readme.txt','runat /persist fileclient.sfk'):
                    start=vm.send(command);vm.expect('FILE DUP APP PASSED',start);vm.expect('FILE SDK APP PASSED',start);vm.expect('sfexec: exit 0',start)
            vm.command('runenvat /tmp MODE=sdk -- fileclient.sfk notes','FILE SDK APP PASSED')
            # Raw forged requests still test the server independently of SDK bounds.
            vm.command('runat /tmp fileprobe.sfk','OPENAT APP PASSED');vm.command('runat /disk/etc readprobe.sfk','OPENAT APP PASSED')
            vm.command('cat /persist/notes','SDK-PRESERVED');vm.command('run hello.sfk','sfexec: exit 0');vm.command('run pieprobe.sfk','sfexec: exit 0')
            vm.command('echo','echo: ping');vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'Read-only client or rejected mutations changed persistence'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('FILE CLIENT QEMU PASS: 2 boots; production codec/INT80, no ambient root, explicit RAM/disk/persist read-only roots, console/metadata/read/seek/poll/enumerate/close, duplicate shared offsets/rights/independent closes and root-copy lifetime, denied writes/mutations, high local handles, 60 repeated process reuses per transport and persistent bytes/leases preserved',flush=True)


if __name__=='__main__':run()
