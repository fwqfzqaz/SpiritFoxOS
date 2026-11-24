"""Reject stale fence evidence using the actual producer and release gate."""
import ast
import copy
import hashlib
from pathlib import Path
import tempfile

ROOT=Path(__file__).resolve().parents[2]
(ROOT/'build').mkdir(exist_ok=True)
paths = ('tests/qemu/qemu_persist_fence_test.py', 'tests/fixtures/persist_write_reply_fault.c', 'tests/qemu/qemu_persist_test.py', 'tests/qemu/qemu_persist_write_reply_test.py', 'tests/qemu/trust_sync_fixture.py', 'services/init.c', 'services/persist_client.c', 'services/storage.c', 'services/namespace_frontend.c', 'services/persist_snapshot.c', 'services/persist_snapshot.h', 'services/trust_fingerprint.c', 'services/trust_fingerprint.h', 'services/persistence.c', 'services/persist_terminal.c', 'services/persist_terminal.h', 'services/persist.c', 'include/sf.h', 'include/persist_server.h', 'services/package.c', 'services/trust.c', 'include/package.h', 'services/persist_verify.c', 'services/persist_verify.h', 'services/persist_stage.c', 'services/persist_stage.h', 'services/storage_reply_owner.c', 'services/storage_reply_owner.h', 'services/storage_reply.h', 'services/persist_driver_info.c', 'services/persist_driver_info.h')
runner='tests/qemu/qemu_persist_fence_test.py';gate='tools/package_release.py'

# Exercise the production pure hash function and complete real release IF.
# The QEMU module/package script are never imported or executed as programs.
rt=ast.parse((ROOT/runner).read_text(encoding='utf-8'));gt=ast.parse((ROOT/gate).read_text(encoding='utf-8'))
hash_nodes=[n for n in rt.body if (isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='FENCE_EVIDENCE_SOURCES' for t in n.targets)) or (isinstance(n,ast.FunctionDef) and n.name=='evidence_hashes')]
def module(nodes):return compile(ast.fix_missing_locations(ast.Module(body=nodes,type_ignores=[])),'actual-proposed-evidence-AST','exec')
release_if=next(n for n in gt.body if isinstance(n,ast.If) and any(isinstance(x,ast.Constant) and x.value=='source_sha256' for x in ast.walk(n.test)))
assigns={n.targets[0].id:n for n in gt.body if isinstance(n,ast.Assign) and len(n.targets)==1 and isinstance(n.targets[0],ast.Name)}
gate_code=module([assigns['fence_source_paths'],assigns['fence_source_sha256'],release_if])
run=next(n for n in rt.body if isinstance(n,ast.FunctionDef) and n.name=='run')
after=next(n for n in run.body if isinstance(n,ast.Assert) and any(isinstance(x,ast.Name) and x.id=='evidence_hashes' for x in ast.walk(n.test)))
assert_code=module([after]);checks=0
with tempfile.TemporaryDirectory(prefix='fence-binding-',dir=ROOT/'build') as temporary:
    root=Path(temporary)
    for i,p in enumerate(paths):
        f=root/p;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('source '+str(i),encoding='utf-8')
    efi=root/'build/esp/EFI/BOOT/BOOTX64.EFI';efi.parent.mkdir(parents=True);efi.write_bytes(b'unit-efi')
    context={'ROOT':root,'hashlib':hashlib};exec(module(hash_nodes),context)
    hashes=context['evidence_hashes']();assert tuple(hashes)==paths and len(hashes)==len(paths)
    markers=ast.literal_eval(assigns['fence_markers'].value)
    good={'passed':True,'evidence':'E2-actual-root-fs-persist','source_head':'a'*40,'source_sha256':hashes,
          'production_efi_sha256':hashlib.sha256(efi.read_bytes()).hexdigest(),'guest_boots':4,'user_denials':4,
          'production_artifacts_restored':True,'base_disk_unchanged':True,'provider_lifecycle':False,'physical_hardware':False,'power_loss':False}
    healthy=[{'transport':t,'state':s,'passed':True,'all_disk_bytes_preserved':True,'markers':markers} for t in ('legacy','modern') for s in ('STABLE','PENDING')]
    unknown=[{'transport':t,'passed':True,'generation_delta':1,'diagnostic_runs':2,'retained_real_dirty':True,'no_post_fault_provider_rpc':True,'all_committed_bytes_preserved':True} for t in ('legacy','modern')]
    recoveries=[{'transport':t,'passed':True,'new_healthy_fence':True,'pending_bytes_preserved':True} for t in ('legacy','modern')]
    def check(value,accept):
        global checks
        ns={'ROOT':root,'BUILD':root/'build','hashlib':hashlib,'fence_guest':value,'fence_healthy':healthy,'fence_unknown':unknown,'fence_recoveries':recoveries,'fence_markers':markers}
        try:exec(gate_code,ns)
        except SystemExit:assert not accept
        else:assert accept
        checks+=1
    check(good,True)
    missing=copy.deepcopy(good);missing.pop('source_sha256');check(missing,False)
    for p in paths:
        bad=copy.deepcopy(good);bad['source_sha256'][p]='0'*64;check(bad,False)
        bad=copy.deepcopy(good);del bad['source_sha256'][p];check(bad,False)
        target=root/p;before=target.read_bytes();target.write_bytes(before+b' changed')
        check(good,False)
        try:exec(assert_code,{**context,'source_hashes':hashes})
        except AssertionError:checks+=1
        else:raise AssertionError('Mid-run change accepted')
        target.write_bytes(before)
    bad=copy.deepcopy(good);bad['source_sha256']['extra']='0'*64;check(bad,False)
    (root/'docs').mkdir();(root/'docs/notes.md').write_text('docs-only commit',encoding='utf-8')
    docs=copy.deepcopy(good);docs['source_head']='b'*40;check(docs,True)
    exec(assert_code,{**context,'source_hashes':hashes});checks+=1
assert checks==5+4*len(paths)
print('PERSIST FENCE EVIDENCE PASS: '+str(checks)+' actual AST checks; stale/missing inputs refused, docs-only allowed')
