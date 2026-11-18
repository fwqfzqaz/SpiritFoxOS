"""Bundle the exact locally tested boot artifacts and public documentation."""
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT/'build'
result = json.loads((BUILD/'test-results.json').read_text(encoding='utf-8'))
if result.get('linux_blob_assembly') != {'passed':True,'payloads':3,'windows_and_posix_paths':True,
                                      'space_in_path':True,'actual_assembler_bytes':True,'cache_refresh':True}:
    raise SystemExit('Run production Linux blob assembler path/byte/cache regression before packaging')
cases = {'missing','badheader','badhash','badsignature','writable','ioerror'}
if result.get('passed') is not True or result.get('version_policy_host') is not True or result.get('version_policy_qemu') is not True or result.get('update_health_qemu') is not True or result.get('update_fault_scenarios') != 3132 or result.get('iommu_qemu') is not True or result.get('persistence_qemu') is not True or result.get('qemu_memory_mib') != [128,256,512] or set(result.get('storage_failure_cases',[])) != cases:
    raise SystemExit('Run the full test suite before packaging')
if result.get('security_host') is not True or result.get('security_qemu') is not True:
    raise SystemExit('Run the kernel boundary and storage failure security regressions before packaging')
if result.get('scheduler_profile') != 'priority-0..3-aging-4ticks-v1' or result.get('scheduler_host_checks') != 3539865 or result.get('scheduler_ipc_host_checks') != 10000 or result.get('scheduler_qemu') is not True or result.get('scheduler_qemu_boots') != 2:
    raise SystemExit('Run priority selection/configuration, fresh IPC wakeups and timer/aging VM regressions before packaging')
if result.get('cap_limit_host_checks') != 6015 or result.get('ipc_budget_host_checks') != 55043 or result.get('cap_limit_qemu') is not True or result.get('cap_limit_qemu_boots') != 2:
    raise SystemExit('Run live capability limits, atomic quota admission and staged authority/recovery VM regressions before packaging')
if result.get('ipc_cancel_host_checks') != 25776 or result.get('ipc_cancel_qemu') is not True or result.get('ipc_cancel_qemu_boots') != 2:
    raise SystemExit('Run authenticated pending send withdrawal, FIFO/width/delivery bounds and production queue VM regressions before packaging')
if result.get('openat_host_assertions') != 140 or result.get('openat_qemu') is not True:
    raise SystemExit('Run the directory capability host and VM regressions before packaging')
if result.get('namespace_mount_host_assertions') != 256:
    raise SystemExit('Run atomic namespace mount publication and trusted handle handoff regressions before packaging')
if result.get('namespace_opaque_host_assertions') != 22 or result.get('namespace_boundary_host_groups') != 7:
    raise SystemExit('Run opaque namespace client and real compile-boundary regressions before packaging')
if result.get('namespace_handle_map_host_assertions') != 502975 or result.get('namespace_restart_host_assertions') != 24996:
    raise SystemExit('Run namespace client-generation and actual namespace restart regressions before packaging')
if result.get('namespace_rpc_host_assertions') != [5506,30,52,2638,48065] or result.get('namespace_rpc_host_groups') != 5 or not result.get('namespace_recovery_qemu') or result.get('namespace_recovery_qemu_boots') != 2 or result.get('namespace_recovery_qemu_deaths') != 6:
    raise SystemExit('Run trusted namespace transport/control, real Fs incarnation loss, dirty drain, stale app/Root and TRYING preservation regressions before packaging')
ownership=result.get('namespace_ownership',{})
if result.get('console_policy_host_assertions') != 689 or result.get('launch_policy_host_assertions') != 2152 or result.get('policy_compile_boundary_tests') != 7:
    raise SystemExit('Run manager console/launch admission and real compile-boundary regressions before packaging')
if result.get('file_ingress_host_checks') != 20001 or result.get('file_ingress_host_groups') != 1 or result.get('file_peer_qemu') is not True or result.get('file_peer_qemu_boots') != 2 or result.get('file_peer_transferred_caps') != 384:
    raise SystemExit('Run actual manager ingress cap cleanup and live rejected-peer budget regressions before packaging')
if result.get('emergency_console_host_assertions') != 33016:
    raise SystemExit('Run bounded emergency console SDK/ownership/generation regressions before packaging')
if result.get('namespace_ownership_host_tests') != 4 or ownership.get('passed') is not True or ownership.get('root_has_namespace_tables') is not False or ownership.get('root_has_transaction_state') is not False:
    raise SystemExit('Verify actual linked ELF namespace/transaction state ownership before packaging')
if result.get('persist_metadata_host_assertions') != 16821 or result.get('persist_metadata_qemu') is not True or result.get('persist_metadata_qemu_boots') != 2:
    raise SystemExit('Run read-only TRYING metadata/reprojection and real persistence IPC regressions before packaging')
if result.get('persist_read_reply_bounds') is not True or result.get('persist_read_qemu_boots') != 2 or result.get('persist_read_qemu_bytes') != 8192:
    raise SystemExit('Run bounded persistence READ replies and full-width sequential guest read/EOF regressions before packaging')
if result.get('persist_write_reply_host') != {'passed':True,'evidence':'E1-host','checks':693161,'malformed_cases':120,'known_errors':60,'unknown_cases':33,'transport_cases':8}:
    raise SystemExit('Run exact persistence WRITE ACK, admitted-chunk progress and retained unknown DIRTY host regressions before packaging')
if result.get('persist_admission_fence_host') != {'passed':True,'evidence':'E1-host-only','checks':3318,'frontend_checks':243,'raw_control_refusals':43,'namespace_peer_operations':34,'non_destructive':True,'provider_lifecycle':False}:
    raise SystemExit('Run non-destructive Root admission fence/token/cancel host regressions before packaging')
