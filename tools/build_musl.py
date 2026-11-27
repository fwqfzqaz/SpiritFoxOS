"""用 zig cc 构建上游 musl 1.2.5 的动态产物：libc.so、libm.so 与动态 hello。

该模块只负责生成 ELF 文件并计算 SHA-256 摘要；嵌入与装载由 build.py / sf-init 完成。
所有产物缓存到 build/musl/，以压缩包与模块内容的摘要作为缓存键。
"""
import concurrent.futures as cf
import hashlib
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'vendor/musl/musl-1.2.5.tar.gz'
WORK = ROOT / 'build/musl'
SRC = WORK / 'src'
OBJ = WORK / 'obj'
OUT = WORK / 'out'
HELLO_SRC = ROOT / 'tests/fixtures/linux_dynamic_hello.c'
LIBM_SRC = ROOT / 'tests/fixtures/linux_dynamic_libm.c'

BASE_FLAGS = ['-target', 'x86_64-linux-musl', '-std=c99', '-ffreestanding', '-nostdinc',
              '-fPIC', '-Os', '-D_XOPEN_SOURCE=700']


def _run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def _mkalltypes(text):
    """复刻 tools/mkalltypes.sed：把 TYPEDEF/STRUCT/UNION 行展开为按需定义块。"""
    out = []
    for line in text.splitlines():
        m = re.match(r'^TYPEDEF (.*) ([^ ]*);$', line)
        if m:
            out += [f'#if defined(__NEED_{m.group(2)}) && !defined(__DEFINED_{m.group(2)})',
                    f'typedef {m.group(1)} {m.group(2)};',
                    f'#define __DEFINED_{m.group(2)}', '#endif', '']
            continue
        m = re.match(r'^STRUCT *([^ ]*) (.*);$', line)
        if m:
            out += [f'#if defined(__NEED_struct_{m.group(1)}) && !defined(__DEFINED_struct_{m.group(1)})',
                    f'struct {m.group(1)} {m.group(2)};',
                    f'#define __DEFINED_struct_{m.group(1)}', '#endif', '']
            continue
        m = re.match(r'^UNION *([^ ]*) (.*);$', line)
        if m:
            out += [f'#if defined(__NEED_union_{m.group(1)}) && !defined(__DEFINED_union_{m.group(1)})',
                    f'union {m.group(1)} {m.group(2)};',
                    f'#define __DEFINED_union_{m.group(1)}', '#endif', '']
            continue
        out.append(line)
    return '\n'.join(out) + '\n'


def _syscall_h(text):
    return '\n'.join(line.replace('__NR_', 'SYS_') for line in text.splitlines()
                     if '__NR_' in line) + '\n'


def _extract():
    if SRC.exists():
        shutil.rmtree(SRC)
    SRC.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(ARCHIVE) as tar:
        tar.extractall(SRC.parent)
    extracted = SRC.parent / 'musl-1.2.5'
    if extracted != SRC:
        if SRC.exists():
            shutil.rmtree(SRC)
        extracted.rename(SRC)


def _patch():
    """把 x86_64 的 syscall 指令替换为 int $0x80（本 OS 的兼容入口）。"""
    f = SRC / 'arch/x86_64/syscall_arch.h'
    f.write_text(f.read_text().replace('"syscall"', '"int $0x80"'))
    pat = re.compile(r'(?m)^(\s*)syscall\b')
    for base in ('src/*/x86_64', 'arch/x86_64'):
        for p in sorted(SRC.glob(base + '/*.s')):
            text = p.read_text()
            new = pat.sub(r'\1int $0x80', text)
            if new != text:
                p.write_text(new)


def _generate_headers():
    (SRC / 'obj/include/bits').mkdir(parents=True, exist_ok=True)
    (SRC / 'obj/src/internal').mkdir(parents=True, exist_ok=True)
    alltypes = ((SRC / 'arch/x86_64/bits/alltypes.h.in').read_text() +
                (SRC / 'include/alltypes.h.in').read_text())
    (SRC / 'obj/include/bits/alltypes.h').write_text(_mkalltypes(alltypes))
    (SRC / 'obj/include/bits/syscall.h').write_text(
        _syscall_h((SRC / 'arch/x86_64/bits/syscall.h.in').read_text()))
    (SRC / 'obj/src/internal/version.h').write_text(
        '#define VERSION "%s"\n' % (SRC / 'VERSION').read_text().strip())


