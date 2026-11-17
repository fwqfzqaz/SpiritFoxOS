"""Actual owner/STOP/exit E2; internal closed terminal state, never restart."""
from contextlib import nullcontext
import hashlib
import json
import re
import subprocess
from qemu_persist_test import VM, ROOT, generation
from qemu_persist_write_reply_test import command, check_transport
from trust_sync_fixture import provider, STATE, FILES

SOURCES = ('tests/qemu/qemu_persist_terminal_test.py',
           'tests/fixtures/persist_terminal_fault.c',
           'tests/qemu/trust_sync_fixture.py', 'services/persist_terminal.c',
           'services/persist.c', 'services/persist_client.c', 'services/init.c',
           'services/console_policy.c', 'include/persistence.h', 'include/sf.h',
           'include/libsf.h', 'services/persist_metadata.c', 'services/persistence.c',
           'services/storage.c', 'kernel/core.c', 'kernel/ipc.c',
           'tests/qemu/qemu_persist_test.py', 'tests/qemu/qemu_persist_write_reply_test.py',
           'services/persist_candidate.c', 'services/persist_candidate.h',
           'services/persist_idle.c', 'services/persist_idle.h')
PRODUCTS = (*FILES, 'build/kernel.elf', 'build/storage.img',
            'build/persistent.img', 'build/trust-policy.json')


def hashes(paths):
    return {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in paths}


def terminal(vm, mode):
    start = vm.send('persist-terminal-test ' + mode)
    vm.expect('PERSIST TERMINAL TEST PASSED mode=' + mode, start, timeout=45)
    vm.expect('fox> ', start, timeout=45)
    text = vm.output[start:].decode(errors='replace').replace('\r', '')
    assert text.endswith('fox> '), 'Missing diagnostic prompt'
    assert not any(line.startswith('persist-terminal: ') for line in text.splitlines())
    pattern = (r'^PERSIST TERMINAL TEST PASSED mode=' + mode +
               r' phase=(\d+) pid=(\d+) ack=(\d+) death=(\d+) retired=(\d+) observations=(\d+)$')
    rows = re.findall(pattern, text, re.MULTILINE)
    assert len(rows) == 1, 'Missing or duplicate terminal result'
    phase, pid, ack, death, retired, observations = map(int, rows[0])
    inspected = mode in ('healthy', 'delayed')
    assert text.splitlines().count('PERSIST CANDIDATE INSPECT PASSED') == int(inspected)
    assert text.splitlines().count('PERSIST CANDIDATE IDLE PASSED') == int(inspected)
    assert pid > 1 and observations >= (0 if mode == 'busy' else 1)
    if mode in ('healthy', 'delayed'):
        assert (phase, ack, death, retired) == (3, 1, 1, 1)
        if mode == 'delayed':
            assert observations > 1, 'Did not actually observe delayed provider exit'
    elif mode == 'unknown':
        assert (phase, ack, death, retired) == (4, 0, 1, 0)
    else:
        assert (phase, ack, death, retired) == (5, 1, 0, 0)
    return {'phase': phase, 'pid': pid, 'ack_authenticated': bool(ack),
            'death_confirmed': bool(death), 'retired': bool(retired),
            'observations': observations, 'candidate_inspected': inspected, 'candidate_idle': inspected}


def run():
    result_path = ROOT / 'build/persist-terminal-results.json'
    result_path.unlink(missing_ok=True)
    frozen = hashes(SOURCES)
    products = hashes(PRODUCTS)
    seed = (ROOT / 'build/persistent.img').read_bytes()
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    cases = []
    denied = 0
    for mode, fault in (('healthy', None), ('unknown', 1), ('delayed', 2), ('busy', 3)):
        context = nullcontext() if fault is None else provider(
            'tests/fixtures/persist_terminal_fault.c', terminal_mode=fault)
        with context:
            for modern in (False, True):
                transport = 'modern' if modern else 'legacy'
                disk = ROOT / 'build' / ('persist-terminal-' + mode + '-' + transport + '.img')
                disk.write_bytes(seed)
                vm = VM(disk, 'terminal-' + mode + '-' + transport, iommu=modern)
                clean = False
                try:
                    vm.boot('PERSIST READY')
                    vm.expect('fox> ', 0)
                    check_transport(vm, modern)
                    command(vm, 'login alice alice', 'login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                    command(vm, 'persist-terminal-test ' + mode, 'denied: root session required')
                    denied += 1
                    assert b'[PERSIST-TERMINAL-FAULT]' not in vm.output
                    command(vm, 'login root fox-root', 'login: root uid=0 gid=0 session=3 home=/home/root')
                    command(vm, 'write /persist/notes TERMINAL-NOTE', 'write: OK')
                    command(vm, 'install /disk/bin/diskhello.sfk', 'install: COMMITTED')
                    command(vm, 'run installed', 'sfexec: exit 0')
                    if mode in ('unknown', 'delayed'):
                        command(vm, 'install /disk/bin/diskupdate.sfk', 'install: COMMITTED')
                    before = disk.read_bytes()
                    old_generation = generation(disk)
                    status = terminal(vm, mode)
                    if mode == 'unknown':
                        assert vm.output.count(b'[PERSIST-TERMINAL-FAULT] accepted ACK dropped') == 1
                    elif mode == 'delayed':
                        assert vm.output.count(b'[PERSIST-TERMINAL-FAULT] accepted ACK delayed exit') == 1
                    elif mode == 'busy':
                        assert vm.output.count(b'[PERSIST-TERMINAL-FAULT] forced busy cache') == 1
                        command(vm, 'persist-quiescent', 'persist: QUIESCENT preflight only')
                        command(vm, 'cat /persist/notes', 'TERMINAL-NOTE')
                        command(vm, 'run hello.sfk', 'sfexec: exit 0')
                    command(vm, 'echo', 'echo: ping')
                    assert disk.read_bytes() == before and generation(disk) == old_generation
                    clean = True
                finally:
                    vm.close(clean)
                assert disk.read_bytes() == before, 'Terminal/shutdown rewrote persistent bytes'
                cases.append({'mode': mode, 'transport': transport, 'passed': True,
                              'status': status, 'disk_bytes_preserved': True,
                              'busy_is_injected_cache_result': mode == 'busy'})
        assert hashes(PRODUCTS) == products, 'Production artifact restoration failed'
        assert not STATE.exists() or not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert hashes(SOURCES) == frozen
    assert len(cases) == denied == 8
    result = {'passed': True, 'evidence': 'E2-real-kernel-IPC-and-provider-exit',
              'source_head': head, 'source_sha256': frozen, 'guest_boots': len(cases),
              'user_denials': denied, 'cases': cases, 'production_artifacts_restored': True,
              'production_sha256': products, 'base_disk_unchanged': (ROOT/'build/persistent.img').read_bytes() == seed,
              'candidate_activation': False, 'generic_restart': False, 'physical_hardware': False}
    result_path.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('PERSIST TERMINAL QEMU PASS:', json.dumps(result), flush=True)


if __name__ == '__main__':
    run()
