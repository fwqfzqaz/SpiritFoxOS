"""Native instance capacity rollback, lease drain, bounded death/health recovery."""
import time
from qemu_persist_test import VM,ROOT


def status(vm,state,attempts,leases=0,draining=0):
    marker=f'native: state={state} attempts={attempts} stable='
    start=vm.send('native-status');vm.expect(marker,start)
    line_start=vm.output.index(marker.encode(),start);vm.expect('fox> ',line_start)
    line=bytes(vm.output[line_start:]).splitlines()[0].strip()
    assert f'leases={leases} draining={draining}'.encode() in line,line


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('native-supervision-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'native-supervision-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes NATIVE-PRESERVED','write: OK');before=disk.read_bytes()
            vm.command('native-capacity-test','NATIVE CAPACITY TEST PASSED');status(vm,'RUNNING',0)
            start=vm.send('native-drain-test');vm.expect('NATIVE DRAIN TEST PASSED',start);vm.expect('native: recovered',start);status(vm,'RUNNING',1)
            vm.command('stop native','native: STOPPED');time.sleep(.75);status(vm,'STOPPED',0)
            vm.command('run hello.sfk','sfexec: failed code=12');vm.command('native-crash','native: crash refused')
            vm.command('echo','echo: ping');vm.command('start native','native: ACTIVE');status(vm,'RUNNING',0)
            faults=[('native-crash','crash',False),('native-hang','hang',True),('native-bad-reply','bad reply',True)]
            for attempt,(command,kind,health) in enumerate(faults,1):
                start=vm.send(command);vm.expect('native: '+kind+' injected',start)
                if health:vm.expect('native: health failure',start)
                vm.expect('native: recovered',start);status(vm,'RUNNING',attempt)
                vm.command('run hello.sfk','sfexec: exit 0')
            start=vm.send('native-bad-op');vm.expect('native: bad opcode injected',start);vm.expect('native: health failure',start);vm.expect('native: recovery blocked',start)
            status(vm,'BLOCKED',3);vm.command('run hello.sfk','sfexec: failed code=12');vm.command('echo','echo: ping')
            vm.command('cat /persist/notes','NATIVE-PRESERVED');vm.command('start native','native: ACTIVE');status(vm,'RUNNING',0)
            vm.command('runenv MODE=native -- argprobe.sfk --require-mode','sfexec: exit 0')
            vm.command('run /disk/bin/pieprobe.sfk','sfexec: exit 0');vm.command('runat /disk/etc readprobe.sfk','sfexec: exit 0')
            for _ in range(12):
                vm.command('stop native','native: STOPPED');vm.command('start native','native: ACTIVE');vm.command('run hello.sfk','sfexec: exit 0')
            assert disk.read_bytes()==before,'Native recovery changed persistent bytes'
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('NATIVE SUPERVISION QEMU PASS: 2 boots; actual capability/process allocation rollback, live app lease drain, bounded death/hang/protocol recovery, explicit stop/retry and private endpoint cleanup',flush=True)


if __name__=='__main__':run()
