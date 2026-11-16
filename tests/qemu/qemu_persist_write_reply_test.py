"""E2 plan: 4 signed-provider WRITE ACK faults + 4 production recovery boots.

Run only after building the fixed Root client. No buggy baseline guest runs.
The fixture signs the real Persist service, changes one successful ACK length
after its durable write, and retains status=requested and kernel identity.
"""
import hashlib
import json
from qemu_persist_test import VM,ROOT,generation,snapshot
from trust_sync_fixture import provider,STATE,FILES


def artifact_hashes():
    return {name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in FILES}


def check_transport(vm,modern):
    assert (b'[VIRTIO] modern ACCESS_PLATFORM transport' in vm.output)==modern
    assert b'[VIRTIO] flush-capable write service in ring 3' in vm.output


def command(vm,text,expected):
    """Wait for this command's result and prompt; reject CLI echo matches."""
    start=vm.send(text)
    vm.expect(expected,start,timeout=45);vm.expect('fox> ',start,timeout=45)
    output=vm.output[start:].decode(errors='replace').replace('\r','')
    assert output.endswith('fox> '),'Missing current command prompt'
    # cat does not add a newline before the prompt. Remove only that final
    # prompt, then require a complete result line rather than an echoed arg.
    assert expected in output[:-len('fox> ')].splitlines(),text+': result was only a substring/echo'
    return start


def unknown_completion(vm):
    command(vm,'persist','persist: UNAVAILABLE code=20')
    command(vm,'persist-quiescent','persist: preflight refused code=20')
    command(vm,'run hello.sfk','sfexec: failed code=20')
    command(vm,'unmount /disk','disk: unmount refused')


def run():
    result_path=ROOT/'build/persist-write-reply-results.json'
    result_path.unlink(missing_ok=True)
    source=(ROOT/'build/persistent.img').read_bytes()
    products=artifact_hashes()
    boots=rejected=recovered=queries=refused=denied=blocked=0
    cases=[]
    for length in ('zero','excess'):
        pending=[]
        with provider('tests/fixtures/persist_write_reply_fault.c',write_reply=length):
            for modern in (False,True):
                transport='modern' if modern else 'legacy';label=transport+'-'+length
                disk=ROOT/'build'/('persist-write-reply-'+label+'.img');disk.write_bytes(source)
                note='PWR-ACK-COMMIT-'+label;payload=note.encode('ascii')
                reply_length=0 if length=='zero' else len(payload)+1
                vm=VM(disk,'write-reply-'+label,iommu=modern)
                try:
                    vm.boot('PERSIST READY');vm.expect('fox> ',0);boots+=1;check_transport(vm,modern)
                    old=generation(disk);before=snapshot(disk.read_bytes())[2]
                    start=command(vm,'write /persist/notes '+note,'write: error')
                    marker='[WRITE-ACK] committed requested='+str(len(payload))+' reply-length='+str(reply_length)+' status='+str(len(payload))
                    vm.expect(marker,start)
                    assert b'write: OK' not in vm.output[start:]
                    committed=disk.read_bytes();newgen,_,files=snapshot(committed)
                    assert newgen==old+1,'Malformed ACK replayed or lost the real transaction'
                    assert files['/notes/user']==payload,'Real committed WRITE differs from admitted bytes'
                    assert {p:data for p,data in files.items() if p!='/notes/user'}=={p:data for p,data in before.items() if p!='/notes/user'}
                    rejected+=1
                    unknown_completion(vm);refused+=1;denied+=1
                    # Repeated explicit queries cannot clear the latch. The
                    # provider's post-fault trace must stay empty, even if an
                    # accidental query would otherwise leave disk unchanged.
                    for _ in range(2):
                        command(vm,'trust','trust: sync unavailable code=20')
                        command(vm,'persist','persist: UNAVAILABLE code=20')
                        command(vm,'persist-quiescent','persist: preflight refused code=20');queries+=1
                    command(vm,'write /persist/notes forbidden-replay','write: error');blocked+=1
                    command(vm,'truncate /persist/notes 0','truncate: failed code=20');blocked+=1
                    command(vm,'install /disk/bin/diskhello.sfk','install: failed code=20');blocked+=1
                    command(vm,'run /disk/bin/diskhello.sfk','sfexec: failed code=20');denied+=1
                    command(vm,'cat /persist/notes','cat: error')
                    unknown_completion(vm);refused+=1;denied+=1
                    command(vm,'echo','echo: ping')
                    assert disk.read_bytes()==committed and generation(disk)==old+1,'Unknown completion was replayed or query-cleared'
                finally:
                    vm.close()
                assert vm.output.count(b'[WRITE-ACK] committed requested=')==1,'Fault must occur exactly once'
                assert b'[WRITE-ACK] post-fault request opcode=' not in vm.output,'Root sent a Persist RPC after unknown completion'
                assert disk.read_bytes()==committed,'Shutdown changed the committed transaction'
                pending.append((modern,label,disk,payload,committed,newgen,reply_length))
        # Recovery uses the restored production service, not another fault
        # incarnation. The context manager also restores durably on failure.
        assert not json.loads(STATE.read_text(encoding='utf-8'))['active']
        assert artifact_hashes()==products,'Signed fixture did not restore production artifacts'
        for modern,label,disk,payload,committed,newgen,reply_length in pending:
            vm=VM(disk,'write-reply-recovery-'+label,iommu=modern)
            try:
                vm.boot('PERSIST READY generation='+str(newgen));vm.expect('fox> ',0);boots+=1;check_transport(vm,modern)
                command(vm,'persist','persist: ACTIVE generation='+str(newgen))
                command(vm,'persist','persist: notes-bytes='+str(len(payload)))
                command(vm,'cat /persist/notes',payload.decode('ascii'))
                command(vm,'persist-quiescent','persist: QUIESCENT preflight only')
                command(vm,'run hello.sfk','sfexec: exit 0')
                command(vm,'run /disk/bin/diskhello.sfk','sfexec: exit 0')
                command(vm,'unmount /disk','disk: UNMOUNTED')
                assert disk.read_bytes()==committed and generation(disk)==newgen,'Fresh boot replayed or rewrote the committed write'
                assert snapshot(disk.read_bytes())[2]['/notes/user']==payload
                assert b'[WRITE-ACK]' not in vm.output,'Recovery did not use production Persist'
                recovered+=1
            finally:
                vm.close()
            assert disk.read_bytes()==committed,'Recovered shutdown changed committed disk'
            cases.append({'transport':'modern' if modern else 'legacy','length':label.rsplit('-',1)[1],
                          'passed':True,'requested':len(payload),'reply_length':reply_length,
                          'generation_delta':1,'recovery_generation_unchanged':True})
    assert boots==8 and rejected==recovered==4 and queries==refused==8 and denied==blocked==12
    assert artifact_hashes()==products and not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert (ROOT/'build/persistent.img').read_bytes()==source
    result={'passed':True,'evidence':'E2-signed-provider','guest_boots':boots,'malformed_acks_rejected':rejected,
            'fresh_boot_recovery':recovered,'query_latch_checks':queries,'unknown_dirty_unmount_refusals':refused,
            'denied_new_authority':denied,'blocked_mutation_commands':blocked,'cases':cases,
            'single_commit_preserved':True,'no_post_fault_provider_rpc':True,'production_artifacts_restored':True,'base_disk_unchanged':True}
    result_path.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PERSIST WRITE REPLY QEMU PASS:',json.dumps(result),flush=True)


if __name__=='__main__':run()
