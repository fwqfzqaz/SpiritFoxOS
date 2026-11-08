"""有界类 niri 合成器的无显示 QEMU 测试：通过 QMP 注入 PS/2 鼠标相对事件，
断言上栏按钮命中、搜索面板点击、工作区切换与 framebuffer 校验和变化。
串口只用于启动 gui 与 SFK 内部文本输入，不再有任何布局快捷键。"""
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/package'))
(ROOT / '.cache').mkdir(exist_ok=True)
os.environ['TMPDIR'] = os.environ['TEMP'] = os.environ['TMP'] = str(ROOT / '.cache')
LINUX = hasattr(os, 'uname') and os.uname().sysname == 'Linux'
if LINUX:
    QEMU = shutil.which('qemu-system-x86_64')
    FIRMWARE = VARS_SRC = None
    for base in ('/usr/share/ovmf/x64', '/usr/share/OVMF', '/usr/share/edk2/x64'):
        for code_name, vars_name in (('OVMF_CODE.4m.fd', 'OVMF_VARS.4m.fd'),
                                     ('OVMF_CODE_4M.fd', 'OVMF_VARS_4M.fd'),
                                     ('OVMF_CODE.fd', 'OVMF_VARS.fd')):
            code, vars_ = Path(base) / code_name, Path(base) / vars_name
            if code.exists() and vars_.exists():
                FIRMWARE, VARS_SRC = code, vars_
                break
        if FIRMWARE:
            break
else:
    QEMU = str(ROOT / '.tools/qemu/qemu-system-x86_64.exe')
    FIRMWARE = ROOT / '.tools/qemu/share/edk2-x86_64-code.fd'
    VARS_SRC = ROOT / '.tools/qemu/share/edk2-i386-vars.fd'

SOCK = ROOT / 'build/qmp-gui-test.sock'


