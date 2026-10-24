"""RAM-only deletion, bounded directory creation and delegated RPC denial."""
import struct
from qemu_persist_test import VM,ROOT
from mkpersist import create


def run():
    source=(ROOT/'build/persistent.img').read_bytes();files={}
    keep={'/etc/readme.txt','/modules/disk-echo.sfmod','/bin/diskhello.sfk','/bin/pieprobe.sfk'}
    for i in range(struct.unpack_from('<I',source,24)[0]):
        name,off,size=struct.unpack_from('<64sQQ',source,512+i*128)
        name=name.rstrip(b'\0').decode()
        if name in keep: files[name]=source[off:off+size]
    assert set(files)==keep
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('ram-unlink-'+label+'.img');disk.write_bytes(create(files))
        vm=VM(disk,'ram-unlink-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes UNLINK-PRESERVED','write: OK');before=disk.read_bytes()
            vm.command('mkdir /tmp/work','mkdir: OK')
            vm.command('write /tmp/work/item ORIGINAL-SECRET','write: OK');vm.command('cat /tmp/work/item','ORIGINAL-SECRET')
            vm.command('rm /tmp/work/item','rm: OK');vm.command('cat /tmp/work/item','cat: error')
            vm.command('write /tmp/work/item NEW','write: OK');start=vm.send('cat /tmp/work/item');vm.expect('NEW',start);vm.expect('fox> ',start)
            assert b'ORIGINAL-SECRET' not in vm.output[start:]
            vm.command('rm /tmp/work/item','rm: OK')
            vm.command('mkdir "/tmp/quoted dir"','mkdir: OK');vm.command('mkdir "/tmp/quoted dir"','mkdir: failed code=7')
            # More creates than the fixed node quota, each reclaimed before reuse.
            for _ in range(40):
                vm.command('write /tmp/work/recycled DATA','write: OK');vm.command('rm "/tmp/work/recycled"','rm: OK')
            for command,code in [('rm /disk/etc/readme.txt',17),('rm /persist/notes',17),('rm /run/pipe',1),('rm /dev/console',1),('rm /tmp/work',1),('rm /tmp/missing',16),('rm /tmp/../notes',1),('rm /tmp//notes',1),('rm /tmp/notes/',1),('rm relative',1),('rm /tmp/a /tmp/b',1),('rm "unterminated',1),('rm',1),('mkdir /disk/etc/new',17),('mkdir /persist/new',17),('mkdir /tmp/../escape',1),('mkdir',1)]:
                vm.command(command,command.split()[0]+': failed code='+str(code))
            vm.command('runat /disk/etc readprobe.sfk','OPENAT APP PASSED');vm.command('runat /tmp fileprobe.sfk','OPENAT APP PASSED')
            vm.command('cat /persist/notes','UNLINK-PRESERVED');vm.command('cat /disk/etc/readme.txt','SpiritFoxOS read-only disk')
            vm.command('echo','echo: ping');vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==before,'RAM mutations or denied delegated unlink changed persistent disk'
        finally: vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('RAM UNLINK QEMU PASS: 2 boots; 40 slot reuse cycles per transport, quoted mkdir/rm, read-only/special/path refusal, real delegated RPC denied, persistent bytes and cleanup preserved',flush=True)


if __name__=='__main__': run()
