"""QEMU 图形化的真实像素验证：用 QMP screendump 读取 VGA 扫描输出，
断言工作区栏、活动页签与窗口色块确实出现在屏幕上——区别于仅检查
framebuffer 校验和，这里验证的是显示设备最终扫描出的像素。"""
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

SOCK = ROOT / 'build/qmp-gui.sock'

# 与 services/sf_wm.c 中的颜色常量保持一致。
BAR = (0x20, 0x28, 0x30)
TAB = (0x30, 0x38, 0x40)
TAB_ACTIVE = (0x40, 0x60, 0x80)
WINDOW = (0x38, 0x48, 0x58)
WINDOW_ALT = (0x40, 0x58, 0x68)
BORDER = (0x60, 0x80, 0x90)
BACKGROUND = (0x10, 0x18, 0x20)
ACCENT = (0x70, 0x90, 0xA0)
FOCUS = (0xE0, 0xA0, 0x40)
TEXT = (0xE8, 0xF0, 0xF8)
TEXT_DIM = (0xA8, 0xBC, 0xC8)
PANEL = (0x18, 0x20, 0x28)
SELECT = (0x40, 0x60, 0x80)
BLACK = (0x00, 0x00, 0x00)


def command():
    # 显式 std VGA 让固件建立 GOP framebuffer；-display none 不打开窗口，
    # 但设备仍在扫描输出，screendump 可读到真实像素。
    return [str(QEMU), '-machine', 'q35', '-accel', 'tcg', '-cpu', 'qemu64,+nx',
            '-m', '256', '-smp', '1',
            '-drive', 'if=pflash,format=raw,readonly=on,file='+str(FIRMWARE),
            '-drive', 'if=pflash,format=raw,file='+str(ROOT / 'build/OVMF_VARS.fd'),
            '-drive', 'format=raw,file=fat:rw:'+str(ROOT / 'build/esp'),
            '-net', 'none', '-display', 'none', '-vga', 'std',
            '-serial', 'stdio', '-monitor', 'none', '-no-reboot',
            '-qmp', 'unix:'+str(SOCK)+',server,nowait']


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(str(path))
        self.buffer = b''
        self.read()  # greeting
        self.execute('qmp_capabilities')

    def read(self):
        while b'\n' not in self.buffer:
            self.buffer += self.sock.recv(4096)
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


def read_ppm(path):
    data = path.read_bytes()
    if not data.startswith(b'P6'):
        raise RuntimeError('not a P6 PPM')
    fields = []
    pos = 2
    while len(fields) < 3:
        while pos < len(data) and data[pos] in b' \t\r\n':
            pos += 1
        if data[pos:pos+1] == b'#':
            while pos < len(data) and data[pos] not in b'\r\n':
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in b' \t\r\n':
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    width, height, maxval = fields
    if maxval != 255:
        raise RuntimeError('unsupported PPM maxval %d' % maxval)
    pixels = data[pos:pos+width*height*3]
    if len(pixels) != width*height*3:
        raise RuntimeError('truncated PPM pixels')
    return width, height, pixels


def pixel(image, x, y):
    width, _, pixels = image
    off = (y*width+x)*3
    return pixels[off], pixels[off+1], pixels[off+2]


def count_color(image, x0, y0, x1, y1, want):
    width, height, pixels = image
    count = 0
    for y in range(max(0, y0), min(height, y1)):
        for x in range(max(0, x0), min(width, x1)):
            off = (y*width+x)*3
            if (pixels[off], pixels[off+1], pixels[off+2]) == want:
                count += 1
    return count


