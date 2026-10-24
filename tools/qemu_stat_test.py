"""Handle metadata, canonical wire handles and unchanged file authority."""
import struct
from qemu_persist_test import VM,ROOT
from mkpersist import create


def run():
    source=(ROOT/'build/persistent.img').read_bytes();files={}
    keep={'/etc/readme.txt','/modules/disk-echo.sfmod','/bin/diskhello.sfk','/bin/pieprobe.sfk'}
    for i in range(struct.unpack_from('<I',source,24)[0]):
        name,off,size=struct.unpack_from('<64sQQ',source,512+i*128);name=name.rstrip(b'\0').decode()
        if name in keep: files[name]=source[off:off+size]
    assert set(files)==keep
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('file-stat-'+label+'.img');disk.write_bytes(create(files))
        vm=VM(disk,'file-stat-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes META-PRESERVED','write: OK');before=disk.read_bytes()
            vm.command('write /tmp/metadata abc','write: OK');vm.command('stat /tmp/metadata','stat: type=2 size=3 offset=0 mode=1 flags=4')
            vm.command('truncate /tmp/metadata 9','truncate: OK');vm.command('stat /tmp/metadata','stat: type=2 size=9 offset=0 mode=1 flags=4')
            vm.command('mkdir "/tmp/metadata dir"','mkdir: OK');vm.command('stat "/tmp/metadata dir"','stat: type=1 size=0 offset=0 mode=1 flags=0')
            vm.command('stat /dev/console','stat: type=4 size=0 offset=0 mode=1 flags=0')
            start=vm.send('stat /run/pipe');vm.expect('stat: type=3 size=',start);vm.expect(' mode=1 flags=0',start)
            vm.command('stat /disk/etc/readme.txt',f'stat: type=5 size={len(files["/etc/readme.txt"])} offset=0 mode=1 flags=7')
            vm.command('stat /persist/notes','stat: type=5 size=14 offset=0 mode=1 flags=6')
            for command,code in [('stat',1),('stat relative',1),('stat /tmp/../metadata',1),('stat "unterminated',1),('stat /tmp/metadata extra',1),('stat /tmp/missing',16)]:
                vm.command(command,'stat: failed code='+str(code))
            for _ in range(40):vm.command('stat /tmp/metadata','stat: type=2 size=9 offset=0 mode=1 flags=4')
            # Each app checks real forged READ/CLOSE handling, unchanged root,
            # 32-byte directory/file records, shared current offset and stale close.
            for _ in range(12):
                vm.command('runat /disk/etc readprobe.sfk','OPENAT APP PASSED');vm.command('runat /tmp fileprobe.sfk','OPENAT APP PASSED')
            vm.command('rm /tmp/metadata','rm: OK');vm.command('stat /tmp/metadata','stat: failed code=16');vm.command('cat /persist/notes','META-PRESERVED')
            vm.command('echo','echo: ping');vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'Metadata or malformed handle changed persistent bytes'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('FILE STAT QEMU PASS: 2 boots; fixed metadata/current sizes, no authority expansion, real high-handle rejection/root preserved, repeated RPC/descriptor cleanup and persistent bytes unchanged',flush=True)


if __name__=='__main__':run()
