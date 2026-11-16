"""Actual signed block driver faults both clients; explicit recovery is bounded."""
import hashlib
import json
from qemu_persist_test import VM,ROOT
from block_reply_fixture import driver,STATE

def run():
    base=(ROOT/'build/persistent.img').read_bytes()
    artifacts={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in ['build/bundle.bin','build/payload.S','build/esp/EFI/BOOT/BOOTX64.EFI']}
    rejected=recovered=0
    with driver():
        for modern in (False,True):
            for owner in ('Root','Persist'):
                label=('modern' if modern else 'legacy')+'-'+owner.lower()
                disk=ROOT/'build'/('block-reply-'+label+'.img');disk.write_bytes(base)
                vm=VM(disk,'block-reply-'+label,iommu=modern)
                try:
                    vm.boot('PERSIST READY')
                    note='BFR-ARM-'+owner.upper()
                    vm.command('write /persist/notes '+note,'write: OK')
                    committed=disk.read_bytes()
                    if owner=='Root':
                        start=vm.send('cat /disk/etc/readme.txt')
                        vm.expect('[BLOCK-FAULT] Root READ wrong opcode',start)
                        vm.expect('cat: error',start);vm.expect('fox> ',start)
                        # Metadata is deliberately a cached read-only query,
                        # not a proof that the dead driver is still healthy.
                        vm.command('persist','persist: notes-bytes='+str(len(note)))
                        vm.command('run hello.sfk','sfexec: exit 0')
                    else:
                        vm.command('unmount /disk','disk: UNMOUNTED')
                        start=vm.send('mount /disk')
                        vm.expect('[BLOCK-FAULT] Persist READ wrong opcode',start)
                        vm.expect('disk: mount refused',start);vm.expect('fox> ',start)
                        vm.command('run hello.sfk','sfexec: failed code=10')
                        vm.command('cat /disk/etc/readme.txt','SpiritFoxOS read-only disk')
                    rejected+=1
                    vm.command('echo','echo: ping')
                    assert disk.read_bytes()==committed,'Rejected READ changed committed volume'
                    vm.command('unmount /disk','disk: UNMOUNTED')
                    if owner=='Root':
                        vm.command('stop disk','disk: STOPPED')
                        vm.command('start disk','disk: ACTIVE')
                    vm.command('mount /disk','disk: MOUNTED')
                    vm.command('cat /persist/notes',note)
                    vm.command('run hello.sfk','sfexec: exit 0')
                    vm.command('run /disk/bin/diskhello.sfk','sfexec: exit 0')
                    assert disk.read_bytes()==committed,'Read-only recovery replayed a mutation'
                    recovered+=1
                finally:vm.close()
    assert not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==h for n,h in artifacts.items())
    assert (ROOT/'build/persistent.img').read_bytes()==base
    result={'passed':True,'guest_boots':4,'malformed_reads_rejected':rejected,'explicit_recoveries':recovered,'root_degraded_to_bundle':True,'persist_required_trust_fail_closed':True,'committed_notes_unchanged':True,'production_artifacts_restored':True,'base_disk_unchanged':True}
    (ROOT/'build/block-reply-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('BLOCK REPLY QEMU PASS:',json.dumps(result),flush=True)

if __name__=='__main__':run()
