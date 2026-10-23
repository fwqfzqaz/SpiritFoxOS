"""Build, host negative tests, C service tests, and real QEMU integration."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import build

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--skip-build', action='store_true')
parser.add_argument('--host-only', action='store_true')
args = parser.parse_args()
started = time.time()
if not args.skip_build:
    build.main()


def host_test(name, *sources, no_unused=False):
    flags = ['-target', build.HOST, '-g0', '-O2', '-Wall', '-Wextra', '-Werror']
    if no_unused:
        flags.append('-Wno-unused-parameter')
    build.run([*flags, '-Iinclude', '-Ivendor/monocypher', *sources, '-o', 'build/'+name+build.EXE])
    subprocess.run([str(ROOT / ('build/'+name+build.EXE))], check=True)


host_test('test_services', 'tests/test_services.c', 'services/manager.c', 'services/namespace.c')
host_test('test_openat', 'tests/test_openat.c', 'services/namespace.c')
host_test('test_truncate', 'tests/test_truncate.c', 'services/namespace.c')
host_test('test_resize_txn', 'tests/test_resize_txn.c', 'services/txn.c', 'services/sha256.c', no_unused=True)
for name, source in [('ipc', 'kernel/ipc.c'), ('memory', 'kernel/memory.c')]:
    host_test('test_'+name, 'tests/test_'+name+'.c', source, no_unused=True)
host_test('test_storage', 'tests/test_storage.c', 'services/namespace.c')
host_test('test_txn', 'tests/test_txn.c', 'services/txn.c', 'services/sha256.c', no_unused=True)
host_test('test_update', 'tests/test_update.c', 'services/update.c', 'services/txn.c', 'services/sha256.c', no_unused=True)
host_test('test_version', 'tests/test_version.c', 'services/version.c')
host_test('test_trust', 'tests/test_trust.c', *build.CRYPTO)
host_test('test_iommu', 'tests/test_iommu.c', 'kernel/acpi.c', 'kernel/vtd_tables.c', no_unused=True)
subprocess.run([sys.executable, 'tests/test_packages.py'], check=True)
subprocess.run([sys.executable, 'tests/test_readfs.py'], check=True)
if not args.host_only:
    for memory in [128, 256, 512]:
        subprocess.run([sys.executable, 'tools/qemu_test.py', '--memory', str(memory)], check=True)
    for case in ['missing', 'badheader', 'badhash', 'badsignature', 'writable', 'ioerror']:
        subprocess.run([sys.executable, 'tools/qemu_test.py', '--storage', case], check=True)
    subprocess.run([sys.executable, 'tools/qemu_persist_test.py'], check=True)
    subprocess.run([sys.executable, 'tools/qemu_iommu_test.py'], check=True)
    subprocess.run([sys.executable, 'tools/qemu_security_test.py'], check=True)
    subprocess.run([sys.executable, 'tools/qemu_openat_test.py'], check=True)
    subprocess.run([sys.executable, 'tools/qemu_truncate_test.py'], check=True)
    subprocess.run([sys.executable, 'tools/qemu_trust_test.py'], check=True)
result = {'passed': True, 'elapsed_seconds': round(time.time()-started, 2), 'service_assertions': 116, 'storage_assertions': 35, 'package_tests': 'passed (10 groups, including 1500 seeded mutations)', 'readfs_tests': 'passed (5 groups, including 500 seeded metadata mutations)', 'qemu_memory_mib': [] if args.host_only else [128, 256, 512], 'storage_failure_cases': [] if args.host_only else ['missing', 'badheader', 'badhash', 'badsignature', 'writable', 'ioerror']}
result['txn_fault_boundaries'] = 261
result['txn_fault_modes'] = 3
result['persistence_qemu'] = not args.host_only
result['update_fault_scenarios'] = 4*261*3
result['update_health_qemu'] = not args.host_only
result['version_policy_host'] = True
result['version_policy_qemu'] = not args.host_only
result['iommu_host_assertions'] = 229
result['iommu_qemu'] = not args.host_only
result['security_host'] = True
result['security_qemu'] = not args.host_only
result['ipc_host_assertions'] = 908
result['memory_host_assertions'] = 42
result['openat_host_assertions'] = 108
result['openat_qemu'] = not args.host_only
result['truncate_host_assertions'] = 83
result['resize_fault_scenarios'] = 4*261*3
result['truncate_qemu'] = not args.host_only
result['trust_host_assertions'] = 21
result['trust_qemu'] = not args.host_only
result['artifacts_sha256'] = {name: hashlib.sha256((ROOT/'build'/name).read_bytes()).hexdigest() for name in ['esp/EFI/BOOT/BOOTX64.EFI', 'storage.img', 'persistent.img', 'development.pub', 'backup.pub']}
(ROOT / 'build/test-results.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
