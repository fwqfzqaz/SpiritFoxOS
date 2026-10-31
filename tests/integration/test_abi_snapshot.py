"""Independent wire sizes plus real compiled opcode/field/type drift detection."""
import copy
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import abi
import build


class AbiSnapshot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p=subprocess.run([sys.executable,str(ROOT/'tools/abi.py'),'capture'],cwd=ROOT,
                         text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        if p.returncode:raise RuntimeError(p.stdout+p.stderr)
        cls.current=abi.load(ROOT/'build/abi-current.json')
        cls.reference=abi.load(abi.REFERENCE)

    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='abi-snapshot-',dir=ROOT/'build')
        self.folder=Path(self.temp.name).resolve()
        self.assertTrue(self.folder.is_relative_to(ROOT/'build'))

    def tearDown(self):
        assert self.folder.is_relative_to(ROOT/'build')
        self.temp.cleanup()

    def cli(self,*args,script=ROOT/'tools/abi.py',cwd=ROOT):
        return subprocess.run([sys.executable,str(script),*map(str,args)],cwd=cwd,
                              text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)

    def write(self,name,value):
        path=self.folder/name
        path.write_text(json.dumps(value),encoding='utf-8')
        return path

    def test_capture_and_independent_wire_sizes(self):
        self.assertEqual(self.current,self.reference)
        values=self.current['values']
        self.assertEqual(len(values),563)
        self.assertEqual(values['constant.OP_DUP'],25)
        self.assertEqual(values['constant.OP_PREAD'],26)
        self.assertEqual(values['constant.SYS_TASK_PRIORITY'],20)
        self.assertEqual(values['constant.SF_PRIORITY_AGING_TICKS'],4)
        self.assertEqual(values['constant.SYS_TASK_CAP_LIMIT'],21)
        self.assertEqual(values['constant.SF_CAP_LIMIT_MAX'],32)
        self.assertEqual(values['constant.SYS_SEND_CANCEL'],22)
        # These fixed little-endian formats are independent of C sizeof/offsetof.
        formats={'SfIpcHeader':'<IHHIIQIIQ','SfMessage':'<IHHIIQIIQqQQII256s',
                 'SfBootArgs':'<IHHQQ12IQQIHH','SfFileStat':'<IIIIQQ',
                 'SfNetInfo':'<6sH6I','SfNetPacket':'<IHH248s','SfTrustKey':'<32sII'}
        for name,fmt in formats.items():
            with self.subTest(layout=name):self.assertEqual(values['size.'+name],struct.calcsize(fmt))
        self.assertEqual(values['offset.SfIpcHeader.request_id'],16)
        self.assertEqual(values['offset.SfIpcHeader.deadline_ns'],32)
        self.assertEqual(values['offset.SfMessage.data'],72)
        self.assertEqual(values['width.SfMessage.data'],256)
        self.assertEqual(values['offset.SfBootArgs.pid'],24)
        self.assertEqual(values['kind.SfMessage.status'],8)
        self.assertEqual(values['kind.SfMessage.handle'],7)
        self.assertEqual(values['constant.SF_TASK_RUNNING'],4294967296)
        self.assertEqual(values['constant.SF_BAD_SIGNATURE'],-4)
        self.assertEqual(values['model.pointer_bits'],64)
        self.assertEqual(values['model.little_endian'],1)

    def test_standalone_check_needs_no_compiler(self):
        isolated=self.folder/'isolated'
        (isolated/'tools').mkdir(parents=True);(isolated/'abi').mkdir()
        shutil.copyfile(ROOT/'tools/abi.py',isolated/'tools/abi.py')
        shutil.copyfile(abi.REFERENCE,isolated/'abi/x86_64-current.json')
        capture=self.write('capture.json',self.current)
        self.assertFalse((isolated/'.tools').exists())
        self.assertFalse((isolated/'build').exists())
        p=self.cli('check',capture,script=isolated/'tools/abi.py',cwd=isolated)
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertTrue(json.loads(p.stdout)['layout_matches'])
        p=self.cli('diff',capture,capture,script=isolated/'tools/abi.py',cwd=isolated)
        self.assertEqual(p.returncode,0,p.stderr)
        p=self.cli('capture',script=isolated/'tools/abi.py',cwd=isolated)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('source checkout',p.stderr)
        self.assertNotIn('Traceback',p.stderr)
        self.assertFalse((isolated/'build').exists())

    def test_diff_reports_add_remove_and_value_changes(self):
        before={'format':abi.FORMAT,'values':{'a':1,'b':2,'same':3}}
        after={'format':abi.FORMAT,'values':{'a':9,'c':4,'same':3}}
        self.assertEqual(abi.difference(before,after),{
            'added':{'c':4},'removed':{'b':2},'changed':{'a':{'before':1,'after':9}}})
        old=copy.deepcopy(before);new=copy.deepcopy(after)
        abi.difference(before,after)
        self.assertEqual(before,old);self.assertEqual(after,new)

    def test_cli_requires_review_for_additions_as_well(self):
        before={'format':abi.FORMAT,'values':{'a':1,'b':2}}
        for values in [{'a':1,'b':2,'c':3},{'a':1},{'a':8,'b':2}]:
            p=self.cli('diff',self.write('before.json',before),
                       self.write('after.json',{'format':abi.FORMAT,'values':values}))
            self.assertEqual(p.returncode,1,p.stderr)
            self.assertFalse(json.loads(p.stdout)['layout_matches'])
        reordered={'format':abi.FORMAT,'values':{'b':2,'a':1}}
        p=self.cli('diff',self.write('before.json',before),self.write('after.json',reordered))
        self.assertEqual(p.returncode,0,p.stderr)

    def test_invalid_ambiguous_or_nested_reports(self):
        bad=[b'{"format":"x","format":"y","values":{"a":1}}',
             ('{"format":'+json.dumps(abi.FORMAT)+',"values":{"a":1,"a":2}}').encode(),
             b'null',b'[]',b'{}',b'{',b'{"x":"\xff"}']
        for raw in bad:
            with self.subTest(raw=raw):
                with self.assertRaises(ValueError):abi.decode(raw)
        for value in [True,1.0,None,{},[],2**64,-2**63-1]:
            with self.assertRaises(ValueError):abi.decode(json.dumps({'format':abi.FORMAT,'values':{'a':value}}).encode())
        for name in ['', '_entry','a\n','a/b','\u4e2d','x'*129]:
            with self.assertRaises(ValueError):abi.validate({'format':abi.FORMAT,'values':{name:1}})
        for snapshot in [{'format':abi.FORMAT,'values':{}},
                         {'format':abi.FORMAT,'values':{'a':1},'extra':1},
                         {'format':abi.FORMAT+'-future','values':{'a':1}}]:
            with self.assertRaises(ValueError):abi.validate(snapshot)
        raw=('{'+'"format":'+json.dumps(abi.FORMAT)+',"values":{"a":').encode()+b'['*1200+b'0'+b']'*1200+b'}}'
        with self.assertRaises(ValueError):abi.decode(raw)
        broken=self.folder/'broken.json';broken.write_bytes(b'{}')
        p=self.cli('check',broken)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('abi:',p.stderr)
        self.assertNotIn('Traceback',p.stderr)

    def test_exact_limits_and_integer_range(self):
        sample={'format':abi.FORMAT,'values':{'a':1}}
        raw=json.dumps(sample).encode()
        path=self.folder/'limit.json'
        path.write_bytes(raw+b' '*(262144-len(raw)))
        self.assertEqual(abi.load(path),sample)
        path.write_bytes(raw+b' '*(262145-len(raw)))
        with self.assertRaisesRegex(ValueError,'256 KiB'):abi.load(path)
        abi.validate({'format':abi.FORMAT,'values':{'x'+str(i):i for i in range(2048)}})
        with self.assertRaisesRegex(ValueError,'2048'):
            abi.validate({'format':abi.FORMAT,'values':{'x'+str(i):i for i in range(2049)}})
        self.assertEqual(abi.validate({'format':abi.FORMAT,'values':{'minimum':-2**63,'maximum':2**64-1}})['values']['maximum'],2**64-1)

    def compiled_drift(self,old,new):
        include=self.folder/'include';include.mkdir()
        text=(ROOT/'include/sf.h').read_text(encoding='utf-8')
        self.assertIn(old,text)
        (include/'sf.h').write_text(text.replace(old,new,1),encoding='utf-8',newline='\n')
        executable=self.folder/('probe'+build.EXE)
        build.run(['-target',build.HOST,'-std=c11','-O2','-Wall','-Wextra','-Werror',
                   '-I'+str(include),'-Iinclude','tools/abi_snapshot.c','-o',str(executable)])
        p=subprocess.run([str(executable)],check=True,stdout=subprocess.PIPE)
        modified=abi.decode(p.stdout)
        changed=abi.difference(self.reference,modified)
        report=self.write('modified.json',modified)
        p=self.cli('check',report)
        self.assertEqual(p.returncode,1,p.stderr)
        self.assertEqual(json.loads(p.stdout)['changed'],changed['changed'])
        return changed

    def test_real_compiled_opcode_renumbering(self):
        diff=self.compiled_drift('SYS_EXIT, SYS_YIELD','SYS_EXIT, SYS_SNAPSHOT_TEST, SYS_YIELD')
        self.assertEqual(diff['changed']['constant.SYS_YIELD'],{'before':1,'after':2})
        self.assertNotIn('size.SfBootArgs',diff['changed'])

    def test_real_compiled_same_size_field_reorder(self):
        diff=self.compiled_drift('uint32_t pid,manager,reply,service;','uint32_t manager,pid,reply,service;')
        self.assertEqual(diff['changed']['offset.SfBootArgs.pid'],{'before':24,'after':28})
        self.assertEqual(diff['changed']['offset.SfBootArgs.manager'],{'before':28,'after':24})
        self.assertNotIn('size.SfBootArgs',diff['changed'])

    def test_real_compiled_same_size_signedness_change(self):
        diff=self.compiled_drift('uint32_t pid,manager,reply,service;','int32_t pid;uint32_t manager,reply,service;')
        self.assertEqual(diff['changed']['kind.SfBootArgs.pid'],{'before':5,'after':6})
        self.assertNotIn('offset.SfBootArgs.pid',diff['changed'])
        self.assertNotIn('size.SfBootArgs',diff['changed'])


if __name__=='__main__':unittest.main(verbosity=2)
