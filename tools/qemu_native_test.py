"""Two-base private native images, signed relocation refusal and persistence."""
import struct
from qemu_persist_test import VM,ROOT
from mkpersist import create
from sfpack import Crypto,seal


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    count=struct.unpack_from('<I',source,24)[0]
    files={}
    for i in range(count):
        name,off,size=struct.unpack_from('<64sQQ',source,512+i*128)
        path=name.rstrip(b'\0').decode()
        if path in ('/etc/readme.txt','/modules/disk-echo.sfmod','/bin/diskhello.sfk','/bin/pieprobe.sfk'):
            files[path]=source[off:off+size]
    good=(ROOT/'build/pieprobe.sfk').read_bytes()
    relocation=struct.unpack_from('<Q',good,96+3*64+8)[0]
    crypto=Crypto();seed=(ROOT/'.keys/application.seed').read_bytes()
    bad=bytearray(good);struct.pack_into('<Q',bad,relocation,0)
    files['/bin/badreloc.sfk']=seal(bad,seed,crypto)
    bad=bytearray(good);struct.pack_into('<Q',bad,relocation+16,struct.unpack_from('<Q',bad,relocation)[0])
    files['/bin/baddup.sfk']=seal(bad,seed,crypto)
    files['/bin/badprofile.sfk']=seal(good.replace(b'relative-v1',b'unknown--v1'),seed,crypto)
    for iommu in (False,True):
        label='modern' if iommu else 'legacy'
        disk=ROOT/'build'/('native-'+label+'.img');disk.write_bytes(create(files))
        vm=VM(disk,'native-'+label,iommu=iommu)
        try:
            vm.boot();vm.expect('NATIVE BASE TESTS PASSED');vm.expect('NATIVE PRIVATE IMAGES PASSED')
            for base in ('44000000','46000000'):
                start=vm.send('run pieprobe.sfk')
                vm.expect('PIE PASS base=0x'+base+' counter=8',start);vm.expect('sfexec: exit 0',start)
            vm.command('runenv MODE=demo -- pieprobe.sfk --require-mode','sfexec: exit 0')
            vm.command('run pieprobe.sfk --require-mode','sfexec: failed code=42')
            vm.command('runenvat /disk/etc MODE=demo -- pieprobe.sfk --require-mode','sfexec: exit 0')
            vm.command('run pieprobe.sfk --write-code','sfexec: failed code=114')
            vm.command('echo','echo: ping')
            before=disk.read_bytes()
            for name in ('badreloc','baddup','badprofile'):
                vm.command('run /disk/bin/'+name+'.sfk','sfexec: failed code=3')
                vm.command('install /disk/bin/'+name+'.sfk','install: failed code=3')
                assert disk.read_bytes()==before,'Rejected relocation package changed persistence'
            vm.command('run /disk/bin/pieprobe.sfk','sfexec: exit 0')
            vm.command('install /disk/bin/pieprobe.sfk','install: COMMITTED')
            start=vm.send('runenv MODE=installed -- installed --require-mode')
            vm.expect('update: CONFIRMED',start);vm.expect('sfexec: exit 0',start)
            confirmed=disk.read_bytes()
            for _ in range(12): vm.command('run installed','sfexec: exit 0')
            assert disk.read_bytes()==confirmed
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally: vm.close()
        vm=VM(disk,'native-'+label+'-reboot',iommu=iommu)
        try:
            vm.boot()
            for _ in range(2): vm.command('run installed','sfexec: exit 0')
            assert disk.read_bytes()==confirmed,'Confirmed PI app rerun wrote persistent state'
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally: vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('NATIVE QEMU PASS: 4 boots; two-base relocation, simultaneous private images, BSS/function/data pointers, RO code fault, argv/env, signed malformed refusal without writes, install confirmation, repeated reap and reboot',flush=True)


if __name__=='__main__': run()
