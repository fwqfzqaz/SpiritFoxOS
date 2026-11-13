"""Real revoke commit plus negative completion: reconcile or retain DIRTY."""
import hashlib
import json
from qemu_persist_test import VM,ROOT,generation,replace_base,unknown_completion
from trust_sync_fixture import provider,STATE
from sfpack import seal

def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    backup=seal((ROOT/'build/diskhello.sfk').read_bytes(),(ROOT/'.keys/backup.seed').read_bytes())
    products={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in ('build/bundle.bin','build/payload.S','build/esp/EFI/BOOT/BOOTX64.EFI')}
    reconciled=unknown=semantic=reboots=0
    for error in (-10,-20):
        with provider('tests/fixtures/persist_revoke_failure.c',revoke_error=error):
            for modern in (False,True):
                label=('modern' if modern else 'legacy')+'-'+str(-error)
                disk=ROOT/'build'/('revoke-failure-'+label+'.img');disk.write_bytes(replace_base(source,{'/bin/disknext.sfk':backup}))
                vm=VM(disk,'revoke-failure-'+label,iommu=modern)
                try:
                    vm.boot('PERSIST READY');original=disk.read_bytes()
                    vm.command('revoke-key /disk/etc/readme.txt','revoke: failed code=3')
                    vm.command('run hello.sfk','sfexec: exit 0');semantic+=1
                    assert disk.read_bytes()==original,'Harmless invalid package changed policy/disk'
                    old=generation(disk)
                    start=vm.send('revoke-key /disk/bin/diskexit.sfk')
                    vm.expect('[PERSIST-FAULT] revoke committed, reports failure code='+str(-error),start)
                    vm.expect('revoke: failed code='+str(-error),start)
                    committed=disk.read_bytes();assert generation(disk)==old+1
                    if error==-10:
                        # No explicit trust query is allowed before this check.
                        vm.command('run hello.sfk','sfexec: failed code=13')
                        vm.command('run /disk/bin/disknext.sfk','sfexec: exit 0')
                        vm.command('trust','trust: anchors=3 revoked=1');reconciled+=1
                    else:
                        vm.command('run hello.sfk','sfexec: failed code=20')
                        vm.command('install /disk/bin/disknext.sfk','install: failed code=20')
                        vm.command('trust','trust: sync unavailable code=20')
                        unknown_completion(vm);unknown+=1
                    vm.command('echo','echo: ping')
                    assert disk.read_bytes()==committed,'Failed policy result was replayed or changed committed disk'
                finally:vm.close()
                if error==-20:
                    vm=VM(disk,'revoke-failure-reboot-'+label,iommu=modern)
                    try:
                        vm.boot('PERSIST READY')
                        vm.command('trust','trust: anchors=3 revoked=1')
                        vm.command('run hello.sfk','sfexec: failed code=13')
                        vm.command('run /disk/bin/disknext.sfk','sfexec: exit 0')
                        vm.command('unmount /disk','disk: UNMOUNTED');reboots+=1
                        assert disk.read_bytes()==committed,'Fresh-boot recovery rewrote stable policy'
                    finally:vm.close()
    assert not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==h for n,h in products.items())
    assert (ROOT/'build/persistent.img').read_bytes()==source
    result={'passed':True,'guest_boots':6,'negative_policy_reconciled':reconciled,'explicit_unknown_latched':unknown,'harmless_semantic_rejections':semantic,'unknown_dirty_unmount_refusals':unknown,'fresh_boot_recovery':reboots,'remaining_signer_available':True,'committed_disk_preserved':True,'production_artifacts_restored':True,'base_disk_unchanged':True}
    (ROOT/'build/revoke-failure-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('REVOKE FAILURE QEMU PASS:',json.dumps(result),flush=True)

if __name__=='__main__':run()
