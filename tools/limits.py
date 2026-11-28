"""系统上限基线：把“提高系统上限”的起点变成可重复测量的数字。

三类测量：
1. 声明上限：从源码常量生成（tests/host/limits_probe.c），避免文档漂移。
2. 宿主实测（E1）：内核 IPC 往返延迟、SFRWFS1 顺序写/读与元数据吞吐。
3. QEMU 实测（E2）：默认 256 MiB / q35 / 2 核 TCG 下，从 QEMU 启动到
   BOOT TESTS PASSED、DEPENDENCY GENERATION TESTS PASSED、SELFTEST OK 与
   shell 提示符的墙钟时间；随后在 shell 执行 `bench` 记录 guest 内 IPC 往返。

结果写入 build/limits-baseline.json；用 --host-only 跳过 QEMU。
"""
import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host-only', action='store_true', help='只跑宿主基准，不启动 QEMU')
parser.add_argument('--memory', type=int, default=256, help='QEMU 内存 MiB（默认 256）')
parser.add_argument('--iterations', type=int, default=200000, help='IPC 往返次数（默认 200000）')
parser.add_argument('--json', default='build/limits-baseline.json', help='结果 JSON 路径')
args = parser.parse_args()


def measured(name, sources, link=(), run_args=()):
    """编译并运行一个基准；收集 RESULT/LIMIT key=value 行。"""
    exe = ROOT / 'build' / (name + build.EXE)
    flags = ['-target', build.HOST, '-g0', '-O2', '-Wall', '-Wextra', '-Werror',
             '-DSF_HOST_TEST', '-Wno-unused-parameter', '-Iinclude', '-Ibuild',
             '-Ivendor/monocypher']
    build.run([*flags, *sources, *link, '-o', str(exe)])
    proc = subprocess.run([str(exe), *run_args], cwd=ROOT, capture_output=True, text=True)
    if proc.stdout:
        print(proc.stdout, end='')
    if proc.returncode:
        sys.stderr.write(proc.stderr)
        raise SystemExit('bench ' + name + ' failed with ' + str(proc.returncode))
    values = {}
    for line in proc.stdout.splitlines():
        for prefix in ('RESULT ', 'LIMIT '):
            if line.startswith(prefix):
                key, _, raw = line[len(prefix):].partition('=')
                try:
                    values[key] = int(raw)
                except ValueError:
                    values[key] = float(raw)
    return values


def measure_qemu(memory):
    """测量 QEMU 启动到关键标记的墙钟时间；不可用或镜像缺失时返回空。"""
    sys.path.insert(0, str(ROOT / 'tests' / 'qemu'))
    import qemu_test as qemu
    if not qemu.QEMU or not qemu.FIRMWARE:
        print('limits: QEMU/OVMF 不可用，跳过启动测量')
        return {}
    if not (ROOT / 'build' / 'esp').exists():
        print('limits: build/esp 不存在，跳过后启动测量（先运行 tools/build.py）')
        return {}
    disk = ROOT / 'build' / 'storage.img'
    os.chdir(ROOT)
    os.environ['TMPDIR'] = os.environ['TEMP'] = os.environ['TMP'] = str(ROOT / '.cache')
    shutil.copyfile(qemu.VARS_SRC, ROOT / 'build/OVMF_VARS.fd')
    command = qemu.command(memory, disk if disk.exists() else None, writable=False)
    markers = [
        ('qemu_boot_ms_boot_tests', b'BOOT TESTS PASSED'),
        ('qemu_boot_ms_dependency_tests', b'DEPENDENCY GENERATION TESTS PASSED'),
        ('qemu_boot_ms_selftest', b'SELFTEST OK'),
        ('qemu_boot_ms_shell_prompt', b'fox> '),
    ]
    results, seen = {}, set()
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    start = time.monotonic()
    output = bytearray()
    requested_selftest = False
    requested_bench = False
    bench_at = None
    try:
        while process.poll() is None:
            data = process.stdout.read(1)
            if not data:
                break
            output.extend(data)
            elapsed = (time.monotonic() - start) * 1000.0
            for key, marker in markers:
                if key not in seen and marker in output:
                    seen.add(key)
                    results[key] = round(elapsed, 1)
            if b'[FAIL]' in output or b'[PANIC]' in output:
                raise SystemExit('limits: QEMU 启动出现失败标记')
            if 'qemu_boot_ms_shell_prompt' in seen and not requested_selftest:
                requested_selftest = True
                process.stdin.write(b'selftest\n')
                process.stdin.flush()
            if 'qemu_boot_ms_selftest' in seen and not requested_bench:
                requested_bench = True
                bench_at = elapsed
                process.stdin.write(b'bench\n')
                process.stdin.flush()
            if 'qemu_boot_ms_selftest' in seen and re.search(
                    rb'IPC ROUNDTRIP (?:ns_per_round=\d+ iterations=\d+ ticks=\d+|unavailable|failed)\r?\n',
                    bytes(output)):
                break
            if bench_at is not None and elapsed - bench_at > 60000.0:
                break
            if elapsed > 240000.0:
                raise SystemExit('limits: QEMU 启动测量超时')
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
    (ROOT / 'build' / 'qemu-baseline.log').write_bytes(bytes(output))
    results['qemu_memory_mib'] = memory
    bench = re.search(rb'IPC ROUNDTRIP ns_per_round=(\d+) iterations=(\d+) ticks=(\d+)', bytes(output))
    if bench:
        results['qemu_ipc_roundtrip_ns'] = int(bench.group(1))
        results['qemu_ipc_iterations'] = int(bench.group(2))
    return results


