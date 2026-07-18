#!/usr/bin/env python3
"""SpiritFoxOS Automated Test Runner - Prompt-synchronized with robust I/O handling"""

import subprocess
import time
import sys
import os
import re
import select
import fcntl

PROJECT_DIR = "/media/fwqfzqaz/王峥/SpiritFoxOS"

TESTS = [
    ("V01", "列出根目录", "ls /", ["bin/", "etc/", "dev/", "tmp/"]),
    ("V02", "创建文件", "touch /tmp/test1.txt", []),
    ("V03", "写入文件", "writefile /tmp/test1.txt Hello", []),
    ("V04", "读取文件", "cat /tmp/test1.txt", ["Hello"]),
    ("V05", "创建目录", "mkdir /tmp/testdir", []),
    ("V06", "目录列表", "ls /tmp", ["test1.txt", "testdir/"]),
    ("V09", "文件stat", "stat /etc/autorun.cfg", ["autorun.cfg"]),
    ("V14", "不存在的文件", "cat /tmp/nonexistent", ["No such file"]),
    ("V10a", "修改工作目录", "cd /tmp", []),
    ("V10b", "确认工作目录", "pwd", ["/tmp"]),
    ("V10c", "回到根目录", "cd /", []),
    ("V11", "复制文件", "cp /etc/autorun.cfg /tmp/autorun_copy", []),
    ("V12", "移动文件", "mv /tmp/autorun_copy /tmp/autorun_mv", []),
    ("V07", "删除文件", "rm /tmp/test1.txt", []),
    ("V08", "删除目录", "rmdir /tmp/testdir", []),
    ("V15", "列出/dev", "ls /dev", ["sda", "sdb", "ram0"]),
    ("V17", "列出/mnt", "ls /mnt", ["bin/", "pkg/"]),
    ("V19", "写入FAT32", "writefile /mnt/fat_test.txt TestData", []),
    ("V20", "读取FAT32", "cat /mnt/fat_test.txt", ["TestData"]),
    ("V21", "FAT32创建目录", "mkdir /mnt/vfs_testdir", []),
    ("V24", "列出/proc", "ls /proc", []),
    ("S01", "help命令", "help", ["Available commands", "ls", "cd", "cat"]),
    ("S02", "echo命令", "echo test output", ["test output"]),
    ("S03", "version命令", "version", ["v0.4.0"]),
    ("S04", "about命令", "about", ["SpiritFoxOS"]),
    ("S05", "uptime命令", "uptime", ["Uptime"]),
    ("S06", "meminfo命令", "meminfo", ["Total", "Used", "Free"]),
    ("S07", "pcilist命令", "pcilist", []),
    ("S08", "blklist命令", "blklist", ["sda", "sdb"]),
    ("S11", "ps命令", "ps", []),
    ("S12", "未知命令", "unknowncmd_test", ["unknown command"]),
    ("R03", "列出注册表", "reg list HKEY_SYSTEM", []),
    ("SB01", "沙箱状态", "sandbox", []),
    ("P01", "列出包", "pkg list", []),
    ("N01", "ifconfig", "ifconfig", []),
    ("I01", "输出重定向", "echo redirect_test > /tmp/redir.txt", []),
    ("I02", "读取重定向文件", "cat /tmp/redir.txt", ["redirect_test"]),
    ("I03", "追加重定向", "echo append_line >> /tmp/redir.txt", []),
    ("I05", "管道", "echo pipe_test | cat", ["pipe_test"]),
    ("K01", "VFS自测试", "vfstest", []),
    ("CL1", "清理临时文件", "rm /tmp/redir.txt", []),
    ("CL2", "清理autorun_mv", "rm /tmp/autorun_mv", []),
    ("CL3", "清理FAT32测试文件", "rm /mnt/fat_test.txt", []),
]


def set_nonblock(fd):
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)


def read_until_prompt(fd, prompt=b"/>", timeout=10.0, min_wait=0.5):
    """Read from fd until we see the shell prompt, with timeout.
    min_wait: minimum time to wait before starting prompt detection.
    This prevents false matches on the old prompt being overwritten."""
    data = b""
    deadline = time.time() + timeout
    # First, do a minimum wait to let the command start executing
    # and the old prompt get overwritten
    early_deadline = time.time() + min_wait
    while time.time() < early_deadline:
        try:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                data += chunk
        except (OSError, ValueError):
            break

    # Now look for the prompt
    while time.time() < deadline:
        try:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                data += chunk
                # Check if prompt appears in the last portion of data
                if prompt in data[-512:]:
                    # Wait a bit more to ensure prompt is complete
                    time.sleep(0.15)
                    # Drain any remaining bytes
                    try:
                        r2, _, _ = select.select([fd], [], [], 0.1)
                        if r2:
                            extra = os.read(fd, 4096)
                            if extra:
                                data += extra
                    except (OSError, ValueError):
                        pass
                    return data
        except (OSError, ValueError):
            break
    return data


