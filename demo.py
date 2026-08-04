#!/usr/bin/env python3
"""
SpiritFoxOS Demo Script
========================
通过 QEMU Monitor 协议模拟键盘输入，自动演示 SpiritFoxOS 主要功能。
每个操作之间停隔 5 秒，最终进入图形化界面后停止。

用法:
    python3 demo.py [--build]
    python3 demo.py --interval 3
"""

import socket
import subprocess
import time
import sys
import os
import argparse
import signal

PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
MONITOR_SOCK = "/tmp/spiritfox_demo_monitor.sock"

# ============================================================
# 演示命令列表 — (注释, shell命令)
# 注释为 None 表示该命令不单独显示注释（属于上一条注释的组）
# ============================================================
DEMO_COMMANDS = [
    # --- 系统信息 ---
    ("# --- System Info ---",          "help"),
    (None,                             "version"),
    (None,                             "about"),
    (None,                             "uptime"),

    # --- 硬件信息 ---
    ("# --- Hardware ---",             "meminfo"),
    (None,                             "pcilist"),
    (None,                             "blklist"),
    (None,                             "ps"),

    # --- 文件系统浏览 ---
    ("# --- Filesystem ---",           "ls /"),
    (None,                             "ls /dev"),
    (None,                             "ls /proc"),
    (None,                             "cat /etc/autorun.cfg"),

    # --- 文件操作 ---
    ("# --- File Operations ---",      "touch /tmp/demo.txt"),
    (None,                             "writefile /tmp/demo.txt HelloSpiritFox"),
    (None,                             "cat /tmp/demo.txt"),
    (None,                             "mkdir /tmp/demo_dir"),
    (None,                             "ls /tmp"),
    (None,                             "stat /tmp/demo.txt"),
    (None,                             "cp /tmp/demo.txt /tmp/demo2.txt"),
    (None,                             "mv /tmp/demo2.txt /tmp/demo3.txt"),
    (None,                             "rm /tmp/demo.txt"),
    (None,                             "rm /tmp/demo3.txt"),
    (None,                             "rmdir /tmp/demo_dir"),

    # --- 注册表/包管理/沙箱 ---
    ("# --- Registry / Packages ---",  "reg dump"),
    (None,                             "pkg list"),
    (None,                             "sandbox"),

    # --- 网络 ---
    ("# --- Network ---",              "ifconfig"),

    # --- 内核日志 ---
    ("# --- Kernel Log ---",           "dmesg"),

    # --- 进入图形界面 ---
    ("# --- Launch GUI ---",           "window"),
]

# ============================================================
# QEMU sendkey 键名映射（US 键盘布局）
# ============================================================
KEY_MAP = {
    # 空白和控制键
    ' ':  'spc',
    '\n': 'ret',
    '\t': 'tab',
    # 直接映射的特殊字符（无 shift）
    '-': 'minus',
    '=': 'equal',
    '/': 'slash',
    '.': 'dot',
    ',': 'comma',
    ';': 'semicolon',
    "'": 'apostrophe',
    '[': 'bracket_left',
    ']': 'bracket_right',
    '\\': 'backslash',
    '`': 'grave_accent',
    # Shift 组合键
    '_': 'shift-minus',
    '+': 'shift-equal',
    '?': 'shift-slash',
    '>': 'shift-dot',
    '<': 'shift-comma',
    ':': 'shift-semicolon',
    '"': 'shift-apostrophe',
    '{': 'shift-bracket_left',
    '}': 'shift-bracket_right',
    '|': 'shift-backslash',
    '~': 'shift-grave_accent',
    '!': 'shift-1',
    '@': 'shift-2',
    '#': 'shift-3',
    '$': 'shift-4',
    '%': 'shift-5',
    '^': 'shift-6',
    '&': 'shift-7',
    '*': 'shift-8',
    '(': 'shift-9',
    ')': 'shift-0',
}


def char_to_sendkey(ch):
    """将单个字符转换为 QEMU sendkey 键名。"""
    if ch in KEY_MAP:
        return KEY_MAP[ch]
    if ch.isalpha():
        return f'shift-{ch.lower()}' if ch.isupper() else ch
    if ch.isdigit():
        return ch
    return None


