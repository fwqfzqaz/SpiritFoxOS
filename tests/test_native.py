"""Authenticated relative records and the actual userspace image preparer."""
import ctypes as C
from pathlib import Path
import random
import struct
import sys
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from sfpack import Crypto,cbor,seal,pack,elf_relative_records,elf_segments


class Native(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.crypto=Crypto()
        cls.seed=(ROOT/'.keys/application.seed').read_bytes()
        cls.pub=cls.crypto.key(cls.seed)[1]
        cls.good=(ROOT/'build/pieprobe.sfk').read_bytes()
        cls.elf=(ROOT/'build/pieprobe.elf').read_bytes()
        cls.manifest={'app_id':'org.spiritfox.pieprobe','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,'native_profile':'relative-v1','requested_caps':['stdio','namespace.read'],'relative_relocations':{'section':3,'offset':0,'bytes':48}}
        cls.sections=[struct.unpack_from('<IIQQQQQ16s',cls.good,96+i*64) for i in range(4)]
        cls.records=[struct.unpack_from('<Qq',cls.good,cls.sections[3][2]+i*16) for i in range(3)]
        cls.prepare=cls.crypto.lib.sf_image_prepare
        cls.prepare.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_uint64,C.c_void_p,C.c_size_t]

    def make(self,manifest=None,records=None,overrides=None):
        records=self.records if records is None else records
        manifest=dict(self.manifest if manifest is None else manifest)
        if 'relative_relocations' in manifest and manifest['relative_relocations']==self.manifest['relative_relocations']:
            manifest['relative_relocations']=dict(manifest['relative_relocations'],bytes=len(records)*16)
        encoded=cbor(manifest)
        data=bytearray(self.good[:96]+bytes(4*64)+encoded)
        for i,s in enumerate(self.sections):
            typ,flags,off,size,memory,rva,alignment,reserved=s
            payload=self.good[off:off+size] if typ!=4 else b''.join(struct.pack('<Qq',*r) for r in records)
            if typ==4:
                size=memory=len(payload)
            fields=[typ,flags,0,size,memory,rva,alignment,reserved]
            if overrides and i in overrides:
                for index,value in overrides[i].items(): fields[index]=value
            data.extend(bytes(-len(data)%alignment));fields[2]=len(data);data.extend(payload)
            struct.pack_into('<IIQQQQQ16s',data,96+i*64,*fields)
        data.extend(bytes(-len(data)%8));sig=len(data);data.extend(bytes(104))
        struct.pack_into('<Q',data,16,len(data));struct.pack_into('<QQQ',data,48,352,len(encoded),sig)
        return seal(data,self.seed,self.crypto)

    def reject(self,data):
        self.assertEqual(self.crypto.verify(data,self.pub),-3)
        output=C.create_string_buffer(b'Q'*16384)
        self.assertEqual(self.prepare(data,len(data),self.pub,0x44000000,output,16384),-3)
        self.assertEqual(output.raw,b'Q'*16384+b'\0')

    def test_exact_fixture_and_preparation(self):
        self.assertEqual(self.make(),self.good)
        for base in (0x44000000,0x46000000,0x4fe00000):
            output=C.create_string_buffer(16384)
            self.assertEqual(self.prepare(self.good,len(self.good),self.pub,base,output,16384),16384)
            expected=bytearray()
            offsets=[]
            for s in self.sections[:3]:
                offsets.append(len(expected));payload=self.good[s[2]:s[2]+s[3]]
                expected.extend(payload+bytes((s[4]+4095)//4096*4096-len(payload)))
            for target,addend in self.records:
                self.assertTrue(any(s[0]==3 and s[5]<=target<=s[5]+s[4]-8 for s in self.sections))
                for off,s in zip(offsets,self.sections):
                    if s[0]==3 and s[5]<=target<=s[5]+s[4]-8:
                        struct.pack_into('<Q',expected,off+target-s[5],base+addend)
            self.assertEqual(output.raw,bytes(expected)+b'\0'*(len(output)-len(expected)))
            self.assertEqual((ROOT/'build/pieprobe.sfk').read_bytes(),self.good)
        for base in (0,0x40000000,0x40001000,0x41010000,0x42000000,0x44001000,0x50000000,2**64-1):
            output=C.create_string_buffer(b'Q'*16384)
            self.assertLess(self.prepare(self.good,len(self.good),self.pub,base,output,16384),0)
            self.assertEqual(output.raw,b'Q'*16384+b'\0')
        output=C.create_string_buffer(b'Q'*16384)
        self.assertEqual(self.prepare(self.good,len(self.good),self.pub,0x44000000,output,16383),-18)
        self.assertEqual(output.raw,b'Q'*16384+b'\0')

    def test_signed_target_and_addend_rejection(self):
        for target in (0,0x1000,0x2001,0x3079,0x3080,0x4000,2**64-8):
            with self.subTest(target=target): self.reject(self.make(records=[(target,0)]))
        for addend in (-1,-2**63,0xfff,0x10d4,0x1fff,0x3080,0x200000,2**63-1):
            with self.subTest(addend=addend): self.reject(self.make(records=[(0x2000,addend)]))
        self.reject(self.make(records=[self.records[0],self.records[0]]))
        self.reject(self.make(records=list(reversed(self.records))))
        self.reject(self.make(overrides={2:{0:2,1:1}}))

    def test_manifest_profile_and_subtable(self):
        for value in ('future-v2','static-v1','',1,None):
            fields=dict(self.manifest,native_profile=value)
            self.reject(self.make(manifest=fields))
        fields=dict(self.manifest);del fields['native_profile'];self.reject(self.make(manifest=fields))
        for subtable in ({},{'section':3,'offset':8,'bytes':48},{'section':2,'offset':0,'bytes':48},{'section':64,'offset':0,'bytes':48},{'section':3,'offset':0,'bytes':32},{'section':3,'offset':0,'bytes':49},{'section':3,'offset':0,'bytes':48,'unknown':0},{'section':3,'offset':0,'bytes':-1}):
            self.reject(self.make(manifest=dict(self.manifest,relative_relocations=subtable)))
        for key in ('native_imports','import_relocations'):
            self.reject(self.make(manifest=dict(self.manifest,**{key:[]})))
        fields=dict(self.manifest);del fields['relative_relocations'];self.reject(self.make(manifest=fields))
        self.reject(self.make(overrides={3:{3:47,4:47}}))

    def test_count_limits_and_bss_targets(self):
        for count in (0,1,256):
            records=[(0x2000+i*8,0) for i in range(count)]
            data=self.make(records=records)
            self.assertEqual(self.crypto.verify(data,self.pub),0)
            output=C.create_string_buffer(16384)
            self.assertEqual(self.prepare(data,len(data),self.pub,0x46000000,output,16384),16384)
        self.reject(self.make(records=[(0x2000+i*8,0) for i in range(257)]))
        self.assertEqual(self.crypto.verify(self.make(records=[(0x3078,0x307f)]),self.pub),0)

    def test_elf_restrictions_and_roundtrip(self):
        self.assertEqual(elf_relative_records(self.elf),b''.join(struct.pack('<Qq',*r) for r in self.records))
        manifest=dict(self.manifest);del manifest['relative_relocations']
        self.assertEqual(pack(manifest,self.elf,2,self.seed),self.good)
        with self.assertRaises(ValueError): pack(manifest,self.elf,1,self.seed)
        with self.assertRaises(ValueError): elf_segments(self.elf)
        fields=struct.unpack_from('<HHIQQQIHHHHHH',self.elf,16)
        rela=next(struct.unpack_from('<IIQQQQIIQQ',self.elf,fields[5]+i*64) for i in range(fields[11]) if struct.unpack_from('<I',self.elf,fields[5]+i*64+4)[0]==4)
        for info in (1,6,7,0x100000008):
            image=bytearray(self.elf);struct.pack_into('<Q',image,rela[4]+8,info)
            with self.assertRaisesRegex(ValueError,'symbol-free'): elf_relative_records(image)
        image=bytearray(self.elf);struct.pack_into('<Q',image,rela[4]+24,self.records[0][0])
        with self.assertRaisesRegex(ValueError,'Duplicate'): elf_relative_records(image)
        for off,fmt,value in ((32,'<Q',2**64-1),(40,'<Q',2**64-1),(58,'<H',63)):
            image=bytearray(self.elf);struct.pack_into(fmt,image,off,value)
            with self.assertRaises(ValueError): pack(manifest,image,2,self.seed)
        for i in range(fields[11]):
            pos=fields[5]+i*64;section=struct.unpack_from('<IIQQQQIIQQ',self.elf,pos)
            if section[1]==4:
                for typ in (9,19):
                    image=bytearray(self.elf);struct.pack_into('<I',image,pos+4,typ)
                    with self.assertRaisesRegex(ValueError,'RELA'): elf_relative_records(image)
            if section[1]==11:
                image=bytearray(self.elf);struct.pack_into('<I',image,section[4],1)
                with self.assertRaisesRegex(ValueError,'imports'): elf_relative_records(image)
        for i in range(fields[9]):
            ph=struct.unpack_from('<IIQQQQQQ',self.elf,fields[4]+i*56)
            if ph[0]==2:
                image=bytearray(self.elf);struct.pack_into('<qQ',image,ph[2],1,1)
                with self.assertRaisesRegex(ValueError,'imports'): pack(manifest,image,2,self.seed)

    def test_signed_seeded_record_mutations(self):
        rng=random.Random(20261024)
        for _ in range(500):
            records=list(self.records);index=rng.randrange(3);target,addend=records[index]
            target^=1<<rng.randrange(64);records[index]=(target,addend)
            targets=[t for t,_ in records]
            valid=targets==sorted(set(targets)) and all(t%8==0 and any(s[0]==3 and s[5]<=t<=s[5]+s[4]-8 for s in self.sections) for t in targets)
            self.assertEqual(self.crypto.verify(self.make(records=records),self.pub),0 if valid else -3)


if __name__=='__main__': unittest.main(verbosity=2)
