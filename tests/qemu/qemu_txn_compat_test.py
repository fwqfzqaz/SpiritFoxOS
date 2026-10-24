"""Read genuine v1 geometry, then atomically migrate it on ordinary writes."""
import hashlib
import json
import struct
from qemu_persist_test import VM, ROOT, generation, snapshot, replace_base
from sfpack import seal


def legacy_banks(data):
    """Independent old-format fixture; keep both generations and all file bytes."""
    result = bytearray(data)
    for slot in (0, 1):
        record_start = (2049+slot)*512
        record = data[record_start:record_start+512]
        assert record[:8] == b'SFCMT1\0\0'
        assert hashlib.sha256(record[:480]).digest() == record[480:]
        bank_start = (2051+slot*256)*512
        current = data[bank_start:bank_start+256*512]
        assert hashlib.sha256(current).digest() == record[40:72]
        assert current[:8] == b'SFBANK1\0'
        assert struct.unpack_from('<Q', current, 8)[0] == struct.unpack_from('<Q', record, 8)[0]
        count = struct.unpack_from('<I', current, 16)[0]
        assert count <= 4
        old = bytearray(256*512)
        old[:512] = current[:512]
        for i in range(count):
            entry = current[512+i*128:512+(i+1)*128]
            size = struct.unpack_from('<I', entry, 64)[0]
            assert size <= 16384
            payload = current[1152+i*16384:1152+i*16384+size]
            assert hashlib.sha256(payload).digest() == entry[96:128]
            old[512+i*128:512+(i+1)*128] = entry
            old[1024+i*16384:1024+i*16384+size] = payload
        result[bank_start:bank_start+len(old)] = old
        result[record_start+40:record_start+72] = hashlib.sha256(old).digest()
        result[record_start+480:record_start+512] = hashlib.sha256(result[record_start:record_start+480]).digest()
    assert result[:2048*512] == data[:2048*512]
    return bytes(result)


def run():
    pristine = (ROOT/'build/persistent.img').read_bytes()
    # Replace an existing entry so the fixed namespace quota stays unchanged.
    standby = seal((ROOT/'build/diskother.sfk').read_bytes(), (ROOT/'.keys/backup.seed').read_bytes())
    source = replace_base(pristine, {'/bin/diskother.sfk': standby})
    for iommu in (False, True):
        label = 'modern' if iommu else 'legacy'
        disk = ROOT/'build'/('txn-v1-'+label+'.img')
        disk.write_bytes(source)
        vm = VM(disk, 'txn-v1-prepare-'+label, iommu=iommu)
        try:
            vm.boot()
            vm.command('write /persist/notes LEGACY-PRESERVED', 'write: OK')
            vm.command('install /disk/bin/diskhello.sfk', 'install: COMMITTED')
            vm.command('run installed', 'update: CONFIRMED')
            vm.command('install /disk/bin/diskupdate.sfk', 'install: COMMITTED')
            vm.command('run installed', 'update: CONFIRMED')
            vm.command('persist', 'update: confirmed-version=0.2.0 policy=monotonic')
        finally:
            vm.close()
        gen, _, files = snapshot(disk.read_bytes())
        assert set(files) == {'/notes/user', '/slots/a.sfk', '/slots/b.sfk', '/boot/current'}
        old = legacy_banks(disk.read_bytes())
        disk.write_bytes(old)
        vm = VM(disk, 'txn-v1-read-upgrade-'+label, iommu=iommu)
        try:
            vm.boot('PERSIST READY generation='+str(gen))
            vm.command('cat /persist/notes', 'LEGACY-PRESERVED')
            vm.command('run installed', 'sfexec: exit 0')
            vm.command('persist', 'update: confirmed-version=0.2.0 policy=monotonic')
            assert disk.read_bytes() == old, 'Mount or read-only access eagerly rewrote legacy volume'
            vm.command('write /persist/notes MIGRATED-PRESERVED', 'write: OK')
            migrated_gen, _, migrated = snapshot(disk.read_bytes())
            assert migrated_gen == gen+1 and migrated['/notes/user'] == b'MIGRATED-PRESERVED'
            for path in ('/slots/a.sfk', '/slots/b.sfk', '/boot/current'):
                assert migrated[path] == files[path], path
            # Importing four old keys must leave room for the new fifth key.
            vm.command('revoke-key /disk/bin/diskother.sfk', 'revoke: OK')
            vm.command('trust', 'trust: anchors=3 revoked=1')
            vm.command('run /disk/bin/diskother.sfk', 'sfexec: failed code=13')
            vm.command('run installed', 'sfexec: exit 0')
        finally:
            vm.close()
        committed = disk.read_bytes()
        final_gen, _, migrated = snapshot(committed)
        assert final_gen == gen+2 and len(migrated) == 5 and len(migrated['/boot/revoked']) == 32
        assert committed[:2048*512] == source[:2048*512]
        vm = VM(disk, 'txn-v1-reboot-'+label, iommu=iommu)
        try:
            vm.boot('PERSIST READY generation='+str(final_gen))
            vm.command('cat /persist/notes', 'MIGRATED-PRESERVED')
            vm.command('trust', 'trust: anchors=3 revoked=1')
            vm.command('run /disk/bin/diskother.sfk', 'sfexec: failed code=13')
            vm.command('run installed', 'sfexec: exit 0')
            vm.command('persist', 'update: confirmed-version=0.2.0 policy=monotonic')
        finally:
            vm.close()
        assert disk.read_bytes() == committed, 'Read-only restart changed migrated state'
    assert (ROOT/'build/persistent.img').read_bytes() == pristine
    (ROOT/'build/txn-compat-results.json').write_text(json.dumps({'passed': True, 'boots': 6, 'legacy_profiles': [4, 1024], 'current_profiles': [5, 1152], 'read_only_mount': True, 'fifth_key': True}, indent=2)+'\n')
    print('TXN COMPAT QEMU PASS: 6 boots; four-key legacy banks, exact read-only mount, notes/slots/version preserved, ordinary-write migration, fifth revocation key and restart durability', flush=True)


if __name__ == '__main__':
    run()
