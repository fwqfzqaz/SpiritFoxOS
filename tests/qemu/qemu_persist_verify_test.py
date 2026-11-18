"""E2 private candidate readonly volume reconstruction and final attestation."""
from contextlib import nullcontext
import json
import re
import subprocess
from qemu_persist_test import ROOT, VM
from qemu_persist_write_reply_test import command, check_transport
from qemu_persist_terminal_test import SOURCES as TERMINAL_SOURCES, PRODUCTS, hashes
from trust_sync_fixture import provider, STATE

SOURCES=(*TERMINAL_SOURCES,'tests/qemu/qemu_persist_verify_test.py','tests/fixtures/persist_verify_fault.c')

def run():
    output=ROOT/'build/persist-verify-results.json';output.unlink(missing_ok=True)
    sources,products=hashes(SOURCES),hashes(PRODUCTS)
    seed=(ROOT/'build/persistent.img').read_bytes();cases=[]
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    profiles=(('verify','STABLE',None),('verify','PENDING',None),('verify-readerror','STABLE',None),
              ('verify-badreply','STABLE',1),('verify-dropped','STABLE',2))
    for mode,state,fault in profiles:
        context=nullcontext()if fault is None else provider('tests/fixtures/persist_verify_fault.c',verify_mode=fault)
        with context:
            for modern in (False,True):
                transport='modern'if modern else'legacy'
                disk=ROOT/'build'/('candidate-'+mode+'-'+state+'-'+transport+'.img');disk.write_bytes(seed)
                vm=VM(disk,'candidate-'+mode+'-'+state+'-'+transport,iommu=modern);clean=False
                try:
                    vm.boot('PERSIST READY');vm.expect('fox> ',0);check_transport(vm,modern)
                    if state=='PENDING':
                        command(vm,'write /persist/notes VERIFY-NOTE','write: OK')
                        command(vm,'install /disk/bin/diskhello.sfk','install: COMMITTED')
                    committed=disk.read_bytes()
                    command(vm,'login alice alice','login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                    command(vm,'persist-terminal-test '+mode,'denied: root session required')
                    command(vm,'login root fox-root','login: root uid=0 gid=0 session=3 home=/home/root')
                    start=vm.send('persist-terminal-test '+mode)
                    vm.expect('PERSIST TERMINAL TEST PASSED mode='+mode,start,timeout=90);vm.expect('fox> ',start,timeout=90)
                    text=vm.output[start:].decode(errors='replace').replace('\r','')
                    assert text.endswith('fox> ')and not any(line.startswith('persist-terminal: ')for line in text.splitlines())
                    assert text.splitlines().count('PERSIST SNAPSHOT CAPTURE PASSED')==1
                    old=re.findall(r'^PERSIST TERMINAL TEST PASSED mode='+mode+r' phase=3 pid=(\d+) ack=1 death=1 retired=1 observations=\d+$',text,re.M)
                    rows=re.findall(r'^PERSIST CANDIDATE IDLE RESULT phase=(\d+) pid=(\d+) probe=(\d+) guard=(\d+) discard=(\d+) death=(\d+) retired=(\d+) observations=(\d+)$',text,re.M)
                    attest=re.findall(r'^PERSIST CANDIDATE VERIFY RESULT verified=(\d+) reads=(\d+) phase=(\d+)$',text,re.M)
                    assert len(old)==len(rows)==len(attest)==1
                    phase,pid,probe,guard,discard,death,retired,observations=map(int,rows[0])
                    verified,reads,attest_phase=map(int,attest[0]);success=mode=='verify'
                    assert pid>1 and pid!=int(old[0])and probe==1 and attest_phase==phase
                    assert 1<=reads<=515
                    if success:
                        assert (phase,verified,guard,discard,death,retired)==(6,1,1,1,1,1)and observations>=1 and reads>=259
                    else:
                        assert (phase,verified,guard,discard,death,retired,observations)==(7,0,0,0,0,0,0)
                        if mode=='verify-readerror':assert reads==1
                    assert text.splitlines().count('PERSIST CANDIDATE VERIFY PASSED')==int(success)
                    assert text.splitlines().count('PERSIST CANDIDATE VERIFY FAILED CLOSED')==int(not success)
                    if fault:
                        marker='final digest corrupted'if fault==1 else'final reply dropped'
                        assert vm.output.count(('[PERSIST-VERIFY-FAULT] '+marker).encode())==1
                    command(vm,'echo','echo: ping');assert disk.read_bytes()==committed;clean=True
                finally:vm.close(clean)
                assert disk.read_bytes()==committed
                cases.append({'mode':mode,'state':state,'transport':transport,'passed':True,'verified':success,
                              'reads':reads,'phase':phase,'normal_discard':bool(discard),'old_capture_preserved':True,
                              'old_stop_preserved':True,'disk_bytes_preserved':True,'root_read_error_injected':mode=='verify-readerror'})
        assert hashes(PRODUCTS)==products
        assert not STATE.exists()or not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert hashes(SOURCES)==sources and len(cases)==10
    result={'passed':True,'evidence':'E2-real-isolated-readonly-volume-and-attestation','source_head':head,
            'source_sha256':sources,'production_sha256':products,'guest_boots':10,'user_denials':10,'cases':cases,
            'production_artifacts_restored':True,'base_disk_unchanged':(ROOT/'build/persistent.img').read_bytes()==seed,
            'candidate_activation':False,'driver_grants':False,'mount_requested':False,'physical_hardware':False}
    output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PERSIST VERIFY QEMU PASS:10 real boots; readonly private reconstruction, no activation or driver grants',flush=True)
if __name__=='__main__':run()
