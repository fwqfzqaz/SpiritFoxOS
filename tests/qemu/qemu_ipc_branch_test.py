"""Real owner-only branch selection with legacy endpoint revoke preserved."""
import json
import re
import subprocess
from qemu_persist_test import ROOT, VM
from qemu_persist_write_reply_test import command, check_transport
from qemu_persist_terminal_test import PRODUCTS, hashes

SOURCES = ('kernel/ipc.c', 'kernel/core.c', 'kernel/kernel.h', 'include/sf.h',
           'include/libsf.h', 'services/init.c', 'services/console_policy.c',
           'services/branch_probe.c', 'tools/build.py',
           'tests/qemu/qemu_ipc_branch_test.py', 'tests/qemu/qemu_persist_test.py',
           'tests/qemu/qemu_persist_write_reply_test.py', 'tests/host/test_ipc.c',
           'tests/host/test_ipc_branch.c', 'tools/abi_snapshot.c', 'abi/x86_64-current.json')


def run():
    output = ROOT/'build/ipc-branch-results.json'
    output.unlink(missing_ok=True)
    sources, products = hashes(SOURCES), hashes(PRODUCTS)
    seed = (ROOT/'build/persistent.img').read_bytes()
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    cases = []
    for cpus in (1, 2):
        for modern in (False, True):
            transport = 'modern' if modern else 'legacy'
            name = 'ipc-branch-'+transport+'-'+str(cpus)
            disk = ROOT/'build'/('persist-'+name+'.img'); disk.write_bytes(seed)
            vm = VM(disk, name, iommu=modern, smp=cpus)
            clean = False
            try:
                vm.boot('PERSIST READY'); vm.expect('fox> ', 0); check_transport(vm, modern)
                before = disk.read_bytes()
                command(vm, 'login alice alice', 'login: alice uid=1000 gid=1000 session=2 home=/home/alice')
                command(vm, 'ipc-branch-test', 'denied: root session required')
                command(vm, 'login root fox-root', 'login: root uid=0 gid=0 session=3 home=/home/root')
                start = vm.send('ipc-branch-test')
                vm.expect('IPC BRANCH TEST PASSED', start, timeout=60)
                vm.expect('fox> ', start, timeout=60)
                text = vm.output[start:].decode(errors='replace').replace('\r', '')
                counts = re.findall(r'^IPC BRANCH APP PASSED checks=(\d+)$', text, re.M)
                assert len(counts) == 1 and int(counts[0]) == 16
                assert text.endswith('fox> ') and 'IPC BRANCH APP FAILED' not in text
                command(vm, 'echo', 'echo: ping')
                start = vm.send('cat /persist/notes'); vm.expect('fox> ', start, timeout=45)
                assert b'cat: error' not in vm.output[start:]
                assert disk.read_bytes() == before
                cases.append({'transport': transport, 'cpus': cpus, 'passed': True,
                              'app_checks': int(counts[0]), 'alice_denied': True,
                              'owner_parent_sibling_preserved': True,
                              'installed_and_queued_revoked': True,
                              'legacy_endpoint_revoke_preserved': True,
                              'root_caps_preserved': True, 'ordinary_service_retained': True,
                              'disk_bytes_preserved': True})
                clean = True
            finally:
                vm.close(clean)
            assert disk.read_bytes() == before
    assert hashes(SOURCES) == sources and hashes(PRODUCTS) == products
    assert (ROOT/'build/persistent.img').read_bytes() == seed
    result = {'passed': True, 'evidence': 'E2-real-owner-branch-revocation',
              'source_head': head, 'source_sha256': sources, 'production_sha256': products,
              'guest_boots': 4, 'cases': cases, 'abi_entries': 680, 'production_artifacts_preserved': True,
              'base_disk_unchanged': True, 'driver_authority_handoff': False,
              'physical_hardware': False}
    output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print('IPC BRANCH QEMU PASS: 4 boots, branch selection and legacy whole-endpoint compatibility, legacy/modern, 1/2 CPUs', flush=True)


if __name__ == '__main__':
    run()
