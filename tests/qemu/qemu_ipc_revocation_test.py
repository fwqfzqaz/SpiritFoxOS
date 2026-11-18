"""Real queued/closed-parent delegation revocation; no driver handoff."""
import json
import re
import subprocess
from qemu_persist_test import ROOT, VM
from qemu_persist_write_reply_test import command, check_transport
from qemu_persist_terminal_test import PRODUCTS, hashes

SOURCES = ('kernel/ipc.c', 'kernel/core.c', 'kernel/kernel.h', 'include/sf.h',
           'include/libsf.h', 'services/init.c', 'services/console_policy.c',
           'services/revocation_probe.c', 'tools/build.py',
           'tests/qemu/qemu_ipc_revocation_test.py', 'tests/qemu/qemu_persist_test.py',
           'tests/qemu/qemu_persist_write_reply_test.py', 'tests/host/test_ipc.c',
           'tests/host/test_ipc_revocation.c')


def run():
    output = ROOT/'build/ipc-revocation-results.json'
    output.unlink(missing_ok=True)
    sources, products = hashes(SOURCES), hashes(PRODUCTS)
    seed = (ROOT/'build/persistent.img').read_bytes()
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    cases = []
    for cpus in (1, 2):
        for modern in (False, True):
            transport = 'modern' if modern else 'legacy'
            name = 'ipc-revocation-'+transport+'-'+str(cpus)
            disk = ROOT/'build'/('persist-'+name+'.img'); disk.write_bytes(seed)
            vm = VM(disk, name, iommu=modern, smp=cpus)
            clean = False
            try:
                vm.boot('PERSIST READY'); vm.expect('fox> ', 0); check_transport(vm, modern)
                before = disk.read_bytes()
                command(vm, 'login alice alice', 'login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                command(vm, 'ipc-revocation-test', 'denied: root session required')
                command(vm, 'login root fox-root', 'login: root uid=0 gid=0 session=3 home=/home/root')
                start = vm.send('ipc-revocation-test')
                vm.expect('IPC REVOCATION TEST PASSED', start, timeout=60)
                vm.expect('fox> ', start, timeout=60)
                text = vm.output[start:].decode(errors='replace').replace('\r', '')
                counts = re.findall(r'^IPC REVOCATION APP PASSED checks=(\d+)$', text, re.M)
                assert len(counts) == 1 and int(counts[0]) > 200
                assert text.endswith('fox> ') and 'IPC REVOCATION APP FAILED' not in text
                command(vm, 'echo', 'echo: ping')
                start = vm.send('cat /persist/notes'); vm.expect('fox> ', start, timeout=45)
                assert b'cat: error' not in vm.output[start:]
                assert disk.read_bytes() == before
                cases.append({'transport': transport, 'cpus': cpus, 'passed': True,
                              'app_checks': int(counts[0]), 'alice_denied': True,
                              'root_caps_preserved': True, 'ordinary_service_retained': True,
                              'disk_bytes_preserved': True})
                clean = True
            finally:
                vm.close(clean)
            assert disk.read_bytes() == before
    assert hashes(SOURCES) == sources and hashes(PRODUCTS) == products
    assert (ROOT/'build/persistent.img').read_bytes() == seed
    result = {'passed': True, 'evidence': 'E2-real-queued-and-closed-parent-cap-revocation',
              'source_head': head, 'source_sha256': sources, 'production_sha256': products,
              'guest_boots': 4, 'cases': cases, 'production_artifacts_preserved': True,
              'base_disk_unchanged': True, 'driver_authority_handoff': False,
              'physical_hardware': False}
    output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print('IPC REVOCATION QEMU PASS: 4 boots, queue/close lineage, legacy/modern, 1/2 CPUs', flush=True)


if __name__ == '__main__':
    run()
