"""Native argv transport, immutable BootArgs and preflight before update trial."""
from qemu_persist_test import VM, ROOT


def run():
    source = (ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label = 'modern' if iommu else 'legacy'
        disk = ROOT/'build'/('arguments-'+label+'.img')
        disk.write_bytes(source)
        vm = VM(disk,'arguments-'+label,iommu=iommu)
        try:
            vm.boot()
            vm.expect('ARGUMENT PLAN TESTS PASSED')
            vm.command('run argprobe.sfk','ARGC=0')
            start = vm.send('run argprobe.sfk alpha "two words" \'\' a\\ b')
            for marker in ['ARGC=5','ARGV[0]=argprobe.sfk','ARGV[1]=alpha','ARGV[2]=two words','ARGV[3]=\r\n','ARGV[4]=a b','sfexec: exit 0']:
                vm.expect(marker,start)
            start = vm.send('runat /disk/etc argprobe.sfk "directory args"')
            for marker in ['ARGC=2','ARGV[0]=argprobe.sfk','ARGV[1]=directory args','sfexec: exit 0']:
                vm.expect(marker,start)
            start = vm.send('run argprobe.sfk a b c d e f g')
            vm.expect('ARGC=8',start)
            vm.expect('ARGV[7]=g',start)
            vm.expect('sfexec: exit 0',start)
            for command,code in [('run argprobe.sfk a b c d e f g h',18),('run argprobe.sfk "unclosed',1),('run argprobe.sfk end\\',1),('run echo.sfmod x',2),('run missing.sfk x',16),('runat /missing argprobe.sfk x',16)]:
                vm.command(command,'sfexec: failed code='+str(code))
            vm.command('run argprobe.sfk --write-bootargs','sfexec: failed code=114')
            vm.command('echo','echo: ping')
            for _ in range(10):
                vm.command('run argprobe.sfk reused','sfexec: exit 0')
            vm.command('run argprobe.sfk','ARGC=0')
            vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
            pending = disk.read_bytes()
            vm.command('run installed "unclosed','sfexec: failed code=1')
            vm.command('run installed a b c d e f g h','sfexec: failed code=18')
            assert disk.read_bytes()==pending, 'Invalid arguments started a persistent trial'
            start = vm.send('run installed ARGUMENT-INVOKE-ONLY')
            vm.expect('update: CONFIRMED',start)
            vm.expect('sfexec: exit 0',start)
            confirmed = disk.read_bytes()
            assert b'ARGUMENT-INVOKE-ONLY' not in confirmed, 'Invocation arguments persisted in update state'
            vm.command('run installed','sfexec: exit 0')
            vm.command('runat /disk/etc installed x','sfexec: failed code=2')
            assert disk.read_bytes()==confirmed
            vm.command('unmount /disk','disk: UNMOUNTED')
            vm.command('stop disk','disk: STOPPED')
        finally:
            vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('ARGUMENTS QEMU PASS: 2 boots; canonical argv, quoting/empty/escapes, max count, malformed preflight without writes, read-only payload fault, reap/reuse, runat and installed health confirmation',flush=True)


if __name__ == '__main__':
    run()
