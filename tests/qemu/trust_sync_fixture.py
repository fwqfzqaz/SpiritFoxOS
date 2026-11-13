"""Build a temporary signed provider and always restore the production EFI."""
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import struct
import sys
from qemu_persist_test import ROOT
sys.path.insert(0,str(ROOT/'tools'))
import build
from sfpack import pack,bundle

STATE=ROOT/'.cache/trust-sync-fixture-state.json'
BACKUP=ROOT/'.cache/trust-sync-fixture-backup'
FILES=('build/bundle.bin','build/payload.S','build/esp/EFI/BOOT/BOOTX64.EFI')


def write_state(value):
    path=STATE.with_suffix('.tmp');path.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8');path.replace(STATE)


def restore():
    assert Path.cwd().resolve()==ROOT
    if not STATE.exists():return
    state=json.loads(STATE.read_text(encoding='utf-8'))
    if not state.get('active'):return
    assert set(state['files'])==set(FILES)
    contents={}
    for i,name in enumerate(FILES):
        target=(ROOT/name).resolve();assert target.is_relative_to(ROOT)
        data=(BACKUP/(str(i)+'.bin')).read_bytes()
        assert hashlib.sha256(data).hexdigest()==state['files'][name],name
        contents[name]=data
    for name,data in contents.items():(ROOT/name).write_bytes(data)
    assert all(hashlib.sha256((ROOT/n).read_bytes()).hexdigest()==h for n,h in state['files'].items())
    state.update(active=False,restored=True);write_state(state)


def packages(data):
    magic,count,total=struct.unpack_from('<4sIQ',data)
    assert magic==b'SFB1' and total==len(data) and 0<count<=1024 and 16+count*48<=total
    result=[]
    for i in range(count):
        raw,offset,size=struct.unpack_from('<32sQQ',data,16+i*48)
        assert offset>=16+count*48 and offset+size<=len(data)
        result.append((raw.split(b'\0',1)[0].decode('ascii'),data[offset:offset+size]))
    assert len({name for name,_ in result})==count
    return result


@contextmanager
def provider():
    assert Path.cwd().resolve()==ROOT
    restore();BACKUP.mkdir(parents=True,exist_ok=True)
    hashes={}
    for i,name in enumerate(FILES):
        data=(ROOT/name).read_bytes();(BACKUP/(str(i)+'.bin')).write_bytes(data);hashes[name]=hashlib.sha256(data).hexdigest()
    write_state({'active':True,'restored':False,'files':hashes})
    try:
        shared=build.objects('trust-fault-common',['services/start.S','services/memory.c','services/libsf.c','services/arguments.c'],extra=build.USER_SECTIONS)
        sources=['tests/fixtures/persist_trust_fault.c','services/persist_metadata.c','services/persistence.c','services/txn.c','services/update.c','services/version.c',*build.CRYPTO]
        image=build.link_elf('trust-fault-persist',shared+build.objects('trust-fault-persist',sources,extra=build.USER_SECTIONS),'services/user.ld')
        build.check_rw_budget('sf-persist',ROOT/'build/trust-fault-persist.elf')
        replacement=pack({'module_id':'org.spiritfox.persist','version':'0.1.0','category':'service','requested_caps':[],'resource_limits':{'memory':524288}},image,1,(ROOT/'.keys/development.seed').read_bytes())
        original=packages((ROOT/'build/bundle.bin').read_bytes());assert sum(n=='sf-persist.sfmod' for n,_ in original)==1
        (ROOT/'build/bundle.bin').write_bytes(bundle([(n,replacement if n=='sf-persist.sfmod' else data) for n,data in original]))
        build.build_efi()
        yield
    finally:restore()


if __name__=='__main__':
    if sys.argv[1:]!=['--restore']:raise SystemExit('Use --restore for saved fixture cleanup')
    restore();print('Production trust fixture artifacts restored or already clean',flush=True)
