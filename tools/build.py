"""Build entirely inside the workspace using the pinned local Zig C toolchain."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
from sfpack import Crypto, pack, bundle
from mkreadfs import create as create_readfs
from mkpersist import create as create_persist
import trust_policy

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
(ROOT / 'build').mkdir(exist_ok=True)
(ROOT / '.cache').mkdir(exist_ok=True)
for key in ['TEMP', 'TMP', 'TMPDIR', 'XDG_CACHE_HOME', 'ZIG_GLOBAL_CACHE_DIR', 'ZIG_LOCAL_CACHE_DIR']:
    os.environ[key] = str(ROOT / '.cache' / key.lower())
    Path(os.environ[key]).mkdir(exist_ok=True)
os.environ['PYTHONPYCACHEPREFIX'] = str(ROOT / '.cache' / 'pycache')
ZIG = str(ROOT / '.tools/zig/zig.exe')
COMMON = ['-Iinclude', '-Ivendor/monocypher', '-Ibuild', '-g0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2']
CRYPTO = ['services/package.c', 'services/sha256.c', 'vendor/monocypher/monocypher.c', 'vendor/monocypher/monocypher-ed25519.c']


def run(args):
    result = subprocess.run([ZIG, 'cc', *args], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout)
        raise RuntimeError('Compilation failed')


def compile_one(item):
    source, target, out, defines = item
    run(['-target', target, *COMMON, *defines, '-c', source, '-o', out])
    return out


def objects(name, sources, target='x86_64-freestanding-none', defines=()):
    folder = ROOT / 'build' / name
    folder.mkdir(exist_ok=True)
    items = [(s, target, str(folder / (s.replace('/', '_')+'.o')), list(defines)) for s in sources]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        return list(pool.map(compile_one, items))


def link_elf(name, objs, script):
    out = 'build/'+name+'.elf'
    entry = '_start64' if script.startswith('kernel/') else '_sf_start'
    run(['-target', 'x86_64-freestanding-none', '-nostdlib', '-static', '-Wl,-e,'+entry, '-Wl,-T,'+script, '-Wl,--build-id=none', *objs, '-o', out])
    return Path(out).read_bytes()


def main(policy_path=None):
    print('Building host package verifier', flush=True)
    run(['-target', 'x86_64-windows-gnu', '-shared', '-O2', '-Iinclude', '-Ivendor/monocypher', 'tools/crypto_host.c', 'services/readfs.c', 'services/native_image.c', *CRYPTO, '-o', 'build/sfcrypto.dll'])
    keys = ROOT / '.keys'
    keys.mkdir(exist_ok=True)
    seedfile = keys / 'development.seed'
    if not seedfile.exists():
        seedfile.write_bytes(os.urandom(32))
    seed = seedfile.read_bytes()
    pub = Crypto().key(seed)[1]
    Path('build/development.pub').write_bytes(pub)
    application_seedfile = keys / 'application.seed'
    if not application_seedfile.exists():
        application_seedfile.write_bytes(os.urandom(32))
    application_seed = application_seedfile.read_bytes()
    application_pub = Crypto().key(application_seed)[1]
    if application_pub == pub:
        raise ValueError('Module and application signing keys must be distinct')
    Path('build/application.pub').write_bytes(application_pub)
    explicit_policy = policy_path is not None
    policy_path = Path(policy_path) if explicit_policy else keys / 'trust-policy.json'
    policy_path = policy_path.resolve()
    if not policy_path.is_relative_to(ROOT):
        raise ValueError('Trust policy must be inside the workspace')
    policy = trust_policy.load(policy_path) if explicit_policy or policy_path.exists() else trust_policy.development(pub, application_pub)
    for signer, usage in [(pub, 'module'), (application_pub, 'app')]:
        if not any(k['public_key'] == signer.hex() and not k['revoked'] and usage in k['usage'] for k in policy['keys']):
            raise ValueError('Current build requires an active ' + usage + ' signing key in the trust policy')
    Path('build/trust.h').write_text(trust_policy.header(policy))
    Path('build/trust-policy.json').write_text(json.dumps(policy, indent=2)+'\n')
    print('Building userspace packages', flush=True)
    shared = objects('user-common', ['services/start.S', 'services/memory.c', 'services/libsf.c', 'services/arguments.c'])
    init = shared + objects('init', ['services/init.c', 'services/native_image.c', 'services/argparse.c', 'services/storage.c', 'services/readfs.c', 'services/txn.c', 'services/update.c', 'services/version.c', 'services/persistence.c', 'services/manager.c', 'services/namespace.c', *CRYPTO])
    packages = []
    image = link_elf('sf-init', init, 'services/user.ld')
    manifest = {'module_id': 'org.spiritfox.init', 'version': '0.1.0', 'category': 'bootstrap', 'requested_caps': ['bootstrap'], 'resource_limits': {'memory': 2097152}}
    packages.append(('sf-init.sfmod', pack(manifest, image, 1, seed)))
    disk_files = {'/etc/readme.txt': b'SpiritFoxOS read-only disk mounted through user-space virtio-blk.\n'}
    for name, role, kind in [('echo', 1, 1), ('sf-native', 2, 1), ('hello', 3, 2), ('fault', 4, 2), ('illegal', 5, 2), ('spinner', 6, 2), ('nx', 7, 2), ('probe', 8, 2), ('mmio', 10, 2), ('exitbusy', 12, 2), ('badrsp', 13, 2), ('fileprobe', 14, 2), ('readprobe', 15, 2), ('argprobe', 16, 2), ('disk-echo', 1, 1), ('diskhello', 9, 2), ('diskupdate', 9, 2), ('disknext', 9, 2), ('diskfault', 4, 2), ('diskexit', 11, 2), ('diskspin', 6, 2), ('diskother', 9, 2)]:
        native_crypto = objects('native-crypto', CRYPTO) if role == 2 else []
        image = link_elf(name, shared + native_crypto + objects(name, ['services/program.c'], defines=['-DROLE='+str(role)]), 'services/user.ld')
        manifest = {('module_id' if kind == 1 else 'app_id'): 'org.spiritfox.'+('native' if name == 'sf-native' else name), 'version': '0.1.0', 'requested_caps': ['stdio'] if role in (3, 9) else [], 'resource_limits': {'memory': 262144 if role == 2 else 131072}}
        if name in ('fileprobe','readprobe','fault','spinner','argprobe'):
            manifest['requested_caps'] = ['stdio','namespace.read']
        if name in ('diskfault', 'diskexit', 'diskspin', 'disknext', 'diskupdate'):
            manifest.update(app_id='org.spiritfox.diskhello', version='0.2.0' if name=='diskupdate' else '0.3.0')
        if kind == 1:
            manifest['category'] = 'runtime' if role == 2 else 'service'
        else:
            manifest['runtime_id'] = 'org.spiritfox.native'
            manifest['app_abi'] = 1
        package_name = name+('.sfmod' if kind == 1 else '.sfk')
        signing_seed = seed if kind == 1 else application_seed
        package_bytes = pack(manifest, image, kind, signing_seed)
        if name=='disknext':
            for label,version in [('short','0.3'),('leading','00.3.0'),('overflow','4294967296.0.0'),('suffix','0.3.0-alpha')]:
                Path('build/version-'+label+'.sfk').write_bytes(pack(dict(manifest,version=version),image,kind,signing_seed))
        if name.startswith('disk'):
            disk_files[('/modules/' if kind == 1 else '/bin/')+package_name] = package_bytes
            Path('build/'+package_name).write_bytes(package_bytes)
        else:
            packages.append((package_name, package_bytes))
    pie_shared = objects('pie-common', ['services/start.S','services/memory.c','services/libsf.c','services/arguments.c'], defines=['-fpie'])
    pie_objs = pie_shared + objects('pieprobe',['services/pieprobe.c'],defines=['-fpie'])
    run(['-target','x86_64-freestanding-none','-nostdlib','-fpie','-pie','-Wl,-Bsymbolic','-Wl,-e,_sf_start','-Wl,-T,services/user-pie.ld','-Wl,--build-id=none',*pie_objs,'-o','build/pieprobe.elf'])
    pie_manifest = {'app_id':'org.spiritfox.pieprobe','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,'native_profile':'relative-v1','requested_caps':['stdio','namespace.read']}
    pie_package = pack(pie_manifest,Path('build/pieprobe.elf').read_bytes(),2,application_seed)
    packages.append(('pieprobe.sfk',pie_package))
    disk_files['/bin/pieprobe.sfk'] = pie_package
    image = link_elf('sf-virtio-blk', shared + objects('virtio-blk', ['services/virtio_blk.c']), 'services/user.ld')
    packages.append(('sf-virtio-blk.sfmod', pack({'module_id': 'org.spiritfox.virtio.blk', 'version': '0.1.0', 'category': 'driver', 'requested_caps': ['device.virtio-blk.trusted'], 'resource_limits': {'memory': 131072}}, image, 1, seed)))
    Path('build/storage.img').write_bytes(create_readfs(disk_files))
    Path('build/persistent.img').write_bytes(create_persist(disk_files))
    for name, data in packages:
        Path('build/'+name).write_bytes(data)
    Path('build/bundle.bin').write_bytes(bundle(packages))
    errors = {8, 10, 11, 12, 13, 14, 17, 21, 29, 30}
    asm = ['.text']
    for i in range(256):
        asm += [f'.global isr_{i}', f'isr_{i}:']
        if i not in errors:
            asm.append('pushq $0')
        asm += [f'pushq ${i}', 'jmp interrupt_common']
    asm += ['.section .rodata', '.global isr_table', 'isr_table:'] + [f'.quad isr_{i}' for i in range(256)]
    Path('build/interrupts.S').write_text('\n'.join(asm)+'\n')
    print('Building Core 0', flush=True)
    kernel = objects('kernel', ['kernel/entry.S', 'kernel/arch.c', 'kernel/core.c', 'kernel/memory.c', 'kernel/ipc.c', 'kernel/device.c', 'kernel/acpi.c', 'kernel/vtd_tables.c', 'kernel/iommu.c', 'services/memory.c', 'services/arguments.c', 'build/interrupts.S'])
    link_elf('kernel', kernel, 'kernel/linker.ld')
    # Content-addressed incbin inputs prevent assembler caches reusing old bytes.
    embedded = {}
    for source in ['kernel.elf', 'bundle.bin']:
        data = Path('build/'+source).read_bytes()
        name = 'build/'+hashlib.sha256(data).hexdigest()[:24]+'-'+source
        Path(name).write_bytes(data)
        embedded[source] = name
    Path('build/payload.S').write_text('.section .rdata,"dr"\n.balign 4096\n.global __kernel_elf_start\n.global __kernel_elf_end\n__kernel_elf_start:\n.incbin "'+embedded['kernel.elf']+'"\n__kernel_elf_end:\n.balign 4096\n.global __bundle_start\n.global __bundle_end\n__bundle_start:\n.incbin "'+embedded['bundle.bin']+'"\n__bundle_end:\n')
    print('Building UEFI image', flush=True)
    efi = objects('efi', ['boot/efi_stub/crt0-efi-x86_64.S', 'boot/efi_stub/efi_main.c', 'build/payload.S', 'services/memory.c', *CRYPTO], 'x86_64-windows-gnu')
    esp = Path('build/esp/EFI/BOOT')
    esp.mkdir(parents=True, exist_ok=True)
    efi_path = esp / 'BOOTX64.EFI'
    run(['-target', 'x86_64-windows-gnu', '-nostdlib', '-s', '-Wl,--entry,_start', '-Wl,--subsystem,efi_application', '-Wl,--image-base,0x10000000', *efi, '-o', str(efi_path)])
    # Zig 0.13's PE linker inserts wall-clock metadata. Normalize the optional
    # timestamp/checksum fields before testing this unsigned development image.
    pe_image = bytearray(efi_path.read_bytes())
    pe = struct.unpack_from('<I', pe_image, 60)[0]
    assert pe_image[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H', pe_image, pe+24)[0] == 0x20b
    struct.pack_into('<I', pe_image, pe+8, 0)
    struct.pack_into('<I', pe_image, pe+24+64, 0)
    efi_path.write_bytes(pe_image)
    print('Build complete:', (esp / 'BOOTX64.EFI').resolve(), flush=True)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trust-policy', type=Path)
    args = parser.parse_args()
    if args.trust_policy and not args.trust_policy.exists():
        parser.error('Explicit trust policy does not exist')
    main(args.trust_policy)
