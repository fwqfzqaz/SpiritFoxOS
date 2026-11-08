"""可视化运行 QEMU 图形切片：VNC 看画面并用鼠标点击上栏/搜索面板。

本环境编译的 QEMU 只有 none/vnc 显示后端，因此默认开 VNC；有 GTK 的机器
可用 --display gtk。启动后自动执行 gui；图形化不接受布局快捷键，终端按键
只在 SFK 打开时作为文本输入，Ctrl-] 退出。
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
LINUX = hasattr(os, 'uname') and os.uname().sysname == 'Linux'


def firmware():
    if LINUX:
        for base in ('/usr/share/ovmf/x64', '/usr/share/OVMF', '/usr/share/edk2/x64'):
            for code_name, vars_name in (('OVMF_CODE.4m.fd', 'OVMF_VARS.4m.fd'),
                                         ('OVMF_CODE_4M.fd', 'OVMF_VARS_4M.fd'),
                                         ('OVMF_CODE.fd', 'OVMF_VARS.fd')):
                code, vars_ = Path(base) / code_name, Path(base) / vars_name
                if code.exists() and vars_.exists():
                    return code, vars_
    else:
        return (ROOT / '.tools/qemu/share/edk2-x86_64-code.fd',
                ROOT / '.tools/qemu/share/edk2-i386-vars.fd')
    raise SystemExit('find OVMF failed: install the ovmf package')


def main():
    parser = argparse.ArgumentParser(description='Run the bounded niri-like GUI under QEMU')
    parser.add_argument('--vnc', default='127.0.0.1:1', help='VNC 监听地址，默认 127.0.0.1:1')
    parser.add_argument('--display', default=None, metavar='BACKEND',
                        help='改用本地显示后端（如 gtk / sdl）；默认用 VNC')
    parser.add_argument('--seconds', type=float, default=0,
                        help='自动退出前的秒数（0 表示手动 Ctrl-] 退出）')
    args = parser.parse_args()

    qemu = shutil.which('qemu-system-x86_64') if LINUX else str(ROOT / '.tools/qemu/qemu-system-x86_64.exe')
    if not qemu:
        raise SystemExit('qemu-system-x86_64 not found')
    if not (ROOT / 'build/esp').is_dir():
        raise SystemExit('build/esp missing: run python3 tools/build.py first')
    code, vars_src = firmware()
    shutil.copyfile(vars_src, ROOT / 'build/OVMF_VARS.fd')

    command = [qemu, '-machine', 'q35', '-accel', 'tcg', '-cpu', 'qemu64,+nx',
               '-m', '256', '-smp', '1',
               '-drive', 'if=pflash,format=raw,readonly=on,file='+str(code),
               '-drive', 'if=pflash,format=raw,file='+str(ROOT / 'build/OVMF_VARS.fd'),
               '-drive', 'format=raw,file=fat:rw:'+str(ROOT / 'build/esp'),
               '-net', 'none', '-vga', 'std', '-serial', 'stdio', '-monitor', 'none',
               '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
    if args.display:
        command += ['-display', args.display]
    else:
        command += ['-vnc', args.vnc]
    print('VNC: %s  (鼠标点击上栏；Ctrl-] 退出 gui)' % ('display='+args.display if args.display else args.vnc),
          flush=True)
    process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    output = bytearray()
    gui_sent = False

    def reader():
        nonlocal gui_sent
        while True:
            data = process.stdout.read(1)
            if not data:
                break
            output.extend(data)
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            if not gui_sent and b'Type help.' in output:
                gui_sent = True
                time.sleep(.1)
                process.stdin.write(b'gui\n')
                process.stdin.flush()

    threading.Thread(target=reader, daemon=True).start()
    if args.seconds:
        time.sleep(args.seconds)
        if process.poll() is None:
            process.stdin.write(b'\x1d')
            process.stdin.flush()
            deadline = time.monotonic()+20
            while process.poll() is None and time.monotonic() < deadline:
                time.sleep(.2)
            if process.poll() is None:
                process.kill()
        process.wait()
        return 0

    import select
    import termios
    import tty
    old = None
    fd = sys.stdin.fileno()
    if sys.stdin.isatty():
        old = termios.tcgetattr(fd)
        tty.setraw(fd)
    try:
        while process.poll() is None:
            if not select.select([fd], [], [], .1)[0]:
                continue
            data = sys.stdin.buffer.read(1)
            if not data:
                continue
            process.stdin.write(data)
            process.stdin.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if old is not None:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
        if process.poll() is None:
            process.kill()
        process.wait()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
