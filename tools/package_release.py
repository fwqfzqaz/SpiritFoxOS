"""Bundle the exact locally tested boot artifacts and public documentation."""
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT/'build'
result = json.loads((BUILD/'test-results.json').read_text(encoding='utf-8'))
cases = {'missing','badheader','badhash','badsignature','writable','ioerror'}
if result.get('passed') is not True or result.get('version_policy_host') is not True or result.get('version_policy_qemu') is not True or result.get('update_health_qemu') is not True or result.get('update_fault_scenarios') != 3132 or result.get('iommu_qemu') is not True or result.get('persistence_qemu') is not True or result.get('qemu_memory_mib') != [128,256,512] or set(result.get('storage_failure_cases',[])) != cases:
    raise SystemExit('Run the full test suite before packaging')
if result.get('security_host') is not True or result.get('security_qemu') is not True:
    raise SystemExit('Run the kernel boundary and storage failure security regressions before packaging')
if result.get('openat_host_assertions') != 108 or result.get('openat_qemu') is not True:
    raise SystemExit('Run the directory capability host and VM regressions before packaging')
if result.get('truncate_host_assertions') != 83 or result.get('resize_fault_scenarios') != 3132 or result.get('truncate_qemu') is not True:
    raise SystemExit('Run truncate/zero-extension fault and VM regressions before packaging')
if result.get('trust_policy_host') is not True or result.get('trust_policy_host_groups') != 7 or result.get('separate_signing_keys') is not True or result.get('trust_policy_qemu') is not True or result.get('trust_policy_qemu_boots') != 14:
    raise SystemExit('Run the key trust/purpose/revocation and rotation VM regressions before packaging')
if result.get('arguments_host_checks') != 8155 or result.get('arguments_qemu') is not True:
    raise SystemExit('Run native argv bounds and read-only/preflight VM regressions before packaging')
if result.get('environment_host_checks') != 12034 or result.get('environment_qemu') is not True:
    raise SystemExit('Run explicit environment bounds and isolation/preflight VM regressions before packaging')
if result.get('native_relative_host_groups') != 6 or result.get('native_relative_mutations') != 500 or result.get('native_relative_qemu') is not True or result.get('native_relative_qemu_boots') != 4:
    raise SystemExit('Run relative native validation/private-image/isolation/reboot regressions before packaging')
if result.get('file_rpc_host_checks') != 353 or result.get('file_rpc_qemu') is not True or result.get('file_rpc_qemu_boots') != 2:
    raise SystemExit('Run file RPC length bounds and isolation regressions before packaging')
if result.get('block_reply_channel_qemu') is not True or result.get('block_reply_channel_qemu_boots') != 2:
    raise SystemExit('Run concurrent block reply channel and restart regressions before packaging')
if result.get('echo_reply_channel_qemu') is not True or result.get('echo_reply_channel_qemu_boots') != 2:
    raise SystemExit('Run per-instance echo reply channel and generation cleanup regressions before packaging')
if result.get('launch_kind_qemu') is not True or result.get('launch_kind_qemu_boots') != 2:
    raise SystemExit('Run authenticated module/application launch intent regressions before packaging')
if result.get('manager_dependency_host_checks') != 25080 or result.get('manager_dependency_qemu') is not True:
    raise SystemExit('Run dependency generation and transactional lease regressions before packaging')
if result.get('supervisor_host_checks') != 97101 or result.get('supervisor_qemu') is not True or result.get('supervisor_qemu_boots') != 2:
    raise SystemExit('Run bounded echo death supervision and restart regressions before packaging')
if result.get('echo_health_qemu') is not True or result.get('echo_health_qemu_boots') != 2:
    raise SystemExit('Run authenticated echo health timeout and protocol failure regressions before packaging')
if result.get('native_supervision_qemu') is not True or result.get('native_supervision_qemu_boots') != 2:
    raise SystemExit('Run native capacity, lease drain and bounded death/health recovery before packaging')
if result.get('task_pid_host_checks') != 8022:
    raise SystemExit('Run actual task_new PID exhaustion and publication regressions before packaging')
if result.get('invocation_preflight_qemu') is not True or result.get('invocation_preflight_qemu_boots') != 4:
    raise SystemExit('Run installed invocation preflight and corrupt candidate rollback regressions before packaging')
if result.get('unlink_host_assertions') != 17340 or result.get('unlink_qemu') is not True or result.get('unlink_qemu_boots') != 2:
    raise SystemExit('Run RAM unlink rights, busy-reference, slot reuse and VM regressions before packaging')