metrics = {}
metrics.update(measured('limits_probe', ['tests/host/limits_probe.c']))
metrics.update(measured('bench_ipc', ['tests/host/bench_ipc.c'], run_args=[str(args.iterations)]))
metrics.update(measured('bench_sfrw', ['tests/host/bench_sfrw.c', 'services/sfrw.c'],
                        link=[str(build.rust_lib('host'))]))
if not args.host_only:
    metrics.update(measure_qemu(args.memory))

report = {
    'generated': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
    'host': {
        'platform': platform.platform(),
        'machine': platform.machine(),
        'cpu_count': os.cpu_count(),
    },
    'metrics': metrics,
}
path = ROOT / args.json
path.write_text(json.dumps(report, indent=1, sort_keys=True) + '\n')

rows = [
    ('任务槽', metrics.get('sf_max_tasks'), '个'),
    ('线程槽', metrics.get('sf_max_threads'), '个'),
    ('CPU 核上限', metrics.get('sf_max_cpus'), '个'),
    ('每任务能力句柄', metrics.get('sf_cap_limit_max'), '个'),
    ('内核页池', metrics.get('sf_kernel_pool_bytes', 0) // (1024 * 1024), 'MiB'),
    ('IPC 正文', metrics.get('sf_msg_bytes'), 'B'),
    ('命名空间节点', metrics.get('sf_ns_node_max'), '个'),
    ('SFRWFS1 卷容量', metrics.get('sf_rw_max_volume_bytes', 0) // (1024 * 1024), 'MiB'),
    ('SFRWFS1 inode', metrics.get('sf_rw_max_inodes'), '个'),
    ('网络连接', metrics.get('sf_net_conn_max'), '个'),
    ('IPC 往返延迟', metrics.get('ipc_roundtrip_ns'), 'ns'),
    ('guest 内 IPC 往返', metrics.get('qemu_ipc_roundtrip_ns'), 'ns'),
    ('SFRWFS1 顺序写', metrics.get('sfrw_write_mib_per_s'), 'MiB/s'),
    ('SFRWFS1 顺序读', metrics.get('sfrw_read_mib_per_s'), 'MiB/s'),
    ('SFRWFS1 每块写入', metrics.get('sfrw_write_us_per_block'), 'us'),
    ('SFRWFS1 写后 fsync', metrics.get('sfrw_write_fsync_ms'), 'ms'),
    ('SFRWFS1 create', metrics.get('sfrw_create_per_s'), 'ops/s'),
    ('SFRWFS1 rename', metrics.get('sfrw_rename_per_s'), 'ops/s'),
    ('SFRWFS1 mkdir', metrics.get('sfrw_mkdir_per_s'), 'ops/s'),
    ('SFRWFS1 readdir', metrics.get('sfrw_readdir_entries_per_s'), 'entry/s'),
    ('SFRWFS1 目录改名', metrics.get('sfrw_dir_rename_per_s'), 'ops/s'),
    ('SFRWFS1 rmdir', metrics.get('sfrw_rmdir_per_s'), 'ops/s'),
    ('SFRWFS1 fsck', metrics.get('sfrw_fsck_ms'), 'ms'),
    ('启动到依赖生成完成', metrics.get('qemu_boot_ms_dependency_tests'), 'ms'),
    ('启动到 BOOT TESTS', metrics.get('qemu_boot_ms_boot_tests'), 'ms'),
    ('启动到 shell 提示符', metrics.get('qemu_boot_ms_shell_prompt'), 'ms'),
    ('启动到 SELFTEST OK', metrics.get('qemu_boot_ms_selftest'), 'ms'),
]
print('\n上限基线（' + report['generated'] + '）：')
for label, value, unit in rows:
    if value is not None:
        print('  ' + label.ljust(20) + (('%.1f' % value) if isinstance(value, float) else str(value)).rjust(12) + ' ' + unit)
print('\n已写入 ' + str(path.relative_to(ROOT)))
