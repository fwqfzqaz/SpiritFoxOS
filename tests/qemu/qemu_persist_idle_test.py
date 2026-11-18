"""E2 isolated candidate spawn/probe/ordinary-denial/discard; no activation."""
from contextlib import nullcontext
import json
import re
import subprocess
from qemu_persist_test import ROOT, VM
from qemu_persist_write_reply_test import command, check_transport
from qemu_persist_terminal_test import SOURCES as TERMINAL_SOURCES, PRODUCTS, hashes
from trust_sync_fixture import provider, STATE

SOURCES = (*TERMINAL_SOURCES, 'tests/qemu/qemu_persist_idle_test.py',
           'tests/fixtures/persist_idle_fault.c')


def run():
    output = ROOT/'build/persist-idle-results.json'
    output.unlink(missing_ok=True)
    frozen, products = hashes(SOURCES), hashes(PRODUCTS)
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    seed = (ROOT/'build/persistent.img').read_bytes()
    cases = []
    for mode, fault in (('healthy', None), ('idle-badprobe', 1),
                        ('idle-dropdiscard', 2), ('idle-delayed', 3)):
        context = nullcontext() if fault is None else provider(
            'tests/fixtures/persist_idle_fault.c', idle_mode=fault)
        with context:
            for modern in (False, True):
                transport = 'modern' if modern else 'legacy'
                disk = ROOT/'build'/('persist-'+mode+'-'+transport+'.img')
                disk.write_bytes(seed)
                vm = VM(disk, 'candidate-'+mode+'-'+transport, iommu=modern)
                clean = False
                try:
                    vm.boot('PERSIST READY'); vm.expect('fox> ', 0)
                    check_transport(vm, modern)
                    command(vm, 'login alice alice', 'login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                    command(vm, 'persist-terminal-test '+mode, 'denied: root session required')
                    command(vm, 'login root fox-root', 'login: root uid=0 gid=0 session=3 home=/home/root')
                    start = vm.send('persist-terminal-test '+mode)
                    vm.expect('PERSIST TERMINAL TEST PASSED mode='+mode, start, timeout=45)
                    vm.expect('fox> ', start, timeout=45)
                    text = vm.output[start:].decode(errors='replace').replace('\r', '')
                    assert text.endswith('fox> ') and not any(s.startswith('persist-terminal: ') for s in text.splitlines())
                    assert text.splitlines().count('PERSIST SNAPSHOT CAPTURE PASSED') == 1
                    old = re.findall(r'^PERSIST TERMINAL TEST PASSED mode='+mode+
                                     r' phase=3 pid=(\d+) ack=1 death=1 retired=1 observations=\d+$', text, re.M)
                    rows = re.findall(r'^PERSIST CANDIDATE IDLE RESULT phase=(\d+) pid=(\d+) probe=(\d+) guard=(\d+) discard=(\d+) death=(\d+) retired=(\d+) observations=(\d+)$', text, re.M)
                    assert len(old) == len(rows) == 1
                    phase, pid, probe, guard, discard, death, retired, observations = map(int, rows[0])
                    assert pid > 1 and pid != int(old[0])
                    expected = ((6, 1, 1, 1, 1, 1) if mode in ('healthy','idle-delayed') else
                                (7, 0, 0, 0, 0, 0) if mode == 'idle-badprobe' else (7, 1, 1, 0, 0, 0))
                    assert (phase, probe, guard, discard, death, retired) == expected
                    assert observations >= (2 if mode == 'idle-delayed' else 1) if phase == 6 else observations == 0
                    success = phase == 6
                    assert text.splitlines().count('PERSIST CANDIDATE IDLE PASSED') == int(success)
                    assert text.splitlines().count('PERSIST CANDIDATE IDLE FAILED CLOSED') == int(not success)
                    if fault:
                        marker = {1:'malformed probe ACK',2:'discard ACK dropped',3:'discard exit delayed'}[fault]
                        assert vm.output.count(('[PERSIST-IDLE-FAULT] '+marker).encode()) == 1
                    command(vm, 'echo', 'echo: ping')
                    assert disk.read_bytes() == seed
                    clean = True
                finally:
                    vm.close(clean)
                assert disk.read_bytes() == seed
                cases.append({'mode':mode,'transport':transport,'passed':True,
                              'pid':pid,'phase':phase,'probe':bool(probe),'ordinary_denied':bool(guard),
                              'discard':bool(discard),'death':bool(death),'retired':bool(retired),
                              'observations':observations,'disk_bytes_preserved':True,
                              'old_stop_snapshot_preserved':True})
        assert hashes(PRODUCTS) == products
        assert not STATE.exists() or not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert hashes(SOURCES) == frozen and len(cases) == 8
    result = {'passed':True,'evidence':'E2-real-private-candidate-IPC-and-exit',
              'source_head':head,'source_sha256':frozen,'production_sha256':products,
              'guest_boots':8,'user_denials':8,'cases':cases,
              'production_artifacts_restored':True,'base_disk_unchanged':(ROOT/'build/persistent.img').read_bytes()==seed,
              'candidate_activation':False,'driver_grants':False,'mount_requested':False,'physical_hardware':False}
    output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PERSIST IDLE CANDIDATE QEMU PASS: 8 boots, source/product restoration, no driver grant/mount/activation',flush=True)


if __name__ == '__main__':
    run()