fence_source_paths = ('tests/qemu/qemu_persist_fence_test.py', 'tests/fixtures/persist_write_reply_fault.c', 'tests/qemu/qemu_persist_test.py', 'tests/qemu/qemu_persist_write_reply_test.py', 'tests/qemu/trust_sync_fixture.py', 'services/init.c', 'services/persist_client.c', 'services/storage.c', 'services/namespace_frontend.c', 'services/persist_snapshot.c', 'services/persist_snapshot.h', 'services/trust_fingerprint.c', 'services/trust_fingerprint.h', 'services/persistence.c', 'services/persist_terminal.c', 'services/persist_terminal.h', 'services/persist.c', 'include/sf.h', 'include/persist_server.h', 'services/package.c', 'services/trust.c', 'include/package.h', 'services/persist_verify.c', 'services/persist_verify.h', 'services/persist_stage.c', 'services/persist_stage.h', 'services/storage_reply_owner.c', 'services/storage_reply_owner.h', 'services/storage_reply.h')
fence_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in fence_source_paths}
fence_guest=result.get('persist_admission_fence_qemu',{})
fence_healthy=fence_guest.get('healthy_cases',[])
fence_unknown=fence_guest.get('unknown_cases',[])
fence_recoveries=fence_guest.get('recoveries',[])
fence_markers=['PERSIST FENCE DRAIN PASSED','PERSIST SNAPSHOT CAPTURE PASSED','PERSIST FENCE OWNER GUARDS PASSED','PERSIST FENCE RAM AND BUNDLE PASSED','PERSIST FENCE TOKENS PASSED','PERSIST FENCE HEALTHY PASSED']
if (fence_guest.get('passed') is not True or fence_guest.get('evidence')!='E2-actual-root-fs-persist' or
    fence_guest.get('source_sha256')!=fence_source_sha256 or
    len(fence_guest.get('source_head',''))!=40 or any(c not in '0123456789abcdef' for c in fence_guest.get('source_head','')) or
    fence_guest.get('production_efi_sha256')!=hashlib.sha256((BUILD/'esp/EFI/BOOT/BOOTX64.EFI').read_bytes()).hexdigest() or
    fence_guest.get('guest_boots')!=4 or fence_guest.get('user_denials')!=4 or
    len(fence_healthy)!=4 or {(c.get('transport'),c.get('state')) for c in fence_healthy}!={(t,s) for t in ('legacy','modern') for s in ('STABLE','PENDING')} or
    not all(c.get('passed') is True and c.get('all_disk_bytes_preserved') is True and c.get('markers')==fence_markers for c in fence_healthy) or
    len(fence_unknown)!=2 or {c.get('transport') for c in fence_unknown}!={'legacy','modern'} or
    not all(c.get('passed') is True and c.get('generation_delta')==1 and c.get('diagnostic_runs')==2 and all(c.get(k) is True for k in ('retained_real_dirty','no_post_fault_provider_rpc','all_committed_bytes_preserved')) for c in fence_unknown) or
    len(fence_recoveries)!=2 or {c.get('transport') for c in fence_recoveries}!={'legacy','modern'} or
    not all(c.get('passed') is True and c.get('new_healthy_fence') is True and c.get('pending_bytes_preserved') is True for c in fence_recoveries) or
    not all(fence_guest.get(k) is True for k in ('production_artifacts_restored','base_disk_unchanged')) or
    not all(fence_guest.get(k) is False for k in ('provider_lifecycle','physical_hardware','power_loss'))):
    raise SystemExit('Run actual Root/Fs admission fence, setter/raw/type guards, retained unknown DIRTY and fresh production recovery before packaging')

# STOP evidence is distinct from non-destructive fencing and from restart.
terminal_source_paths = ('tests/qemu/qemu_persist_terminal_test.py', 'tests/fixtures/persist_terminal_fault.c', 'tests/qemu/trust_sync_fixture.py', 'services/persist_terminal.c', 'services/persist.c', 'services/persist_client.c', 'services/init.c', 'services/console_policy.c', 'include/persistence.h', 'include/sf.h', 'include/libsf.h', 'services/persist_metadata.c', 'services/persistence.c', 'services/storage.c', 'kernel/core.c', 'kernel/ipc.c', 'tests/qemu/qemu_persist_test.py', 'tests/qemu/qemu_persist_write_reply_test.py', 'services/persist_candidate.c', 'services/persist_candidate.h', 'services/persist_idle.c', 'services/persist_idle.h', 'services/persist_snapshot.c', 'services/persist_snapshot.h', 'services/trust_fingerprint.c', 'services/trust_fingerprint.h', 'services/persist_terminal.h', 'include/persist_server.h', 'services/package.c', 'services/trust.c', 'include/package.h', 'services/persist_verify.c', 'services/persist_verify.h', 'services/persist_stage.c', 'services/persist_stage.h', 'services/storage_reply_owner.c', 'services/storage_reply_owner.h', 'services/storage_reply.h')
terminal_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in terminal_source_paths}
terminal_artifact_paths = ('build/bundle.bin', 'build/payload.S', 'build/esp/EFI/BOOT/BOOTX64.EFI', 'build/kernel.elf', 'build/storage.img', 'build/persistent.img', 'build/trust-policy.json')
terminal_artifact_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in terminal_artifact_paths}
terminal_guest = result.get('persist_terminal_qemu',{})
terminal_cases = terminal_guest.get('cases',[])
def terminal_case_valid(c):
    mode=c.get('mode');status=c.get('status',{})
    expected={'healthy':(3,True,True,True),'delayed':(3,True,True,True),
              'unknown':(4,False,True,False),'busy':(5,True,False,False)}
    if (mode not in expected or c.get('passed') is not True or c.get('disk_bytes_preserved') is not True or
        c.get('busy_is_injected_cache_result') is not (mode=='busy') or
        type(status.get('pid')) is not int or not 1<status['pid']<=2147483647 or
        type(status.get('phase')) is not int or type(status.get('observations')) is not int or
        not all(type(status.get(k)) is bool for k in ('ack_authenticated','death_confirmed','retired'))):
        return False
    if status.get('candidate_inspected') is not (mode in ('healthy','delayed')):return False
    if status.get('candidate_idle') is not (mode in ('healthy','delayed')):return False
    if status.get('snapshot_captured') is not True:return False
    if tuple(status.get(k) for k in ('phase','ack_authenticated','death_confirmed','retired'))!=expected[mode]:
        return False
    return status['observations']==0 if mode=='busy' else status['observations']>=(2 if mode=='delayed' else 1)
