"""USB 输入回归：QEMU qemu-xhci + USB HID 键盘/鼠标。
验证 xHCI 初始化、HID 枚举、USB 键盘向串口 shell 输入、USB 鼠标驱动
合成器指针，以及运行期热插拔后的重新枚举。"""
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time
from qmp import QmpListener

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


def command(qmp_endpoint):
    return [str(QEMU), '-machine', 'q35', '-accel', 'tcg', '-cpu', 'qemu64,+nx',
            '-m', '256', '-smp', '1',
            '-drive', 'if=pflash,format=raw,readonly=on,file='+str(FIRMWARE),
            '-drive', 'if=pflash,format=raw,file='+str(ROOT / 'build/OVMF_VARS.fd'),
            '-drive', 'format=raw,file=fat:rw:'+str(ROOT / 'build/esp'),
            '-net', 'none', '-display', 'none', '-vga', 'std',
            '-serial', 'stdio', '-monitor', 'none', '-no-reboot',
            '-qmp', qmp_endpoint.argument(),
            '-device', 'qemu-xhci,id=xhci',
            '-device', 'usb-kbd,id=ukbd,bus=xhci.0',
            '-device', 'usb-mouse,id=umouse,bus=xhci.0',
            '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']


def main():
    if not QEMU or not FIRMWARE:
        raise RuntimeError('Linux needs qemu-system-x86_64 and OVMF (ovmf package) installed')
    os.chdir(ROOT)
    shutil.copyfile(VARS_SRC, ROOT / 'build/OVMF_VARS.fd')
    qmp_endpoint = QmpListener()
    try:
        process = subprocess.Popen(command(qmp_endpoint), stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   bufsize=0)
    except BaseException:
        qmp_endpoint.close()
        raise
    chunks = queue.Queue()

    def read():
        while True:
            data = process.stdout.read(1)
            if not data:
                break
            chunks.put(data)

    threading.Thread(target=read, daemon=True).start()
    output = bytearray()
    log = ROOT / 'build/qemu-usb.log'

    def expect(text, timeout=60):
        until = time.monotonic() + timeout
        while text.encode() not in output:
            if time.monotonic() > until:
                raise RuntimeError('Timed out waiting for ' + text)
            if process.poll() is not None and chunks.empty():
                raise RuntimeError('QEMU exited before ' + text)
            try:
                output.extend(chunks.get(timeout=.1))
            except queue.Empty:
                continue

    def send(data):
        # UART FIFO 有限：按人/波特率节奏发送。
        for byte in data:
            process.stdin.write(bytes([byte]))
            process.stdin.flush()
            time.sleep(.02)

    def pointer_count():
        return output.count(b'WM: pointer x=')

    qmp = None
    try:
        expect('BOOT TESTS PASSED', 60)
        expect('Type help.', 30)
        expect('[USB] ports=', 10)
        expect('[USB] HID keyboard', 20)
        expect('[USB] HID mouse', 20)
        qmp = qmp_endpoint.accept()

        # USB 键盘：按键经 xHCI 中断端点进入内核控制台输入缓冲。
        output.clear()
        qmp.execute('human-monitor-command', {'command-line': 'sendkey h-e-l-p-ret'})
        expect('help | login', 20)
        if b'help' not in output:
            raise RuntimeError('USB keyboard keystrokes missing from shell echo')

        # USB 鼠标：合成器从 SYS_POINTER_READ 拿到注入后的绝对坐标。
        output.clear()
        send(b'gui\n')
        expect('GUI: started pid=', 30)
        expect('WM: fb=', 30)
        before = pointer_count()
        qmp.execute('input-send-event', {'events': [
            {'type': 'rel', 'data': {'axis': 'x', 'value': 60}},
            {'type': 'rel', 'data': {'axis': 'y', 'value': 40}}]})
        until = time.monotonic() + 10
        while pointer_count() <= before:
            if time.monotonic() > until:
                raise RuntimeError('USB mouse movement did not reach the compositor')
            try:
                output.extend(chunks.get(timeout=.1))
            except queue.Empty:
                continue

        # 热插拔：移除后重新插入键盘，必须重新枚举并继续输入。
        output.clear()
        qmp.execute('device_del', {'id': 'ukbd'})
        expect('[USB] device removed', 20)
        output.clear()
        qmp.execute('device_add', {'driver': 'usb-kbd', 'id': 'ukbd2', 'bus': 'xhci.0'})
        expect('[USB] HID keyboard', 20)

        send(b'\x1d')
        expect('GUI: stopped', 30)

        # 重新枚举的键盘仍然可输入。
        output.clear()
        qmp.execute('human-monitor-command', {'command-line': 'sendkey h-e-l-p-ret'})
        expect('help | login', 20)

        send(b'shutdown\n')
        expect('SHUTDOWN OK', 30)
        code = process.wait(timeout=10)
        if code != 1:
            raise RuntimeError('Unexpected QEMU exit code ' + str(code))
        print('USB TEST PASSED', flush=True)
    finally:
        log.write_bytes(output)
        if qmp:
            qmp.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        qmp_endpoint.close()


if __name__ == '__main__':
    main()
