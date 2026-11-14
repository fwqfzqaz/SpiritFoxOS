"""Advisory Persist lifecycle preflight through real authenticated guest IPC."""
import json
from qemu_persist_test import VM,ROOT,snapshot


def run():
    source=(ROOT/'build/persistent.img').read_bytes()
    for modern in (False,True):
        label='modern' if modern else 'legacy'
        disk=ROOT/'build'/('persist-quiescence-'+label+'.img');disk.write_bytes(source)
        vm=VM(disk,'quiescence-'+label,iommu=modern)
        try:
            vm.boot();before=disk.read_bytes()
            vm.command('persist-quiescent','persist: QUIESCENT preflight only')
            assert disk.read_bytes()==before,'Empty stable preflight wrote disk'
            vm.command('write /persist/notes quiescence-note','write: OK')
            vm.command('install /disk/bin/diskhello.sfk','install: COMMITTED')
            before=disk.read_bytes();assert snapshot(before)[2]['/boot/current'][8]==1
            vm.command('persist-quiescent','persist: QUIESCENT preflight only')
            assert disk.read_bytes()==before,'PENDING observation changed bytes'
            vm.command('run installed','sfexec: exit 0')
            vm.command('install /disk/bin/diskupdate.sfk','install: COMMITTED')
            before=disk.read_bytes();gen,_,files=snapshot(before)
            start=vm.send('persist-quiescence-test');vm.expect('PERSIST QUIESCENCE TEST PASSED',start,timeout=90)
            assert b'RECOVERED interrupted trial' not in vm.output[start:]
            newgen,_,after=snapshot(disk.read_bytes())
            assert newgen==gen+2,'Only diagnostic explicit BEGIN/FINISH may commit'
            assert after['/boot/current'][8]==0
            for name in ('/notes/user','/slots/a.sfk','/slots/b.sfk'):
                assert after[name]==files[name]
            before=disk.read_bytes()
            vm.command('persist-quiescent','persist: QUIESCENT preflight only')
            vm.command('cat /persist/notes','quiescence-note')
            vm.command('run installed','sfexec: exit 0')
            assert disk.read_bytes()==before,'Stable preflight changed data or selected package'
            vm.command('login alice alice','login: alice uid=1000')
            vm.command('persist-quiescent','denied: root session required')
            vm.command('persist-quiescence-test','denied: root session required')
            vm.command('login root fox-root','login: root uid=0')
            vm.command('revoke-key /disk/bin/diskexit.sfk','revoke: OK')
            before=disk.read_bytes();revoked=snapshot(before)[2]['/boot/revoked'];assert len(revoked)==32
            vm.command('persist-quiescent','persist: QUIESCENT preflight only')
            vm.command('trust','trust: anchors=3 revoked=1')
            assert disk.read_bytes()==before and snapshot(disk.read_bytes())[2]['/boot/revoked']==revoked
            vm.command('unmount /disk','disk: UNMOUNTED')
            vm.command('persist-quiescent','persist: preflight refused code=10')
            vm.command('mount /disk','disk: MOUNTED')
            vm.command('persist-quiescent','persist: QUIESCENT preflight only')
            assert disk.read_bytes()==before,'Offline/reattach observation changed stable sectors'
        finally:vm.close()
    assert (ROOT/'build/persistent.img').read_bytes()==source
    result={'passed':True,'guest_boots':2,'live_trial_busy':2,'open_description_busy':2,'dirty_ledger_busy':2,
            'user_denials':4,'stable_pending_byte_preserved':True,'base_disk_unchanged':True,
            'advisory_only':True,'service_stop_restart_tested':False}
    (ROOT/'build/persist-quiescence-results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PERSIST QUIESCENCE QEMU PASS: 2 boots; actual live TRYING/open/DIRTY refusal, PENDING/STABLE bytes, root-session gate, revocation/offline/reattach; advisory only, no service stop/restart',flush=True)


if __name__=='__main__':run()
