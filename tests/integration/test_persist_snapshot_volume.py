"""Actual cache snapshot E1 against an independent Python file digest."""
import hashlib,json,re,struct,subprocess,sys,tempfile,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'));import build
def main():
    files={'/z':b'','/notes/user':b'snapshot','/a':bytes((1,2,3))}
    encoded=b'SFFILES-SNAP-V1\0'+struct.pack('<I',len(files))
    for path,data in sorted(files.items()):encoded+=path.encode().ljust(64,b'\0')+struct.pack('<Q',len(data))+hashlib.sha256(data).digest()
    expected=hashlib.sha256(encoded).hexdigest();assert len(encoded)==332
    with tempfile.TemporaryDirectory(prefix='snapshot-volume-',dir=ROOT/'build')as name:
        folder=Path(name);archive=folder/'libsfcore-zig.a';shutil.copyfile(build.rust_lib('host'),archive);exe=folder/('snapshot-volume'+build.EXE)
        build.run(['-target',build.HOST,'-g0','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DSF_HOST_TEST',
                   '-Iinclude','-Iservices','-Ibuild','-Ivendor/monocypher','tests/host/test_persist_snapshot_volume.c',
                   'services/txn.c','services/update.c','services/version.c',*build.CRYPTO,str(archive),'-o',str(exe)])
        result=subprocess.run([str(exe),expected],capture_output=True,text=True,encoding='utf-8',errors='replace')
        print(result.stdout+result.stderr,end='',flush=True);result.check_returncode()
        rows=re.findall(r'^PERSIST SNAPSHOT VOLUME PASS (\d+) checks;.*file-digest vector provided$',result.stdout,re.M);assert len(rows)==1
    proof={'passed':True,'evidence':'E1-actual-cache-zero-IO','checks':int(rows[0]),'canonical_bytes':332,'sha256':expected,'guest_runtime':False}
    (ROOT/'build/persist-snapshot-volume-results.json').write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
