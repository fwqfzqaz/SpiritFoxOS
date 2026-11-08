"""SMP bring-up, per-CPU contexts, thread lifecycle and thread/CPU quotas."""
from qemu_persist_test import VM,ROOT


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for smp in (1,2):
        label='smp-thread-'+str(smp)
        disk=ROOT/'build'/(label+'.img');disk.write_bytes(source)
        vm=VM(disk,label,smp=smp)
        try:
            vm.boot()
            if smp>1:
                vm.expect('SMP: cpus=2')
                vm.expect('SMP: AP 1 online')
            vm.command('threads 1','threads: pid=1 limit=8 used=1')
            vm.command('threads 1 4','threads: pid=1 limit=4 used=1')
            vm.command('threads 1 7','threads: pid=1 limit=7 used=1')
            vm.command('threads 1 8','threads: pid=1 limit=8 used=1')
            for command in ['threads','threads 0','threads 1 0','threads 1 9','threads 4294967297 3',
                            'threads 2147483648 3','threads -1 3','threads 1 -1',
                            'threads 1 2 extra','threads 1x','threads "unterminated']:
                vm.command(command,'threads: failed code=1')
            vm.command('threads 2147483647','threads: failed code=16')
            vm.command('cpu-budget 1','cpu-budget: pid=1 limit=0 used=')
            vm.command('cpu-budget 1 60','cpu-budget: pid=1 limit=60 used=')
            vm.command('cpu-budget 1 0','cpu-budget: pid=1 limit=0 used=')
            for command in ['cpu-budget','cpu-budget 0','cpu-budget 1 101','cpu-budget 4294967297 3',
                            'cpu-budget 2147483648 3','cpu-budget -1 3','cpu-budget 1 -1',
                            'cpu-budget 1 2 extra','cpu-budget 1x','cpu-budget "unterminated']:
                vm.command(command,'cpu-budget: failed code=1')
            vm.command('cpu-budget 2147483647','cpu-budget: failed code=16')
            for _ in range(2):vm.command('threads-test','THREAD TEST PASSED')
            vm.command('threads 1','threads: pid=1 limit=8 used=1')
            vm.command('run hello.sfk','sfexec: exit 0')
            vm.command('echo','echo: ping')
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
            assert disk.read_bytes()==source,'SMP/thread control changed persistent bytes'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('SMP THREAD QEMU PASS: 2 boots (1 and 2 CPUs); MADT/LAPIC AP online, shared-address-space thread probe, thread limit and CPU budget CLI, invalid/busy codes, apps and disk bytes unchanged',flush=True)


if __name__=='__main__':run()
