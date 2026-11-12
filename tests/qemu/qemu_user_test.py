"""Console sessions: authenticated login, per-account isolation, admin gate."""
from qemu_persist_test import VM, ROOT

def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for iommu in (False,True):
        label='modern' if iommu else 'legacy'
        disk=ROOT/'build'/('user-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'user-'+label,iommu=iommu)
        try:
            vm.boot()
            vm.expect('fox> ')
            # Boot attaches an authenticated uid-0 session, so every legacy
            # prompt and administrator command keeps working unchanged.
            vm.command('whoami','whoami: name=root uid=0 gid=0 session=1 home=/home/root')
            vm.command('login','login: expected user name')
            vm.command('users','user: bob uid=1001 gid=1001 home=/home/bob')
            start=vm.send('users')
            for marker in ['user: root uid=0 gid=0 home=/home/root','user: alice uid=1000 gid=1000 home=/home/alice']:
                vm.expect(marker,start)
            vm.command('sessions','session: id=1 user=root uid=0 active=1')
            vm.command('login mallory secret','login: authentication failed')
            vm.command('login alice wrong','login: authentication failed')
            vm.command('login abcdefghi x','login: invalid user name')
            # Hidden password input: the typed secret never echoes.
            start=vm.send('login alice');vm.expect('password: ',start)
            start=vm.send('topsecret')
            vm.expect('login: authentication failed',start)
            assert b'topsecret' not in vm.output[start:],'Console echoed a hidden password'
            vm.expect('fox> ',start)
            start=vm.send('login alice');vm.expect('password: ',start)
            start=vm.send('alice')
            vm.expect('login: alice uid=1000 gid=1000 session=2 home=/home/alice',start)
            vm.expect('alice@fox> ',start)
            vm.command('whoami','whoami: name=alice uid=1000 gid=1000 session=2 home=/home/alice')
            vm.command('write /home/alice/private hello','write: OK')
            vm.command('cat /home/alice/private','hello')
            vm.command('ls /home/alice','private')
            vm.command('stat /home/alice/private','stat: type=2 size=5 offset=0 mode=1 flags=4')
            # Another account's 0700 home is neither listable, readable,
            # writable, creatable nor removable, regardless of path depth.
            vm.command('ls /home/bob','ls: error')
            vm.command('cat /home/bob/private','cat: error')
            vm.command('write /home/bob/x nope','write: error')
            vm.command('mkdir /home/bob/x','mkdir: failed code=2')
            vm.command('rm /home/bob/x','rm: failed code=2')
            vm.command('stat /home/bob','stat: failed code=2')
            # Administrator commands are refused before their handler runs.
            vm.command('persist','denied: root session required')
            vm.command('install /disk/bin/diskhello.sfk','denied: root session required')
            vm.command('shutdown','denied: root session required')
            for command in ['namespace-crash','namespace-recover','stop native','gui','cap-limit 1 1']:
                vm.command(command,'denied: root session required')
            # Launch admission uses a captured identity, never root merely
            # because the manager performs the syscall on its behalf.
            vm.command('run installed','sfexec: failed code=2')
            vm.command('run echo.sfmod','sfexec: failed code=2')
            vm.command('runat /home/bob hello.sfk','sfexec: failed code=2')
            vm.command('run hello.sfk','sfexec: exit 0')
            start=vm.send('login bob bob')
            vm.expect('login: bob uid=1001 gid=1001 session=3 home=/home/bob',start)
            vm.expect('bob@fox> ',start)
            vm.command('cat /home/alice/private','cat: error')
            vm.command('write /home/bob/private mine','write: OK')
            vm.command('cat /home/bob/private','mine')
            start=vm.send('sessions')
            for marker in ['sessions: 3 active=3','session: id=1 user=root uid=0 active=0','session: id=2 user=alice uid=1000 active=0','session: id=3 user=bob uid=1001 active=1']:
                vm.expect(marker,start)
            # Logout closes only the attached session and reattaches the
            # highest remaining slot; files stay private to their owner.
            start=vm.send('logout');vm.expect('logout: session 3 closed',start);vm.expect('alice@fox> ',start)
            vm.command('whoami','whoami: name=alice uid=1000 gid=1000 session=2 home=/home/alice')
            start=vm.send('logout');vm.expect('logout: session 2 closed',start);vm.expect('fox> ',start)
            vm.command('whoami','whoami: name=root uid=0 gid=0 session=1 home=/home/root')
            vm.command('cat /home/alice/private','hello')
            vm.command('cat /home/bob/private','mine')
            # The table is bounded: a fifth valid identity finds no slot.
            vm.command('login alice alice','login: alice uid=1000 gid=1000 session=2 home=/home/alice')
            vm.command('login bob bob','login: bob uid=1001 gid=1001 session=3 home=/home/bob')
            vm.command('login alice alice','login: alice uid=1000 gid=1000 session=4 home=/home/alice')
            vm.command('login root fox-root','login: no free session slot')
            start=vm.send('logout');vm.expect('logout: session 4 closed',start);vm.expect('bob@fox> ',start)
            start=vm.send('logout');vm.expect('logout: session 3 closed',start);vm.expect('alice@fox> ',start)
            start=vm.send('logout');vm.expect('logout: session 2 closed',start);vm.expect('fox> ',start)
            # A console without any session only accepts session commands.
            start=vm.send('logout');vm.expect('logout: session 1 closed',start);vm.expect('login> ',start)
            vm.command('ls','login required; type login <user>')
            vm.command('write /tmp/x nope','login required; type login <user>')
            vm.command('whoami','whoami: no active session')
            vm.command('help','login <user> [password]')
            start=vm.send('login root fox-root')
            vm.expect('login: root uid=0 gid=0 session=1 home=/home/root',start)
            vm.expect('fox> ',start)
            vm.command('whoami','whoami: name=root uid=0 gid=0 session=1 home=/home/root')
            assert disk.read_bytes()==source,'Sessions or private home files wrote persistent media'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    print('USER SESSION QEMU PASS: 2 boots; authenticated/hidden login, bounded slots, per-account home isolation, logout reattachment, admin gate, sessionless console and persistent bytes unchanged',flush=True)

if __name__=='__main__':run()
