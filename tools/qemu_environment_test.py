"""Explicit environment transport, preflight, isolation and no ambient state."""
from qemu_persist_test import VM, ROOT


def run():
    source = (ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label = 'modern' if iommu else 'legacy'
        disk = ROOT/'build'/('environment-'+label+'.img')
        disk.write_bytes(source)
        vm = VM(disk,'environment-'+label,iommu=iommu)
        try:
            vm.boot()
            vm.expect('ARGUMENT PLAN TESTS PASSED')
            vm.command('help','runenvat <dir>')
            vm.command('run argprobe.sfk','ENVC=0')
            start = vm.send('runenv MODE=dev "SPACE=two words" EMPTY= LITERAL=$keep -- argprobe.sfk alpha')
            for marker in ['ARGC=2','ENVC=4','ARGV[1]=alpha','ENV[0]=MODE=dev','ENV[1]=SPACE=two words','ENV[2]=EMPTY=\r\n','ENV[3]=LITERAL=$keep','sfexec: exit 0']:
                vm.expect(marker,start)
            start = vm.send('runenvat /disk/etc MODE=read -- argprobe.sfk granted')
            for marker in ['ENVC=1','ARGV[1]=granted','ENV[0]=MODE=read','sfexec: exit 0']:
                vm.expect(marker,start)
            start = vm.send('runenv A=1 B=2 C=3 D=4 E=5 F=6 G=7 H=8 -- argprobe.sfk a b c d e f g')
            for marker in ['ARGC=8','ENVC=8','ARGV[7]=g','ENV[7]=H=8','sfexec: exit 0']:
                vm.expect(marker,start)
            for command,code in [('runenv A=1 A=2 -- argprobe.sfk',1),('runenv 9BAD=x -- argprobe.sfk',1),('runenv BAD-NAME=x -- argprobe.sfk',1),('runenv =x -- argprobe.sfk',1),('runenv NO_EQUAL -- argprobe.sfk',1),('runenv A=1 B=2 C=3 D=4 E=5 F=6 G=7 H=8 I=9 -- argprobe.sfk',18),('runenv A=1 argprobe.sfk',1),('runenv A=1 --',1),('runenv A=1 -- argprobe.sfk a b c d e f g h',18),('runenv A=1 -- echo.sfmod',2),('runenvat /missing A=1 -- argprobe.sfk',16)]:
                vm.command(command,'sfexec: failed code='+str(code))
            vm.command('run argprobe.sfk --require-mode','sfexec: failed code=42')
            vm.command('runenv MODE= -- argprobe.sfk --require-mode','sfexec: exit 0')
            vm.command('runenv MODE=x -- argprobe.sfk --write-environment','sfexec: failed code=114')
            vm.command('echo','echo: ping')
            vm.command('run argprobe.sfk','ENVC=0')
            vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
            pending = disk.read_bytes()
            vm.command('runenv A=1 A=2 -- installed','sfexec: failed code=1')
            vm.command('runenv 9BAD=x -- installed','sfexec: failed code=1')
            assert disk.read_bytes()==pending, 'Environment preflight changed persistent trial state'
            start = vm.send('runenv NOTE=ENVIRONMENT-INVOKE-ONLY -- installed')
            vm.expect('update: CONFIRMED',start)
            vm.expect('sfexec: exit 0',start)
            confirmed = disk.read_bytes()
            assert b'ENVIRONMENT-INVOKE-ONLY' not in confirmed
            vm.command('run installed','sfexec: exit 0')
            assert disk.read_bytes()==confirmed
            vm.command('unmount /disk','disk: UNMOUNTED')
            vm.command('stop disk','disk: STOPPED')
        finally:
            vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('ENVIRONMENT QEMU PASS: 2 boots; explicit values only, absent/empty distinction, argv+env max counts, identifier/duplicate preflight, no trial writes, read-only environment fault, runenvat, reap and no persistence',flush=True)


if __name__ == '__main__':
    run()
