"""Assemble production-generated Windows/POSIX blob paths and verify bytes."""
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build


class LinuxBlobAssembly(unittest.TestCase):
    def test_actual_assembler_and_cache_refresh(self):
        with tempfile.TemporaryDirectory(prefix='linux-blob-asm-',dir=ROOT/'build') as temporary:
            folder=Path(temporary)
            files=[folder/'hello.bin',folder/'translator with space.bin',folder/'probe.bin']
            data=[bytes(range(256))*3,b'translator\x00\xff\r\n\\"'*97,b'probe\x01'*31]
            for p,raw in zip(files,data):p.write_bytes(raw)
            names=[p.relative_to(ROOT) for p in files]
            blobs=[('hello',str(names[0])),('translator',PureWindowsPath(names[1])),('probe',PurePosixPath(names[2]))]
            source=folder/'blobs.S';obj=folder/'blobs.o';binary=folder/'blobs.bin'

            def assemble():
                text=build.linux_blob_source(blobs)
                self.assertIn(hashlib.sha256(b''.join(data)).hexdigest(),text)
                self.assertNotIn('\\',text)
                source.write_text(text,encoding='utf-8',newline='\n')
                build.run(['-target','x86_64-freestanding-none','-c',str(source.relative_to(ROOT)),'-o',str(obj.relative_to(ROOT))])
                subprocess.run([build.ZIG,'objcopy','-O','binary',str(obj),str(binary)],check=True)
                expected=b''.join(raw+b'\0'*(-len(raw)%4096) for raw in data)
                self.assertEqual(binary.read_bytes(),expected)
                return text,binary.read_bytes()

            first,old=assemble()
            # Same filename and length, different bytes: the generated source
            # must invalidate the assembler cache and embed the new payload.
            data[1]=bytes(x^0x5a for x in data[1]);files[1].write_bytes(data[1])
            second,new=assemble()
            self.assertNotEqual(first,second);self.assertNotEqual(old,new)
            result={'passed':True,'payloads':3,'windows_and_posix_paths':True,
                    'space_in_path':True,'actual_assembler_bytes':True,'cache_refresh':True}
            (ROOT/'build/linux-blob-assembly-results.json').write_text(json.dumps(result)+'\n',encoding='utf-8')


if __name__=='__main__':unittest.main(verbosity=2)
