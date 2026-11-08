"""Build entirely inside the workspace using the pinned local Zig C toolchain."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools' / 'package'))
from sfpack import Crypto, pack, bundle
from mkreadfs import create as create_readfs
from mkpersist import create as create_persist
import trust_policy

os.chdir(ROOT)
(ROOT / 'build').mkdir(exist_ok=True)
(ROOT / '.cache').mkdir(exist_ok=True)
for key in ['TEMP', 'TMP', 'TMPDIR', 'XDG_CACHE_HOME', 'ZIG_GLOBAL_CACHE_DIR', 'ZIG_LOCAL_CACHE_DIR']:
    os.environ[key] = str(ROOT / '.cache' / key.lower())
    Path(os.environ[key]).mkdir(exist_ok=True)
os.environ['PYTHONPYCACHEPREFIX'] = str(ROOT / '.cache' / 'pycache')
LINUX = hasattr(os, 'uname') and os.uname().sysname == 'Linux'
ZIG = str(ROOT / ('.tools/zig/zig' if LINUX else '.tools/zig/zig.exe'))
# Host tests run natively; the EFI stub stays PE/COFF through the windows-gnu target.
HOST = 'x86_64-linux-gnu' if LINUX else 'x86_64-windows-gnu'
EXE = '' if LINUX else '.exe'
COMMON = ['-Iinclude', '-Ivendor/monocypher', '-Ibuild', '-g0', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2']
# 用户映像按函数/数据分区后配合 --gc-sections，剔除未引用的静态库成员。
# 这是 512 KiB 暂存区能够容纳 sf-persist（事务卷缓冲）的前提。
USER_SECTIONS = ['-ffunction-sections', '-fdata-sections']
# CRYPTO 移除 sha256.c：SHA-256 已由 rust/sfcore 提供（同符号 sf_sha256）。
CRYPTO = ['services/trust.c', 'services/package.c', 'vendor/monocypher/monocypher.c', 'vendor/monocypher/monocypher-ed25519.c']
# Rust 组件（rust/sfcore）：纯逻辑静态库，按链接目标选择编译变体。
# - none : x86_64-unknown-none      内核与用户镜像（freestanding）
# - host : 当前宿主 ABI             sfcrypto 动态库与宿主测试
# - efi  : x86_64-pc-windows-gnu    UEFI 引导程序（PE/COFF，双平台同产物）
RUST_TARGET = {'none': 'x86_64-unknown-none', 'host': 'x86_64-unknown-linux-gnu' if LINUX else 'x86_64-pc-windows-gnu', 'efi': 'x86_64-pc-windows-gnu'}


def find_cargo():
    """定位 cargo：优先 PATH，其次 rustup 默认安装位置。"""
    import shutil
    exe = shutil.which('cargo')
    if exe:
        return exe
    home = Path.home() / '.cargo' / 'bin' / ('cargo.exe' if os.name == 'nt' else 'cargo')
    if home.exists():
        return str(home)
    raise RuntimeError('cargo not found; install rustup with targets x86_64-unknown-none, '
                       + RUST_TARGET['host'] + ', x86_64-pc-windows-gnu')


def rust_lib(kind):
    """构建（或增量复用）sfcore 静态库，返回给定变体的 libsfcore.a 路径。

    始终调用 cargo：未变更时增量构建近乎零开销，源码变更则自动重编译，
    避免"产物已存在导致源码更新被忽略"的问题。
    """
    target = RUST_TARGET[kind]
    out = ROOT / 'build' / 'rust' / target / 'release' / 'libsfcore.a'
    env = dict(os.environ, CARGO_TARGET_DIR=str(ROOT / 'build' / 'rust'))
    result = subprocess.run([find_cargo(), 'build', '--release', '--offline', '--target', target],
                            cwd=str(ROOT / 'rust' / 'sfcore'), env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout)
        raise RuntimeError('Rust build failed')
    if not out.exists():
        raise RuntimeError('Rust build produced no artifact: ' + str(out))
    return str(out)


def run(args):
    result = subprocess.run([ZIG, 'cc', *args], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout)
        raise RuntimeError('Compilation failed')


def build_accounts_table():
    """把 config/accounts.txt 编译成带盐值的账户表（仅含摘要，不含明文）。

    salt 由用户名确定性派生，使构建产物可复现；生产部署应换成随机盐。
    """
    source = ROOT / 'config' / 'accounts.txt'
    rounds = int(__import__('re').search(r'#define SF_ACCOUNT_HASH_ROUNDS (\d+)',
                                          (ROOT / 'include' / 'accounts.h').read_text()).group(1))
    entries = []
    for raw in source.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        name, uid, gid, password = line.split(':')
        if not (name[0].islower() and name.replace('-', '').replace('_', '').isalnum()
                and len(name) <= 8 and all(c.isdigit() or c.islower() or c in '-_' for c in name)):
            raise ValueError('Invalid account name: ' + name)
        if not uid.isdigit() or not gid.isdigit() or int(gid) > 65535 or int(uid) > 65535:
            raise ValueError('Invalid uid/gid for ' + name)
        if not password or len(password) > 32 or not all(32 <= ord(c) <= 126 for c in password):
            raise ValueError('Invalid password for ' + name + ' (1..32 printable ASCII)')
        entries.append((name, int(uid), int(gid), password))
    if not entries or len(entries) > 8:
        raise ValueError('Account database must contain 1..8 entries')
    if len({e[0] for e in entries}) != len(entries) or len({e[1] for e in entries}) != len(entries):
        raise ValueError('Account names and uids must be unique')
    roots = [e for e in entries if e[1] == 0]
    if len(roots) != 1 or roots[0][0] != 'root' or roots[0][2] != 0:
        raise ValueError('Account database requires exactly one uid-0 gid-0 root account')
    lines = []
    for name, uid, gid, password in entries:
        salt = hashlib.sha256(b'spiritfox-account:' + name.encode()).digest()[:16]
        digest = hashlib.sha256(salt + password.encode()).digest()
        for _ in range(rounds - 1):
            digest = hashlib.sha256(digest + salt).digest()
        lines.append(f'{name}:{uid}:{gid}:{salt.hex()}:{digest.hex()}')
    text = ''.join(line + '\n' for line in lines)
    header = '#define SF_ACCOUNT_BUILTIN_TEXT \\\n' + ' \\\n'.join(json.dumps(line + '\n') for line in lines)
    header += '\n#define SF_ACCOUNT_BUILTIN_BYTES ' + str(len(text)) + 'u\n'
    (ROOT / 'build' / 'accounts_table.h').write_text(header)


def compile_one(item):
    source, target, out, defines, extra = item
    run(['-target', target, *COMMON, *extra, *defines, '-c', source, '-o', out])
    return out


def objects(name, sources, target='x86_64-freestanding-none', defines=(), extra=()):
    folder = ROOT / 'build' / name
    folder.mkdir(exist_ok=True)
    items = [(s, target, str(folder / (s.replace('/', '_')+'.o')), list(defines), list(extra)) for s in sources]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        return list(pool.map(compile_one, items))


def sf_image_max():
    """从单一 ABI 定义读取 SF_IMAGE_MAX，避免构建门与 C 头文件漂移。"""
    for line in (ROOT / 'include' / 'sf.h').read_text(encoding='utf-8').splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == '#define' and parts[1] == 'SF_IMAGE_MAX':
            return int(parts[2].rstrip('ullUL'), 0)
    raise RuntimeError('SF_IMAGE_MAX not found in include/sf.h')


def check_rw_budget(name, elf_path):
    """门禁用户映像的可写 PT_LOAD 预算：剩余低于 10% 即构建失败。"""
    data = Path(elf_path).read_bytes()
    if data[:4] != b'\x7fELF':
        raise RuntimeError('RW budget: not an ELF image: ' + elf_path)
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phentsize = struct.unpack_from('<H', data, 54)[0]
    phnum = struct.unpack_from('<H', data, 56)[0]
    used = 0
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type = struct.unpack_from('<I', data, off)[0]
        p_flags = struct.unpack_from('<I', data, off + 4)[0]
        p_memsz = struct.unpack_from('<Q', data, off + 40)[0]
        if p_type == 1 and p_flags & 2:
            used += p_memsz
    available = sf_image_max()
    remaining = available - used
    pct = 100.0 * used / available
    print(f'RW budget {name}: {used} B used / {available} B available '
          f'({pct:.1f}%), remaining {remaining} B ({100.0 - pct:.1f}%)', flush=True)
    if used > available or remaining * 10 < available:
        raise RuntimeError(f'RW budget {name}: only {remaining} B left '
                           f'(need >= 10% of {available} B)')


def link_elf(name, objs, script):
    out = 'build/'+name+'.elf'
    entry = '_start64' if script.startswith('kernel/') else '_sf_start'
    flags = ['-target', 'x86_64-freestanding-none', '-nostdlib', '-static', '-Wl,-e,'+entry, '-Wl,-T,'+script, '-Wl,--build-id=none']
    if not script.startswith('kernel/'):
        flags.append('-Wl,--gc-sections')
    # 末尾追加 Rust 纯逻辑静态库：链接器仅拉取被引用的成员，
    # 未用到 Rust 符号的镜像不会因此增大。
    run([*flags, *objs, rust_lib('none'), '-o', out])
    # 内核有独立的内存布局，不参与用户映像 SF_IMAGE_MAX 预算门。
    if script == 'services/user.ld':
        check_rw_budget(name, out)
    return Path(out).read_bytes()


def main(policy_path=None):
    print('Building host package verifier', flush=True)
    crypto_lib = 'build/sfcrypto.so' if LINUX else 'build/sfcrypto.dll'
    # 宿主变体 Rust 静态库随 CRYPTO 一起进入 sfcrypto 动态库（sfpack 经 ctypes 调用）。
    run(['-target', HOST, '-shared', '-O2', *(['-fPIC'] if LINUX else []), '-Iinclude', '-Ivendor/monocypher', 'tools/package/crypto_host.c', 'services/readfs.c', 'services/fs_format.c', 'services/native_image.c', *CRYPTO, rust_lib('host'), '-o', crypto_lib])
    keys = ROOT / '.keys'
    keys.mkdir(exist_ok=True)
    seedfile = keys / 'development.seed'
    if not seedfile.exists():
        seedfile.write_bytes(os.urandom(32))
    seed = seedfile.read_bytes()
    crypto = Crypto()
    pub = crypto.key(seed)[1]
    # Separate module/app signing roles, plus an app-only standby anchor for
    # runtime revocation workflows. Explicit policies remain authoritative.
    backup = keys / 'backup.seed'
    if not backup.exists():
        backup.write_bytes(os.urandom(32))
    backup_pub = crypto.key(backup.read_bytes())[1]
    Path('build/development.pub').write_bytes(pub)
    Path('build/backup.pub').write_bytes(backup_pub)
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
    if not explicit_policy and not policy_path.exists():
        policy['keys'].append({'public_key':backup_pub.hex(),'usage':['app'],'revoked':False})
    for signer, usage in [(pub, 'module'), (application_pub, 'app')]:
        if not any(k['public_key'] == signer.hex() and not k['revoked'] and usage in k['usage'] for k in policy['keys']):
            raise ValueError('Current build requires an active ' + usage + ' signing key in the trust policy')
    Path('build/trust.h').write_text(trust_policy.header(policy))
    Path('build/trust-policy.json').write_text(json.dumps(policy, indent=2)+'\n')
    build_accounts_table()
    print('Building userspace packages', flush=True)
    shared = objects('user-common', ['services/start.S', 'services/memory.c', 'services/libsf.c', 'services/arguments.c'], extra=USER_SECTIONS)
    # sf-init：argparse.c/net_proto.c/sha256.c 已迁入 rust/sfcore，由 link_elf 追加链接。
    init = shared + objects('init', ['services/init.c', 'services/file_rpc.c', 'services/native_image.c', 'services/storage.c', 'services/persist_client.c', 'services/manager.c', 'services/lifecycle_rpc.c', 'services/supervisor.c', 'services/namespace.c', 'services/accounts.c', 'services/session.c', *CRYPTO], extra=USER_SECTIONS)
    packages = []
    image = link_elf('sf-init', init, 'services/user.ld')
    manifest = {'module_id': 'org.spiritfox.init', 'version': '0.1.0', 'category': 'bootstrap', 'requested_caps': ['bootstrap'], 'resource_limits': {'memory': 2097152}}
    packages.append(('sf-init.sfmod', pack(manifest, image, 1, seed)))
    disk_files = {'/etc/readme.txt': b'SpiritFoxOS read-only disk mounted through user-space virtio-blk.\n'}
    for name, role, kind in [('echo', 1, 1), ('sf-native', 2, 1), ('hello', 3, 2), ('fault', 4, 2), ('illegal', 5, 2), ('spinner', 6, 2), ('nx', 7, 2), ('probe', 8, 2), ('mmio', 10, 2), ('exitbusy', 12, 2), ('badrsp', 13, 2), ('fileprobe', 14, 2), ('readprobe', 15, 2), ('argprobe', 16, 2), ('captree', 17, 2), ('threadprobe', 18, 2), ('disk-echo', 1, 1), ('diskhello', 9, 2), ('diskupdate', 9, 2), ('disknext', 9, 2), ('diskfault', 4, 2), ('diskexit', 11, 2), ('diskspin', 6, 2), ('diskother', 9, 2)]:
        native_crypto = objects('native-crypto', CRYPTO, extra=USER_SECTIONS) if role == 2 else []
        image = link_elf(name, shared + native_crypto + objects(name, ['services/program.c'], defines=['-DROLE='+str(role)], extra=USER_SECTIONS), 'services/user.ld')
        manifest = {('module_id' if kind == 1 else 'app_id'): 'org.spiritfox.'+('native' if name == 'sf-native' else name), 'version': '0.1.0', 'requested_caps': ['stdio'] if role in (3, 9, 18) else [], 'resource_limits': {'memory': 262144 if role == 2 else 131072}}
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
    rpc_image = link_elf('rpcprobe',shared+objects('rpcprobe',['services/rpcprobe.c']),'services/user.ld')
    rpc_manifest = {'app_id':'org.spiritfox.rpcprobe','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,'requested_caps':['stdio','namespace.read'],'resource_limits':{'memory':131072}}
    packages.append(('rpcprobe.sfk',pack(rpc_manifest,rpc_image,2,application_seed)))
    fp_image = link_elf('fpprobe',shared+objects('fpprobe',['services/fpprobe.c']),'services/user.ld')
    fp_manifest = {'app_id':'org.spiritfox.fpprobe','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,'requested_caps':['stdio'],'resource_limits':{'memory':131072}}
    packages.append(('fpprobe.sfk',pack(fp_manifest,fp_image,2,application_seed)))
    client_image = link_elf('fileclient',shared+objects('file-client',['services/file_client.c'])+objects('fileclient',['services/fileclient_probe.c']),'services/user.ld')
    caplimit_image = link_elf('caplimit',shared+objects('caplimit-file-client',['services/file_client.c'])+objects('caplimit',['services/caplimit_probe.c']),'services/user.ld')
    caplimit_manifest = {'app_id':'org.spiritfox.caplimit','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,'requested_caps':['stdio','namespace.read'],'resource_limits':{'memory':131072}}
    packages.append(('caplimit.sfk',pack(caplimit_manifest,caplimit_image,2,application_seed)))
    cancel_image = link_elf('cancelprobe',shared+objects('cancelprobe',['services/cancel_probe.c']),'services/user.ld')
    cancel_manifest = {'app_id':'org.spiritfox.cancelprobe','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,'requested_caps':['stdio'],'resource_limits':{'memory':131072}}
    packages.append(('cancelprobe.sfk',pack(cancel_manifest,cancel_image,2,application_seed)))
    client_manifest = {'app_id':'org.spiritfox.fileclient','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,'requested_caps':['stdio','namespace.read'],'resource_limits':{'memory':131072}}
    packages.append(('fileclient.sfk',pack(client_manifest,client_image,2,application_seed)))
    pie_shared = objects('pie-common', ['services/start.S','services/memory.c','services/libsf.c','services/arguments.c'], defines=['-fpie'])
    pie_objs = pie_shared + objects('pieprobe',['services/pieprobe.c'],defines=['-fpie'])
    run(['-target','x86_64-freestanding-none','-nostdlib','-fpie','-pie','-Wl,-Bsymbolic','-Wl,-e,_sf_start','-Wl,-T,services/user-pie.ld','-Wl,--build-id=none',*pie_objs,'-o','build/pieprobe.elf'])
    pie_manifest = {'app_id':'org.spiritfox.pieprobe','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,'native_profile':'relative-v1','requested_caps':['stdio','namespace.read'],'resource_limits':{'memory':131072}}
    pie_package = pack(pie_manifest,Path('build/pieprobe.elf').read_bytes(),2,application_seed)
    packages.append(('pieprobe.sfk',pie_package))
    disk_files['/bin/pieprobe.sfk'] = pie_package
    # sf-libsf：相对地址运行时库，导出有界 POSIX 子集，由导入表按名称/ABI 绑定。
    posix_shared = objects('posix-common', ['services/start.S','services/memory.c','services/file_client.c','services/arguments.c','services/sdk.c'], defines=['-fpie'])
    posix_lib = posix_shared + objects('posix-lib',['services/posix.c'],defines=['-fpie'])
    run(['-target','x86_64-freestanding-none','-nostdlib','-fpie','-pie','-Wl,-Bsymbolic','-Wl,-e,_sf_start','-Wl,-T,services/user-pie.ld','-Wl,--build-id=none',*posix_lib,'-o','build/sf-libsf.elf'])
    posix_exports = ['open','read','write','close','lseek','fstat','stat','getpid','_exit','sleep','printf','puts','getenv','__errno_location','strerror']
    posix_lib_manifest = {'module_id':'org.spiritfox.posix','version':'0.1.0','category':'runtime',
                          'native_profile':'relative-v1','native_role':'library',
                          'native_exports':[{'name':name,'abi':'libsf-posix-1'} for name in posix_exports],
                          'requested_caps':[],'resource_limits':{'memory':131072}}
    posix_lib_package = pack(posix_lib_manifest,Path('build/sf-libsf.elf').read_bytes(),1,seed)
    packages.append(('sf-libsf.sfmod',posix_lib_package))
    posix_probe = pie_shared + objects('posixprobe',['services/posixprobe.c'],defines=['-fpie'])
    run(['-target','x86_64-freestanding-none','-nostdlib','-fpie','-pie','-Wl,-Bsymbolic','-Wl,-e,_sf_start','-Wl,-T,services/user-pie.ld','-Wl,--build-id=none',*posix_probe,'-o','build/posixprobe.elf'])
    posix_probe_manifest = {'app_id':'org.spiritfox.posixprobe','version':'0.1.0','runtime_id':'org.spiritfox.native','app_abi':1,
                            'native_profile':'relative-v1','native_library':'org.spiritfox.posix',
                            'native_imports':[{'name':name,'abi':'libsf-posix-1','slot':'__sf_import_'+name} for name in posix_exports],
                            'requested_caps':['stdio','namespace.read'],'resource_limits':{'memory':131072}}
    posix_probe_package = pack(posix_probe_manifest,Path('build/posixprobe.elf').read_bytes(),2,application_seed)
    packages.append(('posixprobe.sfk',posix_probe_package))
    image = link_elf('sf-virtio-blk', shared + objects('virtio-blk', ['services/virtio_blk.c']), 'services/user.ld')
    packages.append(('sf-virtio-blk.sfmod', pack({'module_id': 'org.spiritfox.virtio.blk', 'version': '0.1.0', 'category': 'driver', 'requested_caps': ['device.virtio-blk.trusted'], 'resource_limits': {'memory': 131072}}, image, 1, seed)))
    # virtio-net：net_proto.c 帧逻辑已迁入 rust/sfcore。
    image = link_elf('sf-virtio-net', shared + objects('virtio-net', ['services/virtio_net.c']), 'services/user.ld')
    packages.append(('sf-virtio-net.sfmod', pack({'module_id': 'org.spiritfox.virtio.net', 'version': '0.1.0', 'category': 'driver', 'requested_caps': ['device.virtio-net.trusted'], 'resource_limits': {'memory': 131072}}, image, 1, seed)))
    # sf-persist：事务卷 + 更新状态机 + 块客户端，独立地址空间（约 296 KiB 常驻）。
    image = link_elf('sf-persist', shared + objects('persist', ['services/persist.c', 'services/persistence.c', 'services/txn.c', 'services/update.c', 'services/version.c', *CRYPTO], extra=USER_SECTIONS), 'services/user.ld')
    packages.append(('sf-persist.sfmod', pack({'module_id': 'org.spiritfox.persist', 'version': '0.1.0', 'category': 'service', 'requested_caps': [], 'resource_limits': {'memory': 524288}}, image, 1, seed)))
    # sf-fs：通用磁盘格式层（SFRFS1/SFRFS2 探测与解析）+ 文件读取服务。
    # 块请求由 sf-init 的 broker 转发，模块自身不持有 virtio-blk 端点。
    image = link_elf('sf-fs', shared + objects('fs', ['services/fs_main.c', 'services/fs_format.c', 'services/readfs.c', *CRYPTO], extra=USER_SECTIONS), 'services/user.ld')
    packages.append(('sf-fs.sfmod', pack({'module_id': 'org.spiritfox.fs.disk', 'version': '0.1.0', 'category': 'service', 'requested_caps': [], 'resource_limits': {'memory': 2097152}}, image, 1, seed)))
    Path('build/storage.img').write_bytes(create_readfs(disk_files))
    Path('build/storage-v2.img').write_bytes(create_readfs(disk_files, format=2))
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
    # 内核：sched_policy.c 策略函数已迁入 rust/sfcore（sf_sched_pick）。
    kernel = objects('kernel', ['kernel/entry.S', 'kernel/arch.c', 'kernel/smp.c', 'kernel/core.c', 'kernel/pid.c', 'kernel/memory.c', 'kernel/ipc.c', 'kernel/device.c', 'kernel/acpi.c', 'kernel/vtd_tables.c', 'kernel/iommu.c', 'services/memory.c', 'services/arguments.c', 'build/interrupts.S'])
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
    # UEFI 引导程序为 PE/COFF：使用 windows-gnu 变体的 Rust 静态库。
    run(['-target', 'x86_64-windows-gnu', '-nostdlib', '-s', '-Wl,--entry,_start', '-Wl,--subsystem,efi_application', '-Wl,--image-base,0x10000000', *efi, rust_lib('efi'), '-o', str(efi_path)])
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
