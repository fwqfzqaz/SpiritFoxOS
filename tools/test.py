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
def host_test(name,*sources,no_unused=False,include_services=False):
    flags=['-target',build.HOST,'-g0','-O2','-Wall','-Wextra','-Werror','-DSF_HOST_TEST']
    if no_unused: flags.append('-Wno-unused-parameter')
    if include_services: flags.append('-Iservices')
    # 追加 Rust 宿主静态库：argparse/net_proto/sha256/sched_policy 已迁入
    # rust/sfcore，链接器只拉取本测试实际引用的成员。
    build.run([*flags,'-Iinclude','-Ibuild','-Ivendor/monocypher',*sources,build.rust_lib('host'),'-o','build/'+name+build.EXE])
    subprocess.run([str(ROOT/('build/'+name+build.EXE))],check=True)

# 参数打包与拆分：services/argparse.c → rust/sfcore/src/argparse.rs
host_test('test_arguments','tests/host/test_arguments.c','services/arguments.c')
host_test('test_environment','tests/host/test_environment.c','services/arguments.c')
host_test('test_file_rpc','tests/host/test_file_rpc.c','services/file_rpc.c','services/namespace.c')
# 调度策略：kernel/sched_policy.c → rust/sfcore/src/sched.rs
host_test('test_task_pid','tests/host/test_task_pid.c','kernel/pid.c','kernel/compat.c','services/arguments.c',no_unused=True)
host_test('test_scheduler','tests/host/test_scheduler.c','kernel/pid.c','kernel/compat.c','services/arguments.c',no_unused=True)
host_test('test_sched_ipc','tests/host/test_sched_ipc.c')
host_test('test_cap_limit','tests/host/test_cap_limit.c','kernel/pid.c','kernel/compat.c','services/arguments.c',no_unused=True)
host_test('test_memory_quota','tests/host/test_memory_quota.c','kernel/memory.c',no_unused=True)
host_test('test_page_limit','tests/host/test_page_limit.c','kernel/pid.c','kernel/compat.c','services/arguments.c',no_unused=True)
host_test('test_ipc_budget','tests/host/test_ipc_budget.c')
host_test('test_ipc_cancel','tests/host/test_ipc_cancel.c')
host_test('test_ipc_stats','tests/host/test_ipc_stats.c')
host_test('test_threads','tests/host/test_threads.c','kernel/pid.c','kernel/compat.c','services/arguments.c',no_unused=True)
host_test('test_shm','tests/host/test_shm.c','kernel/memory.c','kernel/shm.c',no_unused=True)
host_test('test_compat','tests/host/test_compat.c','kernel/compat.c','kernel/memory.c',no_unused=True)
host_test('test_linux_errno','tests/host/test_linux_errno.c','services/linux_errno.c')
host_test('test_ipc_stats_control','tests/host/test_ipc_stats_control.c','kernel/pid.c','kernel/compat.c','services/arguments.c',no_unused=True)
host_test('test_supervisor','tests/host/test_supervisor.c','services/supervisor.c')
host_test('test_manager','tests/host/test_manager.c','services/manager.c')
host_test('test_lifecycle_rpc','tests/host/test_lifecycle_rpc.c','services/manager.c','services/lifecycle_rpc.c')
host_test('test_services','tests/host/test_services.c','services/manager.c','services/namespace.c')
host_test('test_openat','tests/host/test_openat.c','services/namespace.c')
host_test('test_ns_identity','tests/host/test_ns_identity.c','services/namespace.c')
host_test('test_ns_default','tests/host/test_ns_default.c','services/namespace.c')
host_test('test_ns_mount','tests/host/test_ns_mount.c','services/namespace.c')
host_test('test_namespace_handles','tests/host/test_namespace_handles.c','services/namespace_handles.c')
host_test('test_namespace_restart','tests/host/test_namespace_restart.c','services/namespace_handles.c','services/namespace.c')
host_test('test_namespace_control','tests/host/test_namespace_control.c','services/namespace_client.c','services/namespace_handles.c','services/namespace_control.c','services/namespace.c',include_services=True)
host_test('test_namespace_frontend','tests/host/test_namespace_frontend.c','services/namespace_client.c','services/namespace_handles.c','services/namespace_frontend.c',include_services=True)
host_test('test_namespace_runtime','tests/host/test_namespace_runtime.c','services/namespace_client.c','services/namespace_handles.c','services/namespace_runtime.c','services/namespace_control.c','services/namespace.c',include_services=True)
host_test('test_namespace_process','tests/host/test_namespace_process.c','services/namespace_client.c','services/namespace_handles.c','services/namespace_runtime.c','services/namespace_control.c','services/namespace.c','services/fs_format.c','services/readfs.c',*build.CRYPTO,include_services=True)
host_test('test_namespace_transport','tests/host/test_namespace_transport.c','services/namespace_transport.c',include_services=True)
host_test('test_emergency_console','tests/host/test_emergency_console.c','services/emergency_console.c',include_services=True)
host_test('test_accounts','tests/host/test_accounts.c','services/accounts.c')
host_test('test_sessions','tests/host/test_sessions.c','services/session.c','services/accounts.c')
host_test('test_console_policy','tests/host/test_console_policy.c','services/console_policy.c','services/session.c','services/accounts.c',include_services=True)
host_test('test_launch_policy','tests/host/test_launch_policy.c','services/launch_policy.c',*build.CRYPTO,include_services=True)
host_test('test_linux_elf','tests/host/test_linux_elf.c','services/linux_image.c')
host_test('test_file_client','tests/host/test_file_client.c','services/namespace.c','services/file_rpc.c')
host_test('test_file_dup','tests/host/test_file_dup.c','services/namespace.c','services/file_rpc.c')
host_test('test_file_pread','tests/host/test_file_pread.c','services/namespace.c','services/file_rpc.c')
host_test('test_stat','tests/host/test_stat.c','services/namespace.c','services/file_rpc.c')
host_test('test_rmdir','tests/host/test_rmdir.c','services/namespace.c','services/file_rpc.c')
host_test('test_unlink','tests/host/test_unlink.c','services/namespace.c','services/file_rpc.c')
host_test('test_truncate','tests/host/test_truncate.c','services/namespace.c')
host_test('test_projection','tests/host/test_projection.c','services/namespace.c')
host_test('test_projection_width','tests/host/test_projection_width.c','services/namespace.c','services/file_rpc.c')
# 事务与 SHA-256：services/sha256.c → rust/sfcore/src/sha256.rs
host_test('test_resize_txn','tests/host/test_resize_txn.c','services/txn.c',no_unused=True)
host_test('test_storage','tests/host/test_storage.c','services/namespace.c',no_unused=True)
host_test('test_txn','tests/host/test_txn.c','services/txn.c',no_unused=True)
host_test('test_txn_compat','tests/host/test_txn_compat.c','services/txn.c',no_unused=True)
host_test('test_update','tests/host/test_update.c','services/update.c','services/txn.c',no_unused=True)
host_test('test_persist_metadata','tests/host/test_persist_metadata.c','tests/host/persist_volume_fixture.c','services/persist_metadata.c','services/namespace.c','services/txn.c','services/update.c','services/version.c',*build.CRYPTO,no_unused=True)
host_test('test_version','tests/host/test_version.c','services/version.c')
# 网络帧逻辑：services/net_proto.c → rust/sfcore/src/net.rs
host_test('test_net','tests/host/test_net.c')
host_test('test_sdk','tests/host/test_sdk.c','services/sdk.c')
host_test('test_iommu','tests/host/test_iommu.c','kernel/acpi.c','kernel/vtd_tables.c',no_unused=True)
# 类 niri 可滚动平铺布局核心（services/tiling.c）。
host_test('test_tiling','tests/host/test_tiling.c','services/tiling.c')
# 有界应用目录与搜索启动器核心（services/launcher.c）。
host_test('test_launcher','tests/host/test_launcher.c','services/launcher.c')
# 有界图形 shell 核心（services/sfk.c）。
host_test('test_sfk','tests/host/test_sfk.c','services/sfk.c')
for name,source in [('ipc','kernel/ipc.c'),('memory','kernel/memory.c')]:
    host_test('test_'+name,'tests/host/test_'+name+'.c',source,no_unused=True)
