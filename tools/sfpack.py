"""SFMOD1/SFK1 authoring. Native static-address subset of design v0.2."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x40000000
DOMAIN = b'SpiritFoxOS-Package-v1\0'


class Crypto:
    def __init__(self):
        import os
        name = 'sfcrypto.dll' if os.name == 'nt' else 'sfcrypto.so'
        self.lib = C.CDLL(str(ROOT / 'build' / name))
        self.lib.sf_key_pair.argtypes = [C.c_void_p] * 3
        self.lib.sf_sign.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t]
        self.lib.sf_check.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p]
        self.lib.sf_cap_check.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_char_p]
        self.lib.sf_ed_check.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t]
        self.lib.sf_hash.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
        self.lib.sf_fs_check.argtypes = [C.c_void_p, C.c_size_t, C.c_int]
        self.lib.sf_check_set.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_uint]
        self.lib.sf_trust_check.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_uint]
        self.lib.sf_trust_check_revoked.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_uint, C.c_void_p, C.c_uint]
        self.lib.sf_trust_revoke_id.argtypes = [C.c_void_p, C.c_uint, C.c_void_p]

    def key(self, seed):
        if len(seed) != 32:
            raise ValueError('Ed25519 seed must be 32 bytes')
        secret, pub = C.create_string_buffer(64), C.create_string_buffer(32)
        self.lib.sf_key_pair(secret, pub, C.create_string_buffer(seed))
        return secret.raw, pub.raw

    def sign(self, seed, data):
        secret, _ = self.key(seed)
        sig = C.create_string_buffer(64)
        self.lib.sf_sign(sig, secret, data, len(data))
        return sig.raw

    def verify(self, data, pub):
        if len(pub) != 32:
            raise ValueError('Ed25519 public key must be 32 bytes')
        return self.lib.sf_check(data, len(data), pub)


def cbor(value):
    def head(t, n):
        if n < 24:
            return bytes([t * 32 + n])
        for tag, size in [(24, 1), (25, 2), (26, 4), (27, 8)]:
            if n < 1 << (8 * size):
                return bytes([t * 32 + tag]) + n.to_bytes(size, 'big')
        raise ValueError('CBOR integer overflow')
    if value is None:
        return b'\xf6'
    if isinstance(value, bool):
        return b'\xf5' if value else b'\xf4'
    if isinstance(value, int):
        return head(0 if value >= 0 else 1, value if value >= 0 else -1-value)
    if isinstance(value, str):
        text = value.encode('utf-8')
        if b'\0' in text:
            raise ValueError('NUL text')
        return head(3, len(text)) + text
    if isinstance(value, list):
        return head(4, len(value)) + b''.join(cbor(v) for v in value)
    if isinstance(value, dict):
        if not all(isinstance(k, str) for k in value):
            raise ValueError('Manifest keys must be text')
        pairs = sorted(((cbor(k), cbor(v)) for k, v in value.items()), key=lambda kv: (len(kv[0]), kv[0]))
        return head(5, len(pairs)) + b''.join(k + v for k, v in pairs)
    raise ValueError('Unsupported CBOR type')


def elf_segments(image):
    if len(image) < 64 or image[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('Expected little-endian ELF64')
    fields = struct.unpack_from('<HHIQQQIHHHHHH', image, 16)
    if fields[0] != 2 or fields[1] != 62 or fields[8] != 56:
        raise ValueError('Expected x86_64 static executable')
    segments = []
    for i in range(fields[9]):
        typ, flags, off, va, pa, size, memory, align = struct.unpack_from('<IIQQQQQQ', image, fields[4]+i*56)
        if typ != 1 or not memory:
            continue
        if flags not in (4, 5, 6) or va < BASE or va + memory > BASE+0x200000 or size > memory or off+size > len(image) or va % 4096:
            raise ValueError('ELF segment outside native profile')
        kind = {5: 1, 4: 2, 6: 3}[flags]
        segments.append((kind, {5: 5, 4: 1, 6: 3}[flags], va-BASE, memory, image[off:off+size]))
    if not segments or len(segments) > 8:
        raise ValueError('Expected 1..8 mapped segments')
    return fields[3]-BASE, segments


def seal(blob, seed, crypto=None):
    crypto = crypto or Crypto()
    result = bytearray(blob)
    sig_off = struct.unpack_from('<Q', result, 64)[0]
    _, pub = crypto.key(seed)
    struct.pack_into('<32sHHI', result, sig_off, hashlib.sha256(pub).digest(), 1, 0, 64)
    result[-64:] = crypto.sign(seed, DOMAIN+hashlib.sha256(result[:-64]).digest())
    return bytes(result)


def pack(manifest, image, kind, seed):
    entry, segments = elf_segments(image)
    encoded = cbor(manifest)
    result = bytearray(96+64*len(segments))
    moff = len(result)
    result.extend(encoded)
    for i, (typ, flags, rva, memory, data) in enumerate(segments):
        result.extend(b'\0' * (-len(result) % 4096))
        off = len(result)
        result.extend(data)
        struct.pack_into('<IIQQQQQ16s', result, 96+64*i, typ, flags, off, len(data), memory, rva, 4096, bytes(16))
    result.extend(b'\0' * (-len(result) % 8))
    sig = len(result)
    result.extend(bytes(104))
    magic = b'SFMOD1\0\0' if kind == 1 else b'SFK1\r\n\x1a\n'
    struct.pack_into('<8sHHIQHHHHIIQQQQIIQ8s', result, 0, magic, 1, 0, 96, len(result), kind, 1, 1, 0, 0, len(segments), 96, moff, len(encoded), sig, 104, 0, entry, bytes(8))
    crypto = Crypto()
    signed = seal(result, seed, crypto)
    status = crypto.verify(signed, crypto.key(seed)[1])
    if status:
        raise ValueError('C loader rejected generated package: ' + str(status))
    return signed


def bundle(packages):
    result = bytearray(16+48*len(packages))
    for i, (name, data) in enumerate(packages):
        if len(name.encode()) >= 32:
            raise ValueError('Bundle name too long')
        result.extend(bytes(-len(result) % 4096))
        struct.pack_into('<32sQQ', result, 16+i*48, name.encode(), len(result), len(data))
        result.extend(data)
    struct.pack_into('<4sIQ', result, 0, b'SFB1', len(packages), len(result))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    for name in ['module', 'app']:
        s = sub.add_parser(name)
        for flag in ['manifest', 'image', 'out', 'seed']:
            s.add_argument('--'+flag, type=Path, required=True)
    s = sub.add_parser('inspect')
    s.add_argument('file', type=Path)
    s.add_argument('--public-key', type=Path, required=True)
    s = sub.add_parser('sign')
    s.add_argument('file', type=Path)
    s.add_argument('--seed', type=Path, required=True)
    a = p.parse_args()
    if a.command in ('module', 'app'):
        a.out.write_bytes(pack(json.loads(a.manifest.read_text(encoding='utf-8')), a.image.read_bytes(), 1 if a.command == 'module' else 2, a.seed.read_bytes()))
    elif a.command == 'sign':
        a.file.write_bytes(seal(a.file.read_bytes(), a.seed.read_bytes()))
    else:
        data = a.file.read_bytes()
        status = Crypto().verify(data, a.public_key.read_bytes())
        if status:
            raise SystemExit('Rejected package: ' + str(status))
        print(json.dumps({'file': str(a.file), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'kind': struct.unpack_from('<H', data, 24)[0], 'sections': struct.unpack_from('<I', data, 36)[0], 'signature': 'verified'}, indent=2))


if __name__ == '__main__':
    main()
