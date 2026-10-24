"""Periodic authenticated echo health detects actual hangs and malformed replies."""
from qemu_persist_test import VM,ROOT
from qemu_supervision_test import status


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('echo-health-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'echo-health-'+label,iommu=iommu)
        try:
            vm.boot();vm.command('write /persist/notes HEALTH-PRESERVED','write: OK');before=disk.read_bytes()
            faults=[('echo-hang','hang'),('echo-bad-reply','bad reply'),('echo-bad-op','bad opcode')]
            for attempt,(command,kind) in enumerate(faults,1):
                start=vm.send(command);vm.expect('echo: '+kind+' injected',start);vm.expect('echo: health failure',start);vm.expect('echo: recovered',start)
                status(vm,'RUNNING',attempt,0);vm.command('echo','echo: ping')
            start=vm.send('echo-hang');vm.expect('echo: hang injected',start);vm.expect('echo: health failure',start);vm.expect('echo: recovery blocked',start)
            status(vm,'BLOCKED',3,0);vm.command('echo','echo: service unavailable')
            vm.command('run hello.sfk','sfexec: exit 0');vm.command('run /disk/bin/pieprobe.sfk','sfexec: exit 0')
            vm.command('cat /persist/notes','HEALTH-PRESERVED');vm.command('start echo','echo: ACTIVE')
            for _ in range(6):
                vm.command('echo-channel-test','ECHO CHANNEL TEST PASSED')
                vm.command('unmount /disk','disk: UNMOUNTED');vm.command('mount /disk','disk: MOUNTED')
            status(vm,'RUNNING',0)
            assert disk.read_bytes()==before,'Health/recovery changed persistent bytes'
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('ECHO HEALTH QEMU PASS: 2 boots; actual preemptible hang, authenticated error/wrong-opcode replies, 3 recoveries then blocked, explicit retry, apps and cold concurrent reads preserved',flush=True)


if __name__=='__main__':run()
