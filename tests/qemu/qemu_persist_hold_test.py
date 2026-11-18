"""E2 delayed explicit discard of a held private candidate; no activation."""
from contextlib import nullcontext
import json
import re
import subprocess
import time
from qemu_persist_test import ROOT, VM
from qemu_persist_write_reply_test import command, check_transport
from qemu_persist_terminal_test import SOURCES as TERMINAL_SOURCES, PRODUCTS, hashes
from trust_sync_fixture import provider, STATE

SOURCES = (*TERMINAL_SOURCES, 'tests/qemu/qemu_persist_hold_test.py',
           'tests/fixtures/persist_idle_fault.c')


def run():
    output = ROOT/'build/persist-hold-results.json'
    output.unlink(missing_ok=True)
    frozen, products = hashes(SOURCES), hashes(PRODUCTS)
    seed = (ROOT/'build/persistent.img').read_bytes()
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    cases = []
    profiles = (('healthy', 'STABLE', None), ('healthy', 'PENDING', None),
                ('idle-dropdiscard', 'STABLE', 2))
    for mode, state, fault in profiles:
        context = nullcontext() if fault is None else provider(
            'tests/fixtures/persist_idle_fault.c', idle_mode=fault)
        with context:
            for modern in (False, True):
                transport = 'modern' if modern else 'legacy'
                name = 'hold-'+mode+'-'+state+'-'+transport
                disk = ROOT/'build'/('persist-'+name+'.img')
                disk.write_bytes(seed)
                vm = VM(disk, name, iommu=modern)
                clean = False
                committed = None
                try:
                    vm.boot('PERSIST READY'); vm.expect('fox> ', 0); check_transport(vm, modern)
                    if state == 'PENDING':
                        command(vm, 'write /persist/notes HOLD-NOTE', 'write: OK')
                        command(vm, 'install /disk/bin/diskhello.sfk', 'install: COMMITTED')
                    committed = disk.read_bytes()

                    command(vm, 'login alice alice',
                            'login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                    command(vm, 'persist-terminal-test verify-hold', 'denied: root session required')
                    command(vm, 'persist-candidate-discard', 'denied: root session required')
                    command(vm, 'login root fox-root',
                            'login: root uid=0 gid=0 session=3 home=/home/root')

                    start = vm.send('persist-terminal-test verify-hold')
                    vm.expect('PERSIST CANDIDATE HELD', start, timeout=90)
                    vm.expect('fox> ', start, timeout=90)
                    held = vm.output[start:].decode(errors='replace').replace('\r', '')
                    assert held.endswith('fox> ')
                    old = re.findall(r'^PERSIST TERMINAL TEST PASSED mode=verify-hold phase=3 pid=(\d+) ack=1 death=1 retired=1 observations=\d+$', held, re.M)
                    trace = re.findall(r'^PERSIST IDLE TRACE error=0 phase=9 probe=1 guard=1 discard=0 death=0 retired=0 caps-before=(\d+) caps-after=(\d+)$', held, re.M)
                    idle = re.findall(r'^PERSIST CANDIDATE IDLE RESULT phase=9 pid=(\d+) probe=1 guard=1 discard=0 death=0 retired=0 observations=0$', held, re.M)
                    verify = re.findall(r'^PERSIST CANDIDATE VERIFY RESULT verified=1 reads=(\d+) phase=9$', held, re.M)
                    assert len(old) == len(trace) == len(idle) == len(verify) == 1
                    old_pid, pid = int(old[0]), int(idle[0])
                    caps_before, caps_after = map(int, trace[0])
                    reads = int(verify[0])
                    assert pid > 1 and pid != old_pid and caps_after == caps_before + 2
                    assert 259 <= reads <= 515
                    assert held.splitlines().count('PERSIST CANDIDATE HELD') == 1

                    command(vm, 'write /tmp/held-note RAM-HELD', 'write: OK')
                    command(vm, 'cat /tmp/held-note', 'RAM-HELD')
                    command(vm, 'echo', 'echo: ping')
                    command(vm, 'cat /persist/notes', 'cat: error')
                    assert disk.read_bytes() == committed
                    time.sleep(3.5)  # Let the hold's original three-second deadline expire.

                    start = vm.send('persist-candidate-discard')
                    if fault:
                        vm.expect('PERSIST CANDIDATE HELD DISCARD FAILED CLOSED phase=7 verified=1 ack=0 death=0 retired=0 old_expired=1', start, timeout=45)
                        vm.expect('persist-candidate-discard: refused code=9', start, timeout=45)
                        vm.expect('fox> ', start, timeout=45)
                        outcome, phase = 'discard-ack-dropped', 7
                        command(vm, 'persist-candidate-discard', 'persist-candidate-discard: refused code=8')
                        assert vm.output.count(b'[PERSIST-IDLE-FAULT] discard ACK dropped') == 1
                    else:
                        vm.expect('PERSIST CANDIDATE HELD DISCARD PASSED phase=6 pid='+str(pid)+' preserved=1 old_expired=1', start, timeout=45)
                        vm.expect('fox> ', start, timeout=45)
                        outcome, phase = 'discarded', 6
                        command(vm, 'persist-candidate-discard', 'persist-candidate-discard: refused code=16')

                    text = vm.output[start:].decode(errors='replace').replace('\r', '')
                    assert text.endswith('fox> ') and disk.read_bytes() == committed
                    clean = True
                finally:
                    vm.close(clean)
                assert committed is not None and disk.read_bytes() == committed
                cases.append({'mode': mode, 'state': state, 'transport': transport,
                              'passed': True, 'held_pid': pid, 'verified_reads': reads,
                              'phase': phase, 'discard_outcome': outcome,
                              'old_hold_deadline_expired': True, 'alice_denied_hold_and_discard': True,
                              'ram_access_retained': True, 'persist_read_blocked': True,
                              'disk_bytes_preserved': True, 'old_capture_stop_preserved': True})
        assert hashes(PRODUCTS) == products
        assert not STATE.exists() or not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert hashes(SOURCES) == frozen and len(cases) == 6
    result = {'passed': True, 'evidence': 'E2-real-held-candidate-and-delayed-explicit-discard',
              'source_head': head, 'source_sha256': frozen, 'production_sha256': products,
              'guest_boots': 6, 'user_denials': 12, 'cases': cases,
              'production_artifacts_restored': True,
              'base_disk_unchanged': (ROOT/'build/persistent.img').read_bytes() == seed,
              'candidate_activation': False, 'driver_grants': False,
              'mount_requested': False, 'physical_hardware': False}
    output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print('PERSIST HOLD QEMU PASS: 6 boots, delayed explicit discard, no activation or driver grants', flush=True)


if __name__ == '__main__':
    run()