if (result.get('persist_terminal_host')!={'passed':True,'evidence':'E1-host-only','server_checks':550,'owner_checks':642533,'owner_cases':187} or
    result.get('persist_candidate_host')!={'passed':True,'evidence':'E1-host-real-signatures','owner_checks':216690,'owner_cases':67,'image_checks':688,'signed_fixtures':8} or
    terminal_guest.get('passed') is not True or terminal_guest.get('evidence')!='E2-real-kernel-IPC-and-provider-exit' or
    terminal_guest.get('source_sha256')!=terminal_source_sha256 or terminal_guest.get('production_sha256')!=terminal_artifact_sha256 or
    len(terminal_guest.get('source_head',''))!=40 or any(c not in '0123456789abcdef' for c in terminal_guest.get('source_head','')) or
    terminal_guest.get('guest_boots')!=8 or terminal_guest.get('user_denials')!=8 or len(terminal_cases)!=8 or
    {(c.get('mode'),c.get('transport')) for c in terminal_cases}!={(m,t) for m in ('healthy','unknown','delayed','busy') for t in ('legacy','modern')} or
    not all(terminal_case_valid(c) for c in terminal_cases) or
    not all(terminal_guest.get(k) is True for k in ('production_artifacts_restored','base_disk_unchanged')) or
    not all(terminal_guest.get(k) is False for k in ('candidate_activation','generic_restart','physical_hardware'))):
    raise SystemExit('Run actual owned Persist terminal acceptance, exact normal death, unknown ACK refusal, delayed exit and endpoint retirement before packaging')

idle_source_paths = terminal_source_paths + ('tests/qemu/qemu_persist_idle_test.py','tests/fixtures/persist_idle_fault.c')
idle_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in idle_source_paths}
idle_guest = result.get('persist_idle_qemu',{})
idle_cases = idle_guest.get('cases',[])
def idle_case_valid(c):
    mode=c.get('mode');fields=('probe','ordinary_denied','discard','death','retired')
    expected={'healthy':(6,True,True,True,True,True),'idle-delayed':(6,True,True,True,True,True),
              'idle-badprobe':(7,False,False,False,False,False),'idle-dropdiscard':(7,True,True,False,False,False)}
    if (mode not in expected or c.get('passed') is not True or c.get('disk_bytes_preserved') is not True or
        c.get('old_stop_snapshot_preserved') is not True or type(c.get('pid')) is not int or not 1<c['pid']<=2147483647 or
        type(c.get('phase')) is not int or type(c.get('observations')) is not int or
        not all(type(c.get(k)) is bool for k in fields)):
        return False
    if (c['phase'],*(c[k] for k in fields))!=expected[mode]:return False
    return c['observations']>=(2 if mode=='idle-delayed' else 1) if c['phase']==6 else c['observations']==0
if (result.get('persist_idle_host')!={'passed':True,'evidence':'E1-host-mocked-kernel','parser_checks':4173,'owner_checks':437812} or
    idle_guest.get('passed') is not True or idle_guest.get('evidence')!='E2-real-private-candidate-IPC-and-exit' or
    idle_guest.get('source_sha256')!=idle_source_sha256 or idle_guest.get('production_sha256')!=terminal_artifact_sha256 or
    len(idle_guest.get('source_head',''))!=40 or any(c not in '0123456789abcdef' for c in idle_guest.get('source_head','')) or
    idle_guest.get('guest_boots')!=8 or idle_guest.get('user_denials')!=8 or len(idle_cases)!=8 or
    {(c.get('mode'),c.get('transport')) for c in idle_cases}!={(m,t) for m in ('healthy','idle-badprobe','idle-dropdiscard','idle-delayed') for t in ('legacy','modern')} or
    not all(idle_case_valid(c) for c in idle_cases) or
    not all(idle_guest.get(k) is True for k in ('production_artifacts_restored','base_disk_unchanged')) or
    not all(idle_guest.get(k) is False for k in ('candidate_activation','driver_grants','mount_requested','physical_hardware'))):
    raise SystemExit('Run isolated Persist candidate identity, ordinary denial, normal discard/exit and conservative unknown reply guest evidence before packaging')

snapshot_source_paths = terminal_source_paths + ('tests/qemu/qemu_persist_snapshot_test.py','tests/fixtures/persist_snapshot_fault.c')
snapshot_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in snapshot_source_paths}
snapshot_guest = result.get('persist_snapshot_qemu',{})
snapshot_cases = snapshot_guest.get('cases',[])
if (result.get('persist_snapshot_host')!={'passed':True,'evidence':'E1-host-independent-canonical-vectors','parser_checks':8197,'volume_checks':87,'owner_checks':170613,'trust_checks':95} or
    snapshot_guest.get('passed') is not True or snapshot_guest.get('evidence')!='E2-real-snapshot-reply-quarantine' or
    snapshot_guest.get('source_sha256')!=snapshot_source_sha256 or snapshot_guest.get('production_sha256')!=terminal_artifact_sha256 or
    len(snapshot_guest.get('source_head',''))!=40 or any(c not in '0123456789abcdef' for c in snapshot_guest.get('source_head','')) or
    snapshot_guest.get('guest_boots')!=4 or snapshot_guest.get('user_denials')!=4 or len(snapshot_cases)!=4 or
    {(c.get('mode'),c.get('transport')) for c in snapshot_cases}!={(m,t) for m in ('snapshot-badreply','snapshot-dropped') for t in ('legacy','modern')} or
    not all(all(c.get(k) is True for k in ('passed','capture_failed_closed','old_provider_alive','old_stop_unchanged','no_unknown_write','disk_bytes_preserved')) for c in snapshot_cases) or
    not all(snapshot_guest.get(k) is True for k in ('production_artifacts_restored','base_disk_unchanged')) or
    not all(snapshot_guest.get(k) is False for k in ('candidate_activation','provider_lifecycle','mount_requested','physical_hardware'))):
    raise SystemExit('Run actual snapshot trust-reply corruption/loss with quarantined old live owner and unchanged disk/trust/dirty state before packaging')

