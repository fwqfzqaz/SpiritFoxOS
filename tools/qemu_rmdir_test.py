"""Empty RAM directories, canonical RPC, nonempty refusal and node reuse."""
import struct
from qemu_persist_test import VM,ROOT
from mkpersist import create


def run():
    source=(ROOT/'build/persistent.img').read_bytes();files={}
    keep={'/etc/readme.txt','/modules/disk-echo.sfmod','/bin/diskhello.sfk','/bin/pieprobe.sfk'}
    for i in range(struct.unpack_from('<I',source,24)[0]):
        name,off,size=struct.unpack_from('<64sQQ',source,512+i*128);name=name.rstrip(b'\0').decode()
        if name in keep:files[name]=source[off:off+size]
    assert set(files)==keep
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('ram-rmdir-'+label+'.img');disk.write_bytes(create(files))
        vm=VM(disk,'ram-rmdir-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes RMDIR-PRESERVED','write: OK');before=disk.read_bytes()
            vm.command('mkdir /tmp/outer','mkdir: OK');vm.command('mkdir /tmp/outer/inner','mkdir: OK');vm.command('write /tmp/outer/inner/item SECRET','write: OK')
            vm.command('rmdir /tmp/outer','rmdir: failed code=7');vm.command('rmdir /tmp/outer/inner','rmdir: failed code=7')
            vm.command('rm /tmp/outer/inner/item','rm: OK');vm.command('rmdir /tmp/outer/inner','rmdir: OK');vm.command('rmdir /tmp/outer','rmdir: OK');vm.command('stat /tmp/outer','stat: failed code=16')
            vm.command('mkdir /tmp/prefix','mkdir: OK');vm.command('mkdir /tmp/prefix2','mkdir: OK');vm.command('rmdir /tmp/prefix','rmdir: OK');vm.command('rmdir /tmp/prefix2','rmdir: OK')
            vm.command('mkdir "/tmp/quoted dir"','mkdir: OK');vm.command('rmdir "/tmp/quoted dir"','rmdir: OK')
            for _ in range(40):
                vm.command('mkdir /tmp/recycled','mkdir: OK');vm.command('write /tmp/recycled/item DATA','write: OK')
                vm.command('rmdir /tmp/recycled','rmdir: failed code=7');vm.command('rm /tmp/recycled/item','rm: OK');vm.command('rmdir /tmp/recycled','rmdir: OK')
            for command,code in [('rmdir /disk',17),('rmdir /persist',17),('rmdir /run/pipe',1),('rmdir /dev/console',1),('rmdir /tmp/notes',1),('rmdir /tmp',7),('rmdir /tmp/missing',16),('rmdir /',1),('rmdir relative',1),('rmdir /tmp/../notes',1),('rmdir /tmp//notes',1),('rmdir /tmp/notes/',1),('rmdir /tmp/a /tmp/b',1),('rmdir "unterminated',1),('rmdir',1)]:
                vm.command(command,'rmdir: failed code='+str(code))
            for _ in range(12):
                vm.command('runat /tmp fileprobe.sfk','OPENAT APP PASSED');vm.command('runat /disk/etc readprobe.sfk','OPENAT APP PASSED')
            vm.command('cat /persist/notes','RMDIR-PRESERVED');vm.command('cat /disk/etc/readme.txt','SpiritFoxOS read-only disk')
            vm.command('echo','echo: ping');vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'Empty directory mutation changed persistent bytes'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('RAM RMDIR QEMU PASS: 2 boots; nested nonempty/readonly/mounted/special refusal, quoted empty directories, prefix boundary and 40 reuse cycles, real read-only delegated RPC denial and persistent bytes/cleanup preserved',flush=True)


if __name__=='__main__':run()
