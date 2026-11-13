"""Chunked package staging/export and empty-path refusals use actual IPC."""
import json
import struct
from qemu_persist_test import VM,ROOT,create_readfs,generation,snapshot


def fixture(source):
    files={}
    for i in range(struct.unpack_from('<I',source,24)[0]):
        name,offset,size=struct.unpack_from('<64sQQ',source,512+i*128)
        files[name.rstrip(b'\0').decode()]=source[offset:offset+size]
    # Keep the fixed node budget; SFRFS2 explicitly supports empty files.
    files['/bin/diskexit.sfk']=b''
    readonly=create_readfs(files,len(source),format=2)
    assert all(sum(struct.unpack_from('<QQ',readonly,512+i*128+64))<=2048*512 for i in range(len(files)))
    return readonly[:2048*512]+source[2048*512:],files['/bin/diskhello.sfk']


def refuse_empty(vm,disk):
    before=disk.read_bytes();old=generation(disk);refused=0
    for _ in range(4):
        vm.command('install /disk/bin/diskexit.sfk','install: failed code=3');refused+=1
        vm.command('revoke-key /disk/bin/diskexit.sfk','revoke: failed code=3');refused+=1
        assert disk.read_bytes()==before and generation(disk)==old,'Empty file consumed stale staging or changed committed state'
        vm.command('trust','trust: anchors=3 revoked=0')
        vm.command('run hello.sfk','sfexec: exit 0')
    vm.command('cat /persist/notes','CHUNK-PRESERVED')
    assert disk.read_bytes()==before
    return refused


def run():
    source=(ROOT/'build/persistent.img').read_bytes();image,package=fixture(source)
    assert 2*4096<len(package)<=16384,'The real signed package fixture must cross three payloads'
    refused=reattach=0
    for modern in (False,True):
        label='modern' if modern else 'legacy';disk=ROOT/'build'/('persist-chunk-'+label+'.img');disk.write_bytes(image)
        vm=VM(disk,'persist-chunk-'+label,iommu=modern)
        try:
            vm.boot('PERSIST READY')
            vm.command('write /persist/notes CHUNK-PRESERVED','write: OK')
            vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
            vm.command('persist','update: PENDING')
            assert package in snapshot(disk.read_bytes())[2].values(),'Chunked STAGE changed signed package bytes'
            refused+=refuse_empty(vm,disk)
            # Installed launch reads PREVIEW before TRYING, then PACKAGE;
            # signature/image checks and actual execution cover every chunk.
            vm.command('run installed','sfexec: exit 0')
            vm.command('persist','update: STABLE good=a candidate=-')
            assert package in snapshot(disk.read_bytes())[2].values()
            refused+=refuse_empty(vm,disk)
            stable=disk.read_bytes()
            for _ in range(2):
                vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
                vm.command('start disk','disk: ACTIVE');vm.command('mount /disk','disk: MOUNTED');reattach+=1
                vm.command('persist','update: STABLE good=a candidate=-')
                vm.command('run installed','sfexec: exit 0')
                vm.command('cat /persist/notes','CHUNK-PRESERVED')
                assert disk.read_bytes()==stable,'Driver reattach/read-only installed launch changed stable storage'
            vm.command('echo','echo: ping')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source and refused==32 and reattach==4
    result={'passed':True,'guest_boots':2,'empty_file_rejections':refused,'stage_bytes':len(package),'package_chunks':(len(package)+4095)//4096,'driver_reattach_cycles':reattach,'base_disk_unchanged':True}
    (ROOT/'build/persist-chunk-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PERSIST CHUNK QEMU PASS: '+json.dumps(result)+'; signed PENDING/STABLE package, normal preview/export, no stale stage on empty install/revoke, notes/trust/ordinary apps preserved',flush=True)


if __name__=='__main__':run()
