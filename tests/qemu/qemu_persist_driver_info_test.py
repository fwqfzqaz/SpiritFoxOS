"""Exercise real driver INFO and both bounded Root quarantine paths."""
from contextlib import nullcontext
import json
import re
import subprocess
import time

from qemu_persist_test import ROOT, VM
from qemu_persist_terminal_test import PRODUCTS, SOURCES as TERMINAL_SOURCES, hashes
from qemu_persist_write_reply_test import check_transport, command
from trust_sync_fixture import STATE, provider


FAULT_SOURCE = 'tests/fixtures/persist_driver_info_fault.c'
SOURCES = TERMINAL_SOURCES + ('tests/qemu/qemu_persist_driver_info_test.py', 'tests/fixtures/persist_driver_info_fault.c', 'abi/x86_64-current.json', 'include/virtio.h', 'services/block_rpc.h', 'services/virtio_blk.c', 'services/ahci.c', 'services/nvme.c')


def run_case(mode, modern, seed, error=0):
    transport = 'modern' if modern else 'legacy'
    label = f'driver-info-{mode}-{transport}'
    disk = ROOT / 'build' / (label + '.img')
    disk.write_bytes(seed)
    vm = VM(disk, label, iommu=modern)
    clean = False
    try:
        vm.boot('PERSIST READY')
        vm.expect('fox> ', 0)
        check_transport(vm, modern)
        command(vm, 'login alice alice', 'login: alice uid=1000 gid=1000 session=2 home=/home/alice')
        command(vm, 'persist-candidate-driver-info', 'denied: root session required')
        command(vm, 'login root fox-root', 'login: root uid=0 gid=0 session=3 home=/home/root')
        command(vm, 'persist-terminal-test verify-hold', 'PERSIST CANDIDATE HELD')
        start = vm.send('persist-candidate-stage')
        vm.expect('PERSIST PROVISIONAL CAPS READY', start, timeout=45)
        vm.expect('fox> ', start, timeout=45)
        staged = re.findall(rb'^PERSIST PROVISIONAL CAPS READY phase=11 pid=(\d+) root-parents=2 candidate-caps=4 preserved=1\r?$',
                            vm.output[start:], re.M)
        assert len(staged) == 1 and int(staged[0]) > 1
        if error:
            start = vm.send('persist-candidate-driver-info')
            vm.expect(f'PERSIST CANDIDATE DRIVER INFO QUARANTINED code={error} '
                      'parents-retired=1 candidate-caps=2 driver-alive=1 root-io-blocked=1', start)
            vm.expect(f'persist-candidate-driver-info: refused code={error}', start)
            vm.expect('fox> ', start)
            tag = ('malformed' if mode == 'bad-reply' else 'suppressed') + ' candidate reply'
            assert vm.output[start:].count(('[PERSIST-INFO-FAULT] ' + tag).encode()) == 1
            command(vm, 'persist-candidate-discard', 'persist-candidate-discard: refused code=8')
        else:
            command(vm, 'persist-candidate-driver-info',
                    f'PERSIST CANDIDATE DRIVER INFO PASSED sectors={len(seed)//512} serial=1')
            command(vm, 'persist-candidate-driver-info', 'persist-candidate-driver-info: refused code=8')
            time.sleep(3.5)
            command(vm, 'persist-candidate-discard',
                    f'PERSIST CANDIDATE HELD DISCARD PASSED phase=6 pid={int(staged[0])} preserved=1 old_expired=1')
        if error:
            command(vm, 'persist-candidate-driver-info', 'persist-candidate-driver-info: refused code=8')
        command(vm, 'echo', 'echo: ping')
        command(vm, 'cat /persist/notes', 'cat: error')
        clean = True
    finally:
        vm.close(clean)
    assert disk.read_bytes() == seed
    return {'mode': mode, 'transport': transport, 'error': error, 'passed': True,
            'root_released': not error, 'root_quarantined': bool(error),
            'scoped_caps_retired': True, 'disk_unchanged': True}


def run():
    output = ROOT / 'build/persist-driver-info-results.json'
    output.unlink(missing_ok=True)
    seed = (ROOT / 'build/persistent.img').read_bytes()
    sources, products = hashes(SOURCES), hashes(PRODUCTS)
    abi = json.loads((ROOT / 'abi/x86_64-current.json').read_text(encoding='utf-8'))['values']
    profiles = (('healthy', 0, 0), ('bad-reply', 1, -abi['constant.SF_BAD_FORMAT']),
                ('lost-reply', 2, -abi['constant.SF_TIMEOUT']))
    cases = []
    for mode, fault, error in profiles:
        with provider(FAULT_SOURCE, driver_info_mode=fault) if fault else nullcontext():
            for modern in (False, True):
                cases.append(run_case(mode, modern, seed, error))
        assert hashes(PRODUCTS) == products
        assert not STATE.exists() or not json.loads(STATE.read_text(encoding='utf-8'))['active']
    assert hashes(SOURCES) == sources and len(cases) == 6
    assert (ROOT / 'build/persistent.img').read_bytes() == seed
    result = {'evidence': 'E2-real-driver-info-and-candidate-reply-faults', 'passed': True,
              'source_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'source_sha256': sources, 'production_sha256': products, 'cases': cases,
              'guest_boots': len(cases), 'production_artifacts_restored': True}
    output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('PERSIST DRIVER INFO QEMU PASS: 6 boots; confirmed INFO, malformed and suppressed candidate replies', flush=True)


if __name__ == '__main__':
    run()