def _sources():
    """复刻 musl Makefile 的对象选择规则。"""
    base = sorted(SRC.glob('src/*/*.c')) + sorted(SRC.glob('src/malloc/mallocng/*.c'))
    arch = sorted(SRC.glob('src/*/x86_64/*.c')) + sorted(SRC.glob('src/*/x86_64/*.s'))
    arch += sorted(SRC.glob('arch/x86_64/*.c')) + sorted(SRC.glob('arch/x86_64/*.s'))
    keys = {str(Path(str(p.relative_to(SRC)).replace('/x86_64/', '/')).with_suffix(''))
            for p in arch}
    srcs = [p for p in base if str(p.relative_to(SRC).with_suffix('')) not in keys]
    srcs += [p for p in arch if p.suffix == '.c']
    asms = [p for p in arch if p.suffix == '.s']
    return srcs, asms


def _flags():
    return BASE_FLAGS + ['-I' + str(SRC / d) for d in
                         ('arch/x86_64', 'arch/generic', 'obj/src/internal',
                          'src/include', 'src/internal', 'obj/include', 'include')]


def _compile(zig, path, name=None):
    if name is None:
        obj = OBJ / (str(path.relative_to(SRC)) + '.o')
    else:
        obj = OBJ / 'ext' / name
    obj.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(c) for c in [zig, 'cc', *_flags(), '-c', str(path), '-o', str(obj)]],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return obj


def _digest(path):
    return hashlib.sha256(Path(path).read_bytes()).digest()


def _stamp(zig):
    h = hashlib.sha256()
    h.update(ARCHIVE.read_bytes())
    h.update(Path(__file__).read_bytes())
    h.update(HELLO_SRC.read_bytes())
    h.update(LIBM_SRC.read_bytes())
    h.update(str(subprocess.run([str(zig), 'version'], check=True,
                                capture_output=True).stdout).encode())
    return h.hexdigest()


def build(zig, force=False):
    """构建并返回 {'libc': {...}, 'libm': {...}, 'hello': {...}}，每项含 path/sha256/bytes。"""
    zig = Path(zig)
    stamp_file = WORK / 'stamp'
    want = _stamp(zig)
    outputs = [OUT / 'libc.so', OUT / 'libm.so', OUT / 'hello']
    if not force and stamp_file.exists() and stamp_file.read_text().strip() == want \
            and all(p.exists() for p in outputs):
        return _result()

    _extract()
    _patch()
    _generate_headers()
    if OBJ.exists():
        shutil.rmtree(OBJ)
    srcs, asms = _sources()
    with cf.ThreadPoolExecutor(max_workers=8) as pool:
        objs = list(pool.map(lambda p: _compile(zig, p), srcs))
    objs += [_compile(zig, p) for p in asms]

    OUT.mkdir(parents=True, exist_ok=True)
    _run([zig, 'cc', '-target', 'x86_64-linux-musl', '-nostdlib', '-shared',
          '-Wl,--strip-all', '-Wl,--no-entry', '-Wl,-soname,libc.so',
          '-o', OUT / 'libc.so', *objs])
    libm_obj = _compile(zig, LIBM_SRC, 'libm_marker.c.o')
    _run([zig, 'cc', '-target', 'x86_64-linux-musl', '-nostdlib', '-shared',
          '-Wl,--strip-all', '-Wl,--no-entry', '-Wl,-soname,libm.so',
          '-o', OUT / 'libm.so', libm_obj])
    hello_obj = _compile(zig, HELLO_SRC, 'hello.c.o')
    crt = _compile(zig, SRC / 'crt/Scrt1.c')
    _run([zig, 'cc', '-target', 'x86_64-linux-musl', '-nostdlib', '-fpie', '-pie',
          '-Wl,-e,_start', '-Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1',
          '-o', OUT / 'hello', hello_obj, crt, OUT / 'libc.so', OUT / 'libm.so'])
    stamp_file.write_text(want + '\n')
    return _result()


def _result():
    return {name: {'path': OUT / (name + '.so' if name != 'hello' else 'hello'),
                   'sha256': _digest(OUT / (name + '.so' if name != 'hello' else 'hello')),
                   'bytes': (OUT / (name + '.so' if name != 'hello' else 'hello')).stat().st_size}
            for name in ('libc', 'libm', 'hello')}


if __name__ == '__main__':
    import sys
    r = build(ROOT / '.tools/zig/zig', force='--force' in sys.argv)
    for k, v in r.items():
        print(k, v['bytes'], v['sha256'].hex())