def command():
    # 显式 std VGA 让固件建立 GOP framebuffer；-display none 仍不打开窗口。
    return [str(QEMU), '-machine', 'q35', '-accel', 'tcg', '-cpu', 'qemu64,+nx',
            '-m', '256', '-smp', '1',
            '-drive', 'if=pflash,format=raw,readonly=on,file='+str(FIRMWARE),
            '-drive', 'if=pflash,format=raw,file='+str(ROOT / 'build/OVMF_VARS.fd'),
            '-drive', 'format=raw,file=fat:rw:'+str(ROOT / 'build/esp'),
            '-net', 'none', '-display', 'none', '-vga', 'std',
            '-serial', 'stdio', '-monitor', 'none', '-no-reboot',
            '-qmp', 'unix:'+str(SOCK)+',server,nowait',
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(str(path))
        self.buffer = b''
        self.read()
        self.execute('qmp_capabilities')

    def read(self):
        while b'\n' not in self.buffer:
            data = self.sock.recv(4096)
            if not data:
                raise RuntimeError('QMP connection closed')
            self.buffer += data
        line, _, self.buffer = self.buffer.partition(b'\n')
        return json.loads(line)

    def execute(self, name, arguments=None):
        payload = {'execute': name}
        if arguments is not None:
            payload['arguments'] = arguments
        self.sock.sendall((json.dumps(payload)+'\r\n').encode())
        while True:
            reply = self.read()
            if 'return' in reply:
                return reply['return']
            if 'error' in reply:
                raise RuntimeError('QMP %s failed: %r' % (name, reply['error']))

    def close(self):
        self.sock.close()


def main():
    if not QEMU or not FIRMWARE:
        raise RuntimeError('Linux needs qemu-system-x86_64 and OVMF (ovmf package) installed')
    os.chdir(ROOT)
    if SOCK.exists():
        SOCK.unlink()
    shutil.copyfile(VARS_SRC, ROOT / 'build/OVMF_VARS.fd')
    process = subprocess.Popen(command(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    chunks = queue.Queue()

    def read():
        while True:
            data = process.stdout.read(1)
            if not data:
                break
            chunks.put(data)

    threading.Thread(target=read, daemon=True).start()
    output = bytearray()
    log = ROOT / 'build/qemu-gui.log'

    def pump(text, timeout):
        until = time.monotonic()+timeout
        while text.encode() not in output:
            if time.monotonic() > until:
                raise RuntimeError('Timed out waiting for '+text)
            if process.poll() is not None and chunks.empty():
                raise RuntimeError('QEMU exited before '+text)
            try:
                output.extend(chunks.get(timeout=.1))
            except queue.Empty:
                continue

    def expect(text, timeout=60):
        pump(text, timeout)
        log.write_bytes(output)

    def drain(seconds):
        until = time.monotonic()+seconds
        while time.monotonic() < until:
            try:
                output.extend(chunks.get(timeout=.05))
            except queue.Empty:
                pass

    def wm_lines():
        return re.findall(rb'WM: fb=\d+x\d+ bpp=\d+ ws=(\d+) col=(\d+) win=(\d+) scroll=(\d+) checksum=([0-9a-f]{8})', output)

    def last_wm():
        lines = wm_lines()
        if not lines:
            raise RuntimeError('no WM status line yet')
        ws, col, win, scroll, checksum = lines[-1]
        return int(ws), int(col), int(win), int(scroll), checksum.decode()

    def wait_wm(count, timeout=10):
        until = time.monotonic()+timeout
        while len(wm_lines()) < count:
            if time.monotonic() > until:
                raise RuntimeError('Timed out waiting for WM line %d (have %d)' % (count, len(wm_lines())))
            try:
                output.extend(chunks.get(timeout=.1))
            except queue.Empty:
                continue

    def wait_win(count, timeout=10):
        until = time.monotonic()+timeout
        while last_wm()[2] != count:
            if time.monotonic() > until:
                raise RuntimeError('Timed out waiting for win=%d (have %d)' % (count, last_wm()[2]))
            try:
                output.extend(chunks.get(timeout=.1))
            except queue.Empty:
                continue

    def send(keys):
        # 串口 FIFO 有限：按人/波特率节奏发送。
        for key in keys:
            process.stdin.write(bytes([key]))
            process.stdin.flush()
            time.sleep(.03)

    def pointer_lines():
        return re.findall(rb'WM: pointer x=(\d+) y=(\d+) b=(\d+)', output)

    def wait_pointer(count, timeout=8):
        until = time.monotonic()+timeout
        while len(pointer_lines()) < count:
            if time.monotonic() > until:
                raise RuntimeError('Timed out waiting for pointer packet %d' % count)
            try:
                output.extend(chunks.get(timeout=.1))
            except queue.Empty:
                continue
        match = pointer_lines()[-1]
        return int(match[0]), int(match[1]), int(match[2])

    qmp = None
    pos = [640, 400]  # init 在进入图形前把指针复位到屏幕中心。

    def move_to(tx, ty):
        """用相对位移事件把指针移到目标；用串口回报的实际位置闭环校正。"""
        for _ in range(40):
            dx, dy = tx-pos[0], ty-pos[1]
            if abs(dx) <= 1 and abs(dy) <= 1:
                return
            sx = max(-120, min(120, dx))
            sy = max(-120, min(120, dy))
            events = []
            if sx:
                events.append({'type': 'rel', 'data': {'axis': 'x', 'value': sx}})
            if sy:
                # QEMU 的 PS/2 相对事件 y 正值对应屏幕向下。
                events.append({'type': 'rel', 'data': {'axis': 'y', 'value': sy}})
            before = len(pointer_lines())
            qmp.execute('input-send-event', {'events': events})
            pos[0], pos[1], _ = wait_pointer(before+1)
        raise RuntimeError('pointer could not reach (%d,%d) from %r' % (tx, ty, pos))

    def click(tx, ty, label):
        move_to(tx, ty)
        before = len(pointer_lines())
        qmp.execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
        wait_pointer(before+1)
        time.sleep(.4)  # 等合成器处理按下沿后再抬起。
        qmp.execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
        wait_pointer(before+2)
        expect('WM: click '+label, 10)

    try:
        expect('BOOT TESTS PASSED', 60)
        expect('Type help.', 30)
        send(b'gui\n')
        expect('GUI: started pid=', 30)
        expect('WM: fb=', 30)
        # 启动后的 probe 应已把演示应用登记进目录。
        expect('WM: app FILES', 30)
        wait_wm(1, 10)
        if last_wm()[:3] != (1, 1, 1):
            raise RuntimeError('unexpected initial layout %r' % (last_wm()[:3],))

        qmp = Qmp(SOCK)

        # 快速滑动：连续相对位移只应增量重画光标，不应产生整屏重绘/状态行。
        drain(.2)
        baseline = len(wm_lines())
        for _ in range(6):
            qmp.execute('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': 'x', 'value': 40}},
                {'type': 'rel', 'data': {'axis': 'y', 'value': 20}}]})
            time.sleep(.05)
        wait_pointer(len(pointer_lines())+1, 5)
        drain(.3)
        if len(wm_lines()) != baseline:
            raise RuntimeError('pointer movement still triggers full-screen redraw')
        last = pointer_lines()[-1]
        if int(last[0]) < 870 or int(last[1]) < 510:
            raise RuntimeError('fast pointer movement fell behind: %r' % (last,))
        pos[0], pos[1] = int(last[0]), int(last[1])

        # 上栏 SHELL 按钮打开 SFK；文本输入仍只属于 SFK 输入行。
        click(1084, 16, 'SHELL')
        send(b'apps\r')
        send(b'open settings\r')
        wait_win(2, 10)

        # 面板右上角 X 关闭 SFK。
        click(1218, 67, 'CLOSE')

        # 所有旧布局快捷键必须完全失效：按下去不产生任何新的 WM 状态行。
        drain(.4)
        baseline = len(wm_lines())
        wins = last_wm()[2]
        send(b'n`/2hjklx.')
        drain(1.0)
        if len(wm_lines()) != baseline or last_wm()[2] != wins:
            raise RuntimeError('layout shortcut still active after removal')

        # 上栏 SEARCH 按钮打开应用列表，点击行打开应用。
        click(1206, 16, 'SEARCH')
        click(640, 370, 'APP NOTES')
        wait_win(3, 10)

        # 点击面板外区域关闭搜索；布局不变。
        click(1206, 16, 'SEARCH')
        click(200, 700, 'CLOSE')
        if last_wm()[2] != 3:
            raise RuntimeError('closing search changed layout')

        # 工作区 pill：点击切换到空工作区再切回。
        click(76, 16, 'WS2')
        if last_wm()[:3] != (2, 0, 0):
            raise RuntimeError('clicking workspace 2 failed')
        click(28, 16, 'WS1')
        if last_wm()[:3] != (1, 1, 3):
            raise RuntimeError('clicking workspace 1 failed')

        # 内容区点击聚焦窗口。
        before = last_wm()[4]
        click(640, 100, 'FOCUS W1')
        if last_wm()[4] == before:
            raise RuntimeError('focus click did not alter framebuffer')

        if len(set(item[4] for item in wm_lines())) < 2:
            raise RuntimeError('framebuffer checksum never changed')
        send(b'\x1d')
        expect('GUI: stopped', 30)
        # 退出后串口 shell 仍然可用。
        send(b'help\n')
        expect('help | login', 30)
        print('GUI TEST PASSED: %d status lines, checksums=%d' % (
            len(wm_lines()), len(set(item[4] for item in wm_lines())),), flush=True)
    finally:
        log.write_bytes(output)
        if qmp:
            qmp.close()
        if process.poll() is None:
            process.kill()
            process.wait()
        if SOCK.exists():
            SOCK.unlink()


if __name__ == '__main__':
    main()
