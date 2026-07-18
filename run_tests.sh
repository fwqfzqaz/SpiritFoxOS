#!/bin/bash
# SpiritFoxOS Automated Test Runner
# Sends commands to QEMU via serial port and captures output

set -e

PROJECT_DIR="/media/fwqfzqaz/王峥/SpiritFoxOS"
BUILD_DIR="$PROJECT_DIR/build"
SERIAL_LOG="$BUILD_DIR/test_serial.log"
TEST_OUTPUT="$BUILD_DIR/test_results.log"
QEMU_PID_FILE="$BUILD_DIR/qemu.pid"

# Test commands to execute (each line is a shell command)
TEST_COMMANDS=(
    # VFS/文件系统测试 - memfs
    "ls /"
    "touch /tmp/test1.txt"
    "writefile /tmp/test1.txt Hello"
    "cat /tmp/test1.txt"
    "mkdir /tmp/testdir"
    "ls /tmp"
    "stat /etc/autorun.cfg"
    "cat /tmp/nonexistent"
    "cd /tmp"
    "pwd"
    "cd /"
    "cp /etc/autorun.cfg /tmp/autorun_copy"
    "ls /tmp"
    "mv /tmp/autorun_copy /tmp/autorun_mv"
    "ls /tmp"
    "rm /tmp/test1.txt"
    "rmdir /tmp/testdir"
    "ls /tmp"
    # devfs 测试
    "ls /dev"
    # FAT32 测试
    "ls /mnt"
    "writefile /mnt/fat_test.txt TestData"
    "cat /mnt/fat_test.txt"
    "mkdir /mnt/test_dir"
    "ls /mnt"
    # procfs 测试
    "ls /proc"
    # Shell 命令测试
    "help"
    "echo test output"
    "version"
    "about"
    "uptime"
    "meminfo"
    "pcilist"
    "blklist"
    "ps"
    "unknowncmd_test"
    # 注册表测试
    "reg list HKEY_SYSTEM"
    # 沙箱测试
    "sandbox"
    # 包管理测试
    "pkg list"
    # 网络测试
    "ifconfig"
    # 管道重定向测试
    "echo redirect_test > /tmp/redir.txt"
    "cat /tmp/redir.txt"
    "echo append_line >> /tmp/redir.txt"
    "cat /tmp/redir.txt"
    "echo pipe_test | cat"
    # VFS 自测试
    "vfstest"
    # 稳定性测试
    "ls /"
    "ls /"
    "ls /"
    "ls /tmp"
    # 清理
    "rm /tmp/redir.txt"
    "rm /tmp/autorun_mv"
    "rm /mnt/fat_test.txt"
    "ls /tmp"
)

echo "========================================="
echo "  SpiritFoxOS Automated Test Runner"
echo "========================================="

# Ensure disk images exist
cd "$PROJECT_DIR"
test -f build/disk.img || dd if=/dev/zero of=build/disk.img bs=1M count=16 2>/dev/null
test -f build/fat32.img || {
    dd if=/dev/zero of=build/fat32.img bs=1M count=512 2>/dev/null
    mkfs.fat -F 32 build/fat32.img 2>/dev/null
    mmd -i build/fat32.img ::bin 2>/dev/null
    mmd -i build/fat32.img ::pkg 2>/dev/null
}

# Start QEMU in background with serial to pty
echo "[TEST] Starting QEMU..."
rm -f "$SERIAL_LOG"

# Use a FIFO for sending commands to QEMU
FIFO_IN="$BUILD_DIR/qemu_input.fifo"
rm -f "$FIFO_IN"
mkfifo "$FIFO_IN"

# Start QEMU with serial connected to our FIFO for input and a log for output
qemu-system-x86_64 \
    -cdrom build/spiritfox.iso \
    -boot d \
    -m 1G \
    -serial pipe:"$BUILD_DIR/qemu_serial" \
    -display none \
    -netdev user,id=net0,hostfwd=tcp::25565-:25565 \
    -device rtl8139,netdev=net0 \
    -no-reboot \
    -device ahci,id=ahci0 \
    -drive id=fat32disk,file=build/fat32.img,if=none,format=raw \
    -device ide-hd,drive=fat32disk,bus=ahci0.0 \
    -drive id=disk0,file=build/disk.img,if=none,format=raw \
    -device ide-hd,drive=disk0,bus=ahci0.1 \
    &

QEMU_PID=$!
echo $QEMU_PID > "$QEMU_PID_FILE"
echo "[TEST] QEMU PID: $QEMU_PID"

# Wait for QEMU to create the serial pipe files
sleep 2

# The pipe creates qemu_serial.in and qemu_serial.out
SERIAL_IN="$BUILD_DIR/qemu_serial.in"
SERIAL_OUT="$BUILD_DIR/qemu_serial.out"

if [ ! -p "$SERIAL_IN" ] || [ ! -p "$SERIAL_OUT" ]; then
    echo "[TEST] ERROR: Serial pipes not found. Using stdio method instead."
    kill $QEMU_PID 2>/dev/null || true
    rm -f "$FIFO_IN"
    
    # Fallback: use expect-like approach with stdin/stdout
    echo "[TEST] Using fallback method (timeout + stdin/stdout)..."
    
    # Create a script that sends commands with delays
    {
        sleep 8  # Wait for boot
        for cmd in "${TEST_COMMANDS[@]}"; do
            echo "$cmd"
            sleep 1
        done
        sleep 2
    } | timeout 120 qemu-system-x86_64 \
        -cdrom build/spiritfox.iso \
        -boot d \
        -m 1G \
        -serial stdio \
        -display none \
        -netdev user,id=net0,hostfwd=tcp::25565-:25565 \
        -device rtl8139,netdev=net0 \
        -no-reboot \
        -device ahci,id=ahci0 \
        -drive id=fat32disk,file=build/fat32.img,if=none,format=raw \
        -device ide-hd,drive=fat32disk,bus=ahci0.0 \
        -drive id=disk0,file=build/disk.img,if=none,format=raw \
        2>/dev/null | tee "$SERIAL_LOG"
    
    echo "[TEST] Test session completed."
else
    echo "[TEST] Serial pipes ready."
    
    # Read output in background
    cat "$SERIAL_OUT" > "$SERIAL_LOG" &
    CAT_PID=$!
    
    # Wait for boot
    sleep 8
    
    # Send commands
    for cmd in "${TEST_COMMANDS[@]}"; do
        echo "$cmd" > "$SERIAL_IN"
        sleep 1
    done
    
    sleep 3
    
    # Cleanup
    kill $CAT_PID 2>/dev/null || true
    kill $QEMU_PID 2>/dev/null || true
fi

rm -f "$FIFO_IN"
echo "[TEST] Results saved to $SERIAL_LOG"