class QEMUMonitor:
    """QEMU Monitor 协议客户端（Unix socket）。"""

    def __init__(self, sock_path):
        self.sock_path = sock_path
        self.sock = None

    def connect(self, timeout=30):
        start = time.time()
        while time.time() - start < timeout:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(self.sock_path)
                self.sock.settimeout(5)
                self._drain()
                print("[DEMO] 已连接到 QEMU Monitor")
                return True
            except (ConnectionRefusedError, FileNotFoundError, OSError):
                time.sleep(0.5)
                if self.sock:
                    self.sock.close()
                    self.sock = None
        print("[DEMO] 错误: 无法连接到 QEMU Monitor")
        return False

    def _drain(self):
        """读取 monitor 输出直到出现 (qemu) 提示符。"""
        buf = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
                if b"(qemu)" in buf:
                    break
        except socket.timeout:
            pass

    def _send_cmd(self, cmd):
        """发送一条 monitor 命令并等待提示符。"""
        self.sock.sendall((cmd + "\n").encode())
        time.sleep(0.02)
        self._drain()

    def send_key(self, key_name):
        """发送单个按键事件。"""
        self._send_cmd(f"sendkey {key_name}")

    def type_string(self, text, delay=0.05):
        """逐字符模拟键盘输入。"""
        for ch in text:
            key = char_to_sendkey(ch)
            if key is None:
                continue
            self.send_key(key)
            time.sleep(delay)

    def type_command(self, cmd, delay=0.05):
        """输入一条 shell 命令并按回车。"""
        self.type_string(cmd, delay)
        time.sleep(0.1)
        self.send_key("ret")

    def screendump(self, path):
        """截屏保存到指定路径。"""
        self._send_cmd(f"screendump {path}")

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None


