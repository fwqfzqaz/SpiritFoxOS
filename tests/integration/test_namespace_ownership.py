"""Inspect independently linked ELF state ownership, not build-list strings."""
import json
from pathlib import Path
import struct
import unittest

ROOT=Path(__file__).resolve().parents[2]


def inspect(name):
    data=(ROOT/'build'/(name+'.elf')).read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01':raise ValueError('Expected ELF64 little endian')
    sections=[]
    shoff=struct.unpack_from('<Q',data,40)[0]
    entsize,count=struct.unpack_from('<HH',data,58)
    if entsize!=64 or shoff+entsize*count>len(data):raise ValueError('Invalid section table')
    for i in range(count):sections.append(struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*entsize))
    objects={};functions=set()
    for section in sections:
        if section[1]!=2:continue
        offset,size,link,entry=section[4],section[5],section[6],section[9]
        if entry!=24 or link>=len(sections) or offset+size>len(data):raise ValueError('Invalid symbol table')
        strings=sections[link];table=data[strings[4]:strings[4]+strings[5]]
        for i in range(0,size,entry):
            key,info,_,defined,_,length=struct.unpack_from('<IBBHQQ',data,offset+i)
            if not defined or not key:continue
            end=table.find(b'\0',key)
            if end<0:raise ValueError('Invalid symbol name')
            symbol=table[key:end].decode('utf-8')
            if info&15==1:objects[symbol]=length
            elif info&15==2:functions.add(symbol)
    loads=[]
    phoff=struct.unpack_from('<Q',data,32)[0];entsize,count=struct.unpack_from('<HH',data,54)
    if entsize!=56 or phoff+entsize*count>len(data):raise ValueError('Invalid program table')
    for i in range(count):
        kind,flags,_,_,_,_,memory,_=struct.unpack_from('<IIQQQQQQ',data,phoff+i*entsize)
        if kind==1:loads.append((flags,memory))
    return {'objects':objects,'functions':functions,'rw':sum(n for f,n in loads if f&2),
            'load_pages':sum((n+4095)&~4095 for _,n in loads)}


class NamespaceOwnership(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=inspect('sf-init');cls.fs=inspect('sf-fs');cls.persist=inspect('sf-persist')

    def test_namespace_tables_only_in_fs(self):
        self.assertNotIn('service_namespace',self.root['objects'])
        self.assertGreater(self.fs['objects']['service_namespace'],128*1024)
        self.assertLess(self.root['objects']['frontend'],16*1024)
        self.assertNotIn('frontend',self.fs['objects'])

    def test_transaction_state_only_in_persist(self):
        self.assertNotIn('volume',self.root['objects']);self.assertNotIn('volume',self.fs['objects'])
        self.assertGreater(self.persist['objects']['volume'],256*1024)

    def test_root_has_no_parser_or_transaction_machine(self):
        for symbol in ('sf_readfs_read','sf_fs_mount_auto','sf_txn_commit','persist_mount','sf_update_begin'):
            self.assertNotIn(symbol,self.root['functions'])
        self.assertIn('sf_fs_mount_auto',self.fs['functions'])
        self.assertIn('sf_txn_commit',self.persist['functions'])

    def test_writable_budget_and_load_staging(self):
        for image in (self.root,self.fs,self.persist):self.assertLessEqual(image['rw']*10,2097152*9)
        self.assertLessEqual(self.fs['load_pages'],1280*1024)
        self.assertLessEqual(self.persist['load_pages'],1280*1024)


if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(NamespaceOwnership))
    if result.wasSuccessful():
        (ROOT/'build/namespace-ownership.json').write_text(json.dumps({'passed':True,'tests':result.testsRun,
            'root_rw_bytes':NamespaceOwnership.root['rw'],'fs_rw_bytes':NamespaceOwnership.fs['rw'],
            'fs_load_pages_bytes':NamespaceOwnership.fs['load_pages'],'root_has_namespace_tables':False,
            'root_has_transaction_state':False},indent=2)+'\n',encoding='utf-8')
    raise SystemExit(not result.wasSuccessful())
