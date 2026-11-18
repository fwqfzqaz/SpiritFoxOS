"""Actual provisional authority ownership/retirement; no activation."""
from contextlib import nullcontext
import json
import re
import subprocess
import time
from qemu_persist_test import ROOT, VM
from qemu_persist_write_reply_test import command, check_transport
from qemu_persist_terminal_test import SOURCES as TERMINAL_SOURCES, PRODUCTS, hashes
from trust_sync_fixture import provider, STATE

SOURCES = (*TERMINAL_SOURCES, 'tests/qemu/qemu_persist_stage_test.py',
           'tests/fixtures/persist_stage_fault.c')


def run():
    output = ROOT/'build/persist-stage-results.json'; output.unlink(missing_ok=True)
    sources, products = hashes(SOURCES), hashes(PRODUCTS)
    seed = (ROOT/'build/persistent.img').read_bytes()
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    cases = []
    profiles = (('healthy', 'STABLE', 0), ('healthy', 'PENDING', 0),
                ('bad-first-ack', 'STABLE', 1), ('lost-second-ack', 'STABLE', 2))
    for mode, state, fault in profiles:
        context = provider('tests/fixtures/persist_stage_fault.c', stage_mode=fault) if fault else nullcontext()
        with context:
            for modern in (False, True):
                transport = 'modern' if modern else 'legacy'
                name = 'stage-'+mode+'-'+state+'-'+transport
                disk = ROOT/'build'/('persist-'+name+'.img'); disk.write_bytes(seed)
                vm = VM(disk, name, iommu=modern); clean = False
                try:
                    vm.boot('PERSIST READY'); vm.expect('fox> ', 0); check_transport(vm, modern)
                    if state == 'PENDING':
                        command(vm, 'write /persist/notes STAGE-NOTE', 'write: OK')
                        command(vm, 'install /disk/bin/diskhello.sfk', 'install: COMMITTED')
                    before = disk.read_bytes()
                    command(vm, 'login alice alice', 'login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                    command(vm, 'persist-candidate-stage', 'denied: root session required')
                    command(vm, 'login root fox-root', 'login: root uid=0 gid=0 session=3 home=/home/root')
                    command(vm, 'persist-terminal-test verify-hold', 'PERSIST CANDIDATE HELD')
                    start = vm.send('persist-candidate-stage')
                    if fault:
                        abi = json.loads((ROOT/'abi/x86_64-current.json').read_text(encoding='utf-8'))['values']
                        error = -abi['constant.SF_BAD_FORMAT' if fault == 1 else 'constant.SF_TIMEOUT']
                        marker = 'PERSIST PROVISIONAL CAPS FAILED CLOSED code='+str(error)+' parents-retired=1 candidate-caps=2 preserved=1'
                        vm.expect(marker, start, timeout=45); vm.expect('fox> ', start, timeout=45)
                        command(vm, 'persist-candidate-stage', 'persist-candidate-stage: refused code=8')
                        command(vm, 'persist-candidate-discard', 'persist-candidate-discard: refused code=8')
                        tag = b'first ACK malformed' if fault == 1 else b'second ACK dropped'
                        assert vm.output.count(b'[PERSIST-STAGE-FAULT] '+tag) == 1
                        phase, outcome = 7, 'quarantined-branches-retired'
                    else:
                        vm.expect('PERSIST PROVISIONAL CAPS READY phase=11', start, timeout=45)
                        vm.expect('fox> ', start, timeout=45)
                        text = vm.output[start:].decode(errors='replace').replace('\r', '')
                        found = re.findall(r'^PERSIST PROVISIONAL CAPS READY phase=11 pid=(\d+) root-parents=2 candidate-caps=4 preserved=1$', text, re.M)
                        assert len(found) == 1 and int(found[0]) > 1
                        command(vm, 'persist-candidate-stage', 'persist-candidate-stage: refused code=8')
                        command(vm, 'write /tmp/staged-note RAM-STAGED', 'write: OK')
                        command(vm, 'cat /tmp/staged-note', 'RAM-STAGED')
                        time.sleep(3.5)
                        command(vm, 'persist-candidate-discard', 'PERSIST CANDIDATE HELD DISCARD PASSED phase=6 pid='+found[0]+' preserved=1 old_expired=1')
                        command(vm, 'persist-candidate-discard', 'persist-candidate-discard: refused code=16')
                        phase, outcome = 6, 'normal-discard-all-four-root-handles-retired'
                    command(vm, 'echo', 'echo: ping')
                    command(vm, 'cat /persist/notes', 'cat: error')
                    assert disk.read_bytes() == before
                    cases.append({'mode': mode, 'state': state, 'transport': transport,
                                  'passed': True, 'phase': phase, 'outcome': outcome,
                                  'alice_denied': True, 'duplicate_denied': True,
                                  'driver_originals_usable': True, 'root_cap_budget_preserved': True,
                                  'old_capture_stop_trust_preserved': True, 'disk_bytes_preserved': True})
                    clean = True
                finally:
                    vm.close(clean)
                assert disk.read_bytes() == before
        assert hashes(PRODUCTS) == products
        assert not STATE.exists() or not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert hashes(SOURCES) == sources and len(cases) == 8
    assert (ROOT/'build/persistent.img').read_bytes() == seed
    result = {'passed': True, 'evidence': 'E2-real-provisional-driver-cap-ownership-and-retirement',
              'source_head': head, 'source_sha256': sources, 'production_sha256': products,
              'guest_boots': 8, 'cases': cases, 'production_artifacts_restored': True,
              'base_disk_unchanged': True, 'ordinary_backend_bound': False,
              'candidate_activation': False, 'physical_hardware': False}
    output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print('PERSIST STAGE QEMU PASS: 8 boots; provisional caps, exact branch cleanup, no activation', flush=True)


if __name__ == '__main__':
    run()
