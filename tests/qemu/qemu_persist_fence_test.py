"""Four serial E2 boots: live healthy fence, actual unknown DIRTY, recovery.

Requires the reviewed frontend/create/raw-exchange owner guards composed with
the diagnostic, then a fresh build. This runner creates no synthetic latch,
provider restart, new wire operation, or physical hardware evidence.
"""
import hashlib
import json
import subprocess
from qemu_persist_test import VM,ROOT,generation,snapshot
from qemu_persist_write_reply_test import artifact_hashes,check_transport,command
from trust_sync_fixture import provider,STATE

HEALTHY_MARKERS=('PERSIST FENCE DRAIN PASSED','PERSIST FENCE OWNER GUARDS PASSED',
                 'PERSIST FENCE RAM AND BUNDLE PASSED','PERSIST FENCE TOKENS PASSED',
                 'PERSIST FENCE HEALTHY PASSED')


def diagnostic(vm,mode):
    markers=HEALTHY_MARKERS if mode=='healthy' else ('PERSIST FENCE UNKNOWN DIRTY PASSED',)
    start=vm.send('persist-fence-test '+mode)
    vm.expect(markers[-1],start,timeout=90);vm.expect('fox> ',start,timeout=90)
    output=vm.output[start:].decode(errors='replace').replace('\r','')
    assert output.endswith('fox> '),'Missing diagnostic prompt'
    lines=output[:-len('fox> ')].splitlines()
    assert all(lines.count(marker)==1 for marker in markers),'Missing, echoed or duplicate diagnostic result'
    assert not any(line.startswith('persist-fence: ') for line in lines),'Diagnostic reported an error or retained owner'


def healthy(vm,disk,transport,state):
    before=disk.read_bytes();gen,_,files=snapshot(before)
    assert files['/boot/current'][8]==(0 if state=='STABLE' else 1),'Wrong selection phase for fence test'
    diagnostic(vm,'healthy')
    assert disk.read_bytes()==before and generation(disk)==gen,'Healthy fence changed transaction sectors'
    command(vm,'cat /persist/notes','FENCE-NOTE')
    command(vm,'cat /disk/etc/readme.txt','SpiritFoxOS read-only disk mounted through user-space virtio-blk.')
    command(vm,'run hello.sfk','sfexec: exit 0')
    command(vm,'persist-quiescent','persist: QUIESCENT preflight only')
    assert disk.read_bytes()==before,'Cancelled fence changed selection/notes during ordinary access'
    return {'transport':transport,'state':state,'passed':True,'generation':gen,
            'markers':list(HEALTHY_MARKERS),'all_disk_bytes_preserved':True}