verify_source_paths = terminal_source_paths + ('tests/qemu/qemu_persist_verify_test.py','tests/fixtures/persist_verify_fault.c')
verify_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in verify_source_paths}
verify_guest = result.get('persist_verify_qemu',{})
verify_cases = verify_guest.get('cases',[])
def verify_case_valid(c):
    mode=c.get('mode');success=mode=='verify';reads=c.get('reads')
    if (mode not in ('verify','verify-readerror','verify-badreply','verify-dropped') or
        c.get('passed') is not True or type(reads)is not int or type(c.get('phase'))is not int or
        c.get('verified') is not success or c.get('normal_discard') is not success or
        c.get('phase')!=(6 if success else 7) or
        c.get('root_read_error_injected') is not (mode=='verify-readerror') or
        not all(c.get(k)is True for k in ('old_capture_preserved','old_stop_preserved','disk_bytes_preserved'))):return False
    return reads==1 if mode=='verify-readerror' else 259<=reads<=515
if (result.get('persist_verify_host')!={'passed':True,'evidence':'E1-real-txn-and-mocked-IPC','volume_checks':5907,'parser_checks':4441242,'parser_cases':66,'owner_checks':751802,'owner_cases':58} or
    verify_guest.get('passed') is not True or verify_guest.get('evidence')!='E2-real-isolated-readonly-volume-and-attestation' or
    verify_guest.get('source_sha256')!=verify_source_sha256 or verify_guest.get('production_sha256')!=terminal_artifact_sha256 or
    len(verify_guest.get('source_head',''))!=40 or any(c not in '0123456789abcdef' for c in verify_guest.get('source_head','')) or
    verify_guest.get('guest_boots')!=10 or verify_guest.get('user_denials')!=10 or len(verify_cases)!=10 or
    {(c.get('mode'),c.get('state'),c.get('transport')) for c in verify_cases}!=
        {(m,s,t)for m,s in (('verify','STABLE'),('verify','PENDING'),('verify-readerror','STABLE'),('verify-badreply','STABLE'),('verify-dropped','STABLE'))for t in ('legacy','modern')} or
    not all(verify_case_valid(c)for c in verify_cases) or
    not all(verify_guest.get(k)is True for k in ('production_artifacts_restored','base_disk_unchanged')) or
    not all(verify_guest.get(k)is False for k in ('candidate_activation','driver_grants','mount_requested','physical_hardware'))):
    raise SystemExit('Run actual isolated readonly transaction reconstruction, exact old cache/trust attestation, conservative failed replies and normal private discard before packaging')

hold_source_paths = terminal_source_paths + ('tests/qemu/qemu_persist_hold_test.py','tests/fixtures/persist_idle_fault.c')
hold_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in hold_source_paths}
hold_guest = result.get('persist_hold_qemu',{})
hold_cases = hold_guest.get('cases',[])
def hold_case_valid(c):
    mode=c.get('mode');success=mode=='healthy'
    return (mode in ('healthy','idle-dropdiscard') and c.get('passed') is True and
            type(c.get('held_pid'))is int and 1<c['held_pid']<=2147483647 and
            type(c.get('verified_reads'))is int and 259<=c['verified_reads']<=515 and
            type(c.get('phase'))is int and c['phase']==(6 if success else 7) and
            c.get('discard_outcome')==('discarded'if success else'discard-ack-dropped') and
            all(c.get(k)is True for k in ('old_hold_deadline_expired','alice_denied_hold_and_discard',
                'ram_access_retained','persist_read_blocked','disk_bytes_preserved','old_capture_stop_preserved')))
if (result.get('persist_hold_host')!={'passed':True,'evidence':'E1-held-owner-mocked-kernel-transport','checks':247421,'cases':14} or
    result['persist_hold_host'].get('passed') is not True or
    any(type(result['persist_hold_host'].get(k))is not int for k in ('checks','cases')) or
    hold_guest.get('passed') is not True or hold_guest.get('evidence')!='E2-real-held-candidate-and-delayed-explicit-discard' or
    hold_guest.get('source_sha256')!=hold_source_sha256 or hold_guest.get('production_sha256')!=terminal_artifact_sha256 or
    len(hold_guest.get('source_head',''))!=40 or any(c not in '0123456789abcdef'for c in hold_guest.get('source_head','')) or
    type(hold_guest.get('guest_boots'))is not int or hold_guest['guest_boots']!=6 or
    type(hold_guest.get('user_denials'))is not int or hold_guest['user_denials']!=12 or len(hold_cases)!=6 or
    {(c.get('mode'),c.get('state'),c.get('transport'))for c in hold_cases}!=
        {(m,s,t)for m,s in (('healthy','STABLE'),('healthy','PENDING'),('idle-dropdiscard','STABLE'))for t in ('legacy','modern')} or
    not all(hold_case_valid(c)for c in hold_cases) or
    not all(hold_guest.get(k)is True for k in ('production_artifacts_restored','base_disk_unchanged')) or
    not all(hold_guest.get(k)is False for k in ('candidate_activation','driver_grants','mount_requested','physical_hardware'))):
    raise SystemExit('Run actual live prepared ownership across Root commands, refreshed explicit discard and retained unknown ACK guest evidence before packaging')

stage_source_paths = terminal_source_paths + ('tests/qemu/qemu_persist_stage_test.py','tests/fixtures/persist_stage_fault.c')
stage_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in stage_source_paths}
stage_guest = result.get('persist_stage_qemu',{})
stage_host = result.get('persist_stage_host',{})
stage_cases = stage_guest.get('cases',[]) if isinstance(stage_guest,dict) else []
def stage_case_valid(c):
    mode=c.get('mode');state=c.get('state');transport=c.get('transport')
    if mode=='healthy':
        valid_state=state in ('STABLE','PENDING');phase=6;outcome='normal-discard-all-four-root-handles-retired'
    elif mode in ('bad-first-ack','lost-second-ack'):
        valid_state=state=='STABLE';phase=7;outcome='quarantined-branches-retired'
    else:return False
    fields=('alice_denied','duplicate_denied','driver_originals_usable',
            'root_cap_budget_preserved','old_capture_stop_trust_preserved','disk_bytes_preserved')
    return (valid_state and transport in ('legacy','modern') and c.get('passed') is True and
            type(c.get('phase')) is int and c['phase']==phase and c.get('outcome')==outcome and
            all(type(c.get(k)) is bool and c[k] is True for k in fields))
