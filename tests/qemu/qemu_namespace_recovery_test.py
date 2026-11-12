"""Actual Fs namespace process replacement, stale app references and TRYING."""
from qemu_persist_test import VM,ROOT,snapshot,replace_base
from sfpack import seal


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    standby=seal((ROOT/'build/disknext.sfk').read_bytes(),(ROOT/'.keys/backup.seed').read_bytes())
    fixture=replace_base(source,{'/bin/disknext.sfk':standby})
    for modern in (False,True):
        label='modern' if modern else 'legacy';disk=ROOT/'build'/('namespace-recovery-'+label+'.img');disk.write_bytes(fixture)
        vm=VM(disk,'namespace-recovery-'+label,iommu=modern)
        try:
            vm.boot();vm.command('namespace-status','namespace: ACTIVE pid=')
            vm.command('write /persist/notes durable-note','write: OK')
            vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED');vm.command('run installed','sfexec: exit 0')
            vm.command('revoke-key /disk/bin/disknext.sfk','revoke: OK')
            vm.command('login alice alice','login: alice uid=1000');vm.command('login root fox-root','login: root uid=0')
            vm.command('sessions','sessions: 3 active=3')
            for _ in range(2):
                vm.command('install /disk/bin/diskupdate.sfk','install: COMMITTED')
                gen,_,before=snapshot(disk.read_bytes());assert before['/boot/current'][8]==1
                start=vm.send('namespace-recovery-test')
                vm.expect('NAMESPACE APP STALE PASSED',start,timeout=90);vm.expect('NAMESPACE RECOVERY TEST PASSED',start)
                assert b'RECOVERED interrupted trial' not in vm.output[start:]
                newgen,_,after=snapshot(disk.read_bytes());assert newgen==gen+2,'Only explicit trial begin/end may commit'
                assert after['/notes/user']==before['/notes/user'] and after['/boot/revoked']==before['/boot/revoked']
                assert after['/slots/a.sfk']==before['/slots/a.sfk'] and after['/slots/b.sfk']==before['/slots/b.sfk']
                assert after['/boot/current'][8]==0
                vm.command('namespace-status','namespace: ACTIVE pid=');vm.command('persist','update: STABLE good=a candidate=-')
                vm.command('cat /persist/notes','durable-note');vm.command('trust','trust: anchors=3 revoked=1')
                vm.command('sessions','sessions: 3 active=3');vm.command('whoami','whoami: name=root uid=0')
            # Account/session policy remains the author's existing Root state.
            vm.command('logout','logout: session 3 closed');vm.command('whoami','whoami: name=alice uid=1000')
            vm.command('cat /home/bob/private','cat: error');vm.command('write /home/alice/private retained-identity','write: OK')
            vm.command('namespace-recover','denied: root session required');vm.command('login root fox-root','login: root uid=0')
            # A separate real death reaches the ordinary shell, outside the
            # in-command diagnostic, with bundle execution available.
            stable=disk.read_bytes();vm.command('namespace-crash','namespace: crash injected')
            vm.command('namespace-status','namespace: UNAVAILABLE pid=')
            vm.command('selftest','namespace: unavailable; use namespace-recover')
            vm.command('whoami','whoami: name=root uid=0');vm.command('cat /tmp/notes','cat: error')
            vm.command('run hello.sfk','sfexec: exit 0');vm.command('namespace-recover','namespace: RECOVERED')
            assert disk.read_bytes()==stable,'Namespace-only shell recovery wrote stable volume'
            vm.command('unmount /disk','disk: UNMOUNTED');vm.command('stop disk','disk: STOPPED')
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('NAMESPACE RECOVERY QEMU PASS: 2 boots/6 actual Fs deaths; disk-independent RAM/pipe, dirty refusal, confirmed-dead leases, stale Root/app rejection, TRYING bytes/package/revocations and accounts/sessions preserved, unavailable shell and bundle fallback',flush=True)


if __name__=='__main__':run()
