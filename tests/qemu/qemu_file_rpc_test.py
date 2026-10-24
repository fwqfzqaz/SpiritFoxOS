"""IPC rejects oversized RPCs; later valid calls retain file/console isolation."""
import struct
from qemu_persist_test import VM,ROOT
from mkpersist import create


def run():
    source=(ROOT/'build/persistent.img').read_bytes();files={}
    for i in range(struct.unpack_from('<I',source,24)[0]):
        name,off,size=struct.unpack_from('<64sQQ',source,512+i*128)
        files[name.rstrip(b'\0').decode()]=source[off:off+size]
    files['/etc/readme.txt']+=b'B'*768
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('file-rpc-'+label+'.img');disk.write_bytes(create(files))
        vm=VM(disk,'file-rpc-'+label,iommu=iommu)
        try:
            vm.boot();before=disk.read_bytes()
            vm.command('run rpcprobe.sfk','sfexec: failed code=2')
            for _ in range(12):
                start=vm.send('runat /disk/etc rpcprobe.sfk')
                vm.expect('FILE RPC APP PASSED',start);vm.expect('sfexec: exit 0',start)
            assert disk.read_bytes()==before,'Read-only malformed RPCs wrote the disk'
            vm.command('write /persist/notes RPC-PRESERVED','write: OK')
            before=disk.read_bytes()
            start=vm.send('runat /persist rpcprobe.sfk notes')
            vm.expect('FILE RPC APP PASSED',start);vm.expect('sfexec: exit 0',start)
            assert disk.read_bytes()==before,'Malformed/delegated RPCs changed persistent notes'
            vm.command('cat /persist/notes','RPC-PRESERVED')
            vm.command('echo','echo: ping')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally: vm.close()
        assert b'^'*64 not in vm.output,'Oversized console write exposed bytes from the manager'
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('FILE RPC QEMU PASS: 2 boots; IPC rejects 257/512/2^31/UINT32_MAX read/write/enumerate, later bounded large-file read, console/persistent data/rights preserved and repeated cleanup',flush=True)


if __name__=='__main__': run()