if (not isinstance(stage_guest,dict) or not isinstance(stage_host,dict) or
    stage_host.get('passed') is not True or stage_host.get('evidence')!='E1-real-stage-codec-and-mocked-owner-IPC' or
    stage_host!={'passed':True,'evidence':'E1-real-stage-codec-and-mocked-owner-IPC',
                 'provider_checks':22098,'provider_cases':4003,
                 'owner_checks':1220783,'owner_cases':50} or
    any(type(stage_host.get(k)) is not int for k in ('provider_checks','provider_cases','owner_checks','owner_cases')) or
    stage_guest.get('passed') is not True or stage_guest.get('evidence')!='E2-real-provisional-driver-cap-ownership-and-retirement' or
    stage_guest.get('source_sha256')!=stage_source_sha256 or stage_guest.get('production_sha256')!=terminal_artifact_sha256 or
    type(stage_guest.get('source_head')) is not str or len(stage_guest['source_head'])!=40 or
    any(c not in '0123456789abcdef' for c in stage_guest['source_head']) or
    type(stage_guest.get('guest_boots')) is not int or stage_guest['guest_boots']!=8 or
    not isinstance(stage_cases,list) or len(stage_cases)!=8 or
    {(c.get('mode'),c.get('state'),c.get('transport')) for c in stage_cases}!=
        {(m,s,t) for m,s in (('healthy','STABLE'),('healthy','PENDING'),
                              ('bad-first-ack','STABLE'),('lost-second-ack','STABLE'))
         for t in ('legacy','modern')} or
    not all(stage_case_valid(c) for c in stage_cases) or
    stage_guest.get('production_artifacts_restored') is not True or stage_guest.get('base_disk_unchanged') is not True or
    any(stage_guest.get(k) is not False for k in ('ordinary_backend_bound','candidate_activation','physical_hardware'))):
    raise SystemExit('Run exact healthy provisional-cap stage/discard and bad/lost ACK cleanup evidence before packaging')

reply_owner_source_paths = terminal_source_paths + ('tests/qemu/qemu_storage_reply_owner_test.py',)
reply_owner_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in reply_owner_source_paths}
reply_owner_host = result.get('storage_reply_owner_host',{})
reply_owner_guest = result.get('storage_reply_owner_qemu',{})
reply_owner_cases = reply_owner_guest.get('cases',[]) if isinstance(reply_owner_guest,dict) else []
def reply_owner_case_valid(c):
    if not isinstance(c,dict):return False
    mode=c.get('mode');state=c.get('state');transport=c.get('transport')
    if type(mode)is not str or type(state)is not str or type(transport)is not str:return False
    if mode not in ('known-release','explicit-quarantine') or transport not in ('legacy','modern'):return False
    known=mode=='known-release'
    if state not in (('STABLE','PENDING') if known else ('STABLE',)):return False
    serials=c.get('lease_serials')
    if type(serials)is not list or not all(type(n)is int for n in serials) or serials!=([1,2] if known else [1]):return False
    return (all(c.get(k)is True for k in ('passed','alice_denied','root_transport_gated','driver_alive',
               'ram_echo_retained','disk_bytes_preserved','old_stop_capture_trust_preserved')) and
            all(c.get(k)is known for k in ('known_root_reads_resumed','normal_discard')) and
            all(c.get(k)is (not known) for k in ('explicit_quarantine','candidate_branches_retired')))
if (not isinstance(reply_owner_host,dict) or reply_owner_host.get('passed') is not True or
    reply_owner_host!={'passed':True,'evidence':'E1-pure-state-and-mocked-Root-integration',
                      'primitive_checks':24243,'primitive_cases':1003,
                      'controller_checks':546258,'controller_cases':19} or
    any(type(reply_owner_host.get(k))is not int for k in ('primitive_checks','primitive_cases','controller_checks','controller_cases')) or
    not isinstance(reply_owner_guest,dict) or reply_owner_guest.get('passed')is not True or
    reply_owner_guest.get('evidence')!='E2-real-Root-reply-gate-and-explicit-quarantine' or
    reply_owner_guest.get('source_sha256')!=reply_owner_source_sha256 or
    reply_owner_guest.get('production_sha256')!=terminal_artifact_sha256 or
    type(reply_owner_guest.get('source_head'))is not str or len(reply_owner_guest['source_head'])!=40 or
    any(c not in '0123456789abcdef'for c in reply_owner_guest['source_head']) or
    type(reply_owner_guest.get('guest_boots'))is not int or reply_owner_guest['guest_boots']!=6 or
    type(reply_owner_cases)is not list or len(reply_owner_cases)!=6 or
    not all(reply_owner_case_valid(c)for c in reply_owner_cases) or
    {(c['mode'],c['state'],c['transport'])for c in reply_owner_cases}!=
        {(m,s,t)for m,s in (('known-release','STABLE'),('known-release','PENDING'),('explicit-quarantine','STABLE'))for t in ('legacy','modern')} or
    any(reply_owner_guest.get(k)is not True for k in ('production_artifacts_preserved','base_disk_unchanged')) or
    any(reply_owner_guest.get(k)is not False for k in ('candidate_driver_rpc','real_driver_timeout','ordinary_backend_bound','candidate_activation','physical_hardware'))):
    raise SystemExit('Run current Root reply-stream ownership, known release and explicit-quarantine evidence before packaging')


