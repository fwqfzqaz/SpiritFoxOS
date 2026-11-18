"""Real Root shared-reply gating; explicit quarantine, no candidate driver RPC."""
import json
import re
import subprocess
import time
from qemu_persist_test import ROOT, VM
from qemu_persist_write_reply_test import command, check_transport
from qemu_persist_terminal_test import SOURCES as TERMINAL_SOURCES, PRODUCTS, hashes

SOURCES = (*TERMINAL_SOURCES, 'tests/qemu/qemu_storage_reply_owner_test.py')


def run():
    output = ROOT/'build/storage-reply-owner-results.json'
    output.unlink(missing_ok=True)
    sources, products = hashes(SOURCES), hashes(PRODUCTS)
    seed = (ROOT/'build/persistent.img').read_bytes()
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    cases = []
    for mode, state in (('known-release', 'STABLE'), ('known-release', 'PENDING'),
                        ('explicit-quarantine', 'STABLE')):
        for modern in (False, True):
            transport = 'modern' if modern else 'legacy'
            name = 'reply-owner-'+mode+'-'+state+'-'+transport
            disk = ROOT/'build'/('persist-'+name+'.img'); disk.write_bytes(seed)
            vm = VM(disk, name, iommu=modern); clean = False
            try:
                vm.boot('PERSIST READY'); vm.expect('fox> ', 0); check_transport(vm, modern)
                if state == 'PENDING':
                    command(vm, 'write /persist/notes REPLY-OWNER-NOTE', 'write: OK')
                    command(vm, 'install /disk/bin/diskhello.sfk', 'install: COMMITTED')
                before = disk.read_bytes()
                command(vm, 'login alice alice', 'login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                for line in ('persist-reply-owner-test', 'persist-reply-owner-test taint'):
                    command(vm, line, 'denied: root session required')
                command(vm, 'login root fox-root', 'login: root uid=0 gid=0 session=3 home=/home/root')
                command(vm, 'persist-terminal-test verify-hold', 'PERSIST CANDIDATE HELD')
                start = vm.send('persist-candidate-stage')
                vm.expect('PERSIST PROVISIONAL CAPS READY phase=11', start, timeout=45)
                vm.expect('fox> ', start, timeout=45)
                text = vm.output[start:].decode(errors='replace').replace('\r', '')
                staged = re.findall(r'^PERSIST PROVISIONAL CAPS READY phase=11 pid=(\d+) root-parents=2 candidate-caps=4 preserved=1$', text, re.M)
                assert len(staged) == 1 and int(staged[0]) > 1
                if mode == 'known-release':
                    serials = []
                    for _ in range(2):
                        start = vm.send('persist-reply-owner-test')
                        vm.expect('PERSIST REPLY OWNER KNOWN RELEASE PASSED serial=', start, timeout=45)
                        vm.expect('fox> ', start, timeout=45)
                        text = vm.output[start:].decode(errors='replace').replace('\r', '')
                        found = re.findall(r'^PERSIST REPLY OWNER KNOWN RELEASE PASSED serial=(\d+) candidate-caps=4 preserved=1$', text, re.M)
                        assert len(found) == 1; serials.append(int(found[0]))
                    assert serials == [1, 2]
                    time.sleep(3.5)
                    command(vm, 'persist-candidate-discard', 'PERSIST CANDIDATE HELD DISCARD PASSED phase=6 pid='+staged[0]+' preserved=1 old_expired=1')
                    command(vm, 'persist-reply-owner-test', 'persist-reply-owner-test: refused code=16')
                else:
                    serials = [1]
                    command(vm, 'persist-reply-owner-test taint',
                            'PERSIST REPLY OWNER EXPLICIT QUARANTINE PASSED serial=1 candidate-caps=2 driver-alive=1 io-blocked=1 preserved=1')
                    command(vm, 'persist-reply-owner-test', 'persist-reply-owner-test: refused code=8')
                    command(vm, 'persist-candidate-discard', 'persist-candidate-discard: refused code=8')
                command(vm, 'write /tmp/reply-owner-note RAM-REPLY-OWNER', 'write: OK')
                command(vm, 'cat /tmp/reply-owner-note', 'RAM-REPLY-OWNER')
                command(vm, 'echo', 'echo: ping')
                command(vm, 'cat /persist/notes', 'cat: error')
                assert disk.read_bytes() == before
                cases.append({'mode': mode, 'state': state, 'transport': transport,
                              'passed': True, 'lease_serials': serials,
                              'alice_denied': True, 'root_transport_gated': True,
                              'driver_alive': True, 'ram_echo_retained': True,
                              'disk_bytes_preserved': True, 'old_stop_capture_trust_preserved': True,
                              'known_root_reads_resumed': mode == 'known-release',
                              'normal_discard': mode == 'known-release',
                              'explicit_quarantine': mode == 'explicit-quarantine',
                              'candidate_branches_retired': mode == 'explicit-quarantine'})
                clean = True
            finally:
                vm.close(clean)
            assert disk.read_bytes() == before
    assert hashes(SOURCES) == sources and hashes(PRODUCTS) == products and len(cases) == 6
    assert (ROOT/'build/persistent.img').read_bytes() == seed
    result = {'passed': True, 'evidence': 'E2-real-Root-reply-gate-and-explicit-quarantine',
              'source_head': head, 'source_sha256': sources, 'production_sha256': products,
              'guest_boots': 6, 'cases': cases, 'production_artifacts_preserved': True,
              'base_disk_unchanged': True, 'candidate_driver_rpc': False,
              'real_driver_timeout': False, 'ordinary_backend_bound': False,
              'candidate_activation': False, 'physical_hardware': False}
    output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print('STORAGE REPLY OWNER QEMU PASS: 6 boots; known release/explicit quarantine, no candidate driver RPC', flush=True)


if __name__ == '__main__':
    run()
