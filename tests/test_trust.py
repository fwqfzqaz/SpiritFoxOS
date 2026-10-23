"""Trust decisions use the same C verifier as firmware and both user loaders."""
import copy
import ctypes as C
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from sfpack import Crypto, seal
import trust_policy


class Trust(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.crypto = Crypto()
        cls.seeds = [hashlib.sha256(('TRUST TEST ONLY '+str(i)).encode()).digest() for i in range(10)]
        cls.pubs = [cls.crypto.key(seed)[1] for seed in cls.seeds]
        cls.app = (ROOT/'build/hello.sfk').read_bytes()
        cls.mod = (ROOT/'build/echo.sfmod').read_bytes()

    def key(self, index, usage=('module','app'), revoked=False):
        return {'public_key':self.pubs[index].hex(),'usage':list(usage),'revoked':revoked}

    def policy(self, *keys):
        return {'version':1,'keys':list(keys)}

    def test_multiple_signers_and_usage(self):
        policy = self.policy(self.key(0),self.key(1,('app',)),self.key(2,('module',)))
        for i in range(4):
            for blob, expected in [(self.app,[0,0,-2,-4][i]),(self.mod,[0,-2,0,-4][i])]:
                with self.subTest(signer=i,kind=blob[:8]):
                    self.assertEqual(self.crypto.verify_policy(seal(blob,self.seeds[i]),policy),expected)

    def test_revocation_and_no_fallback(self):
        data = seal(self.app,self.seeds[1])
        for keys in [(self.key(0),self.key(1,revoked=True)),(self.key(1,revoked=True),self.key(0))]:
            self.assertEqual(self.crypto.verify_policy(data,self.policy(*keys)),-13)
        active = self.policy(self.key(0),self.key(1))
        self.assertEqual(self.crypto.verify_policy(data,active),0)
        damaged = bytearray(data)
        damaged[-1] ^= 1
        self.assertEqual(self.crypto.verify_policy(bytes(damaged),active),-4)
        # Relabeling a correctly signed package cannot select another valid key.
        sig = struct.unpack_from('<Q',data,64)[0]
        damaged = bytearray(data)
        damaged[sig:sig+32] = hashlib.sha256(self.pubs[0]).digest()
        self.assertEqual(self.crypto.verify_policy(bytes(damaged),active),-4)

    def test_bounded_c_policy(self):
        raw = lambda i,usage=3,revoked=0: struct.pack('<32sII',self.pubs[i],usage,revoked)
        check = self.crypto.lib.sf_trust_policy_check
        self.assertEqual(check(None,1),-3)
        self.assertEqual(check(raw(0),0),-3)
        self.assertEqual(check(raw(0),9),-3)
        for usage,revoked in [(0,0),(4,0),(0xffffffff,0),(3,2),(3,0xffffffff)]:
            self.assertEqual(check(raw(0,usage,revoked),1),-3)
        self.assertEqual(check(raw(0)+raw(0,1,1),2),-3)
        self.assertEqual(self.crypto.lib.sf_trust_check(self.app,len(self.app),raw(0)+raw(0,1,1),2),-3)
        self.assertEqual(check(b''.join(raw(i) for i in range(8)),8),0)
        self.assertEqual(self.crypto.verify_policy(seal(self.app,self.seeds[7]),self.policy(*(self.key(i) for i in range(8)))),0)

    def test_python_policy_rejection(self):
        good = self.policy(self.key(0))
        invalid = [None,[],{},dict(good,version=True),dict(good,version=2),dict(good,extra=1),dict(good,keys=[]),self.policy(*(self.key(i) for i in range(9))),self.policy(self.key(0),self.key(0,revoked=True))]
        for field,values in {'public_key':['00','G'*64,'A'*64,32], 'usage':[[],['app','app'],['kernel'],'app',[1],[[]]], 'revoked':[0,1,'false',None]}.items():
            for value in values:
                key = self.key(0)
                key[field] = value
                invalid.append(self.policy(key))
        invalid.append(self.policy(dict(self.key(0),private_seed='not-allowed')))
        for policy in invalid:
            with self.subTest(policy=policy),self.assertRaises(ValueError):
                trust_policy.validate(policy)
        folder = ROOT/'.cache/trust-policy-tests'
        folder.mkdir(exist_ok=True)
        path = folder/'invalid.json'
        for content in ['{"version":1,"version":1,"keys":[]}',json.dumps(good).replace('"revoked": false','"revoked": false, "revoked": true'),' '*16385]:
            path.write_text(content,encoding='utf-8')
            with self.assertRaises(ValueError):
                trust_policy.load(path)

    def test_public_policy_roundtrip(self):
        policy = self.policy(self.key(0),self.key(1,('app',),True))
        original = copy.deepcopy(policy)
        desc = trust_policy.describe(policy)
        self.assertEqual(desc['keys'][1]['key_id'],hashlib.sha256(self.pubs[1]).hexdigest())
        self.assertTrue(desc['keys'][1]['revoked'])
        self.assertEqual(policy,original)
        folder = ROOT/'.cache/trust-policy-tests'
        folder.mkdir(exist_ok=True)
        path = folder/'valid.json'
        path.write_text(json.dumps(policy),encoding='utf-8')
        self.assertEqual(trust_policy.load(path),policy)
        command = [sys.executable,str(ROOT/'tools/sfpack.py')]
        result = subprocess.run(command+['trust','--policy',str(path)],capture_output=True,text=True,check=True)
        self.assertEqual(json.loads(result.stdout),desc)
        package = folder/'signed.sfk'
        package.write_bytes(seal(self.app,self.seeds[0]))
        result = subprocess.run(command+['inspect',str(package),'--trust-policy',str(path)],capture_output=True,text=True,check=True)
        self.assertEqual(json.loads(result.stdout)['verification'],'trust-policy')
        package.write_bytes(seal(self.app,self.seeds[1]))
        result = subprocess.run(command+['inspect',str(package),'--trust-policy',str(path)],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('Rejected package: -13',result.stderr)

    def test_seeded_signed_mutations(self):
        rng = random.Random(20261024)
        policy = self.policy(self.key(0),self.key(1,('app',)),self.key(2,revoked=True))
        for _ in range(1000):
            index = rng.randrange(4)
            data = bytearray(seal(self.app,self.seeds[index]))
            data[rng.randrange(len(data))] ^= 1 << rng.randrange(8)
            self.assertLess(self.crypto.verify_policy(bytes(data),policy),0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