revoke_source_paths = ('kernel/ipc.c','kernel/core.c','kernel/kernel.h','include/sf.h','include/libsf.h','services/init.c','services/console_policy.c','services/revocation_probe.c','tools/build.py','tests/qemu/qemu_ipc_revocation_test.py','tests/qemu/qemu_persist_test.py','tests/qemu/qemu_persist_write_reply_test.py','tests/host/test_ipc.c','tests/host/test_ipc_revocation.c')
revoke_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in revoke_source_paths}
revoke_guest = result.get('ipc_revocation_qemu',{})
revoke_host = result.get('ipc_revocation_host',{})
revoke_cases = revoke_guest.get('cases',[])
def revoke_case_valid(c):
    return (c.get('passed') is True and type(c.get('cpus'))is int and c['cpus'] in (1,2) and
            type(c.get('app_checks'))is int and c['app_checks']==204 and
            all(c.get(k)is True for k in ('alice_denied','root_caps_preserved','ordinary_service_retained','disk_bytes_preserved')))
if (revoke_host!={'passed':True,'evidence':'E1-real-kernel-mocked-user-copy','checks':9151} or
    revoke_host.get('passed') is not True or type(revoke_host.get('checks'))is not int or
    revoke_guest.get('passed') is not True or revoke_guest.get('evidence')!='E2-real-queued-and-closed-parent-cap-revocation' or
    revoke_guest.get('source_sha256')!=revoke_source_sha256 or revoke_guest.get('production_sha256')!=terminal_artifact_sha256 or
    len(revoke_guest.get('source_head',''))!=40 or any(c not in '0123456789abcdef'for c in revoke_guest.get('source_head','')) or
    type(revoke_guest.get('guest_boots'))is not int or revoke_guest['guest_boots']!=4 or len(revoke_cases)!=4 or
    {(c.get('transport'),c.get('cpus'))for c in revoke_cases}!={(t,n)for t in ('legacy','modern')for n in (1,2)} or
    not all(revoke_case_valid(c)for c in revoke_cases) or
    not all(revoke_guest.get(k)is True for k in ('production_artifacts_preserved','base_disk_unchanged')) or
    not all(revoke_guest.get(k)is False for k in ('driver_authority_handoff','physical_hardware'))):
    raise SystemExit('Run real queued/closed-parent capability revocation with preserved sibling/owner references and 1/2-CPU legacy/modern guest evidence before packaging')

branch_source_paths = ('kernel/ipc.c', 'kernel/core.c', 'kernel/kernel.h', 'include/sf.h', 'include/libsf.h', 'services/init.c', 'services/console_policy.c', 'services/branch_probe.c', 'tools/build.py', 'tests/qemu/qemu_ipc_branch_test.py', 'tests/qemu/qemu_persist_test.py', 'tests/qemu/qemu_persist_write_reply_test.py', 'tests/host/test_ipc.c', 'tests/host/test_ipc_branch.c', 'tools/abi_snapshot.c', 'abi/x86_64-current.json')
branch_source_sha256 = {p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in branch_source_paths}
branch_guest = result.get('ipc_branch_qemu',{})
branch_host = result.get('ipc_branch_host',{})
branch_cases = branch_guest.get('cases',[])
def branch_case_valid(c):
    return (c.get('passed') is True and type(c.get('cpus'))is int and c['cpus'] in (1,2) and
            type(c.get('app_checks'))is int and c['app_checks']==16 and
            all(c.get(k)is True for k in ('alice_denied','root_caps_preserved','ordinary_service_retained','disk_bytes_preserved','owner_parent_sibling_preserved','installed_and_queued_revoked','legacy_endpoint_revoke_preserved')))
if (branch_host!={'passed':True,'evidence':'E1-real-kernel-branch-revoke-mocked-user-copy','checks':9101} or
    branch_host.get('passed') is not True or type(branch_host.get('checks'))is not int or
    type(branch_guest.get('abi_entries'))is not int or branch_guest['abi_entries']!=680 or
    branch_guest.get('passed') is not True or branch_guest.get('evidence')!='E2-real-owner-branch-revocation' or
    branch_guest.get('source_sha256')!=branch_source_sha256 or branch_guest.get('production_sha256')!=terminal_artifact_sha256 or
    len(branch_guest.get('source_head',''))!=40 or any(c not in '0123456789abcdef'for c in branch_guest.get('source_head','')) or
    type(branch_guest.get('guest_boots'))is not int or branch_guest['guest_boots']!=4 or len(branch_cases)!=4 or
    {(c.get('transport'),c.get('cpus'))for c in branch_cases}!={(t,n)for t in ('legacy','modern')for n in (1,2)} or
    not all(branch_case_valid(c)for c in branch_cases) or
    not all(branch_guest.get(k)is True for k in ('production_artifacts_preserved','base_disk_unchanged')) or
    not all(branch_guest.get(k)is False for k in ('driver_authority_handoff','physical_hardware'))):
    raise SystemExit('Run explicit owner branch revocation with preserved parent/sibling references and legacy endpoint-revoke compatibility and 1/2-CPU legacy/modern guest evidence before packaging')

write_reply=result.get('persist_write_reply_qemu',{})
write_cases=write_reply.get('cases',[])
if (write_reply.get('passed') is not True or write_reply.get('evidence')!='E2-signed-provider' or
    write_reply.get('guest_boots')!=8 or write_reply.get('malformed_acks_rejected')!=4 or
    write_reply.get('fresh_boot_recovery')!=4 or write_reply.get('query_latch_checks')!=8 or
    write_reply.get('unknown_dirty_unmount_refusals')!=8 or write_reply.get('denied_new_authority')!=12 or
    write_reply.get('blocked_mutation_commands')!=12 or len(write_cases)!=4 or
    {(c.get('transport'),c.get('length')) for c in write_cases}!={(t,n) for t in ('legacy','modern') for n in ('zero','excess')} or
    not all(c.get('passed') is True and c.get('generation_delta')==1 and c.get('recovery_generation_unchanged') is True and
            0<c.get('requested',0)<=4096 and c.get('reply_length')==(0 if c.get('length')=='zero' else c['requested']+1) for c in write_cases) or
    not all(write_reply.get(k) is True for k in ('single_commit_preserved','no_post_fault_provider_rpc','production_artifacts_restored','base_disk_unchanged'))):
    raise SystemExit('Run signed Persist zero/excess WRITE ACK faults, unknown DIRTY/query gates and fresh production recovery on both transports before packaging')
