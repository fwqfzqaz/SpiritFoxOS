import ctypes as C
import hashlib
from pathlib import Path
import random
import struct
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/package'))
from mkreadfs import create
from sfpack import Crypto


class ReadFs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.crypto=Crypto()
        cls.good=create({'/bin/a':bytes(range(256))*7,'/etc/readme.txt':b'hello\n'})

    def check(self,data,contents=True):
        return self.crypto.lib.sf_fs_check(bytes(data),len(data),int(contents))

    def rehash(self,data):
        size=struct.unpack_from('<Q',data,40)[0]
        data[48:80]=hashlib.sha256(data[512:512+size]).digest()
        return data

    def test_valid_disk_and_builder(self):
        self.assertEqual(self.check(self.good),0)
        self.assertEqual(self.check((ROOT/'build/storage.img').read_bytes()),0)
        for name in ['/../a','/a//b','/a/','/a/./b','/a\\b','/a b']:
            with self.subTest(name=name),self.assertRaises(ValueError):
                create({name:b'a'})
        with self.assertRaises(ValueError):
            create({'/a':b'a','/a/b':b'b'})

    def test_header_bounds(self):
        for off,fmt,value in [(0,'Q',0),(8,'I',2),(12,'I',511),(16,'Q',2**64-1),(24,'I',17),(28,'I',127),(32,'Q',2**64-1),(40,'Q',2049),(80,'Q',1)]:
            data=bytearray(self.good)
            struct.pack_into('<'+fmt,data,off,value)
            with self.subTest(offset=off):self.assertLess(self.check(data),0)
        for n in [0,1,511,512,1023,len(self.good)-1]:
            self.assertLess(self.check(self.good[:n]),0)

    def test_signed_metadata_cannot_bypass_bounds(self):
        for off,fmt,value in [(512+64,'Q',2**64-1),(512+64,'Q',512),(512+64,'Q',1025),(512+72,'Q',2**64-1),(512+72,'Q',0),(512+112,'I',0),(512+116,'I',1)]:
            data=bytearray(self.good);struct.pack_into('<'+fmt,data,off,value)
            with self.subTest(offset=off,value=value):self.assertLess(self.check(self.rehash(data)),0)
        data=bytearray(self.good);struct.pack_into('<Q',data,640+64,struct.unpack_from('<Q',data,576)[0]);self.assertLess(self.check(self.rehash(data)),0)
        data=bytearray(self.good);data[640:704]=data[512:576];self.assertLess(self.check(self.rehash(data)),0)
        data=bytearray(self.good);data[512:576]=b'/../evil\0'+bytes(55);self.assertLess(self.check(self.rehash(data)),0)
        data=bytearray(self.good);data[512:576]=bytes([255])*64;self.assertLess(self.check(self.rehash(data)),0)

    def test_contents_hash_is_checked(self):
        data=bytearray(self.good);off=struct.unpack_from('<Q',data,576)[0];data[off]^=1
        self.assertEqual(self.check(data,contents=False),0)
        self.assertEqual(self.check(data),-3)

    def test_seeded_metadata_mutations(self):
        rng=random.Random(20261023)
        for _ in range(500):
            data=bytearray(self.good);data[rng.randrange(768)]^=1<<rng.randrange(8)
            self.assertLess(self.check(data),0)


if __name__=='__main__':unittest.main(verbosity=2)
