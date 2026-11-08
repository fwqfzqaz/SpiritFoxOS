"""Read-only /proc, /sys and /net projections over the real kernel."""
from qemu_persist_test import VM,ROOT


def run():
    disk=ROOT/'build/projection.img'
    disk.write_bytes((ROOT/'build/persistent.img').read_bytes())
    vm=VM(disk,'projection')
    try:
        vm.boot()
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
    finally:
        vm.close()
    print('PROJECTION QEMU PASS: read-only /proc /sys, live text projection, protected system directories and repeated reads',flush=True)


if __name__=='__main__':run()
