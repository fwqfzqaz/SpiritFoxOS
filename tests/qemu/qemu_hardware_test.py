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


def boot(model):
    disk = ROOT / ('build/hw-' + model + '.img')
    shutil.copyfile(ROOT / 'build/persistent.img', disk)
    process = subprocess.Popen(qemu_test.command(256, disk, writable=True, model=model),
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

    def expect(text, timeout=90, start=0):
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

    try:
        # 可写卷跳过只读存储自检，改用持久化服务就绪作为挂载完成信号。
        expect('PERSIST READY', 120)
        marker = b'[AHCI]' if model == 'ahci' else b'[NVME]'
        if marker not in output:
            raise RuntimeError(model + ' driver did not claim the controller')
        for command, text in (('cat /disk/etc/readme.txt', 'SpiritFoxOS'),
                              ('run /disk/bin/diskhello.sfk', 'sfexec: exit 0'),
                              ('shutdown', 'SHUTDOWN OK')):
            start = len(output)
            for byte in command.encode() + b'\n':
                process.stdin.write(bytes([byte]))
                process.stdin.flush()
                time.sleep(.004)
            expect(text, 20, start)
        code = process.wait(timeout=10)
        if code != 1:
            raise RuntimeError(model + ' unexpected QEMU exit code ' + str(code))
        print('HARDWARE PASS: ' + model, flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        (ROOT / 'build' / ('qemu-hw-' + model + '.log')).write_bytes(output)


if __name__ == '__main__':
    boot('ahci')
    boot('nvme')