def run():
    result_path=ROOT/'build/persist-fence-results.json';result_path.unlink(missing_ok=True)
    source=(ROOT/'build/persistent.img').read_bytes();products=artifact_hashes()
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    cases=[];unknown=[];recoveries=[];pending=[];boots=user_denials=0
    # No fixture/VM overlap: EFI, OVMF and the durable restoration state are shared.
    with provider('tests/fixtures/persist_write_reply_fault.c',write_reply='zero'):
        for modern in (False,True):
            transport='modern' if modern else 'legacy';disk=ROOT/'build'/('persist-fence-'+transport+'.img')
            disk.write_bytes(source);vm=VM(disk,'fence-'+transport,iommu=modern)
            try:
                vm.boot('PERSIST READY');vm.expect('fox> ',0);check_transport(vm,modern);boots+=1
                command(vm,'login alice alice','login: alice uid=1000')
                for mode in ('healthy','unknown'):
                    command(vm,'persist-fence-test '+mode,'denied: root session required');user_denials+=1
                command(vm,'login root fox-root','login: root uid=0')
                command(vm,'persist-fence-test','persist-fence: refused code=1')
                command(vm,'write /persist/notes FENCE-NOTE','write: OK')
                command(vm,'install /disk/bin/diskhello.sfk','install: COMMITTED')
                command(vm,'run installed','sfexec: exit 0')
                cases.append(healthy(vm,disk,transport,'STABLE'))
                command(vm,'install /disk/bin/diskupdate.sfk','install: COMMITTED')
                cases.append(healthy(vm,disk,transport,'PENDING'))
                before=disk.read_bytes();old,_,protected=snapshot(before)
                note='PWR-ACK-COMMIT-FENCE-'+transport.upper();payload=note.encode('ascii')
                assert len(payload)>=len(protected['/notes/user'])
                start=command(vm,'write /persist/notes '+note,'write: error')
                marker='[WRITE-ACK] committed requested='+str(len(payload))+' reply-length=0 status='+str(len(payload))
                vm.expect(marker,start);committed=disk.read_bytes();gen,_,files=snapshot(committed)
                assert gen==old+1 and files['/notes/user']==payload,'WRITE fault was not one real full commit'
                assert {p:d for p,d in files.items() if p!='/notes/user'}=={p:d for p,d in protected.items() if p!='/notes/user'}
                diagnostic(vm,'unknown')
                for _ in range(2):
                    command(vm,'trust','trust: sync unavailable code=20')
                    command(vm,'persist','persist: UNAVAILABLE code=20')
                    command(vm,'persist-quiescent','persist: preflight refused code=20')
                    command(vm,'unmount /disk','disk: unmount refused')
                command(vm,'write /persist/notes forbidden-replay','write: error')
                command(vm,'truncate /persist/notes 0','truncate: failed code=20')
                command(vm,'install /disk/bin/diskhello.sfk','install: failed code=20')
                command(vm,'run hello.sfk','sfexec: failed code=20')
                command(vm,'run /disk/bin/diskhello.sfk','sfexec: failed code=20')
                command(vm,'cat /persist/notes','cat: error')
                diagnostic(vm,'unknown');command(vm,'echo','echo: ping')
                assert disk.read_bytes()==committed and generation(disk)==gen,'Unknown fence admission replayed or repaired a write'
            finally:
                vm.close()
            assert vm.output.count(b'[WRITE-ACK] committed requested=')==1
            assert b'[WRITE-ACK] post-fault request opcode=' not in vm.output,'Unknown query/cancel sent a provider RPC'
            assert disk.read_bytes()==committed,'Shutdown changed the fault commit'
            unknown.append({'transport':transport,'passed':True,'generation_delta':1,'diagnostic_runs':2,
                            'retained_real_dirty':True,'no_post_fault_provider_rpc':True,'all_committed_bytes_preserved':True})
            pending.append((modern,transport,disk,payload,committed,gen))
    assert artifact_hashes()==products and not json.loads(STATE.read_text(encoding='utf-8'))['active']
    # The production service is restored before either fresh recovery boot.
    for modern,transport,disk,payload,committed,gen in pending:
        vm=VM(disk,'fence-recovery-'+transport,iommu=modern)
        try:
            vm.boot('PERSIST READY generation='+str(gen));vm.expect('fox> ',0);check_transport(vm,modern);boots+=1
            command(vm,'persist','persist: ACTIVE generation='+str(gen))
            command(vm,'persist','persist: notes-bytes='+str(len(payload)))
            command(vm,'cat /persist/notes',payload.decode('ascii'))
            assert snapshot(disk.read_bytes())[2]['/boot/current'][8]==1
            diagnostic(vm,'healthy')
            command(vm,'run hello.sfk','sfexec: exit 0')
            command(vm,'run /disk/bin/diskhello.sfk','sfexec: exit 0')
            command(vm,'persist-quiescent','persist: QUIESCENT preflight only')
            assert b'[WRITE-ACK]' not in vm.output,'Recovery is using a fault provider'
            assert disk.read_bytes()==committed and generation(disk)==gen,'Fresh recovery rewrote/replayed PENDING state'
        finally:
            vm.close()
        assert disk.read_bytes()==committed
        recoveries.append({'transport':transport,'passed':True,'new_healthy_fence':True,'pending_bytes_preserved':True})
    assert boots==4 and user_denials==4 and len(cases)==4 and len(unknown)==len(recoveries)==2
    assert artifact_hashes()==products and not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert (ROOT/'build/persistent.img').read_bytes()==source
    result={'passed':True,'evidence':'E2-actual-root-fs-persist','source_head':head,
            'production_efi_sha256':products['build/esp/EFI/BOOT/BOOTX64.EFI'],'guest_boots':boots,
            'healthy_cases':cases,'unknown_cases':unknown,'recoveries':recoveries,'user_denials':user_denials,
            'production_artifacts_restored':True,'base_disk_unchanged':True,
            'provider_lifecycle':False,'physical_hardware':False,'power_loss':False}
    result_path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PERSIST FENCE QEMU PASS:',json.dumps(result),flush=True)


if __name__=='__main__':run()
