#!/usr/bin/env python3
"""S1：用真实工具链生成的静态 PIE 验证 Linux 装载核心与检查工具。"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build  # noqa: E402

BUILD = ROOT / 'build'
CHECKER = BUILD / ('linux_elf_check' + build.EXE)
FIXTURE_C = BUILD / 'linux_fixture.c'
FIXTURE_ELF = BUILD / 'linux_fixture.elf'
PAGE = 4096
IMAGE_MAX = 8 * 1024 * 1024


def run_checker(*args):
    out = subprocess.run([str(CHECKER), *map(str, args)], capture_output=True, text=True)
    try:
        payload = json.loads(out.stdout.strip().splitlines()[-1])
    except (IndexError, json.JSONDecodeError):
        raise SystemExit(f'linux_elf_check produced no JSON: rc={out.returncode} out={out.stdout!r} err={out.stderr!r}')
    return out.returncode, payload


def build_checker():
    build.run(['-target', build.HOST, '-g0', '-O2', '-Wall', '-Wextra', '-Werror', '-Iinclude',
               'tools/linux_elf_check.c', 'services/linux_image.c', '-o', str(CHECKER)])


def build_fixture():
    FIXTURE_C.write_text(
        'static int helper(void){return 7;}\n'
        'static int (*volatile slot)(void)=helper;\n'
        'void _start(void){__asm__ volatile("mov $60,%rax\\nxor %rdi,%rdi\\nsyscall");(void)slot();}\n',
        encoding='utf-8')
    build.run(['-target', 'x86_64-linux-musl', '-nostdlib', '-static', '-pie',
               '-fPIE', '-O2', '-Wl,-e,_start', '-o', str(FIXTURE_ELF), str(FIXTURE_C)])


def plan_invariants(loaded, entry_rva):
    assert 1 <= loaded['count'] <= 8, loaded
    assert loaded['entry'] == loaded['base'] + entry_rva, loaded
    ranges = []
    executable = False
    for seg in loaded['segments']:
        assert seg['rva'] % PAGE == 0 and seg['bytes'] > 0, seg
        assert seg['rva'] < IMAGE_MAX and seg['bytes'] <= IMAGE_MAX - seg['rva'], seg
        assert seg['flags'] in (1, 3, 5), seg
        end = (seg['rva'] + seg['bytes'] + PAGE - 1) // PAGE * PAGE
        for other in ranges:
            assert not (seg['rva'] < other[1] and other[0] < end), (seg, other)
        ranges.append((seg['rva'], end))
        if seg['flags'] == 5 and seg['rva'] <= entry_rva < seg['rva'] + seg['bytes']:
            executable = True
    assert executable, loaded
    return [seg['flags'] for seg in loaded['segments']]


def main():
    build_checker()
    build_fixture()
    rc, valid = run_checker(FIXTURE_ELF)
    assert rc == 0 and valid['status'] == 'valid', valid
    assert valid['phdr_rva'] == 64, valid
    assert valid['relocations'] >= 1, valid
    assert 0 < valid['entry_rva'] < valid['image_bytes'] <= IMAGE_MAX, valid

    base = 0x44000000
    rc, loaded = run_checker(FIXTURE_ELF, hex(base))
    assert rc == 0 and loaded['status'] == 'loaded', loaded
    flags = plan_invariants(loaded, valid['entry_rva'])
    assert flags == [1, 5, 3], (flags, loaded)

    corrupted = BUILD / 'linux_fixture_exec.elf'
    corrupted.write_bytes(FIXTURE_ELF.read_bytes())
    data = bytearray(corrupted.read_bytes())
    data[16:18] = (2).to_bytes(2, 'little')  # ET_EXEC
    corrupted.write_bytes(data)
    rc, rejected = run_checker(corrupted)
    assert rc == 2 and rejected['status'] == 'rejected' and rejected['code'] == -3, rejected

    truncated = BUILD / 'linux_fixture_truncated.elf'
    truncated.write_bytes(FIXTURE_ELF.read_bytes()[:100])
    rc, rejected = run_checker(truncated)
    assert rc == 2 and rejected['status'] == 'rejected', rejected

    rc, rejected = run_checker(FIXTURE_ELF, '0')
    assert rc == 2 and rejected['status'] == 'rejected' and rejected['code'] == -1, rejected

    print(f"LINUX ELF TOOL PASS: entry_rva={valid['entry_rva']} image_bytes={valid['image_bytes']} "
          f"relocations={valid['relocations']} segments={loaded['count']}")


if __name__ == '__main__':
    main()
