"""Legacy virtio-net in QEMU user-mode networking: ring-3 driver, ARP, ICMP echo, UDP and TCP.

The boot has no disk so the single DMA arena and single device owner are free;
the service must bring the device up and answer real slirp traffic.
"""
import os
import queue
import re
import shutil
import socket
import subprocess
import threading
import time
import qemu_test

ROOT = qemu_test.ROOT
UDP_PORT = 56001
TCP_PORT = 56002


def udp_echo_server(stop):
    """slirp maps 10.0.2.2 to the host loopback for guest-initiated UDP."""
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', UDP_PORT))
    server.settimeout(.2)
    try:
        while not stop.is_set():
            try:
                data, address = server.recvfrom(512)
            except socket.timeout:
                continue
            server.sendto(b'ACK:' + data, address)
    finally:
        server.close()


def tcp_echo_server(stop):
    """slirp redirects guest-initiated TCP to 10.0.2.2 to the host loopback."""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', TCP_PORT))
    server.listen(4)
    server.settimeout(.2)
    try:
        while not stop.is_set():
            try:
                connection, _ = server.accept()
            except socket.timeout:
                continue
            connection.settimeout(.5)
            try:
                data = connection.recv(512)
                if data:
                    connection.sendall(b'ACK:' + data)
                    while connection.recv(512):
                        pass
            except OSError:
                pass
            finally:
                connection.close()
    finally:
        server.close()


def main():
    if not qemu_test.QEMU or not qemu_test.FIRMWARE:
        raise RuntimeError('Linux needs qemu-system-x86_64 and OVMF (ovmf package) installed')
    os.chdir(ROOT)
    os.environ['TMPDIR'] = os.environ['TEMP'] = os.environ['TMP'] = str(ROOT / '.cache')
    shutil.copyfile(qemu_test.VARS_SRC, ROOT / 'build/OVMF_VARS.fd')
    stop = threading.Event()
    server = threading.Thread(target=udp_echo_server, args=(stop,), daemon=True)
    server.start()
    stream = threading.Thread(target=tcp_echo_server, args=(stop,), daemon=True)
    stream.start()
    args = qemu_test.command(memory=128)
    index = args.index('-net')
    assert args[index + 1] == 'none'
    args[index:index + 2] = ['-netdev', 'user,id=net0']
    args += ['-device', 'virtio-net-pci,netdev=net0,disable-modern=on,disable-legacy=off,addr=0x6']
    process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0, cwd=ROOT)
    chunks = queue.Queue()
    output = bytearray()
    def read():
        while data := process.stdout.read(1):
            chunks.put(data)
    threading.Thread(target=read, daemon=True).start()

    def wait(text, start=0, timeout=60):
        end = time.monotonic() + timeout
        while text.encode() not in output[start:]:
            if time.monotonic() > end:
                raise RuntimeError('Timed out waiting for ' + text + '\n' + output[-2500:].decode(errors='replace'))
            if process.poll() is not None and chunks.empty():
                raise RuntimeError('QEMU exited before ' + text)
            try:
                output.extend(chunks.get(timeout=.1))
            except queue.Empty:
                continue
            if any(x in output for x in (b'[FAIL]', b'[PANIC]', b'[EFISTUB] rejected')):
                raise RuntimeError('guest failure\n' + output[-2500:].decode(errors='replace'))

    def command(line, marker, timeout=30):
        start = len(output)
        for byte in line.encode() + b'\n':
            process.stdin.write(bytes([byte]))
            process.stdin.flush()
            time.sleep(.004)
        wait(marker, start, timeout)
        return bytes(output[start:])

    try:
        print('Network VM: no disk, slirp 10.0.2.0/24', flush=True)
        wait('BOOT TESTS PASSED')
        if b'net: READY' not in output:
            raise RuntimeError('network service did not report readiness\n' + output[-2500:].decode(errors='replace'))
        if b'STORAGE READY' in output:
            raise RuntimeError('network VM unexpectedly mounted storage')
        info = command('ifconfig', 'link=up\r\n')
        if b'ip=10.0.2.15' not in info or b'gateway=10.0.2.2' not in info or b'netmask=255.255.255.0' not in info:
            raise RuntimeError('ifconfig did not report the slirp profile: ' + repr(info))
        reply = command('ping 10.0.2.2', ' ms\r\n', 40)
        if b'ping: reply from 10.0.2.2' not in reply:
            raise RuntimeError('ping reply missing: ' + repr(reply))
        again = command('ifconfig', 'link=up\r\n')
        mac = re.search(rb'net: mac=([0-9a-f:]{17})', again)
        counts = re.search(rb'rx=(\d+) tx=(\d+)', again)
        if not mac or mac.group(1) != b'52:54:00:12:34:56':
            raise RuntimeError('device MAC not read from virtio config: ' + repr(again))
        if not counts or int(counts.group(1)) < 1 or int(counts.group(2)) < 1:
            raise RuntimeError('traffic counters did not advance: ' + repr(again))
        command('ping 10.0.2.2', 'ping: reply from 10.0.2.2', 40)
        datagram = command('udp 10.0.2.2 %d hello' % UDP_PORT, 'udp: reply from 10.0.2.2:%d "ACK:hello"' % UDP_PORT, 40)
        if b'udp: reply' not in datagram:
            raise RuntimeError('UDP round trip missing: ' + repr(datagram))
        session = command('tcp 10.0.2.2 %d hello' % TCP_PORT, 'tcp: reply from 10.0.2.2:%d "ACK:hello"' % TCP_PORT, 40)
        if b'tcp: reply' not in session:
            raise RuntimeError('TCP round trip missing: ' + repr(session))
        command('shutdown', 'SHUTDOWN OK')
        code = process.wait(timeout=10)
        if code != 1:
            raise RuntimeError('Unexpected QEMU exit code ' + str(code))
        print('NETWORK QEMU PASS (rx/tx advanced, ARP, ICMP, UDP and TCP over slirp)', flush=True)
    finally:
        stop.set()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        while not chunks.empty():
            output.extend(chunks.get())
        (ROOT / 'build/qemu-net.log').write_bytes(output)


if __name__ == '__main__':
    main()