chunks=result.get('persist_chunk_qemu',{})
if chunks.get('passed') is not True or chunks.get('guest_boots')!=2 or chunks.get('empty_file_rejections')!=32 or chunks.get('driver_reattach_cycles')!=4 or chunks.get('base_disk_unchanged') is not True or not 8192<chunks.get('stage_bytes',0)<=16384 or chunks.get('package_chunks')!=(chunks.get('stage_bytes',0)+4095)//4096:
    raise SystemExit('Run actual chunked STAGE/preview/export, empty-path stale-stage refusal and driver reattach regressions before packaging')
revoke_failure=result.get('revoke_failure_qemu',{})
if revoke_failure.get('passed') is not True or revoke_failure.get('guest_boots')!=6 or revoke_failure.get('negative_policy_reconciled')!=2 or revoke_failure.get('explicit_unknown_latched')!=2 or revoke_failure.get('harmless_semantic_rejections')!=4 or revoke_failure.get('unknown_dirty_unmount_refusals')!=2 or revoke_failure.get('fresh_boot_recovery')!=2 or not all(revoke_failure.get(k) is True for k in ('remaining_signer_available','committed_disk_preserved','production_artifacts_restored','base_disk_unchanged')):
    raise SystemExit('Complete revoke reconciliation/explicit unknown-completion evidence required')
block_host=result.get('block_reply_host',{})
block_guest=result.get('block_reply_qemu',{})
if block_host.get('passed') is not True or block_host.get('checks')!=6096 or block_host.get('clients')!=[{'owner':'persist','checks':1984,'owner_checks':0},{'owner':'storage','checks':4112,'owner_checks':2128}] or block_guest.get('passed') is not True or block_guest.get('guest_boots')!=4 or block_guest.get('malformed_reads_rejected')!=4 or block_guest.get('explicit_recoveries')!=4 or not all(block_guest.get(k) is True for k in ('root_degraded_to_bundle','persist_required_trust_fail_closed','committed_notes_unchanged','production_artifacts_restored','base_disk_unchanged')):
    raise SystemExit('Complete authenticated block-client host/guest recovery evidence required')
trust_sync=result.get('trust_sync_qemu',{})
if result.get('trust_sync_host_checks')!=1003 or result.get('trust_admission_host_tests')!=1 or result.get('trust_admission_host_checks')!=92 or trust_sync.get('passed') is not True or trust_sync.get('guest_boots')!=2 or trust_sync.get('malformed_snapshots')!=4 or trust_sync.get('denied_new_authority')!=8 or trust_sync.get('valid_resyncs')!=4 or not all(trust_sync.get(k) is True for k in ('pending_and_known_commit_preserved','ordinary_standby_app','production_artifacts_restored','base_disk_unchanged')):
    raise SystemExit('Run atomic runtime trust/actual Root admission and signed-provider fault/resync regressions before packaging')
if result.get('projection_width_host_assertions') != 9130 or result.get('projection_sdk_qemu') is not True or result.get('projection_sdk_qemu_boots') != 2:
    raise SystemExit('Run projection metadata/full-width seek host and both transport SDK regressions before packaging')
if result.get('host_poll_deadline_host_tests') != 8:
    raise SystemExit('Run paused-host polling budgets and silent-guest rejection regressions before packaging')
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
if result.get('file_rpc_host_checks') != 364 or result.get('file_rpc_qemu') is not True or result.get('file_rpc_qemu_boots') != 2:
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
if result.get('task_pid_host_checks') != 10022:
    raise SystemExit('Run actual task_new PID exhaustion and publication regressions before packaging')
if result.get('invocation_preflight_qemu') is not True or result.get('invocation_preflight_qemu_boots') != 4:
    raise SystemExit('Run installed invocation preflight and corrupt candidate rollback regressions before packaging')
if result.get('unlink_host_assertions') != 17372 or result.get('unlink_qemu') is not True or result.get('unlink_qemu_boots') != 2:
    raise SystemExit('Run RAM unlink rights, busy-reference, slot reuse and VM regressions before packaging')
if result.get('stat_host_assertions') != 6212 or result.get('canonical_file_handles') is not True or result.get('stat_qemu') is not True or result.get('stat_qemu_boots') != 2:
    raise SystemExit('Run canonical file handle and fixed metadata regressions before packaging')
if result.get('ipc_host_assertions') != 40950 or result.get('kernel_capability_width') is not True or result.get('capability_width_qemu') is not True or result.get('capability_width_qemu_boots') != 2:
    raise SystemExit('Run canonical kernel capability host and VM regressions before packaging')
if result.get('fpu_task_init_host_checks') != 3000 or result.get('fpu_profile') != 'fxsave64-x87-mmx-sse' or result.get('fpu_context_qemu') is not True or result.get('fpu_context_qemu_boots') != 2:
    raise SystemExit('Run fresh/interleaved FP context and isolated exception VM regressions before packaging')
if set(result.get('fpu_simd_exception_observation',{})) != {'legacy','modern'} or any(v not in ('exception-delivered','tcg-status-only') for v in result['fpu_simd_exception_observation'].values()):
    raise SystemExit('Record actual SIMD exception delivery limitations before packaging')
if result.get('rmdir_host_assertions') != 11391 or result.get('rmdir_qemu') is not True or result.get('rmdir_qemu_boots') != 2:
    raise SystemExit('Run empty directory rights, busy/nonempty, reuse and VM regressions before packaging')
if result.get('file_client_host_assertions') != 4256541 or result.get('file_client_qemu') is not True or result.get('file_client_qemu_boots') != 2:
    raise SystemExit('Run file client codec/peer/deadline/counter host and production transport VM regressions before packaging')
if result.get('file_dup_host_checks') != 23740 or result.get('file_dup_qemu') is not True or result.get('file_dup_qemu_boots') != 2:
    raise SystemExit('Run duplicate handle rights/cursor/lifetime host and production transport VM regressions before packaging')
if result.get('file_pread_host_checks') != 4060596 or result.get('file_pread_qemu') is not True or result.get('file_pread_qemu_boots') != 2:
    raise SystemExit('Run positional read rights/cursor/backend host and production transport VM regressions before packaging')
