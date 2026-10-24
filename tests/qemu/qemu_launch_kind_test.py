"""Authenticated package kind must match launch intent, independent of suffix."""
import struct
from qemu_persist_test import VM, ROOT
from mkpersist import create


def run():
    source=(ROOT/'build/persistent.img').read_bytes(); original={}
    for i in range(struct.unpack_from('<I',source,24)[0]):
        name,off,size=struct.unpack_from('<64sQQ',source,512+i*128)
        original[name.rstrip(b'\0').decode()]=source[off:off+size]
    hello=(ROOT/'build/hello.sfk').read_bytes()
    echo=(ROOT/'build/echo.sfmod').read_bytes()
    files={key:original[key] for key in ['/etc/readme.txt','/bin/diskhello.sfk','/bin/pieprobe.sfk']}
    files.update({'/modules/disk-echo.sfmod':hello,'/bin/module.sfk':echo,'/modules/application.sfmod':hello})
    for iommu in (False,True):
        label='modern' if iommu else 'legacy'
        disk=ROOT/'build'/('launch-kind-'+label+'.img');disk.write_bytes(create(files))
        vm=VM(disk,'launch-kind-'+label,iommu=iommu)
        try:
            vm.boot();vm.expect('DISK PACKAGE REJECTED module code=2')
            before=disk.read_bytes()
            rejected=['run echo.sfmod','run sf-native.sfmod','run sf-virtio-blk.sfmod','run /disk/bin/module.sfk',
                      'runat /disk/etc echo.sfmod','runenv X=1 -- echo.sfmod','runenvat /disk/etc X=1 -- /disk/bin/module.sfk']
            for _ in range(12):
                for command in rejected:
                    start=vm.send(command);vm.expect('sfexec: failed code=2',start)
                    assert b'sfexec: exit ' not in vm.output[start:]
                vm.command('run /disk/modules/application.sfmod','sfexec: exit 0')
                vm.command('run hello.sfk','sfexec: exit 0')
                vm.command('run /disk/bin/pieprobe.sfk','sfexec: exit 0')
                vm.command('echo','echo: ping')
            assert disk.read_bytes()==before,'Launch-kind rejection/readonly applications changed disk'
            vm.command('install /disk/bin/module.sfk','install: failed code=6')
            assert disk.read_bytes()==before,'Module install changed persistent state'
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            vm.command('run hello.sfk','sfexec: exit 0')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('LAUNCH KIND QEMU PASS: 2 boots; authenticated module/app intent, suffix independence, repeated failure cleanup, ordinary and PI apps, runtime availability and disk bytes preserved',flush=True)


if __name__=='__main__':run()
