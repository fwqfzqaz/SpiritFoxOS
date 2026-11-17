"""E1: compare the actual C trust fingerprint with independent Python bytes."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
import build


def main():
    rows = [(bytes((i*41+j*3) & 255 for j in range(32)), (1,2,3)[i], int(i==1))
            for i in range(3)]
    revoked = [bytes(0xe0-i for i in range(32)), bytes(0x20+i*2 for i in range(32))]
    canonical = b'SFTRUST-SNAP-V1\0'+struct.pack('<I',len(rows))
    canonical += b''.join(key+struct.pack('<II',usage,immutable) for key,usage,immutable in sorted(rows))
    canonical += struct.pack('<I',len(revoked))+b''.join(sorted(revoked))
    assert len(canonical)==208
    expected = hashlib.sha256(canonical).hexdigest()
    with tempfile.TemporaryDirectory(prefix='trust-fingerprint-', dir=ROOT/'build') as name:
        folder = Path(name)
        archive = folder/'libsfcore-zig.a'
        shutil.copyfile(build.rust_lib('host'),archive)
        executable = folder/('test_trust_fingerprint'+build.EXE)
        build.run(['-target',build.HOST,'-g0','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                   '-DSF_HOST_TEST','-Iinclude','-Iservices','-Ibuild','-Ivendor/monocypher',
                   *dict.fromkeys(['tests/host/test_trust_fingerprint.c','services/trust_fingerprint.c',
                                   *build.CRYPTO]),str(archive),'-o',str(executable)])
        result = subprocess.run([str(executable),expected],cwd=ROOT,capture_output=True,
                                text=True,encoding='utf-8',errors='replace')
        print(result.stdout+result.stderr,end='',flush=True)
        result.check_returncode()
        found = re.findall(r'^TRUST FINGERPRINT PASS (\d+) checks;.*canonical expected digest provided$',result.stdout,re.M)
        assert len(found)==1
    proof = {'passed':True,'evidence':'E1-real-helper-independent-canonical-vector',
             'checks':int(found[0]),'canonical_bytes':len(canonical),'sha256':expected,
             'guest_runtime':False,'package_signature':False}
    (ROOT/'build/trust-fingerprint-results.json').write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8')


if __name__ == '__main__':
    main()