if result.get('txn_compat_host_assertions') != 2046559 or result.get('txn_compat_fault_scenarios') != 783 or result.get('txn_compat_qemu') is not True or result.get('txn_compat_qemu_boots') != 6:
    raise SystemExit('Run legacy transaction geometry, first-upgrade faults and migration/restart VM regressions before packaging')
if result.get('trust_host_assertions') != 29 or result.get('trust_qemu') is not True:
    raise SystemExit('Run persistent revocation host and VM regressions before packaging')
schema_path = ROOT/'schemas/native-source-v1.schema.json'
if result.get('manifest_source_profile') != 'native-source-v1' or result.get('manifest_profile_host_tests') != 12:
    raise SystemExit('Run source manifest/profile and real authoring CLI regressions before packaging')
if result.get('manifest_schema_sha256') != hashlib.sha256(schema_path.read_bytes()).hexdigest():
    raise SystemExit('Manifest schema changed since source validation')
abi_reference = ROOT/'abi/x86_64-current.json'
abi_capture = BUILD/'abi-current.json'
quiescence=result.get('persist_quiescence_qemu',{})
if (result.get('persist_quiescence_client_checks')!=386 or result.get('persist_quiescence_volume_checks')!=22713 or
    result.get('persist_quiescence_guard')!={'passed':True,'checks':104} or not quiescence.get('passed') or
    quiescence.get('guest_boots')!=2 or quiescence.get('live_trial_busy')!=2 or
    quiescence.get('open_description_busy')!=2 or quiescence.get('dirty_ledger_busy')!=2 or
    quiescence.get('user_denials')!=4 or not quiescence.get('stable_pending_byte_preserved') or
    not quiescence.get('base_disk_unchanged') or not quiescence.get('advisory_only') or
    quiescence.get('service_stop_restart_tested') is not False):
    raise SystemExit('Run authenticated Persist quiescence client/volume/ledger and actual trial/busy QEMU regressions before packaging')
if result.get('abi_snapshot_host_tests') != 9 or result.get('abi_snapshot_entries') != 681:
    raise SystemExit('Run compiled ABI capture, offline comparison and header-drift regressions before packaging')
if result.get('ipc_copy_host_checks') != 10041 or result.get('ipc_stats_control_host_checks') != 6001 or result.get('ipc_copy_profile') != 'successful-user-copy-bytes-v1' or not result.get('ipc_stats_qemu') or result.get('ipc_stats_qemu_boots') != 2:
    raise SystemExit('Run actual IPC copy event/control oracles and both production stats VM transports before packaging')
if result.get('memory_quota_host_checks') != 298045 or result.get('page_limit_host_checks') != 3033 or result.get('memory_quota_profile') != 'private-pool-pages-v1' or not result.get('page_quota_qemu') or result.get('page_quota_qemu_boots') != 2:
    raise SystemExit('Run actual pool-page accounting, limited-spawn/control and both production quota VM transports before packaging')
if result.get('abi_reference_sha256') != hashlib.sha256(abi_reference.read_bytes()).hexdigest() or result.get('abi_capture_sha256') != hashlib.sha256(abi_capture.read_bytes()).hexdigest():
    raise SystemExit('ABI reference or capture changed since validation')
for name in ['esp/EFI/BOOT/BOOTX64.EFI','storage.img','persistent.img','development.pub','application.pub','backup.pub','trust-policy.json']:
    actual = hashlib.sha256((BUILD/name).read_bytes()).hexdigest()
    if result.get('artifacts_sha256',{}).get(name) != actual:
        raise SystemExit('Artifact changed since validation: '+name)
items = [(BUILD/'esp/EFI/BOOT/BOOTX64.EFI','EFI/BOOT/BOOTX64.EFI'),(BUILD/'storage.img','storage.img'),(BUILD/'persistent.img','persistent.img'),(BUILD/'development.pub','development.pub'),(BUILD/'test-results.json','test-results.json'),(ROOT/'README.md','README.md')]
items.append((BUILD/'trust-policy.json','trust-policy.json'))
items.append((BUILD/'application.pub','application.pub'))
items.append((BUILD/'backup.pub','backup.pub'))
items.append((schema_path,'schemas/native-source-v1.schema.json'))
items.append((abi_reference,'abi/x86_64-current.json'))
items.append((abi_capture,'abi/x86_64-captured.json'))
items.append((ROOT/'tools/abi.py','tools/abi.py'))
for name in ['tools/package/sfpack.py','tools/package/manifest_profile.py','tools/trust_policy.py']:
    items.append((ROOT/name,name))
items += [(p,p.relative_to(ROOT).as_posix()) for p in sorted((ROOT/'docs').rglob('*.md'))]
manifest = {'files_sha256': {name: hashlib.sha256(path.read_bytes()).hexdigest() for path,name in items}, 'validation': 'tools/test.py: host + legacy storage + VT-d/modern DMA isolation + persistence/update-health/version-policy/fault/restart + kernel-boundary/storage-failure security + directory-capability openat + truncate/zero-extension fault/recovery + trust-policy/rotation/immutable-and-durable-runtime-revocation + native-argv/explicit-environment/read-only/preflight + relative-native/private-image/reboot + file-RPC/local-bounds/IPC-rejection + block-reply/private-channel/restart + echo-reply/per-instance/capacity-recovery + authenticated-launch-kind/suffix-independent + dependency-generation/transactional-leases + echo-supervision/bounded-restart + echo-health/hang/protocol-failure + native-capacity/lease-drain/supervision VMs + actual-task-PID exhaustion host checks + installed-invocation/preflight/corrupt-rollback + RAM-unlink/directory/rights/slot-reuse + canonical-file-handle/stat VMs + canonical-kernel-capabilities + eager-FP-context/fresh/interleaved/timer/fault VMs + empty-RAM-directory/rmdir/rights/nonempty/reuse VMs + public-file-client/codec/private-transport/read-only/duplicate-shared-description VMs'}
archive = BUILD/'SpiritFoxOS-M6-Updates.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for path,name in items:
        z.write(path,name)
    z.writestr('manifest.json',json.dumps(manifest,indent=2)+'\n')
print(str(archive),archive.stat().st_size,'bytes')
