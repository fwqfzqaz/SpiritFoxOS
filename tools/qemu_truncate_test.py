"""Durable truncate/zero extension, exact recovery and read-only RPC policy."""
import time
from qemu_persist_test import VM, ROOT, generation, snapshot


def notes(disk, expected, gen, protected):
    actual, _, files = snapshot(disk.read_bytes())
    assert actual == gen and files['/notes/user'] == expected
    assert {p: data for p, data in files.items() if p != '/notes/user'} == protected


def run():
    pristine = (ROOT/'build/persistent.img').read_bytes()
    for iommu in (False, True):
        prefix = 'truncate-'+('modern-' if iommu else 'legacy-')
        disk = ROOT/'build'/(prefix+'test.img')
        disk.write_bytes(pristine)
        def machine(name, **kwargs):
            return VM(disk, prefix+name, iommu=iommu, **kwargs)
        vm = machine('write')
        try:
            vm.boot('PERSIST READY generation=1')
            vm.command('truncate /persist/notes 0', 'truncate: OK')
            for value,code in [('16385',18),('4294967296',18),('18446744073709551615',18),('18446744073709551616',1),('-1',1),('+1',1),('1x',1),('1 2',1)]:
                vm.command('truncate /persist/notes '+value, 'truncate: failed code='+str(code))
            for path,code in [('/disk/etc/readme.txt',17),('/missing',16),('/run/pipe',1),('/dev/console',1),('/',1)]:
                vm.command('truncate '+path+' 0', 'truncate: failed code='+str(code))
            vm.command('truncate /tmp/notes 4', 'truncate: OK')
            vm.command('cat /tmp/notes', 'Spir')
            vm.command('truncate /tmp/notes 1025', 'truncate: failed code=18')
            assert disk.read_bytes() == pristine, 'Rejected/no-op truncate wrote media'
            vm.command('write /persist/notes abcdefghij', 'write: OK')
            vm.command('install /disk/bin/diskhello.sfk', 'install: COMMITTED')
            vm.command('run installed', 'sfexec: exit 0')
            assert generation(disk) == 5
            protected = {p: data for p, data in snapshot(disk.read_bytes())[2].items() if p != '/notes/user'}
            vm.command('truncate /persist/notes 4', 'truncate: OK')
            notes(disk, b'abcd', 6, protected)
            vm.command('truncate /persist/notes 12', 'truncate: OK')
            notes(disk, b'abcd'+bytes(8), 7, protected)
            unchanged = disk.read_bytes()
            vm.command('truncate /persist/notes 12', 'truncate: OK')
            assert disk.read_bytes() == unchanged
            vm.command('truncate /persist/notes 16384', 'truncate: OK')
            notes(disk, b'abcd'+bytes(16380), 8, protected)
            vm.command('truncate /persist/notes 0', 'truncate: OK')
            notes(disk, b'', 9, protected)
            vm.command('write /persist/notes persistent-tail', 'write: OK')
            vm.command('truncate /persist/notes 4', 'truncate: OK')
            vm.command('truncate /persist/notes 10', 'truncate: OK')
            notes(disk, b'pers'+bytes(6), 12, protected)
            committed = disk.read_bytes()
            start = vm.send('runat /persist fileprobe.sfk')
            vm.expect('OPENAT APP PASSED', start)
            vm.expect('sfexec: exit 0', start)
            assert disk.read_bytes() == committed, 'Read-only application truncated file'
        finally:
            vm.close()
        vm = machine('reboot')
        try:
            vm.boot('PERSIST READY generation=12')
            vm.command('cat /persist/notes', 'pers')
            vm.command('truncate /persist/notes 10', 'truncate: OK')
            vm.command('run installed', 'sfexec: exit 0')
            vm.command('persist', 'update: confirmed-version=0.1.0 policy=monotonic')
        finally:
            vm.close()
        assert disk.read_bytes() == committed
        conf = ROOT/'build'/(prefix+'error.conf')
        for event in ('write','flush','interrupt'):
            disk.write_bytes(committed)
            if event == 'flush':
                config = '[inject-error]\nevent = "flush_to_disk"\niotype = "flush"\nerrno = "5"\nonce = "on"\n'
            else:
                sector = 2180 if event == 'interrupt' else 2049
                config = f'[inject-error]\nevent = "write_aio"\nsector = "{sector}"\nerrno = "5"\nonce = "on"\n'
            conf.write_text(config)
            vm = machine(event+'-error', debug=conf.name, stop_on_error=event=='interrupt')
            try:
                vm.boot()
                if event == 'interrupt':
                    vm.send('truncate /persist/notes 2')
                    end = time.monotonic()+15
                    while vm.query('query-status')['status'] != 'io-error':
                        if time.monotonic() > end:
                            raise RuntimeError('Truncate VM did not stop inside bank write')
                        time.sleep(.05)
                else:
                    vm.command('truncate /persist/notes 2', 'truncate: failed code=20')
                    vm.command('persist', 'persist: UNAVAILABLE')
                    vm.command('cat /persist/notes', 'cat: error')
                    vm.command('truncate /persist/notes 10', 'truncate: failed code=10')
                    vm.command('unmount /disk', 'disk: UNMOUNTED')
                assert b'truncate: OK' not in vm.output
            finally:
                vm.close(event!='interrupt')
            vm = machine(event+'-recovery')
            try:
                vm.boot('PERSIST READY generation=12')
                vm.command('cat /persist/notes', 'pers')
                vm.command('run installed', 'sfexec: exit 0')
            finally:
                vm.close()
            notes(disk, b'pers'+bytes(6), 12, protected)
        assert disk.read_bytes()[:2048*512] == pristine[:2048*512]
    assert (ROOT/'build/persistent.img').read_bytes() == pristine
    print('TRUNCATE QEMU PASS: shrink/zero extension/clear/reboot, no-op and invalid input unchanged, read-only RPC refusal, unrelated package/state preserved, actual OUT/FLUSH EIO and interrupted bank recovery', flush=True)


if __name__ == '__main__':
    run()
