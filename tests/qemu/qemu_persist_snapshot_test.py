"""E2 snapshot trust-corruption/lost-reply quarantine with the old peer alive."""
import json
import re
import subprocess
from qemu_persist_test import ROOT,VM
from qemu_persist_write_reply_test import command,check_transport
from qemu_persist_terminal_test import SOURCES as TERMINAL_SOURCES,PRODUCTS,hashes
from trust_sync_fixture import provider,STATE

SOURCES=(*TERMINAL_SOURCES,'tests/qemu/qemu_persist_snapshot_test.py','tests/fixtures/persist_snapshot_fault.c')
def run():
    output=ROOT/'build/persist-snapshot-results.json';output.unlink(missing_ok=True)
    sources,products=hashes(SOURCES),hashes(PRODUCTS);seed=(ROOT/'build/persistent.img').read_bytes()
    head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip();cases=[]
    for mode,fault in (('snapshot-badreply',1),('snapshot-dropped',2)):
        with provider('tests/fixtures/persist_snapshot_fault.c',snapshot_mode=fault):
            for modern in (False,True):
                transport='modern'if modern else'legacy';disk=ROOT/'build'/(mode+'-'+transport+'.img');disk.write_bytes(seed)
                vm=VM(disk,mode+'-'+transport,iommu=modern);clean=False
                try:
                    vm.boot('PERSIST READY');vm.expect('fox> ',0);check_transport(vm,modern)
                    command(vm,'login alice alice','login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                    command(vm,'persist-terminal-test '+mode,'denied: root session required')
                    command(vm,'login root fox-root','login: root uid=0 gid=0 session=3 home=/home/root')
                    start=vm.send('persist-terminal-test '+mode)
                    vm.expect('PERSIST TERMINAL TEST PASSED mode='+mode,start,timeout=45);vm.expect('fox> ',start,timeout=45)
                    text=vm.output[start:].decode(errors='replace').replace('\r','')
                    assert text.endswith('fox> ')and not any(line.startswith('persist-terminal: ')for line in text.splitlines())
                    assert text.splitlines().count('PERSIST SNAPSHOT FAILED CLOSED')==1
                    assert 'PERSIST SNAPSHOT CAPTURE PASSED'not in text and 'PERSIST CANDIDATE'not in text
                    assert len(re.findall(r'^PERSIST TERMINAL TEST PASSED mode='+mode+r' phase=0 pid=\d+ ack=0 death=0 retired=0 observations=0$',text,re.M))==1
                    marker='trust digest corrupted'if fault==1 else'reply dropped'
                    assert vm.output.count(('[PERSIST-SNAPSHOT-FAULT] '+marker).encode())==1
                    command(vm,'echo','echo: ping');assert disk.read_bytes()==seed;clean=True
                finally:vm.close(clean)
                assert disk.read_bytes()==seed
                cases.append({'mode':mode,'transport':transport,'passed':True,'capture_failed_closed':True,
                              'old_provider_alive':True,'old_stop_unchanged':True,'no_unknown_write':True,'disk_bytes_preserved':True})
        assert hashes(PRODUCTS)==products
        assert not STATE.exists()or not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert hashes(SOURCES)==sources and len(cases)==4
    result={'passed':True,'evidence':'E2-real-snapshot-reply-quarantine','source_head':head,'source_sha256':sources,
            'production_sha256':products,'guest_boots':4,'user_denials':4,'cases':cases,
            'production_artifacts_restored':True,'base_disk_unchanged':(ROOT/'build/persistent.img').read_bytes()==seed,
            'candidate_activation':False,'provider_lifecycle':False,'mount_requested':False,'physical_hardware':False}
    output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PERSIST SNAPSHOT QEMU PASS:4 real fault boots; old provider retained, no writes or activation',flush=True)
if __name__=='__main__':run()
