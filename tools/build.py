"""Build entirely inside the workspace using the pinned local Zig C toolchain."""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import subprocess
import sys
from sfpack import Crypto, pack, bundle
from mkreadfs import create as create_readfs

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


def main():
    print('Building host package verifier', flush=True)
    run(['-target', 'x86_64-windows-gnu', '-shared', '-O2', '-Iinclude', '-Ivendor/monocypher', 'tools/crypto_host.c', 'services/readfs.c', *CRYPTO, '-o', 'build/sfcrypto.dll'])
    keys = ROOT / '.keys'
    keys.mkdir(exist_ok=True)
    seedfile = keys / 'development.seed'
    if not seedfile.exists():
        seedfile.write_bytes(os.urandom(32))
    seed = seedfile.read_bytes()
    pub = Crypto().key(seed)[1]
    Path('build/development.pub').write_bytes(pub)
    Path('build/trust.h').write_text('static const unsigned char sf_trust_key[32]={' + ','.join(map(str, pub)) + '};\n')
    print('Building userspace packages', flush=True)
    shared = objects('user-common', ['services/start.S', 'services/memory.c', 'services/libsf.c'])
    init = shared + objects('init', ['services/init.c', 'services/storage.c', 'services/readfs.c', 'services/manager.c', 'services/namespace.c', *CRYPTO])
    packages = []
    image = link_elf('sf-init', init, 'services/user.ld')
    manifest = {'module_id': 'org.spiritfox.init', 'version': '0.1.0', 'category': 'bootstrap', 'requested_caps': ['bootstrap'], 'resource_limits': {'memory': 2097152}}
    packages.append(('sf-init.sfmod', pack(manifest, image, 1, seed)))
    disk_files = {'/etc/readme.txt': b'SpiritFoxOS read-only disk mounted through user-space virtio-blk.\n'}
    for name, role, kind in [('echo', 1, 1), ('sf-native', 2, 1), ('hello', 3, 2), ('fault', 4, 2), ('illegal', 5, 2), ('spinner', 6, 2), ('nx', 7, 2), ('probe', 8, 2), ('disk-echo', 1, 1), ('diskhello', 9, 2)]:
        native_crypto = objects('native-crypto', CRYPTO) if role == 2 else []
        image = link_elf(name, shared + native_crypto + objects(name, ['services/program.c'], defines=['-DROLE='+str(role)]), 'services/user.ld')
        manifest = {('module_id' if kind == 1 else 'app_id'): 'org.spiritfox.'+('native' if name == 'sf-native' else name), 'version': '0.1.0', 'requested_caps': ['stdio'] if role in (3, 9) else [], 'resource_limits': {'memory': 262144 if role == 2 else 131072}}
        if kind == 1:
            manifest['category'] = 'runtime' if role == 2 else 'service'
        else:
            manifest['runtime_id'] = 'org.spiritfox.native'
            manifest['app_abi'] = 1
        package_name = name+('.sfmod' if kind == 1 else '.sfk')
        package_bytes = pack(manifest, image, kind, seed)
        if name in ('disk-echo', 'diskhello'):
            disk_files[('/modules/' if kind == 1 else '/bin/')+package_name] = package_bytes
            Path('build/'+package_name).write_bytes(package_bytes)
        else:
            packages.append((package_name, package_bytes))
    image = link_elf('sf-virtio-blk', shared + objects('virtio-blk', ['services/virtio_blk.c']), 'services/user.ld')
    packages.append(('sf-virtio-blk.sfmod', pack({'module_id': 'org.spiritfox.virtio.blk', 'version': '0.1.0', 'category': 'driver', 'requested_caps': ['device.virtio-blk.trusted'], 'resource_limits': {'memory': 131072}}, image, 1, seed)))
    Path('build/storage.img').write_bytes(create_readfs(disk_files))
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
    kernel = objects('kernel', ['kernel/entry.S', 'kernel/arch.c', 'kernel/core.c', 'kernel/memory.c', 'kernel/ipc.c', 'kernel/device.c', 'services/memory.c', 'build/interrupts.S'])
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
    run(['-target', 'x86_64-windows-gnu', '-nostdlib', '-s', '-Wl,--entry,_start', '-Wl,--subsystem,efi_application', '-Wl,--image-base,0x10000000', *efi, '-o', str(esp / 'BOOTX64.EFI')])
    print('Build complete:', (esp / 'BOOTX64.EFI').resolve(), flush=True)


if __name__ == '__main__':
    main()
