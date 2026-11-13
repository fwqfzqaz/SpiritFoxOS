"""A real signed provider emits bad trust replies; Root stays fail closed."""
import hashlib
import json
from qemu_persist_test import VM,ROOT,replace_base,generation
from sfpack import seal
from trust_sync_fixture import provider,STATE


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    standby=seal((ROOT/'build/diskhello.sfk').read_bytes(),(ROOT/'.keys/backup.seed').read_bytes())
    fingerprint=hashlib.sha256((ROOT/'build/application.pub').read_bytes()).digest()[:8].hex()
    before_boot=hashlib.sha256((ROOT/'build/esp/EFI/BOOT/BOOTX64.EFI').read_bytes()).hexdigest()
    refused=resynced=0
    with provider():
        for modern in (False,True):
            label='modern' if modern else 'legacy';disk=ROOT/'build'/('trust-sync-'+label+'.img')
            disk.write_bytes(replace_base(source,{'/bin/disknext.sfk':standby}))
            vm=VM(disk,'trust-sync-'+label,iommu=modern)
            try:
                vm.boot('PERSIST READY')
                vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
                vm.command('write /persist/notes SYNC-PRESERVED','write: OK')
                vm.command('truncate /persist/notes 12345','truncate: OK')
                pending=disk.read_bytes()
                start=vm.send('trust');vm.expect('trust: sync unavailable code=3',start);vm.expect('trust: anchors=3 revoked=0',start)
                vm.command('run hello.sfk','sfexec: failed code=3');refused+=1
                vm.command('run installed','sfexec: failed code=3');refused+=1
                assert disk.read_bytes()==pending,'Unknown runtime trust wrote TRYING or changed pending data'
                vm.command('echo','echo: ping')
                vm.command('trust','trust: anchors=3 revoked=0');resynced+=1
                vm.command('run hello.sfk','sfexec: exit 0')
                vm.command('run installed','sfexec: exit 0')
                vm.command('persist','update: STABLE good=a candidate=-')
                # The provider commits revocation but drops the first complete
                # list. It is a known write, not an uncertain DIRTY bracket.
                old=generation(disk)
                vm.command('revoke-key /disk/bin/diskexit.sfk','revoke: failed code=3')
                committed=disk.read_bytes();assert generation(disk)==old+1
                vm.command('run hello.sfk','sfexec: failed code=3');refused+=1
                vm.command('run /disk/bin/disknext.sfk','sfexec: failed code=3');refused+=1
                assert disk.read_bytes()==committed,'Faulted trust cache replayed a mutation'
                vm.command('trust','trust: anchors=3 revoked=1');resynced+=1
                vm.command('trust','trust: revoked key='+fingerprint)
                vm.command('run hello.sfk','sfexec: failed code=13')
                vm.command('run /disk/bin/disknext.sfk','sfexec: exit 0')
                vm.command('cat /persist/notes','SYNC-PRESERVED')
                assert disk.read_bytes()==committed
            finally:vm.close()
    assert not json.loads(STATE.read_text())['active']
    assert hashlib.sha256((ROOT/'build/esp/EFI/BOOT/BOOTX64.EFI').read_bytes()).hexdigest()==before_boot
    assert (ROOT/'build/persistent.img').read_bytes()==source and refused==8 and resynced==4
    result={'passed':True,'guest_boots':2,'malformed_snapshots':4,'denied_new_authority':refused,'valid_resyncs':resynced,'pending_and_known_commit_preserved':True,'ordinary_standby_app':True,'production_artifacts_restored':True,'base_disk_unchanged':True}
    (ROOT/'build/trust-sync-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('TRUST SYNC QEMU PASS: '+json.dumps(result)+'; actual signed provider faults, Root admission/TRYING gate, explicit repair and durable revocation',flush=True)


if __name__=='__main__':run()
