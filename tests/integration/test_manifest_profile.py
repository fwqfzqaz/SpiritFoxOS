"""Source/profile boundaries and real sfpack CLI, including signed C verification."""
import copy
import json
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools/package'))
import manifest_profile as profile
from sfpack import Crypto, cbor, pack, source_manifest

APP = {'app_id':'org.spiritfox.source-test', 'version':'0.1.0',
       'runtime_id':'org.spiritfox.native', 'app_abi':1,
       'requested_caps':['stdio'], 'resource_limits':{'memory':131072}}
MODULE = {'module_id':'org.spiritfox.source-test', 'version':'0.1.0', 'category':'service'}
CLI = ROOT/'tools/package/sfpack.py'


class ManifestProfile(unittest.TestCase):
    def setUp(self):
        (ROOT/'build').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='manifest-profile-', dir=ROOT/'build')
        self.folder = Path(self.temp.name).resolve()
        self.assertTrue(self.folder.is_relative_to(ROOT/'build'))

    def tearDown(self):
        assert self.folder.is_relative_to(ROOT/'build')
        self.temp.cleanup()

    def source(self, value, raw=False):
        path = self.folder/'source.json'
        path.write_bytes(value if raw else json.dumps(value).encode())
        return path

    def run_cli(self, *args, cli=CLI, cwd=ROOT):
        return subprocess.run([sys.executable, str(cli), *map(str,args)], cwd=cwd,
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def reject(self, value, kind='app'):
        with self.assertRaises(ValueError):
            profile.validate(value, kind)

    def test_valid_profiles_and_required_app_budget(self):
        for kind, value in [('app',APP), ('module',MODULE)]:
            with self.subTest(kind=kind):
                self.assertEqual(profile.validate(copy.deepcopy(value),kind),value)
        for field in APP:
            value = copy.deepcopy(APP)
            del value[field]
            with self.subTest(missing=field):
                self.reject(value)
        for field in MODULE:
            value = copy.deepcopy(MODULE)
            del value[field]
            self.reject(value,'module')

    def test_closed_shape_kind_abi_and_generated_fields(self):
        for value in [dict(APP,module_id='org.spiritfox.module'),dict(MODULE,app_id='org.spiritfox.app')]:
            self.reject(value, 'app' if 'app_abi' in value else 'module')
        for field in ['extra','relative_relocations','import_relocations','native_imports']:
            self.reject(dict(APP,**{field:{}}))
        for abi in [True,False,0,2,'1',1.0,None]:
            self.reject(dict(APP,app_abi=abi))
        for runtime in ['', 'org.spiritfox.native\n','org.spiritfox.other']:
            self.reject(dict(APP,runtime_id=runtime))
        self.reject(dict(MODULE,native_profile='relative-v1'),'module')
        self.reject(dict(MODULE,category='kernel'),'module')
        profile.validate(dict(APP,native_profile='relative-v1'),'app')
        profile.validate(dict(APP,native_profile='static-v1'),'app')

    def test_identifier_ascii_and_ends(self):
        for value in ['a','A0._-','x'*63]:
            profile.validate(dict(APP,app_id=value),'app')
        for value in ['', 'x'*64, '.name','name/part','name\n','name\r\n','name\x00','\u4e2d\u6587','name\ud800']:
            with self.subTest(identifier=repr(value)):
                self.reject(dict(APP,app_id=value))
        # JSON Schema pattern uses search semantics, unlike the fixed validator's
        # fullmatch. Check exported anchors against trailing newlines as well.
        rule = profile.schema()['oneOf'][1]['properties']['app_id']
        self.assertIsNone(re.search(rule['pattern'],'name\n'))

    def test_versions_against_numeric_oracle(self):
        def expected(text):
            parts=text.split('.')
            return len(parts)==3 and all(p and len(p)<=10 and
                (p=='0' or p[0] in '123456789') and all('0'<=c<='9' for c in p) and
                int(p)<=4294967295 for p in parts)
        fixed=['0.0.0','4294967295.4294967295.4294967295','4294967296.0.0',
               '4294967294.0.0','00.0.0','0.1','0.0.0.0','-1.0.0','+1.0.0',
               '1.0.0\n','1.0.0\r\n','1.0.0-alpha','\u0661.0.0',' 1.0.0','1\\.0\\.0']
        rng=random.Random(0x53464d30)
        for _ in range(2000):
            parts=[str(rng.randrange(4294967298)) for _ in range(3)]
            if rng.randrange(3)==0:parts[rng.randrange(3)]='0'+parts[rng.randrange(3)]
            fixed.append('.'.join(parts))
        pattern=profile.schema()['oneOf'][1]['properties']['version']['pattern']
        for text in fixed:
            with self.subTest(version=text):
                self.assertEqual(re.search(pattern,text) is not None,expected(text))
                if expected(text):profile.validate(dict(APP,version=text),'app')
                else:self.reject(dict(APP,version=text))

    def test_capability_and_resource_boundaries(self):
        profile.validate(dict(APP,requested_caps=[]),'app')
        profile.validate(dict(APP,requested_caps=['x'*64]),'app')
        caps=['cap.'+str(i) for i in range(32)]
        profile.validate(dict(APP,requested_caps=caps),'app')
        for bad in [caps+['extra'],['stdio','stdio'],['x'*65],[''],[1],None,'stdio',['space cap'],['\u4e2d']]:
            self.reject(dict(APP,requested_caps=bad))
        for memory in [1,33554432]:
            profile.validate(dict(APP,resource_limits={'memory':memory}),'app')
        for budget in [{},{'memory':0},{'memory':33554433},{'memory':-1},{'memory':True},
                       {'memory':1.0},{'memory':'131072'},{'memory':131072,'other':0},None]:
            self.reject(dict(APP,resource_limits=budget))

    def test_duplicate_fields_invalid_numbers_unicode_and_root(self):
        bad=[b'{"x":1,"x":2}',b'{"x":{"y":1,"y":2}}',b'{"a":1,"\\u0061":2}',
             b'{"x":1.0}',b'{"x":1e0}',b'{"x":NaN}',b'{"x":Infinity}',b'{"x":-Infinity}',
             b'{"x":"\\ud800"}',b'{"x":"\\u0000"}',b'{"\\u0000":1}',b'{"x":"\xff"}',
             b'[]',b'null',b'1',b'{',b'\xef\xbb\xbf{}']
        for raw in bad:
            with self.subTest(source=raw):
                with self.assertRaises(ValueError):profile.load(self.source(raw,raw=True))
        self.assertEqual(profile.load(self.source(b'{"x":true,"y":null}',raw=True)),{'x':True,'y':None})

    def test_source_depth_and_node_exact_bounds(self):
        raw=b'{"x":0}'
        profile.load(self.source(raw+b' '*(262144-len(raw)),raw=True))
        with self.assertRaisesRegex(ValueError,'source limit'):
            profile.load(self.source(raw+b' '*(262145-len(raw)),raw=True))
        value=0
        for _ in range(15):value=[value]
        profile.load(self.source({'x':value}))
        with self.assertRaisesRegex(ValueError,'nesting'):
            profile.load(self.source({'x':[value]}))
        # root + key + array + 4093 scalar nodes = 4096 nodes.
        profile.load(self.source({'x':[0]*4093}))
        with self.assertRaisesRegex(ValueError,'node'):
            profile.load(self.source({'x':[0]*4094}))
        with self.assertRaisesRegex(ValueError,'nesting'):
            profile.load(self.source(b'{"x":'+b'['*1200+b'0'+b']'*1200+b'}',raw=True))

    def test_encoded_size_and_integer_bounds_before_image_io(self):
        exact={'x':'a'*(65536-6)}
        self.assertEqual(len(cbor(source_manifest(self.source(exact),'app'))),65536)
        with self.assertRaisesRegex(ValueError,'64 KiB'):
            source_manifest(self.source({'x':exact['x']+'a'}),'app')
        for value in [2**64,-(2**64)-1]:
            with self.assertRaisesRegex(ValueError,'CBOR integer overflow'):
                source_manifest(self.source({'x':value}),'app')
        for value in [2**64-1,-(2**64)]:
            self.assertEqual(source_manifest(self.source({'x':value}),'app')['x'],value)

    def test_standalone_schema_validation_without_build_or_keys(self):
        isolated=self.folder/'isolated'
        for relative in ['tools/package/sfpack.py','tools/package/manifest_profile.py',
                         'tools/trust_policy.py','schemas/native-source-v1.schema.json']:
            dst=isolated/relative
            dst.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/relative,dst)
        cli=isolated/'tools/package/sfpack.py'
        self.assertFalse((isolated/'build').exists())
        self.assertFalse((isolated/'.keys').exists())
        exported=self.run_cli('manifest-schema',cli=cli,cwd=isolated)
        self.assertEqual(exported.returncode,0,exported.stderr)
        self.assertEqual(json.loads(exported.stdout),profile.schema())
        out=isolated/'export.json'
        p=self.run_cli('manifest-schema','--out',out,cli=cli,cwd=isolated)
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(json.loads(out.read_text()),profile.schema())
        for kind,value in [('module',MODULE),('app',APP)]:
            p=self.run_cli('validate-manifest',self.source(value),'--kind',kind,cli=cli,cwd=isolated)
            self.assertEqual(p.returncode,0,p.stderr)
            report=json.loads(p.stdout)
            self.assertEqual(report['status'],'valid')
            self.assertEqual(report['validation'],'source-profile-only')
            self.assertEqual(report['manifest_bytes'],len(cbor(value)))
        p=self.run_cli('validate-manifest',self.source(dict(APP,app_abi=True)),'--kind','app',cli=cli,cwd=isolated)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('expected integer',p.stderr)
        self.assertNotIn('Traceback',p.stderr)

    def test_cli_rejects_before_missing_inputs_and_preserves_output(self):
        bad=[(b'{"app_id":"a","app_id":"b"}',False,'Duplicate'),
             (b'{"x":1.0}',False,'Floating'),
             (json.dumps({'x':'a'*65531}).encode(),False,'64 KiB'),
             (json.dumps(dict(APP,app_abi=True)).encode(),True,'expected integer'),
             (json.dumps({k:v for k,v in APP.items() if k!='resource_limits'}).encode(),True,'missing')]
        output=self.folder/'result.sfk'
        for raw,strict,message in bad:
            for existing in [False,True]:
                with self.subTest(raw=raw[:50],strict=strict,existing=existing):
                    if existing:output.write_bytes(b'keep-existing-output')
                    else:output.unlink(missing_ok=True)
                    flags=['--strict-manifest'] if strict else []
                    p=self.run_cli('app','--manifest',self.source(raw,raw=True),
                        '--image',self.folder/'missing-image.elf','--seed',self.folder/'missing-seed',
                        '--out',output,*flags)
                    self.assertNotEqual(p.returncode,0)
                    self.assertIn(message,p.stderr)
                    self.assertNotIn('missing-image',p.stderr)
                    self.assertNotIn('missing-seed',p.stderr)
                    self.assertNotIn('Traceback',p.stderr)
                    if existing:self.assertEqual(output.read_bytes(),b'keep-existing-output')
                    else:self.assertFalse(output.exists())

    def test_cli_packs_real_static_relative_and_module_images(self):
        # A public deterministic test seed, unrelated to repository private keys.
        seed=bytes(range(32));seed_path=self.folder/'test.seed';seed_path.write_bytes(seed)
        crypto=Crypto();public=crypto.key(seed)[1]
        cases=[('app',APP,'hello.elf'),('module',MODULE,'echo.elf'),
               ('app',dict(APP,native_profile='relative-v1'),'pieprobe.elf')]
        for kind,manifest,image_name in cases:
            image=ROOT/'build'/image_name
            with self.subTest(kind=kind,image=image_name):
                output=self.folder/(image_name+'.package')
                p=self.run_cli(kind,'--manifest',self.source(manifest),'--image',image,
                               '--seed',seed_path,'--out',output,'--strict-manifest')
                self.assertEqual(p.returncode,0,p.stderr)
                actual=output.read_bytes()
                self.assertEqual(crypto.verify(actual,public),0)
                self.assertEqual(actual,pack(manifest,image.read_bytes(),1 if kind=='module' else 2,seed))
        # Default source mode retains generic metadata; strict profile is opt-in.
        extended=dict(APP,metadata={'enabled':True,'answer':42,'tags':['x']})
        image=ROOT/'build/hello.elf';output=self.folder/'metadata.sfk'
        p=self.run_cli('app','--manifest',self.source(extended),'--image',image,'--seed',seed_path,'--out',output)
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(output.read_bytes(),pack(extended,image.read_bytes(),2,seed))

    def test_profile_does_not_replace_mapped_budget_validation(self):
        # Schema validation alone cannot know how much memory the ELF maps.
        small=dict(APP,resource_limits={'memory':1})
        p=self.run_cli('validate-manifest',self.source(small),'--kind','app')
        self.assertEqual(p.returncode,0,p.stderr)
        seed_path=self.folder/'test.seed';seed_path.write_bytes(bytes(range(32)))
        output=self.folder/'underbudget.sfk';output.write_bytes(b'keep')
        p=self.run_cli('app','--manifest',self.source(small),'--image',ROOT/'build/hello.elf',
                       '--seed',seed_path,'--out',output,'--strict-manifest')
        self.assertNotEqual(p.returncode,0)
        self.assertIn('C loader rejected',p.stderr)
        self.assertEqual(output.read_bytes(),b'keep')


if __name__=='__main__':
    unittest.main(verbosity=2)
