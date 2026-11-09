"""Portable QMP transport for the workspace's headless GUI VMs."""
import json
import socket


class QmpListener:
    """Reserve a loopback port until QEMU connects, avoiding a port race.

    QEMU is the TCP client; it still sends the normal QMP greeting. This
    works with Windows Python/QEMU without an AF_UNIX implementation.
    """
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.sock.bind(('127.0.0.1', 0))
            self.sock.listen(1)
            self.sock.settimeout(10)
        except BaseException:
            self.sock.close()
            raise

    def argument(self):
        return 'tcp:127.0.0.1:%d,server=off' % self.sock.getsockname()[1]

    def accept(self):
        try:
            connection, _ = self.sock.accept()
        finally:
            self.close()
        return Qmp(connection)

    def close(self):
        self.sock.close()


class Qmp:
    def __init__(self, connection):
        self.sock = connection
        self.sock.settimeout(10)
        self.buffer = b''
        try:
            if 'QMP' not in self.read():
                raise RuntimeError('Missing QMP greeting')
            self.execute('qmp_capabilities')
        except BaseException:
            self.close()
            raise

    def read(self):
        while b'\n' not in self.buffer:
            data = self.sock.recv(4096)
            if not data:
                raise RuntimeError('QMP connection closed')
            self.buffer += data
        line, _, self.buffer = self.buffer.partition(b'\n')
        return json.loads(line)

    def execute(self, name, arguments=None):
        payload = {'execute': name}
        if arguments is not None:
            payload['arguments'] = arguments
        self.sock.sendall((json.dumps(payload) + '\r\n').encode())
        while True:
            reply = self.read()
            if 'return' in reply:
                return reply['return']
            if 'error' in reply:
                raise RuntimeError('QMP %s failed: %r' % (name, reply['error']))

    def close(self):
        self.sock.close()
