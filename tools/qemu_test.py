"""Real UEFI/ring-3 end-to-end tests. Only this workspace's VM is controlled."""
import argparse
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def command(memory=256):
    return [str(ROOT / '.tools/qemu/qemu-system-x86_64.exe'), '-L', str(ROOT / '.tools/qemu/share'), '-machine', 'q35', '-accel', 'tcg', '-cpu', 'qemu64,+nx', '-m', str(memory), '-smp', '1', '-drive', 'if=pflash,format=raw,readonly=on,file='+str(ROOT / '.tools/qemu/share/edk2-x86_64-code.fd'), '-drive', 'if=pflash,format=raw,file='+str(ROOT / 'build/OVMF_VARS.fd'), '-drive', 'format=raw,file=fat:rw:'+str(ROOT / 'build/esp'), '-net', 'none', '-display', 'none', '-serial', 'stdio', '-monitor', 'none', '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']


def run(memory=256, interactive=False):
    os.chdir(ROOT)
    os.environ['TEMP'] = os.environ['TMP'] = str(ROOT / '.cache')
    shutil.copyfile(ROOT / '.tools/qemu/share/edk2-i386-vars.fd', ROOT / 'build/OVMF_VARS.fd')
    if interactive:
        return subprocess.call(command(memory))
    process = subprocess.Popen(command(memory), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    chunks = queue.Queue()
    def read():
        while True:
            data = process.stdout.read(1)
            if not data:
                break
            chunks.put(data)
    threading.Thread(target=read, daemon=True).start()
    output = bytearray()
    log = ROOT / 'build' / ('qemu-'+str(memory)+'.log')
    def expect(text, timeout=60):
        until = time.monotonic()+timeout
        while text.encode() not in output:
            if time.monotonic() > until:
                raise RuntimeError('Timed out waiting for '+text)
            if process.poll() is not None and chunks.empty():
                raise RuntimeError('QEMU exited before '+text)
            try:
                data = chunks.get(timeout=.1)
            except queue.Empty:
                continue
            output.extend(data)
            if output.endswith(b'\n'):
                line = output.splitlines()[-1].decode(errors='replace')
                print(line, flush=True)
                if '[FAIL]' in line or '[PANIC]' in line or '[EFISTUB] rejected' in line:
                    raise RuntimeError(line)
    try:
        expect('BOOT TESTS PASSED', 60)
        tests = [('help', 'help | ls'), ('modules', 'sf-init ACTIVE pinned'), ('echo', 'echo: ping'), ('run hello.sfk', 'sfexec: exit 0'), ('stop echo', 'echo: STOPPED'), ('start echo', 'echo: ACTIVE'), ('write /tmp/check persistent-in-ram', 'write: OK'), ('cat /tmp/check', 'persistent-in-ram'), ('ls /tmp', 'notes'), ('selftest', 'SELFTEST OK'), ('shutdown', 'SHUTDOWN OK')]
        for cmd, marker in tests:
            # A UART has a finite FIFO; emulate a human/baud-paced sender.
            for byte in cmd.encode()+b'\n':
                process.stdin.write(bytes([byte]))
                process.stdin.flush()
                time.sleep(.004)
            # Search only output produced after this command.
            old = bytes(output)
            output.clear()
            try:
                expect(marker, 10)
            finally:
                output[:0] = old
        code = process.wait(timeout=10)
        if code != 1:
            raise RuntimeError('Unexpected QEMU exit code '+str(code))
        print('QEMU PASS ('+str(memory)+' MiB)', flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        while not chunks.empty():
            output.extend(chunks.get())
        log.write_bytes(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--memory', type=int, default=256, choices=[128, 256, 512])
    parser.add_argument('--interactive', action='store_true')
    args = parser.parse_args()
    run(args.memory, args.interactive)