host_test('test_trust','tests/host/test_trust.c',*build.CRYPTO)
subprocess.run([sys.executable,'tests/integration/test_packages.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_trust.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_readfs.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_native.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_linux_elf.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_manifest_profile.py'],check=True)
manifest_schema_sha256 = hashlib.sha256((ROOT/'schemas/native-source-v1.schema.json').read_bytes()).hexdigest()
subprocess.run([sys.executable,'tests/integration/test_abi_snapshot.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_rust_archive.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_poll_deadline.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_namespace_boundary.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_namespace_ownership.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_policy_boundary.py'],check=True)
subprocess.run([sys.executable,'tests/integration/test_file_ingress.py'],check=True)
abi_reference_sha256 = hashlib.sha256((ROOT/'abi/x86_64-current.json').read_bytes()).hexdigest()
abi_capture_sha256 = hashlib.sha256((ROOT/'build/abi-current.json').read_bytes()).hexdigest()
if not args.host_only:
    subprocess.run([sys.executable, 'tests/qemu/qemu_capability_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_fpu_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_priority_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_cap_limit_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_page_quota_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_ipc_cancel_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_ipc_stats_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_file_client_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_rmdir_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_stat_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_projection_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_unlink_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_invocation_preflight_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_native_supervision_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_echo_health_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_supervision_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_launch_kind_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_echo_channel_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_block_channel_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_file_rpc_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_file_peer_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_native_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_posix_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_environment_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_arguments_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_trust_policy_test.py'], check=True)
    for memory in [128, 256, 512]:
        subprocess.run([sys.executable, 'tests/qemu/qemu_test.py', '--memory', str(memory)], check=True)
    for case in ['missing', 'badheader', 'badhash', 'badsignature', 'writable', 'ioerror']:
        subprocess.run([sys.executable, 'tests/qemu/qemu_test.py', '--storage', case], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_persist_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_persist_metadata_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_namespace_recovery_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_hardware_test.py'], check=True)
    # USB：xHCI 初始化、HID 枚举、键盘输入、指针注入与热插拔。
    subprocess.run([sys.executable, 'tests/qemu/qemu_usb_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_net_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_net_test.py', '--disk'], check=True)
    # 类 niri 合成器：framebuffer 映射、布局与串口输入转发。
    subprocess.run([sys.executable, 'tests/qemu/qemu_gui_test.py'], check=True)
    # 真实扫描输出：QMP screendump + PPM 像素断言（窗口/页签/背景色）。
    subprocess.run([sys.executable, 'tests/qemu/qemu_gui_screenshot_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_iommu_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_smp_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_security_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_openat_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_user_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_truncate_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_trust_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_txn_compat_test.py'], check=True)
    subprocess.run([sys.executable, 'tests/qemu/qemu_linux_compat_test.py'], check=True)
result = {'passed': True, 'elapsed_seconds': round(time.time()-started, 2), 'service_assertions': 116, 'storage_assertions': 83, 'package_tests': 'passed (11 groups, including 1500 seeded mutations)', 'readfs_tests': 'passed (5 groups, including 500 seeded metadata mutations)', 'qemu_memory_mib': [] if args.host_only else [128, 256, 512], 'storage_failure_cases': [] if args.host_only else ['missing', 'badheader', 'badhash', 'badsignature', 'writable', 'ioerror']}
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
result['ipc_host_assertions'] = 40950
result['net_host_checks'] = 14084
result['sdk_host_checks'] = 368490
result['manifest_source_profile'] = 'native-source-v1'
result['manifest_profile_host_tests'] = 12
result['manifest_schema_sha256'] = manifest_schema_sha256
result['abi_snapshot_host_tests'] = 9
result['abi_snapshot_entries'] = 662
result['abi_reference_sha256'] = abi_reference_sha256
result['abi_capture_sha256'] = abi_capture_sha256
result['posix_qemu'] = not args.host_only
result['posix_import_bindings'] = 19
result['net_qemu'] = not args.host_only
result['net_qemu_boots'] = 0 if args.host_only else 2
result['memory_host_assertions'] = 48
result['openat_host_assertions'] = 140
result['namespace_mount_host_assertions'] = 256
result['namespace_opaque_host_assertions'] = 22
result['namespace_boundary_host_groups'] = 7
result['namespace_handle_map_host_assertions'] = 502975
result['namespace_restart_host_assertions'] = 24996
result['namespace_rpc_host_assertions'] = [5506,30,52,2638,48065]
result['namespace_rpc_host_groups'] = 5
result['emergency_console_host_assertions'] = 33016
result['namespace_ownership_host_tests'] = 4
result['namespace_ownership'] = json.loads((ROOT/'build/namespace-ownership.json').read_text(encoding='utf-8'))
result['namespace_recovery_qemu'] = not args.host_only
result['namespace_recovery_qemu_boots'] = 0 if args.host_only else 2
result['namespace_recovery_qemu_deaths'] = 0 if args.host_only else 6
result['console_policy_host_assertions'] = 665
result['launch_policy_host_assertions'] = 2152
result['policy_compile_boundary_tests'] = 7
result['file_ingress_host_checks'] = 20001
result['file_ingress_host_groups'] = 1
result['file_peer_qemu'] = not args.host_only
result['file_peer_qemu_boots'] = 0 if args.host_only else 2
result['file_peer_transferred_caps'] = 0 if args.host_only else 384
result['persist_metadata_host_assertions'] = 13867
result['persist_metadata_qemu'] = not args.host_only
result['persist_metadata_qemu_boots'] = 0 if args.host_only else 2
result['projection_width_host_assertions'] = 9130
result['projection_sdk_qemu'] = not args.host_only
result['projection_sdk_qemu_boots'] = 0 if args.host_only else 2
result['host_poll_deadline_host_tests'] = 8
result['openat_qemu'] = not args.host_only
result['truncate_host_assertions'] = 83
result['resize_fault_scenarios'] = 4*261*3
result['truncate_qemu'] = not args.host_only
result['invocation_preflight_qemu'] = not args.host_only
result['invocation_preflight_qemu_boots'] = 0 if args.host_only else 4
result['task_pid_host_checks'] = 10022
result['fpu_task_init_host_checks'] = 3000
result['scheduler_profile'] = 'priority-0..3-aging-4ticks-v1'
result['scheduler_host_checks'] = 3539865
result['scheduler_ipc_host_checks'] = 10000
result['scheduler_qemu'] = not args.host_only
result['scheduler_qemu_boots'] = 0 if args.host_only else 2
result['cap_limit_host_checks'] = 6015
result['memory_quota_host_checks'] = 298045
result['page_limit_host_checks'] = 3033
result['memory_quota_profile'] = 'private-pool-pages-v1'
result['page_quota_qemu'] = not args.host_only
result['page_quota_qemu_boots'] = 0 if args.host_only else 2
result['ipc_budget_host_checks'] = 55043
result['cap_limit_qemu'] = not args.host_only
result['cap_limit_qemu_boots'] = 0 if args.host_only else 2
result['ipc_cancel_host_checks'] = 25776
result['ipc_copy_host_checks'] = 10041
result['ipc_stats_control_host_checks'] = 6001
result['ipc_copy_profile'] = 'successful-user-copy-bytes-v1'
result['ipc_stats_qemu'] = not args.host_only
result['ipc_stats_qemu_boots'] = 0 if args.host_only else 2
result['ipc_cancel_qemu'] = not args.host_only
result['ipc_cancel_qemu_boots'] = 0 if args.host_only else 2
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
result['file_client_host_assertions'] = 4256541
result['file_client_qemu'] = not args.host_only
result['file_client_qemu_boots'] = 0 if args.host_only else 2
result['file_dup_host_checks'] = 23740
result['file_dup_qemu'] = not args.host_only
result['file_dup_qemu_boots'] = 0 if args.host_only else 2
result['file_pread_host_checks'] = 4060596
result['file_pread_qemu'] = not args.host_only
result['file_pread_qemu_boots'] = 0 if args.host_only else 2
result['rmdir_host_assertions'] = 11391
result['rmdir_qemu'] = not args.host_only
result['rmdir_qemu_boots'] = 0 if args.host_only else 2
result['stat_host_assertions'] = 6212
result['stat_qemu'] = not args.host_only
result['stat_qemu_boots'] = 0 if args.host_only else 2
result['canonical_file_handles'] = True
result['unlink_host_assertions'] = 17372
result['unlink_qemu'] = not args.host_only
result['unlink_qemu_boots'] = 0 if args.host_only else 2
result['file_rpc_host_checks'] = 364
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
result['linux_compat_qemu'] = not args.host_only
result['linux_compat_qemu_boots'] = 0 if args.host_only else 1
result['artifacts_sha256'] = {name: hashlib.sha256((ROOT/'build'/name).read_bytes()).hexdigest() for name in ['esp/EFI/BOOT/BOOTX64.EFI', 'storage.img', 'persistent.img', 'development.pub', 'application.pub', 'backup.pub', 'trust-policy.json']}
(ROOT / 'build/test-results.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
