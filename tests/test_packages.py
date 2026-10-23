"""Exercise the same C parser used by UEFI and sf-init, including signed negatives."""
import ctypes as C
import hashlib
from pathlib import Path
import random
import struct
import sys
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from sfpack import Crypto, seal, cbor, pack


class Packages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.crypto = Crypto()
        cls.seed = (ROOT / '.keys/development.seed').read_bytes()
        cls.pub = cls.crypto.key(cls.seed)[1]
        cls.good = (ROOT / 'build/hello.sfk').read_bytes()

    def reject(self, blob, signed=False, status=None):
        if signed:
            blob = seal(blob, self.seed, self.crypto)
        actual = self.crypto.verify(bytes(blob), self.pub)
        self.assertLess(actual, 0)
        if status is not None:
            self.assertEqual(actual, status)

    def test_valid_every_built_package(self):
        for path in list((ROOT / 'build').glob('*.sfmod')) + list((ROOT / 'build').glob('*.sfk')):
            with self.subTest(package=path.name):
                self.assertEqual(self.crypto.verify(path.read_bytes(), self.pub), 0)

    def test_sha256_vectors(self):
        for data in [b'', b'abc', b'a'*55, b'a'*56, b'a'*64, b'a'*1000000]:
            digest = C.create_string_buffer(32)
            self.crypto.lib.sf_hash(digest, data, len(data))
            self.assertEqual(digest.raw, hashlib.sha256(data).digest())

    def test_requested_directory_capability(self):
        manifest = {'app_id':'org.spiritfox.cap-test','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1}
        image = (ROOT/'build/hello.elf').read_bytes()
        for caps, expected in [(None,0),([],0),(['stdio'],0),(['namespace.read'],1),(['namespace.read.extra'],0),(['stdio','namespace.read'],1),('namespace.read',-3),([1],-3),(['namespace.read',1],-3),({'namespace.read':True},-3)]:
            with self.subTest(caps=caps):
                fields = dict(manifest)
                if caps is not None:
                    fields['requested_caps'] = caps
                data = pack(fields,image,2,self.seed)
                self.assertEqual(self.crypto.lib.sf_cap_check(data,len(data),self.pub,b'namespace.read'),expected)
                self.assertEqual(self.crypto.lib.sf_cap_check(data,len(data),bytes(32),b'namespace.read'),-4)

    def test_rfc8032_ed25519_vector(self):
        seed = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
        expected_pub = bytes.fromhex('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
        expected_sig = bytes.fromhex('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
        self.assertEqual(self.crypto.key(seed)[1], expected_pub)
        self.assertEqual(self.crypto.sign(seed, b''), expected_sig)
        self.assertEqual(self.crypto.lib.sf_ed_check(expected_sig, expected_pub, b'', 0), 0)

    def test_header_boundaries(self):
        for off, fmt, value in [(0, '<Q', 0), (8, '<H', 2), (12, '<I', 95), (16, '<Q', len(self.good)+1), (24, '<H', 1), (26, '<H', 2), (28, '<H', 2), (32, '<I', 1), (36, '<I', 65), (40, '<Q', 2**64-1), (48, '<Q', 2**64-1), (56, '<Q', 65537), (64, '<Q', 2**64-1), (72, '<I', 103), (76, '<I', 1), (80, '<Q', 2**64-1), (88, '<Q', 1)]:
            with self.subTest(offset=off):
                data = bytearray(self.good)
                struct.pack_into(fmt, data, off, value)
                self.reject(data)
        self.reject(self.good+b'\0')
        for n in [0, 1, 64, 95, 96, 263, len(self.good)-1]:
            self.reject(self.good[:n])

    def test_sections_and_zero_padding(self):
        for off, value in [(96+4, 7), (96+8, 2**64-1), (96+16, 2**64-1), (96+24, 2**64-1), (96+32, 1), (96+40, 3), (96+48, 1)]:
            data = bytearray(self.good)
            struct.pack_into('<Q' if off != 100 else '<I', data, off, value)
            self.reject(data)
        data = bytearray(self.good)
        # Segment overlaps manifest even though its data is otherwise in bounds.
        struct.pack_into('<Q', data, 104, 0)
        self.reject(data)
        data = bytearray(self.good)
        manifest_end = struct.unpack_from('<Q', data, 48)[0]+struct.unpack_from('<Q', data, 56)[0]
        data[manifest_end] = 1
        self.reject(data, signed=True)

    def test_tamper_and_unknown_signer(self):
        data = bytearray(self.good)
        off = struct.unpack_from('<Q', data, 104)[0]
        data[off] ^= 1
        self.reject(data, status=-4)
        data = bytearray(self.good)
        data[-1] ^= 1
        self.reject(data, status=-4)
        self.assertEqual(self.crypto.verify(self.good, bytes(32)), -4)

    def test_canonical_manifest(self):
        manifest_off = struct.unpack_from('<Q', self.good, 48)[0]
        def replace(encoded):
            data = bytearray(self.good)
            old = struct.unpack_from('<Q', data, 56)[0]
            data[manifest_off:manifest_off+old] = bytes(old)
            data[manifest_off:manifest_off+len(encoded)] = encoded
            struct.pack_into('<Q', data, 56, len(encoded))
            self.reject(data, signed=True, status=-3)
        for bad in [b'\xa2\x61a\x01\x61a\x02', b'\xa2\x61b\x01\x61a\x02', b'\xa1\x61a\x18\x01', b'\xa1\x61a\xf9\x00\x00', b'\xbf\xff', b'\xa1\x61a\xc0\x01', b'\xa1\x61a\x61\xff', b'\xa1\x01\x01', b'\xa1\x61a'+b'\x81'*18+b'\x00']:
            replace(bad)

    def test_seeded_mutations(self):
        rng = random.Random(20261023)
        accepted = 0
        for _ in range(1500):
            data = bytearray(self.good)
            data[rng.randrange(len(data))] ^= 1 << rng.randrange(8)
            accepted += self.crypto.verify(bytes(data), self.pub) == 0
        self.assertEqual(accepted, 0)

    def test_application_abi(self):
        manifest = {'app_id': 'org.spiritfox.test', 'version': '0.1.0', 'runtime_id': 'org.spiritfox.native'}
        for abi in [None, 0, 2, '1', True]:
            current = dict(manifest)
            if abi is not None:
                current['app_abi'] = abi
            with self.assertRaisesRegex(ValueError, 'C loader rejected generated package: -5'):
                pack(current, (ROOT/'build/hello.elf').read_bytes(), 2, self.seed)


if __name__ == '__main__':
    unittest.main(verbosity=2)
