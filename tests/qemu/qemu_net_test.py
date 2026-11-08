"""Legacy virtio-net in QEMU user-mode networking: ring-3 driver, IOAPIC INTx,
DHCP, ARP, ICMP echo, UDP, TCP and bounded connection objects.

The default boot has no disk; the driver must acquire its own DMA arena and
device grant, lease an address from slirp DHCP, receive real interrupts through
the IOAPIC, and answer network traffic. ``--disk`` boots the storage image as
well, proving a disk and a network device hold both kernel device slots, DMA
arenas, IOMMU windows and interrupt lines at the same time.
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


def main(disk=False):
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
    args = qemu_test.command(memory=256 if disk else 128,
                             disk=(ROOT / 'build/storage.img') if disk else None)
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
        label = 'disk+net' if disk else 'net'
        print('Network VM: %s, slirp 10.0.2.0/24' % label, flush=True)
        wait('BOOT TESTS PASSED')
        if b'net: READY' not in output:
            raise RuntimeError('network service did not report readiness\n' + output[-2500:].decode(errors='replace'))
        grants = output.count(b'[DEV] bdf=')
        if disk:
            wait('STORAGE READY')
            if b'STORAGE READY' not in output:
                raise RuntimeError('disk did not become ready alongside the network device\n' + output[-2500:].decode(errors='replace'))
            if grants < 2:
                raise RuntimeError('disk and network did not hold both device slots: ' + repr(output[-2500:]))
            app = command('run /disk/bin/diskhello.sfk', 'sfexec: exit 0', 30)
            if b'Hello from diskhello.sfk!' not in app:
                raise RuntimeError('disk application did not run while the network device was active: ' + repr(app))
        elif b'STORAGE READY' in output:
            raise RuntimeError('network VM unexpectedly mounted storage')
        info = command('ifconfig', 'link=up\r\n')
        if b'ip=10.0.2.15' not in info or b'gateway=10.0.2.2' not in info or b'netmask=255.255.255.0' not in info or b'dhcp=leased' not in info:
            raise RuntimeError('ifconfig did not report the DHCP lease: ' + repr(info))
        if b'[IRQ] IOAPIC ready' not in output:
            raise RuntimeError('IOAPIC route was not established\n' + output[-2500:].decode(errors='replace'))
        if b'[NET] interrupt-driven' not in output:
            raise RuntimeError('driver never observed an interrupt wakeup\n' + output[-2500:].decode(errors='replace') + repr(info))
        lease = command('dhcp', 'dhcp: LEASED ip=10.0.2.15', 40)
        if b'dhcp: LEASED' not in lease:
            raise RuntimeError('DHCP renewal did not lease from slirp: ' + repr(lease))
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
        irq = re.search(rb'irq=(\d+)', again)
        if not irq or int(irq.group(1)) < 1:
            raise RuntimeError('interrupt counter did not advance: ' + repr(again))
        command('ping 10.0.2.2', 'ping: reply from 10.0.2.2', 40)
        datagram = command('udp 10.0.2.2 %d hello' % UDP_PORT, 'udp: reply from 10.0.2.2:%d "ACK:hello"' % UDP_PORT, 40)
        if b'udp: reply' not in datagram:
            raise RuntimeError('UDP round trip missing: ' + repr(datagram))
        session = command('tcp 10.0.2.2 %d hello' % TCP_PORT, 'tcp: reply from 10.0.2.2:%d "ACK:hello"' % TCP_PORT, 40)
        if b'tcp: reply' not in session:
            raise RuntimeError('TCP round trip missing: ' + repr(session))
        command('conns', 'conns: 0 active', 20)
        netinfo = command('cat /net/info', 'dhcp=leased', 30)
        if b'state=up' not in netinfo or b'ip=10.0.2.15' not in netinfo:
            raise RuntimeError('/net/info projection missing lease: ' + repr(netinfo))
        opened = command('open 10.0.2.2 %d' % TCP_PORT, 'state=established', 60)
        match = re.search(rb'handle=(\d+)', opened)
        if not match:
            raise RuntimeError('connection object was not created: ' + repr(opened))
        handle = int(match.group(1))
        sent = command('send %d hello' % handle, 'sent=5', 40)
        if b'sent=5' not in sent:
            raise RuntimeError('connection send did not report progress: ' + repr(sent))
        received = command('recv %d' % handle, '"ACK:hello" state=', 40)
        if b'"ACK:hello"' not in received:
            raise RuntimeError('connection receive lost the echo: ' + repr(received))
        listed = command('cat /net/connections', 'peer=10.0.2.2:%d' % TCP_PORT, 30)
        if b'conn handle=%d' % handle not in listed:
            raise RuntimeError('/net/connections did not list the object: ' + repr(listed))
        active = command('conns', 'handle=%d' % handle, 30)
        if b'conns: 1 active' not in active or b'recv=9' not in active:
            raise RuntimeError('connection listing did not report traffic: ' + repr(active))
        closed = command('close %d' % handle, 'state=closed', 30)
        if b'state=closed' not in closed:
            raise RuntimeError('connection close did not complete: ' + repr(closed))
        command('conns', 'conns: 0 active', 30)
        command('shutdown', 'SHUTDOWN OK')
        code = process.wait(timeout=10)
        if code != 1:
            raise RuntimeError('Unexpected QEMU exit code ' + str(code))
        print('NETWORK QEMU PASS (%s: IOAPIC INTx, DHCP, ARP, ICMP, UDP, TCP and connection objects over slirp)' % label, flush=True)
    finally:
        stop.set()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        while not chunks.empty():
            output.extend(chunks.get())
        (ROOT / ('build/qemu-net-disk.log' if disk else 'build/qemu-net.log')).write_bytes(output)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--disk', action='store_true', help='boot the storage image alongside the network device')
    main(parser.parse_args().disk)
