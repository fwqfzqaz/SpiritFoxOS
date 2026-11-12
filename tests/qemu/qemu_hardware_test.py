"""真实硬件控制器回归：QEMU q35 的 SATA AHCI 与 NVMe 存储路径。
使用 persistent.img 的可写副本，验证驱动探测、挂载、读文件与运行磁盘应用。"""
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qemu_test

ROOT = qemu_test.ROOT


def launch(model, base, logical_block_size):
    disk = ROOT / ('build/hw-' + model + '.img')
    process = subprocess.Popen(qemu_test.command(256, disk, writable=True, model=base,
                                                 logical_block_size=logical_block_size),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0, cwd=ROOT)
    chunks, output = queue.Queue(), bytearray()

    def read():
        while True:
            data = process.stdout.read(1)
            if not data:
                break
            chunks.put(data)

    threading.Thread(target=read, daemon=True).start()
    return process, chunks, output


def expect(model, process, chunks, output, text, timeout=90, start=0):
    until = time.monotonic() + timeout
    while text.encode() not in output[start:]:
        if time.monotonic() > until:
            raise RuntimeError(model + ' timed out waiting for ' + text)
        if process.poll() is not None and chunks.empty():
            raise RuntimeError(model + ' exited before ' + text)
        try:
            output.extend(chunks.get(timeout=.1))
        except queue.Empty:
            continue


def session(model, base, logical_block_size, commands):
    process, chunks, output = launch(model, base, logical_block_size)
    try:
        # 可写卷跳过只读存储自检，改用持久化服务就绪作为挂载完成信号。
        expect(model, process, chunks, output, 'PERSIST READY', 120)
        marker = b'[AHCI]' if base == 'ahci' else b'[NVME]'
        if marker not in output:
            raise RuntimeError(model + ' driver did not claim the controller')
        if logical_block_size:
            sectors = (str(logical_block_size) + '-byte logical sectors').encode()
            if sectors not in output:
                raise RuntimeError(model + ' driver did not report ' + str(logical_block_size) + '-byte logical sectors')
        for command, text in commands:
            start = len(output)
            for byte in command.encode() + b'\n':
                process.stdin.write(bytes([byte]))
                process.stdin.flush()
                time.sleep(.004)
            expect(model, process, chunks, output, text, 20, start)
        code = process.wait(timeout=10)
        if code != 1:
            raise RuntimeError(model + ' unexpected QEMU exit code ' + str(code))
        return bytes(output)
    finally:
        (ROOT / 'build' / ('qemu-hw-' + model + '-last.log')).write_bytes(output)
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def boot(model):
    base = 'ahci' if model.startswith('ahci') else 'nvme'
    logical_block_size = 4096 if model.endswith('4k') else 0
    shutil.copyfile(ROOT / 'build/persistent.img', ROOT / ('build/hw-' + model + '.img'))
    output = bytearray()
    try:
        commands = [('cat /disk/etc/readme.txt', 'SpiritFoxOS'),
                    ('run /disk/bin/diskhello.sfk', 'sfexec: exit 0')]
        if logical_block_size:
            # 写入 512 字节扇区，驱动必须保留同一 4096 字节逻辑块的其余内容。
            commands.append(('write /persist/notes hw-' + model + '-read-modify-write-marker', 'write: OK'))
        commands.append(('shutdown', 'SHUTDOWN OK'))
        output += session(model, base, logical_block_size, commands)
        if logical_block_size:
            output += session(model, base, logical_block_size,
                              [('cat /persist/notes', 'hw-' + model + '-read-modify-write-marker'),
                               ('shutdown', 'SHUTDOWN OK')])
        print('HARDWARE PASS: ' + model, flush=True)
    finally:
        (ROOT / 'build' / ('qemu-hw-' + model + '.log')).write_bytes(output)


if __name__ == '__main__':
    boot('ahci')
    boot('nvme')
    boot('nvme4k')