def read_available(fd, timeout=0.5):
    """Read all available data from fd with timeout"""
    data = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                data += chunk
        except (OSError, ValueError):
            break
    return data


def run_tests():
    os.chdir(PROJECT_DIR)

    # Rebuild FAT32 and disk images to ensure clean state
    print("Rebuilding disk images for clean test state...")
    ret = subprocess.run(["make", "build/fat32.img", "build/disk.img"],
                         capture_output=True, timeout=60)
    if ret.returncode != 0:
        print(f"  WARNING: Failed to rebuild disk images: {ret.stderr.decode()}")
    else:
        print("  Disk images rebuilt successfully")

    qemu_cmd = [
        "qemu-system-x86_64",
        "-cdrom", "build/spiritfox.iso",
        "-boot", "d",
        "-m", "1G",
        "-serial", "stdio",
        "-display", "none",
        "-netdev", "user,id=net0,hostfwd=tcp::25565-:25565",
        "-device", "rtl8139,netdev=net0",
        "-no-reboot",
        "-device", "ahci,id=ahci0",
        "-drive", "id=fat32disk,file=build/fat32.img,if=none,format=raw",
        "-device", "ide-hd,drive=fat32disk,bus=ahci0.0",
        "-drive", "id=disk0,file=build/disk.img,if=none,format=raw",
        "-device", "ide-hd,drive=disk0,bus=ahci0.1",
    ]

    print("Starting QEMU...")
    proc = subprocess.Popen(
        qemu_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )

    # Set stdout to non-blocking
    set_nonblock(proc.stdout.fileno())

    # Wait for boot and first prompt
    print("Waiting for boot...")
    boot_output = read_until_prompt(proc.stdout.fileno(), prompt=b"/>", timeout=20.0, min_wait=1.0)
    # After finding the first prompt, drain a bit more to catch late boot output
    time.sleep(0.5)
    extra_boot = read_available(proc.stdout.fileno(), 1.0)
    boot_output += extra_boot
    print(f"  Boot complete ({len(boot_output)} bytes)")

    # Send commands one by one, wait for prompt after each
    all_output = boot_output
    test_outputs = {}

    for i, (test_id, desc, cmd, expected) in enumerate(TESTS):
        try:
            proc.stdin.write((cmd + "\n").encode())
            proc.stdin.flush()
        except BrokenPipeError:
            print(f"  QEMU died at command: {cmd}")
            break

        # Wait for the prompt to appear (command completed)
        # Use longer timeout for FAT32 and vfstest commands
        timeout = 10.0 if "mnt" in cmd or "vfstest" in cmd else 8.0
        response = read_until_prompt(proc.stdout.fileno(), prompt=b"/>", timeout=timeout, min_wait=0.3)
        all_output += response
        test_outputs[i] = response.decode("utf-8", errors="replace")

    # Final drain
    remaining = read_available(proc.stdout.fileno(), 1.0)
    all_output += remaining

    # Terminate QEMU
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except:
        proc.kill()

    # Save full output
    full_text = all_output.decode("utf-8", errors="replace")
    with open("build/test_full_output.log", "w") as f:
        f.write(full_text)

    # Evaluate tests
    results = {"PASS": [], "FAIL": []}

    print("\n" + "=" * 70)
    print("  SpiritFoxOS Test Results")
    print("=" * 70)

    for i, (test_id, desc, cmd, expected) in enumerate(TESTS):
        output = test_outputs.get(i, "").strip()

        if expected:
            missing = [exp for exp in expected if exp not in output]
            if not missing:
                results["PASS"].append((test_id, desc))
                status = "PASS"
            else:
                results["FAIL"].append((test_id, desc, f"Missing: {missing}", output))
                status = "FAIL"
        else:
            # Check for error keywords in output
            error_keywords = ["error", "Error", "ERROR", "panic", "triple fault", "cannot"]
            has_error = any(kw in output for kw in error_keywords)
            if has_error:
                results["FAIL"].append((test_id, desc, "Error in output", output))
                status = "FAIL"
            else:
                results["PASS"].append((test_id, desc))
                status = "PASS"

        output_preview = output[:120].replace("\n", "\\n").strip()
        print(f"  [{status:4s}] {test_id}: {desc:20s} | cmd='{cmd}' | out='{output_preview}'")

    # Summary
    print("\n" + "=" * 70)
    print("  Summary")
    print("=" * 70)
    total = len(TESTS)
    passed = len(results["PASS"])
    failed = len(results["FAIL"])
    print(f"  Total: {total}  |  PASS: {passed}  |  FAIL: {failed}")
    if total > 0:
        print(f"  Pass rate: {passed * 100 / total:.1f}%")

    if results["FAIL"]:
        print("\n  Failed tests details:")
        for item in results["FAIL"]:
            test_id, desc, reason, output = item
            print(f"    {test_id}: {desc} - {reason}")
            clean_output = output.replace("\n", "\\n")[:300]
            print(f"      Output: {clean_output}")

    print(f"\n  Full output saved to: build/test_full_output.log")
    return results


if __name__ == "__main__":
    run_tests()
