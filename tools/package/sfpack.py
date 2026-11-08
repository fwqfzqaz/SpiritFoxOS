"""SFMOD1/SFK1 authoring. Native static and relative-only subset of design v0.2."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
import trust_policy
import manifest_profile

ROOT = Path(__file__).resolve().parents[2]
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
        self.lib.sf_trust_check.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_uint32]
        self.lib.sf_trust_policy_check.argtypes = [C.c_void_p, C.c_uint32]
        self.lib.sf_cap_check.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_char_p]
        self.lib.sf_ed_check.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t]
        self.lib.sf_hash.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
        self.lib.sf_fs_check.argtypes = [C.c_void_p, C.c_size_t, C.c_int]
        self.lib.sf_check_set.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_uint]
        self.lib.sf_runtime_trust_check.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_uint, C.c_void_p, C.c_uint]
        self.lib.sf_runtime_policy_check.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_uint32, C.c_void_p, C.c_uint]
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

    def verify_policy(self, data, policy):
        trust_policy.validate(policy)
        keys = b''.join(struct.pack('<32sII', bytes.fromhex(k['public_key']), sum(trust_policy.USAGE[x] for x in k['usage']), int(k['revoked'])) for k in policy['keys'])
        return self.lib.sf_trust_check(data, len(data), keys, len(policy['keys']))


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


def elf_segments(image, relative=False):
    if len(image) < 64 or image[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('Expected little-endian ELF64')
    fields = struct.unpack_from('<HHIQQQIHHHHHH', image, 16)
    if fields[0] != (3 if relative else 2) or fields[1] != 62 or fields[2] != 1 or fields[8] != 56:
        raise ValueError('Expected x86_64 '+('PIE' if relative else 'static executable'))
    if fields[4] > len(image) or fields[9]*56 > len(image)-fields[4]:
        raise ValueError('ELF program table out of bounds')
    origin = 0 if relative else BASE
    segments = []
    for i in range(fields[9]):
        typ, flags, off, va, pa, size, memory, align = struct.unpack_from('<IIQQQQQQ', image, fields[4]+i*56)
        if typ in (3,7):
            raise ValueError('ELF interpreter/TLS is unsupported')
        if typ == 2 and relative:
            if off > len(image) or size > len(image)-off or size % 16:
                raise ValueError('ELF dynamic table out of bounds')
            for pos in range(off,off+size,16):
                tag,value = struct.unpack_from('<qQ',image,pos)
                if tag == 0:
                    break
                if tag in (1,17,18,19,22,23,35,36,37) or (tag == 2 and value) or (tag == 30 and value & 4):
                    raise ValueError('ELF imports or non-relative linking is unsupported')
        if typ != 1 or not memory:
            continue
        if flags not in (4,5,6) or va < origin or memory > 0x200000 or va-origin > 0x200000-memory or size > memory or off > len(image) or size > len(image)-off or va % 4096:
            raise ValueError('ELF segment outside native profile')
        kind = {5:1,4:2,6:3}[flags]
        segments.append((kind,{5:5,4:1,6:3}[flags],va-origin,memory,image[off:off+size]))
    if not segments or len(segments)>8:
        raise ValueError('Expected 1..8 mapped segments')
    return fields[3]-origin,segments


def elf_relative_records(image):
    fields = struct.unpack_from('<HHIQQQIHHHHHH',image,16)
    shoff,entsize,count = fields[5],fields[10],fields[11]
    if entsize != 64 or not count or shoff > len(image) or count*64 > len(image)-shoff:
        raise ValueError('Expected bounded ELF section table')
    records = []
    for i in range(count):
        section = struct.unpack_from('<IIQQQQIIQQ',image,shoff+i*64)
        typ,off,size,stride = section[1],section[4],section[5],section[9]
        if typ in (9,19):
            raise ValueError('Only explicit RELA relocations are supported')
        if typ not in (4,11):
            continue
        if off > len(image) or size > len(image)-off or stride != 24 or size % 24:
            raise ValueError('ELF relocation/symbol table out of bounds')
        for pos in range(off,off+size,24):
            if typ == 11:
                name,info,other,shndx,value,symbol_size = struct.unpack_from('<IBBHQQ',image,pos)
                if shndx == 0 and (name or info or other or value or symbol_size):
                    raise ValueError('ELF imports are unsupported')
                continue
            target,info,addend = struct.unpack_from('<QQq',image,pos)
            if info != 8:
                raise ValueError('Only symbol-free R_X86_64_RELATIVE is supported')
            records.append((target,addend))
            if len(records)>256:
                raise ValueError('Relative relocation limit is 256')
    records.sort()
    if len({target for target,_ in records}) != len(records):
        raise ValueError('Duplicate relative relocation targets')
    return b''.join(struct.pack('<Qq',target,addend) for target,addend in records)


def elf_symbols(image):
    """Read defined symbols from .symtab (preferred) or .dynsym."""
    fields = struct.unpack_from('<HHIQQQIHHHHHH', image, 16)
    shoff, entsize, count = fields[5], fields[10], fields[11]
    if entsize != 64 or not count or shoff > len(image) or count*64 > len(image)-shoff:
        raise ValueError('Expected bounded ELF section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', image, shoff+i*64) for i in range(count)]
    table = None
    for want in (2, 11):
        for section in sections:
            typ, off, size, link, stride = section[1], section[4], section[5], section[6], section[9]
            if typ == want and stride == 24 and off <= len(image) and size <= len(image)-off:
                table = (off, size, link)
                break
        if table:
            break
    if not table:
        raise ValueError('ELF symbol table is required for imports/exports')
    off, size, link = table
    if link >= count:
        raise ValueError('ELF symbol table string link out of bounds')
    string = sections[link]
    soff, ssize = string[4], string[5]
    if soff > len(image) or ssize > len(image)-soff:
        raise ValueError('ELF string table out of bounds')
    symbols = {}
    for pos in range(off, off+size, 24):
        name, info, other, shndx, value, symbol_size = struct.unpack_from('<IBBHQQ', image, pos)
        if not shndx or not name:
            continue
        if name >= ssize:
            raise ValueError('ELF symbol name out of bounds')
        end = image.find(b'\0', soff+name, soff+ssize)
        if end < 0:
            raise ValueError('Unterminated ELF symbol name')
        text = image[soff+name:end].decode('ascii', 'strict')
        symbols.setdefault(text, (info, shndx, value, symbol_size))
    return symbols


def _identifier(value, limit=31):
    if not isinstance(value, str) or not 1 <= len(value) <= limit:
        raise ValueError('Bounded identifier required')
    if not (value[0].isalpha() or value[0] == '_') or not all(c.isalnum() or c == '_' for c in value):
        raise ValueError('Identifier must be a C symbol name')
    return value


def _writable(segments, rva):
    return any(kind == 3 and rva >= start and rva-start+8 <= memory
               for kind, _flags, start, memory, _data in segments)


def _executable(segments, rva):
    return any(kind == 1 and rva >= start and rva-start < memory
               for kind, _flags, start, memory, _data in segments)


def native_imports(manifest, image, segments):
    imports = manifest.get('native_imports')
    if imports is None:
        return None
    if not isinstance(imports, list) or not 1 <= len(imports) <= 32:
        raise ValueError('native_imports must contain 1..32 entries')
    if 'import_relocations' in manifest:
        raise ValueError('sfpack owns the generated import relocation subtable')
    symbols = elf_symbols(image)
    result, seen_name, seen_slot = [], set(), set()
    for entry in imports:
        if not isinstance(entry, dict) or set(entry) != {'name', 'abi', 'slot'}:
            raise ValueError('Import entries require exactly name, abi and slot')
        name, abi, slot = (_identifier(entry['name']), entry['abi'], _identifier(entry['slot']))
        if abi != 'libsf-posix-1':
            raise ValueError('Unsupported import ABI')
        if name in seen_name:
            raise ValueError('Duplicate import name')
        symbol = symbols.get(slot)
        if not symbol:
            raise ValueError('Missing import slot symbol ' + slot)
        info, _shndx, value, size = symbol
        if (info >> 4) not in (1, 2) or (info & 15) != 1 or size != 8:
            raise ValueError('Import slot must be a global 8-byte object')
        if value % 8 or not _writable(segments, value):
            raise ValueError('Import slot must be aligned in writable memory')
        if value in seen_slot:
            raise ValueError('Duplicate import slot address')
        seen_name.add(name)
        seen_slot.add(value)
        result.append({'name': name, 'abi': abi, 'slot_rva': value})
    return result


def native_exports(manifest, image, segments):
    exports = manifest.get('native_exports')
    if exports is None:
        return None
    if not isinstance(exports, list) or not 1 <= len(exports) <= 64:
        raise ValueError('native_exports must contain 1..64 entries')
    symbols = elf_symbols(image)
    result, seen = [], set()
    for entry in exports:
        if not isinstance(entry, dict) or set(entry) != {'name', 'abi'}:
            raise ValueError('Export entries require exactly name and abi')
        name, abi = _identifier(entry['name']), entry['abi']
        if abi != 'libsf-posix-1':
            raise ValueError('Unsupported export ABI')
        if name in seen:
            raise ValueError('Duplicate export name')
        symbol = symbols.get(name)
        if not symbol:
            raise ValueError('Missing export symbol ' + name)
        info, _shndx, value, _size = symbol
        if (info >> 4) not in (1, 2) or (info & 15) != 2:
            raise ValueError('Exports must be global functions')
        if not _executable(segments, value):
            raise ValueError('Export must live in executable memory')
        seen.add(name)
        result.append({'name': name, 'abi': abi, 'rva': value})
    return result


def native_library(manifest):
    library = manifest.get('native_library')
    if not isinstance(library, str) or not 1 <= len(library) <= 63:
        raise ValueError('native_library must name the runtime library module')
    if not all(c.isalnum() or c in '._-' for c in library):
        raise ValueError('native_library must be a module identifier')
    return library


def seal(blob, seed, crypto=None):
    crypto = crypto or Crypto()
    result = bytearray(blob)
    sig_off = struct.unpack_from('<Q', result, 64)[0]
    _, pub = crypto.key(seed)
    struct.pack_into('<32sHHI', result, sig_off, hashlib.sha256(pub).digest(), 1, 0, 64)
    result[-64:] = crypto.sign(seed, DOMAIN+hashlib.sha256(result[:-64]).digest())
    return bytes(result)


def pack(manifest, image, kind, seed):
    relative = manifest.get('native_profile') == 'relative-v1'
    role = manifest.get('native_role')
    if role not in (None, 'library'):
        raise ValueError('native_role must be library')
    if role == 'library' and (kind != 1 or not relative):
        raise ValueError('Library images must be relative-v1 modules')
    if manifest.get('native_imports') is not None and not relative:
        raise ValueError('Import tables require the relative-v1 profile')
    if manifest.get('native_exports') is not None and not relative:
        raise ValueError('Export tables require the relative-v1 profile')
    if relative and kind != 2 and not (kind == 1 and role == 'library'):
        raise ValueError('PIE profile is for native applications and runtime libraries only')
    entry, segments = elf_segments(image,relative)
    if relative:
        records = elf_relative_records(image)
        imports = native_imports(manifest, image, segments)
        exports = native_exports(manifest, image, segments)
        if imports and kind != 2:
            raise ValueError('Import tables are for native applications only')
        if exports and (kind != 1 or role != 'library'):
            raise ValueError('Export tables require a library module')
        if role == 'library' and (not exports or imports):
            raise ValueError('A runtime library exports symbols and imports none')
        if imports:
            native_library(manifest)
        manifest = dict(manifest)
        for key in ('relative_relocations','import_relocations'):
            if key in manifest:
                raise ValueError('sfpack owns generated relocation subtables')
        manifest['relative_relocations'] = {'section':len(segments),'offset':0,'bytes':len(records)}
        import_records = b''
        if imports:
            manifest['native_imports'] = imports
            order = sorted(range(len(imports)), key=lambda i: imports[i]['slot_rva'])
            import_records = b''.join(struct.pack('<QII', imports[i]['slot_rva'], i, 0)
                                      for i in order)
            manifest['import_relocations'] = {'section':len(segments),'offset':len(records),
                                              'bytes':len(import_records)}
        if exports:
            manifest['native_exports'] = exports
        segments.append((4,0,0,len(records)+len(import_records),records+import_records))
    encoded = cbor(manifest)
    result = bytearray(96+64*len(segments))
    moff = len(result)
    result.extend(encoded)
    for i, (typ, flags, rva, memory, data) in enumerate(segments):
        alignment = 4096 if typ<=3 else 8
        result.extend(b'\0' * (-len(result) % alignment))
        off = len(result)
        result.extend(data)
        struct.pack_into('<IIQQQQQ16s', result, 96+64*i, typ, flags, off, len(data), memory, rva, alignment, bytes(16))
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


def source_manifest(path, kind, strict=False):
    """Reject ambiguous/unbounded JSON before reading ELF or signing material."""
    manifest = manifest_profile.load(path)
    if strict:
        manifest_profile.validate(manifest, kind)
    if len(cbor(manifest)) > manifest_profile.MAX_ENCODED_BYTES:
        raise ValueError('Manifest exceeds 64 KiB encoded limit')
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    for name in ['module', 'app']:
        s = sub.add_parser(name)
        for flag in ['manifest', 'image', 'out', 'seed']:
            s.add_argument('--'+flag, type=Path, required=True)
        s.add_argument('--strict-manifest', action='store_true',
                       help='require the native-source-v1 authoring profile')
    s = sub.add_parser('validate-manifest', help='validate source without ELF, keys or a C library')
    s.add_argument('file', type=Path)
    s.add_argument('--kind', choices=['module', 'app'], required=True)
    s = sub.add_parser('manifest-schema', help='export the native-source-v1 JSON Schema')
    s.add_argument('--out', type=Path)
    s = sub.add_parser('inspect')
    s.add_argument('file', type=Path)
    group = s.add_mutually_exclusive_group(required=True)
    group.add_argument('--public-key', type=Path)
    group.add_argument('--trust-policy', type=Path)
    s = sub.add_parser('trust')
    s.add_argument('--policy', type=Path, required=True)
    s = sub.add_parser('sign')
    s.add_argument('file', type=Path)
    s.add_argument('--seed', type=Path, required=True)
    a = p.parse_args()
    if a.command in ('module', 'app'):
        manifest = source_manifest(a.manifest, a.command, a.strict_manifest)
        a.out.write_bytes(pack(manifest, a.image.read_bytes(), 1 if a.command == 'module' else 2, a.seed.read_bytes()))
    elif a.command == 'validate-manifest':
        manifest = source_manifest(a.file, a.kind, True)
        print(json.dumps({'profile':'native-source-v1', 'kind':a.kind,
                          'manifest_bytes':len(cbor(manifest)), 'status':'valid',
                          'validation':'source-profile-only'}))
    elif a.command == 'manifest-schema':
        text = json.dumps(manifest_profile.schema(), indent=2)+'\n'
        if a.out:
            a.out.write_text(text, encoding='utf-8')
        else:
            print(text, end='')
    elif a.command == 'sign':
        a.file.write_bytes(seal(a.file.read_bytes(), a.seed.read_bytes()))
    elif a.command == 'trust':
        print(json.dumps(trust_policy.describe(trust_policy.load(a.policy)), indent=2))
    else:
        data = a.file.read_bytes()
        status = Crypto().verify_policy(data, trust_policy.load(a.trust_policy)) if a.trust_policy else Crypto().verify(data, a.public_key.read_bytes())
        if status:
            raise SystemExit('Rejected package: ' + str(status))
        sig = struct.unpack_from('<Q', data, 64)[0]
        print(json.dumps({'file': str(a.file), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'kind': struct.unpack_from('<H', data, 24)[0], 'sections': struct.unpack_from('<I', data, 36)[0], 'key_id': data[sig:sig+32].hex(), 'signature': 'verified', 'verification': 'trust-policy' if a.trust_policy else 'explicit-public-key'}, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit('sfpack: '+str(error)) from None