def kill_stale_qemu():
    """杀掉残留的 QEMU 进程。"""
    result = subprocess.run(["pgrep", "-f", "qemu-system-x86_64"],
                           capture_output=True, text=True)
    pids = result.stdout.strip().split()
    for pid in pids:
        if pid:
            try:
                os.kill(int(pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
    if pids and pids[0]:
        print(f"[DEMO] 已终止 {len(pids)} 个残留 QEMU 进程")
        time.sleep(2)


def build_iso():
    print("[DEMO] 正在构建 SpiritFoxOS ...")
    ret = subprocess.run(["make", "iso"], cwd=PROJECT_DIR)
    if ret.returncode != 0:
        print("[DEMO] 构建失败，退出")
        sys.exit(1)
    print("[DEMO] 构建完成")


def ensure_disk_images():
    build_dir = os.path.join(PROJECT_DIR, "build")
    disk_img = os.path.join(build_dir, "disk.img")
    if not os.path.exists(disk_img):
        print("[DEMO] 创建 disk.img ...")
        subprocess.run(["dd", "if=/dev/zero", f"of={disk_img}",
                        "bs=1M", "count=16"],
                       check=True, capture_output=True)
    fat32_img = os.path.join(build_dir, "fat32.img")
    if not os.path.exists(fat32_img):
        print("[DEMO] 创建 fat32.img ...")
        subprocess.run(["dd", "if=/dev/zero", f"of={fat32_img}",
                        "bs=1M", "count=512"],
                       check=True, capture_output=True)
        subprocess.run(["mkfs.fat", "-F", "32", fat32_img],
                       check=True, capture_output=True)


def find_free_port(start=25565, end=25580):
    """寻找一个未被占用的端口。"""
    for port in range(start, end):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind(("", port))
            s.close()
            return port
        except OSError:
            s.close()
            continue
    return start


def start_qemu():
    iso_file = os.path.join(PROJECT_DIR, "build", "spiritfox.iso")
    if not os.path.exists(iso_file):
        print(f"[DEMO] 错误: ISO 不存在 ({iso_file})")
        print("[DEMO] 请使用 --build 选项先构建")
        sys.exit(1)

    # 清理旧 socket
    if os.path.exists(MONITOR_SOCK):
        os.unlink(MONITOR_SOCK)

    build_dir = os.path.join(PROJECT_DIR, "build")
    disk_img = os.path.join(build_dir, "disk.img")
    fat32_img = os.path.join(build_dir, "fat32.img")

    port = find_free_port()
    print(f"[DEMO] 使用主机转发端口 {port}")

    cmd = [
        "qemu-system-x86_64",
        "-cdrom", iso_file,
        "-boot", "d",
        "-m", "1G",
        "-monitor", f"unix:{MONITOR_SOCK},server,nowait",
        "-vga", "std",
        "-netdev", f"user,id=net0,hostfwd=tcp::{port}-:25565",
        "-device", "rtl8139,netdev=net0",
        "-no-reboot",
        "-device", "ahci,id=ahci0",
        "-drive", f"id=fat32disk,file={fat32_img},if=none,format=raw",
        "-device", "ide-hd,drive=fat32disk,bus=ahci0.0",
        "-drive", f"id=disk0,file={disk_img},if=none,format=raw",
        "-device", "ide-hd,drive=disk0,bus=ahci0.1",
    ]

    print("[DEMO] 启动 QEMU ...")
    proc = subprocess.Popen(cmd, cwd=PROJECT_DIR)
    time.sleep(0.5)
    if proc.poll() is not None:
        print("[DEMO] 错误: QEMU 启动失败！")
        sys.exit(1)
    return proc


def run_demo(monitor, commands, interval=5, typing_delay=0.05):
    total = len(commands)
    for i, (comment, cmd) in enumerate(commands):
        print(f"\n[DEMO] ({i+1}/{total}) {comment or cmd}")
        # 只有注释非 None 时才输入注释行（一组命令只显示一次注释）
        if comment is not None:
            print(f"[DEMO] >>> {comment}")
            monitor.type_command(comment, delay=typing_delay)
            time.sleep(0.3)
        # 输入实际命令
        print(f"[DEMO] >>> {cmd}")
        monitor.type_command(cmd, delay=typing_delay)
        print(f"[DEMO] 等待 {interval} 秒 ...")
        time.sleep(interval)


def main():
    parser = argparse.ArgumentParser(
        description="SpiritFoxOS 自动演示脚本 — 通过模拟键盘输入展示主要功能")
    parser.add_argument("--build", action="store_true",
                        help="启动前先构建 ISO 镜像 (make iso)")
    parser.add_argument("--interval", type=int, default=5,
                        help="每个操作之间的停隔秒数 (默认 5)")
    parser.add_argument("--typing-delay", type=float, default=0.05,
                        help="每个按键之间的延迟秒数 (默认 0.05)")
    parser.add_argument("--boot-wait", type=int, default=20,
                        help="等待系统启动的秒数 (默认 20)")
    args = parser.parse_args()

    # 1. 构建
    if args.build:
        build_iso()

    # 2. 清理残留进程和资源
    kill_stale_qemu()
    ensure_disk_images()

    # 3. 启动 QEMU
    qemu_proc = start_qemu()

    def cleanup(signum=None, frame=None):
        print("\n[DEMO] 清理中 ...")
        if qemu_proc.poll() is None:
            qemu_proc.terminate()
            try:
                qemu_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu_proc.kill()
        if os.path.exists(MONITOR_SOCK):
            os.unlink(MONITOR_SOCK)
        print("[DEMO] 已退出")
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    # 4. 等待系统启动
    print(f"[DEMO] 等待系统启动 ({args.boot_wait} 秒) ...")
    time.sleep(args.boot_wait)

    # 5. 连接 Monitor
    monitor = QEMUMonitor(MONITOR_SOCK)
    if not monitor.connect(timeout=15):
        print("[DEMO] 无法连接 Monitor，退出")
        cleanup()
        return

    # 6. 再等几秒确保 shell 就绪，然后按回车激活
    print("[DEMO] 激活 Shell ...")
    time.sleep(3)
    monitor.send_key("ret")
    time.sleep(1)

    # 7. 执行演示
    print("\n" + "=" * 60)
    print("  SpiritFoxOS 功能演示")
    print("=" * 60)

    run_demo(monitor, DEMO_COMMANDS,
             interval=args.interval,
             typing_delay=args.typing_delay)

    print("\n" + "=" * 60)
    print("  演示完成！系统已进入图形化界面。")
    print("  按 Ctrl+C 退出 QEMU。")
    print("=" * 60)

    # 保持 QEMU 运行
    try:
        qemu_proc.wait()
    except KeyboardInterrupt:
        cleanup()


if __name__ == "__main__":
    main()
