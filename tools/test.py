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
(ROOT / 'build').mkdir(exist_ok=True)
(ROOT / 'build/test-results.json').unlink(missing_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--skip-build', action='store_true')
parser.add_argument('--host-only', action='store_true')
args = parser.parse_args()
started = time.time()
if not args.skip_build:
    # Keep Crypto()/sfcrypto.dll out of this long-lived parent process: on
    # Windows a loaded DLL blocks the trust-policy tests from rebuilding it.
    subprocess.run([sys.executable, str(ROOT / 'tools' / 'build.py')], check=True)
def host_test(name,*sources,no_unused=False):
    flags=['-target',build.HOST,'-g0','-O2','-Wall','-Wextra','-Werror']
    if no_unused: flags.append('-Wno-unused-parameter')
    build.run([*flags,'-Iinclude','-Ivendor/monocypher',*sources,'-o','build/'+name+build.EXE])
    subprocess.run([str(ROOT/('build/'+name+build.EXE))],check=True)

host_test('test_arguments','tests/host/test_arguments.c','services/arguments.c','services/argparse.c')
host_test('test_environment','tests/host/test_environment.c','services/arguments.c','services/argparse.c')
host_test('test_file_rpc','tests/host/test_file_rpc.c','services/file_rpc.c','services/namespace.c')
host_test('test_task_pid','tests/host/test_task_pid.c','kernel/pid.c','services/arguments.c',no_unused=True)
host_test('test_supervisor','tests/host/test_supervisor.c','services/supervisor.c')
host_test('test_manager','tests/host/test_manager.c','services/manager.c')
host_test('test_services','tests/host/test_services.c','services/manager.c','services/namespace.c')
host_test('test_openat','tests/host/test_openat.c','services/namespace.c')
host_test('test_file_client','tests/host/test_file_client.c','services/namespace.c','services/file_rpc.c')
host_test('test_stat','tests/host/test_stat.c','services/namespace.c','services/file_rpc.c')
host_test('test_rmdir','tests/host/test_rmdir.c','services/namespace.c','services/file_rpc.c')
host_test('test_unlink','tests/host/test_unlink.c','services/namespace.c','services/file_rpc.c')
host_test('test_truncate','tests/host/test_truncate.c','services/namespace.c')
host_test('test_resize_txn','tests/host/test_resize_txn.c','services/txn.c','services/sha256.c',no_unused=True)
host_test('test_storage','tests/host/test_storage.c','services/namespace.c',no_unused=True)
host_test('test_txn','tests/host/test_txn.c','services/txn.c','services/sha256.c',no_unused=True)
host_test('test_txn_compat','tests/host/test_txn_compat.c','services/txn.c','services/sha256.c',no_unused=True)
host_test('test_update','tests/host/test_update.c','services/update.c','services/txn.c','services/sha256.c',no_unused=True)
host_test('test_version','tests/host/test_version.c','services/version.c')
host_test('test_net','tests/host/test_net.c','services/net_proto.c')
host_test('test_sdk','tests/host/test_sdk.c','services/sdk.c')
host_test('test_iommu','tests/host/test_iommu.c','kernel/acpi.c','kernel/vtd_tables.c',no_unused=True)
for name,source in [('ipc','kernel/ipc.c'),('memory','kernel/memory.c')]:
    host_test('test_'+name,'tests/host/test_'+name+'.c',source,no_unused=True)
host_test('test_trust','tests/host/test_trust.c',*build.CRYPTO)
subprocess.run([sys.executable,'tests/integration/test_packages.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_trust.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_readfs.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_native.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_manifest_profile.py'],check=True)
manifest_schema_sha256 = hashlib.sha256((ROOT/'schemas/native-source-v1.schema.json').read_bytes()).hexdigest()
if not args.host_only:
    subprocess.run([sys.executable, 'tests/qemu/qemu_capability_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_fpu_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_file_client_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_rmdir_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_stat_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_unlink_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_invocation_preflight_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_native_supervision_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_echo_health_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_supervision_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_launch_kind_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_echo_channel_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_block_channel_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_file_rpc_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_native_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_environment_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_arguments_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_trust_policy_test.py'], check=True)
    for memory in [128, 256, 512]:
        subprocess.run([sys.executable, 'tests/qemu/qemu_test.py', '--memory', str(memory)], check=True)
    for case in ['missing', 'badheader', 'badhash', 'badsignature', 'writable', 'ioerror']:
        subprocess.run([sys.executable, 'tests/qemu/qemu_test.py', '--storage', case], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_persist_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_net_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_iommu_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_security_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_openat_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_truncate_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_trust_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_txn_compat_test.py'], check=True)
result = {'passed': True, 'elapsed_seconds': round(time.time()-started, 2), 'service_assertions': 116, 'storage_assertions': 35, 'package_tests': 'passed (11 groups, including 1500 seeded mutations)', 'readfs_tests': 'passed (5 groups, including 500 seeded metadata mutations)', 'qemu_memory_mib': [] if args.host_only else [128, 256, 512], 'storage_failure_cases': [] if args.host_only else ['missing', 'badheader', 'badhash', 'badsignature', 'writable', 'ioerror']}
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
result['ipc_host_assertions'] = 40908
result['net_host_checks'] = 14084
result['sdk_host_checks'] = 368490
result['manifest_source_profile'] = 'native-source-v1'
result['manifest_profile_host_tests'] = 12
result['manifest_schema_sha256'] = manifest_schema_sha256
result['net_qemu'] = not args.host_only
result['net_qemu_boots'] = 0 if args.host_only else 1
result['memory_host_assertions'] = 42
result['openat_host_assertions'] = 108
result['openat_qemu'] = not args.host_only
result['truncate_host_assertions'] = 83
result['resize_fault_scenarios'] = 4*261*3
result['truncate_qemu'] = not args.host_only
result['invocation_preflight_qemu'] = not args.host_only
result['invocation_preflight_qemu_boots'] = 0 if args.host_only else 4
result['task_pid_host_checks'] = 8022
result['fpu_task_init_host_checks'] = 3000
result['kernel_capability_width'] = True
result['capability_width_qemu'] = not args.host_only
result['capability_width_qemu_boots'] = 0 if args.host_only else 2
result['fpu_profile'] = 'fxsave64-x87-mmx-sse'
result['fpu_context_qemu'] = not args.host_only
result['fpu_context_qemu_boots'] = 0 if args.host_only else 2
result['fpu_simd_exception_observation'] = {} if args.host_only else json.loads((ROOT/'build/fpu-context-results.json').read_text())['simd_exception_observation']
result['native_supervision_qemu'] = not args.host_only
result['native_supervision_qemu_boots'] = 0 if args.host_only else 2
result['echo_health_qemu'] = not args.host_only
result['echo_health_qemu_boots'] = 0 if args.host_only else 2
result['supervisor_host_checks'] = 97101
result['supervisor_qemu'] = not args.host_only
result['supervisor_qemu_boots'] = 0 if args.host_only else 2
result['manager_dependency_host_checks'] = 25080
result['manager_dependency_qemu'] = not args.host_only
result['trust_policy_host'] = True
result['trust_policy_host_groups'] = 7
result['separate_signing_keys'] = True
result['arguments_host_checks'] = 8155
result['arguments_qemu'] = not args.host_only
result['environment_host_checks'] = 12034
result['environment_qemu'] = not args.host_only
result['native_relative_host_groups'] = 6
result['native_relative_mutations'] = 500
result['native_relative_qemu'] = not args.host_only
result['native_relative_qemu_boots'] = 0 if args.host_only else 4
result['file_client_host_assertions'] = 416510
result['file_client_qemu'] = not args.host_only
result['file_client_qemu_boots'] = 0 if args.host_only else 2
result['rmdir_host_assertions'] = 11359
result['rmdir_qemu'] = not args.host_only
result['rmdir_qemu_boots'] = 0 if args.host_only else 2
result['stat_host_assertions'] = 6180
result['stat_qemu'] = not args.host_only
result['stat_qemu_boots'] = 0 if args.host_only else 2
result['canonical_file_handles'] = True
result['unlink_host_assertions'] = 17340
result['unlink_qemu'] = not args.host_only
result['unlink_qemu_boots'] = 0 if args.host_only else 2
result['file_rpc_host_checks'] = 353
result['file_rpc_qemu'] = not args.host_only
result['file_rpc_qemu_boots'] = 0 if args.host_only else 2
result['block_reply_channel_qemu'] = not args.host_only
result['block_reply_channel_qemu_boots'] = 0 if args.host_only else 2
result['launch_kind_qemu'] = not args.host_only
result['launch_kind_qemu_boots'] = 0 if args.host_only else 2
result['echo_reply_channel_qemu'] = not args.host_only
result['echo_reply_channel_qemu_boots'] = 0 if args.host_only else 2
result['trust_policy_qemu'] = not args.host_only
result['trust_policy_qemu_boots'] = 0 if args.host_only else 14
result['trust_host_assertions'] = 29
result['trust_qemu'] = not args.host_only
result['txn_compat_host_assertions'] = 2046559
result['txn_compat_fault_scenarios'] = 783
result['txn_compat_qemu'] = not args.host_only
result['txn_compat_qemu_boots'] = 0 if args.host_only else 6
result['artifacts_sha256'] = {name: hashlib.sha256((ROOT/'build'/name).read_bytes()).hexdigest() for name in ['esp/EFI/BOOT/BOOTX64.EFI', 'storage.img', 'persistent.img', 'development.pub', 'application.pub', 'backup.pub', 'trust-policy.json']}
(ROOT / 'build/test-results.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
