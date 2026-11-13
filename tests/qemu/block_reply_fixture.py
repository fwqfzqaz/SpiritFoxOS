"""Temporary signed block driver with independent durable artifact recovery."""
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import sys
from qemu_persist_test import ROOT
from trust_sync_fixture import packages,FILES
sys.path.insert(0,str(ROOT/'tools'))
import build
from sfpack import pack,bundle

STATE=ROOT/'.cache/block-reply-fixture-state.json'
BACKUP=ROOT/'.cache/block-reply-fixture-backup'

def state_write(value):
    p=STATE.with_suffix('.tmp');p.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8');p.replace(STATE)

def restore():
    assert Path.cwd().resolve()==ROOT
    if not STATE.exists():return
    state=json.loads(STATE.read_text(encoding='utf-8'))
    if not state.get('active'):return
    assert set(state['files'])==set(FILES)
    data={}
    for i,name in enumerate(FILES):
        target=(ROOT/name).resolve();backup=(BACKUP/(str(i)+'.bin')).resolve()
        assert target.is_relative_to(ROOT) and backup.is_relative_to(ROOT)
        data[name]=backup.read_bytes();assert hashlib.sha256(data[name]).hexdigest()==state['files'][name],name
    for name,contents in data.items():(ROOT/name).write_bytes(contents)
    assert all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==h for n,h in state['files'].items())
    state.update(active=False,restored=True);state_write(state)

@contextmanager
def driver():
    assert Path.cwd().resolve()==ROOT
    restore()
    other=ROOT/'.cache/trust-sync-fixture-state.json'
    assert not other.exists() or not json.loads(other.read_text(encoding='utf-8'))['active'],'Do not overlap signed fixtures'
    BACKUP.mkdir(parents=True,exist_ok=True)
    hashes={}
    for i,name in enumerate(FILES):
        data=(ROOT/name).read_bytes();(BACKUP/(str(i)+'.bin')).write_bytes(data);hashes[name]=hashlib.sha256(data).hexdigest()
    state_write({'active':True,'restored':False,'files':hashes})
    try:
        shared=build.objects('block-fault-common',['services/start.S','services/memory.c','services/libsf.c','services/arguments.c'],extra=build.USER_SECTIONS)
        image=build.link_elf('block-fault-driver',shared+build.objects('block-fault-driver',['tests/fixtures/block_reply_fault.c'],extra=build.USER_SECTIONS),'services/user.ld')
        build.check_rw_budget('sf-virtio-blk',ROOT/'build/block-fault-driver.elf')
        replacement=pack({'module_id':'org.spiritfox.virtio.blk','version':'0.1.0','category':'driver','requested_caps':['device.virtio-blk.trusted'],'resource_limits':{'memory':131072}},image,1,(ROOT/'.keys/development.seed').read_bytes())
        original=packages((ROOT/'build/bundle.bin').read_bytes());assert sum(n=='sf-virtio-blk.sfmod' for n,_ in original)==1
        (ROOT/'build/bundle.bin').write_bytes(bundle([(n,replacement if n=='sf-virtio-blk.sfmod' else d) for n,d in original]))
        build.build_efi()
        yield
    finally:restore()

if __name__=='__main__':
    if sys.argv[1:]!=['--restore']:raise SystemExit('Use --restore for saved fixture cleanup')
    restore();print('Production block fixture artifacts restored or already clean',flush=True)
