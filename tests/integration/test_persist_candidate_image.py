"""Build real signed candidate fixtures and run the in-memory host inspection."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import struct

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
import build
sys.path.insert(0, str(ROOT/'tools'/'package'))
from sfpack import Crypto, elf_symbols, pack
from rust_archive import stack_probe_member


def key_id(package):
    offset = struct.unpack_from('<Q', package, 64)[0]
    assert offset+32 <= len(package)
    return package[offset:offset+32]


def module_manifest(module_id='org.spiritfox.persist', category='service'):
    return {'module_id': module_id, 'version': '0.1.0', 'category': category,
            'requested_caps': [], 'resource_limits': {'memory': 524288}}


def prepare_fixtures(folder):
    persist_image = (ROOT/'build/sf-persist.elf').read_bytes()
    dev_seed = (ROOT/'.keys/development.seed').read_bytes()
    app_seed = (ROOT/'.keys/application.seed').read_bytes()
    assert len(dev_seed) == len(app_seed) == 32
    crypto = Crypto()
    dev_pub, app_pub = crypto.key(dev_seed)[1], crypto.key(app_seed)[1]
    assert dev_pub != app_pub
    built_dev = (ROOT/'build/development.pub').read_bytes()
    built_app = (ROOT/'build/application.pub').read_bytes()
    assert built_dev == dev_pub and built_app == app_pub
    (folder/'development.pub').write_bytes(dev_pub)
    (folder/'application.pub').write_bytes(app_pub)

    cases = {
        'valid': pack(module_manifest(), persist_image, 1, dev_seed),
        'wrong_id': pack(module_manifest('org.spiritfox.not-persist'), persist_image, 1, dev_seed),
        'wrong_category': pack(module_manifest(category='driver'), persist_image, 1, dev_seed),
        'module_wrong_usage': pack(module_manifest(), persist_image, 1, app_seed),
    }
    app_manifest = {'app_id': 'org.spiritfox.persist-app', 'version': '0.1.0',
                    'runtime_id': 'org.spiritfox.native', 'app_abi': 1,
                    'requested_caps': [], 'resource_limits': {'memory': 524288}}
    cases['app_kind'] = pack(app_manifest, persist_image, 2, app_seed)

    # A real signed relative-v1 library module has the right service identity,
    # so the C helper reaches and rejects the unsupported profile/role.
    library_elf = ROOT/'build/sf-libsf.elf'
    assert library_elf.is_file(), 'Build sf-libsf before running the required relative-profile rejection fixture'
    if library_elf.is_file():
        available = elf_symbols(library_elf.read_bytes())
        exports = tuple(name for name in (
            'open', 'read', 'write', 'close', 'lseek', 'fstat', 'stat', 'getpid',
            '_exit', 'sleep', 'printf', 'puts', 'getenv', '__errno_location',
            'strerror', 'malloc', 'free', 'calloc', 'realloc') if name in available)
        assert exports, 'existing relative library ELF has no available export symbols'
        relative_manifest = module_manifest()
        relative_manifest.update(native_profile='relative-v1', native_role='library',
                                 native_exports=[{'name': name, 'abi': 'libsf-posix-1'}
                                                 for name in exports])
        cases['relative'] = pack(relative_manifest, library_elf.read_bytes(), 1, dev_seed)

    valid = cases['valid']
    corrupt = bytearray(valid);corrupt[-1] ^= 1
    cases['bad_signature'] = bytes(corrupt)
    cases['truncated'] = valid[:-1]
    records = []
    for name, data in cases.items():
        (folder/(name+'.pkg')).write_bytes(data)
        records.append(f'{name} {len(data)} {hashlib.sha256(data).hexdigest()} {key_id(data).hex()}')
    (folder/'expected.txt').write_text('\n'.join(records)+'\n', encoding='utf-8', newline='\n')
    return tuple(cases)


def private_rust_library(folder):
    # Keep the adapted archive isolated from the shared build/rust cache.
    source = folder/'rust-source'
    shutil.copytree(ROOT/'rust/sfcore', source,
                    ignore=shutil.ignore_patterns('target', '__pycache__', '*.pyc'))
    target = folder/'rust-target'
    env = dict(os.environ, CARGO_TARGET_DIR=str(target))
    cargo = build.find_cargo()
    subprocess.run([cargo, 'build', '--offline', '--release', '--target', build.RUST_TARGET['host'],
                    '--manifest-path', str(source/'Cargo.toml')], cwd=source, env=env, check=True)
    archive = target/build.RUST_TARGET['host']/'release'/'libsfcore.a'
    if not archive.is_file():
        raise FileNotFoundError('private host Rust archive was not produced: '+str(archive))
    if build.RUST_TARGET['host'] == 'x86_64-pc-windows-gnu':
        # Match build.rust_lib('host'): Zig supplies compiler-rt's probe, so
        # remove the private Rust archive's duplicate strong stack probe.
        compatible = archive.with_name('libsfcore-zig.a')
        shutil.copyfile(archive, compatible)
        member = stack_probe_member(archive.read_bytes())
        if member:
            subprocess.run([build.ZIG, 'ar', 'd', str(compatible), member], check=True)
        return compatible
    return archive


def main():
    build_dir = ROOT/'build'
    with tempfile.TemporaryDirectory(prefix='persist-candidate-image-', dir=build_dir) as temp:
        folder = Path(temp)
        cases = prepare_fixtures(folder)
        rust_archive = private_rust_library(folder)
        executable = folder/('test_persist_candidate_image'+build.EXE)
        sources = ['tests/host/test_persist_candidate_image.c',
                   'services/persist_candidate.c', 'services/native_image.c',
                   'services/launch_policy.c', *build.CRYPTO]
        build.run(['-target', build.HOST, '-g0', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-parameter', '-DSF_HOST_TEST', '-Iinclude', '-Ibuild',
                   '-Iservices', '-Ivendor/monocypher', *sources, str(rust_archive),
                   '-o', str(executable)])
        subprocess.run([str(executable), str(folder)], cwd=ROOT, check=True)
        print('PERSIST CANDIDATE IMAGE E1 PASS: '+str(len(cases))+
              ' Python-signed fixtures; actual host signature/profile/image path; private Rust archive')


if __name__ == '__main__':
    main()
