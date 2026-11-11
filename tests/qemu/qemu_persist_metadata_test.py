"""Read-only mounted-volume metadata through real Root/persist kernel IPC."""
import hashlib
from qemu_persist_test import VM,ROOT,snapshot


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for modern in (False,True):
        label='modern' if modern else 'legacy'
        disk=ROOT/'build'/('persist-metadata-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'metadata-'+label,iommu=modern)
        try:
            vm.boot()
            vm.command('write /persist/notes metadata-note','write: OK')
            vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
            before=disk.read_bytes();gen,slot,files=snapshot(before)
            assert files['/boot/current'][8]==1,'Fixture must be a real PENDING signed update'
            for _ in range(4):
                start=vm.send('persist');vm.expect('update: PENDING good=- candidate=a',start)
                vm.expect('persist: notes-bytes=13',start)
            assert disk.read_bytes()==before,'Metadata changed PENDING, bank generation or bytes'
            assert snapshot(disk.read_bytes())==(gen,slot,files)
            vm.command('run installed','sfexec: exit 0')
            vm.command('truncate /persist/notes 257','truncate: OK')
            before=disk.read_bytes()
            start=vm.send('persist');vm.expect('update: STABLE good=a candidate=-',start);vm.expect('persist: notes-bytes=257',start)
            assert disk.read_bytes()==before,'Stable metadata wrote disk'
            vm.command('revoke-key /disk/bin/diskexit.sfk','revoke: OK')
            before=disk.read_bytes();revoked=snapshot(before)[2]['/boot/revoked']
            assert len(revoked)==32
            vm.command('persist','persist: notes-bytes=257')
            assert disk.read_bytes()==before and snapshot(disk.read_bytes())[2]['/boot/revoked']==revoked
            vm.command('unmount /disk','disk: UNMOUNTED')
            vm.command('persist','persist: notes-unavailable code=10')
            vm.command('stop disk','disk: STOPPED')
            vm.command('persist','persist: notes-unavailable code=10')
            assert disk.read_bytes()==before,'Offline query changed storage'
            vm.command('start disk','disk: ACTIVE');vm.command('mount /disk','disk: MOUNTED')
            vm.command('persist','persist: notes-bytes=257')
            vm.command('trust','trust: anchors=3 revoked=1')
            assert disk.read_bytes()==before,'Stable reattach changed transaction bytes'
        finally:vm.close()
    assert hashlib.sha256((ROOT/'build/persistent.img').read_bytes()).digest()==hashlib.sha256(source).digest()
    print('PERSIST METADATA QEMU PASS: 2 boots; actual authenticated IPC, PENDING/STABLE/length/revocation/offline/reattach, byte-identical read-only queries; TRYING preservation separately host-verified',flush=True)


if __name__=='__main__':run()