def save_png(ppm_path):
    """可选把截图转成 PNG，便于人工查看；缺少 Pillow 时静默跳过。"""
    try:
        from PIL import Image
    except ImportError:
        return
    Image.open(ppm_path).save(ppm_path.with_suffix('.png'))


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
    log = ROOT / 'build/qemu-gui-shot.log'

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

    def wm_lines():
        return re.findall(rb'WM: fb=(\d+)x(\d+) bpp=\d+ ws=(\d+) col=(\d+) win=(\d+) scroll=(\d+) checksum=([0-9a-f]{8})', output)

    def wait_wm(count, timeout=10):
        until = time.monotonic()+timeout
        while len(wm_lines()) < count:
            if time.monotonic() > until:
                raise RuntimeError('Timed out waiting for WM line %d' % count)
            try:
                output.extend(chunks.get(timeout=.1))
            except queue.Empty:
                continue

    def send(keys):
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

    pos = [640, 400]  # init 在进入图形前把指针复位到屏幕中心。

    def move_to(tx, ty):
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
                events.append({'type': 'rel', 'data': {'axis': 'y', 'value': sy}})
            before = len(pointer_lines())
            qmp.execute('input-send-event', {'events': events})
            pos[0], pos[1], _ = wait_pointer(before+1)
        raise RuntimeError('pointer could not reach (%d,%d) from %r' % (tx, ty, pos))

    def click(tx, ty):
        move_to(tx, ty)
        before = len(pointer_lines())
        qmp.execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': True}}]})
        wait_pointer(before+1)
        time.sleep(.4)
        qmp.execute('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': False}}]})
        wait_pointer(before+2)

    qmp = None
    try:
        expect('Type help.', 60)
        send(b'gui\n')
        expect('GUI: started pid=', 30)
        wait_wm(1, 10)
        # probe 注册完成后再截图，保证启动器目录包含演示应用。
        expect('WM: app SETTINGS', 30)
        qmp = Qmp(SOCK)
        fb_width, fb_height = (int(v) for v in wm_lines()[0][:2])

        def capture(name):
            time.sleep(.6)  # 指针事件只保证日志先于重绘，截屏前给合成器留出时间。
            path = ROOT / ('build/'+name+'.ppm')
            qmp.execute('screendump', {'filename': str(path)})
            for _ in range(50):
                if path.exists() and path.stat().st_size > 0:
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('screendump produced no file for '+name)
            image = read_ppm(path)
            save_png(path)
            return image

        def probes(image, checks):
            for name, x, y, want in checks:
                got = pixel(image, x, y)
                if got != want:
                    raise RuntimeError('%s at (%d,%d): got %r want %r' % (name, x, y, got, want))

        image = capture('gui-ws1')
        if image[0] != fb_width or image[1] != fb_height:
            raise RuntimeError('screen %dx%d != framebuffer %dx%d' % (image[0], image[1], fb_width, fb_height))
        probes(image, [
            ('bar', 2, 2, BAR),
            ('active workspace pill', 14, 12, TAB_ACTIVE),
            ('inactive workspace pill', 74, 12, TAB),
            ('window top border', 100, 32, BORDER),
            ('focus ring', 100, 29, FOCUS),
            ('window body', 100, 100, WINDOW),
            ('window badge', 22, 44, ACCENT),
            ('shell button', 1040, 16, TAB),
            ('search button', 1160, 16, TAB),
            ('cursor tip', 640, 400, TEXT),
            ('cursor outline', 639, 400, BLACK),
        ])
        # 上栏工作区数字与 WS/COL/WIN 状态文本必须真实出现在扫描输出中。
        if count_color(image, 8, 4, 500, 26, TEXT) < 30:
            raise RuntimeError('bar labels/status text missing')

        # 指针悬停 SEARCH：按钮底色变为选中色。
        move_to(1206, 16)
        image = capture('gui-hover')
        probes(image, [('search button hover', 1160, 16, SELECT)])

        # 点击 SEARCH 打开应用列表；悬停行高亮，按钮保持激活色。
        click(1206, 16)
        expect('WM: click SEARCH', 20)
        move_to(640, 370)
        image = capture('gui-search')
        probes(image, [
            ('panel background', 880, 320, PANEL),
            ('panel border', 640, 187, ACCENT),
            ('row hover', 920, 364, SELECT),
            ('search button active', 1160, 16, TAB_ACTIVE),
        ])
        if count_color(image, 350, 204, 700, 230, TEXT) < 20:
            raise RuntimeError('search title text missing')
        if count_color(image, 350, 340, 600, 412, TEXT) < 15:
            raise RuntimeError('search row text missing')
        if count_color(image, 350, 578, 900, 596, TEXT_DIM) < 8:
            raise RuntimeError('search hint text missing')

        # 点击 NOTES 行：新建第二个窗口，同一列内上下平铺。
        click(640, 370)
        expect('WM: click APP NOTES', 20)
        time.sleep(.4)
        if int(wm_lines()[-1][4]) != 2:
            raise RuntimeError('clicking NOTES did not add a window')
        image = capture('gui-stacked')
        probes(image, [
            ('rounded corner', 12, 32, BACKGROUND),
            ('first window body', 300, 100, WINDOW),
            ('second window body', 300, 430, WINDOW_ALT),
        ])

        # 点击上栏 SHELL 打开 SFK；文本输入仍然只影响输入行。
        click(1084, 16)
        expect('WM: click SHELL', 20)
        send(b'help\r')
        time.sleep(.5)
        image = capture('gui-sfk')
        probes(image, [
            ('sfk panel background', 1000, 600, PANEL),
            ('sfk panel border', 30, 400, ACCENT),
            ('sfk input background', 72, 480, SELECT),
            ('sfk close button', 1210, 60, SELECT),
        ])
        if count_color(image, 56, 60, 700, 74, TEXT) < 20:
            raise RuntimeError('SFK title text missing')
        if count_color(image, 56, 88, 1100, 400, TEXT) < 30:
            raise RuntimeError('SFK history text missing')
        if count_color(image, 64, 490, 200, 508, TEXT) < 10:
            raise RuntimeError('SFK prompt text missing')
        if count_color(image, 56, 700, 1050, 740, TEXT_DIM) < 10:
            raise RuntimeError('SFK hint text missing')

        # 面板右上角 X 可点击关闭。
        click(1218, 67)
        expect('WM: click CLOSE', 20)
        send(b'\x1d')
        expect('GUI: stopped', 30)
        print('GUI SCREENSHOT PASSED: %dx%d, 23 pixel probes, 5 captures' % (fb_width, fb_height), flush=True)
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