if result.get('stat_host_assertions') != 6180 or result.get('canonical_file_handles') is not True or result.get('stat_qemu') is not True or result.get('stat_qemu_boots') != 2:
    raise SystemExit('Run canonical file handle and fixed metadata regressions before packaging')
if result.get('ipc_host_assertions') != 40908 or result.get('kernel_capability_width') is not True or result.get('capability_width_qemu') is not True or result.get('capability_width_qemu_boots') != 2:
    raise SystemExit('Run canonical kernel capability host and VM regressions before packaging')
if result.get('fpu_task_init_host_checks') != 3000 or result.get('fpu_profile') != 'fxsave64-x87-mmx-sse' or result.get('fpu_context_qemu') is not True or result.get('fpu_context_qemu_boots') != 2:
    raise SystemExit('Run fresh/interleaved FP context and isolated exception VM regressions before packaging')
if set(result.get('fpu_simd_exception_observation',{})) != {'legacy','modern'} or any(v not in ('exception-delivered','tcg-status-only') for v in result['fpu_simd_exception_observation'].values()):
    raise SystemExit('Record actual SIMD exception delivery limitations before packaging')
if result.get('rmdir_host_assertions') != 11359 or result.get('rmdir_qemu') is not True or result.get('rmdir_qemu_boots') != 2:
    raise SystemExit('Run empty directory rights, busy/nonempty, reuse and VM regressions before packaging')
if result.get('file_client_host_assertions') != 416510 or result.get('file_client_qemu') is not True or result.get('file_client_qemu_boots') != 2:
    raise SystemExit('Run file client codec/peer/deadline/counter host and production transport VM regressions before packaging')
if result.get('txn_compat_host_assertions') != 2046559 or result.get('txn_compat_fault_scenarios') != 783 or result.get('txn_compat_qemu') is not True or result.get('txn_compat_qemu_boots') != 6:
    raise SystemExit('Run legacy transaction geometry, first-upgrade faults and migration/restart VM regressions before packaging')
if result.get('trust_host_assertions') != 29 or result.get('trust_qemu') is not True:
    raise SystemExit('Run persistent revocation host and VM regressions before packaging')
for name in ['esp/EFI/BOOT/BOOTX64.EFI','storage.img','persistent.img','development.pub','application.pub','backup.pub','trust-policy.json']:
    actual = hashlib.sha256((BUILD/name).read_bytes()).hexdigest()
    if result.get('artifacts_sha256',{}).get(name) != actual:
        raise SystemExit('Artifact changed since validation: '+name)
items = [(BUILD/'esp/EFI/BOOT/BOOTX64.EFI','EFI/BOOT/BOOTX64.EFI'),(BUILD/'storage.img','storage.img'),(BUILD/'persistent.img','persistent.img'),(BUILD/'development.pub','development.pub'),(BUILD/'test-results.json','test-results.json'),(ROOT/'README.md','README.md')]
items.append((BUILD/'trust-policy.json','trust-policy.json'))
items.append((BUILD/'application.pub','application.pub'))
items.append((BUILD/'backup.pub','backup.pub'))
items += [(p,p.relative_to(ROOT).as_posix()) for p in sorted((ROOT/'docs').rglob('*.md'))]
manifest = {'files_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest() for path,name in items}, 'validation': 'tools/test.py: host + legacy storage + VT-d/modern DMA isolation + persistence/update-health/version-policy/fault/restart + kernel-boundary/storage-failure security + directory-capability openat + truncate/zero-extension fault/recovery + trust-policy/rotation/immutable-and-durable-runtime-revocation + native-argv/explicit-environment/read-only/preflight + relative-native/private-image/reboot + file-RPC/local-bounds/IPC-rejection + block-reply/private-channel/restart + echo-reply/per-instance/capacity-recovery + authenticated-launch-kind/suffix-independent + dependency-generation/transactional-leases + echo-supervision/bounded-restart + echo-health/hang/protocol-failure + native-capacity/lease-drain/supervision VMs + actual-task-PID exhaustion host checks + installed-invocation/preflight/corrupt-rollback + RAM-unlink/directory/rights/slot-reuse + canonical-file-handle/stat VMs + canonical-kernel-capabilities + eager-FP-context/fresh/interleaved/timer/fault VMs + empty-RAM-directory/rmdir/rights/nonempty/reuse VMs + public-file-client/codec/private-transport/read-only VMs'}
archive = BUILD/'SpiritFoxOS-M6-Updates.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for path,name in items:
        z.write(path,name)
    z.writestr('manifest.json',json.dumps(manifest,indent=2)+'\n')
print(str(archive),archive.stat().st_size,'bytes')
