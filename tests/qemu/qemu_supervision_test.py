"""Real echo deaths, bounded recovery, explicit stop/retry and stable budget reset."""
import time
from qemu_persist_test import VM, ROOT


def status(vm, state, attempts, stable=None):
    marker=f'echo: state={state} attempts={attempts} stable='
    start=vm.send('echo-status');vm.expect(marker,start)
    line_start=vm.output.index(marker.encode(),start);vm.expect('fox> ',line_start)
    line=bytes(vm.output[line_start:]).splitlines()[0].strip()
    if stable is not None:assert line.endswith(str(stable).encode()),line
    return line.endswith(b'1')


def crash(vm, result):
    start=vm.send('echo-crash');vm.expect('echo: crash injected',start);vm.expect('echo: '+result,start)


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy';disk=ROOT/'build'/('supervision-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'supervision-'+label,iommu=iommu)
        try:
            vm.boot();status(vm,'RUNNING',0,0)
            vm.command('write /persist/notes SUPERVISION-PRESERVED','write: OK');before=disk.read_bytes()
            for attempt in range(1,4):
                crash(vm,'recovered');status(vm,'RUNNING',attempt,0);vm.command('echo','echo: ping')
            crash(vm,'recovery blocked');status(vm,'BLOCKED',3,0)
            vm.command('echo','echo: service unavailable');vm.command('run hello.sfk','sfexec: exit 0')
            vm.command('run /disk/bin/pieprobe.sfk','sfexec: exit 0');vm.command('cat /persist/notes','SUPERVISION-PRESERVED')
            vm.command('start echo','echo: ACTIVE');status(vm,'RUNNING',0,0)
            vm.command('stop echo','echo: STOPPED');time.sleep(.75);status(vm,'STOPPED',0,0)
            vm.command('echo-crash','echo: crash refused');vm.command('echo','echo: service unavailable')
            vm.command('start echo','echo: ACTIVE');crash(vm,'recovered');status(vm,'RUNNING',1,0)
            deadline=time.monotonic()+40
            while not status(vm,'RUNNING',1):
                assert time.monotonic()<deadline,'Echo never reached stable interval'
                time.sleep(.25)
            crash(vm,'recovered');status(vm,'RUNNING',1,0)
            for _ in range(12):
                vm.command('stop echo','echo: STOPPED');vm.command('start echo','echo: ACTIVE');vm.command('echo','echo: ping')
            assert disk.read_bytes()==before,'Echo death/restart changed persistent bytes'
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('SUPERVISION QEMU PASS: 2 boots; actual echo deaths, 3 automatic restarts then blocked, shell/disk/apps intact, explicit stop/retry, stable interval resets budget and endpoints/tasks reclaimed',flush=True)


if __name__=='__main__':run()
