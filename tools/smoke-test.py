#!/usr/bin/env python3
"""UEFI boot smoke check with a hard timeout and explicit success markers."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--timeout", type=int, default=120)
parser.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-x86_64"))
parser.add_argument("--build-dir", type=Path, default=root / "build")
args = parser.parse_args()
candidates = [
    "/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_CODE_4M.fd",
    "/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/ovmf/OVMF_CODE.fd",
]
code = os.environ.get("OVMF_CODE") or next((p for p in candidates if Path(p).is_file()), "")
variables = os.environ.get("OVMF_VARS") or code.replace("OVMF_CODE", "OVMF_VARS")
if not Path(code).is_file() or not Path(variables).is_file():
    raise SystemExit("OVMF firmware pair missing; set OVMF_CODE and OVMF_VARS")
build = args.build_dir.resolve()
build.mkdir(exist_ok=True)
shutil.copyfile(variables, build / "smoke-vars.fd")
log = build / "smoke-serial.log"
error = build / "smoke-qemu.log"
# Do not let a previous run's markers win the race with QEMU opening the file.
log.write_bytes(b"")
command = [args.qemu, "-machine", "pc,accel=tcg", "-m", "2G", "-smp", "1",
    "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
    "-drive", f"if=pflash,format=raw,file={build / 'smoke-vars.fd'}",
    "-drive", f"if=ide,format=raw,file={build / 'spiritfox.img'}",
    "-device", "ahci,id=ahci0",
    "-drive", f"id=fat32disk,file={build / 'fat32.img'},if=none,format=raw,snapshot=on",
    "-device", "ide-hd,drive=fat32disk,bus=ahci0.0",
    "-drive", f"id=disk0,file={build / 'disk.img'},if=none,format=raw,snapshot=on",
    "-device", "ide-hd,drive=disk0,bus=ahci0.1",
    "-display", "none", "-serial", f"file:{log}", "-monitor", "none",
    "-net", "none", "-no-reboot", "-no-shutdown"]
required = [b"[SpiritFoxOS] Enabling interrupts...", b"[SpiritFoxOS] shell_run...", b"[autorun] done,",
            b"[sfmod] self-test passed:", b"[sfmod] VFS bridge passed:",
            b"[sfmod-user] file IO passed", b"[sfmod-user] fork/wait stress passed",
            b"[sfmod] tmpfs mounted: /tmp /run", b"[sfmod-user] tmpfs trees/lifetimes passed",
            b"[sfmod] device service self-test passed", b"[sfmod] special devices attached:",
            b"[sfmod-user] special devices passed",
            b"[sfmod] PCI snapshot matches platform inventory",
            b"[sfmod] early services ready before storage",
            b"[AHCI] Controller selected via dev.pci BAR5=",
            b"[AHCI] Port 0: SATA disk sda", b"[AHCI] Port 1: SATA disk sdb",
            b"[RAMDISK] Created ram0: 2048 sectors",
            b"[FAT32] Mounted on blkdev 0", b"[VFS] Mounted fat32 at /mnt",
            b"[sfmod] filesystem services ready after VFS/process",
            b"[sfmod] sealed block inventory matches platform devices",
            b"[bochs_drm] VGA selected via dev.pci",
            b"[process] reserved PID preemption passed",
            b"[sfmod] unified device registry matches public /dev",
            b"[sfmod-user] unified device directories passed",
            b"[client] Window created", b"[client] frame 10"]
bad = [b"KERNEL PANIC", b"Triple fault", b"[EXCEPTION]", b"!!! EXCEPTION:", b"System Halted.",
       b"[SCHED] PANIC:", b"[SCHED] HALT:",
       b"[sfmod] PCI snapshot self-test failed",
       b"[sfmod-user] redirect leaked",
       b"[client] ERROR:", b"[client] wl_display_dispatch error:", b"[sfmod-user] FAILED", b"[exit] BUG:", b"[sched] BUG:", b"[wait] BUG:", b"[sfmod] bridge release failed"]
with error.open("wb") as errors:
    process = subprocess.Popen(command, cwd=root, stdout=subprocess.DEVNULL, stderr=errors)
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            data = log.read_bytes() if log.exists() else b""
            if any(marker in data for marker in bad):
                raise SystemExit(f"Kernel failed; inspect {log}")
            if all(marker in data for marker in required):
                # Catch a double package load, allocator reset or late PCI service.
                ordered = [b"[SpiritFoxOS] kmalloc_init...", b"[SpiritFoxOS] slab_init...",
                    b"[sfmod] early services ready before storage",
                    b"[AHCI] Controller selected via dev.pci BAR5=",
                    b"[sfmod] sealed block inventory matches platform devices",
                    b"[SpiritFoxOS] vfs_init...", b"[SpiritFoxOS] process_init...",
                    b"[sfmod] self-test passed:", b"[sfmod] VFS bridge passed:",
                    b"[sfmod] filesystem services ready after VFS/process",
                    b"[sfmod] unified device registry matches public /dev",
                    b"[SpiritFoxOS] Enabling interrupts..."]
                positions = [data.find(marker) for marker in ordered]
                modules = [b"fs.vfs", b"demo.hello", b"fs.tmpfs", b"dev.tmpfs", b"dev.pci", b"dev.registry", b"dev.block"]
                once = ordered + [b"[sfmod] ready: " + name + b"\n" for name in modules]
                if any(data.count(marker) != 1 for marker in once) or positions != sorted(positions):
                    raise SystemExit(f"Boot phase order/package single-load check failed; inspect {log}")
                # Keep running briefly: a boot marker alone can precede a fault.
                time.sleep(3)
                data = log.read_bytes()
                if any(marker in data for marker in bad) or process.poll() is not None:
                    raise SystemExit(f"Kernel failed after boot; inspect {log} and {error}")
                print(f"UEFI boot, module IPC/file IO and Wayland frames passed; log: {log}")
                break
            if process.poll() is not None:
                raise SystemExit(f"QEMU exited ({process.returncode}); inspect {error}")
            time.sleep(0.25)
        else:
            raise SystemExit(f"UEFI boot timed out after {args.timeout}s; inspect {log} and {error}")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
