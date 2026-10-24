"""Unsupported installed invocations do not consume pending updates."""
from qemu_persist_test import VM,ROOT,snapshot,rewrite


def unchanged(vm,disk,command,code,pending):
    start=vm.send(command);vm.expect('sfexec: failed code='+str(code),start)
    assert b'update: TRIAL started' not in vm.output[start:],'Invalid invocation started update trial'
    assert disk.read_bytes()==pending,'Invalid invocation changed pending update bytes'
    vm.command('persist','update: PENDING')


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('invocation-preflight-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'invocation-preflight-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes PREFLIGHT-PRESERVED','write: OK')
            for package,version in [('diskhello','0.1.0'),('diskupdate','0.2.0')]:
                vm.command('install /disk/bin/'+package+'.sfk','install: COMMITTED');pending=disk.read_bytes()
                unchanged(vm,disk,'runat /disk/etc installed',2,pending)
                unchanged(vm,disk,'runenvat /disk/etc MODE=x -- /installed.sfk',2,pending)
                vm.command('stop native','native: STOPPED')
                unchanged(vm,disk,'run installed',12,pending)
                unchanged(vm,disk,'runenv MODE=x -- /installed.sfk',12,pending)
                vm.command('start native','native: ACTIVE')
                start=vm.send('run installed');vm.expect('update: CONFIRMED',start);vm.expect('sfexec: exit 0',start)
                vm.command('persist','update: confirmed-version='+version+' policy=monotonic')
            vm.command('install /disk/bin/disknext.sfk','install: COMMITTED')
        finally:vm.close()
        data=disk.read_bytes();files=snapshot(data)[2];candidate=chr(files['/boot/current'][10]);assert candidate in 'ab'
        path='/slots/'+candidate+'.sfk';broken=bytearray(files[path]);broken[-1]^=1;disk.write_bytes(rewrite(data,{path:bytes(broken)}))
        vm=VM(disk,'invocation-preflight-corrupt-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('persist','update: PENDING')
            start=vm.send('runat /disk/etc installed');vm.expect('update: TRIAL started',start);vm.expect('update: ROLLED BACK',start);vm.expect('sfexec: failed code=4',start)
            vm.command('run installed','sfexec: exit 0');vm.command('persist','update: confirmed-version=0.2.0 policy=monotonic')
            vm.command('cat /persist/notes','PREFLIGHT-PRESERVED')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('INVOCATION PREFLIGHT QEMU PASS: 4 boots; invalid root capability/native unavailable preserve exact PENDING bytes, valid retry confirms, corrupt candidate still trials/rolls back, good version/notes and cleanup preserved',flush=True)


if __name__=='__main__':run()
