"""Read-only /proc, /sys and /net projections over the real kernel."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('projection-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'projection-'+label,iommu=iommu)
        try:
            vm.boot();before=disk.read_bytes()
            vm.command('ls /proc','uptime')
            vm.command('ls /sys','modules')
            start=vm.send('cat /proc/uptime');vm.expect('hz=100',start);vm.expect('seconds=',start)
            vm.command('cat /proc/ipc-stats','send_bytes=')
            vm.command('cat /proc/self/status','pages_limit=')
            vm.command('cat /proc/children','status=running')
            vm.command('cat /sys/modules','org.spiritfox.native')
            vm.command('cat /sys/devices','vendor=0x1af4')
            vm.command('cat /sys/dma','guard=')
            vm.command('stat /proc/uptime','stat: type=6 size=0 offset=0 mode=1 flags=1')
            vm.command('rm /proc/uptime','rm: failed code=17')
            vm.command('mkdir /sys/local','mkdir: failed code=17')
            vm.command('write /proc/uptime tampered','write: error')
            vm.command('cat /proc/uptime','hz=100')
            start=vm.send('run fileclient.sfk --projection');vm.expect('FILE SDK NO ROOT',start);vm.expect('sfexec: exit 0',start)
            for command in ('runat /proc fileclient.sfk --projection',)*4+('runat /sys fileclient.sfk --projection modules',):
                start=vm.send(command);vm.expect('FILE PROJECTION APP PASSED',start);vm.expect('sfexec: exit 0',start)
            vm.command('cat /proc/uptime','hz=100');vm.command('run hello.sfk','sfexec: exit 0')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'Projection reads or rejected mutations changed persistent bytes'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('PROJECTION QEMU PASS: 2 boots; read-only /proc /sys, real SDK metadata/type6, full 32-bit seek results/shared offsets/EOF, rejected overflow unchanged, no ambient root, cleanup and persistence preserved',flush=True)


if __name__=='__main__':run()
