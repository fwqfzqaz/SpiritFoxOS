"""Mock the production USB runner and exercise release gates without a VM/build."""
import ast
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import queue
import re
from types import SimpleNamespace
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def code(nodes, filename):
    return compile(ast.Module(body=nodes, type_ignores=[]), str(filename), 'exec')


USB_PATH = ROOT/'tests/qemu/qemu_usb_test.py'
USB_MAIN = code([next(n for n in ast.parse(USB_PATH.read_text(encoding='utf-8')).body
                      if isinstance(n, ast.FunctionDef) and n.name == 'main')], USB_PATH)
RELEASE_PATH = ROOT/'tools/package_release.py'
release_nodes = ast.parse(RELEASE_PATH.read_text(encoding='utf-8')).body
gate_start = next(i for i, n in enumerate(release_nodes)
                  if isinstance(n, ast.If) and 'linux_compat_qemu' in ast.unparse(n.test))
gate_end = next(i for i, n in enumerate(release_nodes)
                if isinstance(n, ast.If) and 'usb' in ast.unparse(n.test))
GATES = code(release_nodes[gate_start:gate_end+1], RELEASE_PATH)
TEST_PATH = ROOT/'tools/test.py'
test_nodes = ast.parse(TEST_PATH.read_text(encoding='utf-8')).body
SUMMARY = code([n for n in test_nodes if isinstance(n, ast.Assign) and
                any(isinstance(t, ast.Subscript) and isinstance(t.value, ast.Name) and
                    t.value.id == 'result' and isinstance(t.slice, ast.Constant) and
                    t.slice.value in ('linux_compat_qemu', 'linux_compat_qemu_boots', 'usb_qemu')
                    for t in n.targets)], TEST_PATH)


class FastQueue(queue.Queue):
    def get(self, *args, **kwargs):
        return super().get(block=False)


class UsbMock:
    """Only transport/process/time are modeled; execute the owning main unchanged."""
    def __init__(self, failure=None):
        self.failure = failure
        self.chunks = FastQueue()
        self.clock = 0
        self.returncode = None
        self.endpoint_closed = self.qmp_closed = False
        self.command_bytes = bytearray()
        self.help_calls = 0
        self.stdout = io.BytesIO(b''.join(text.encode()+b'\n' for name, text in (
            ('boot', 'BOOT TESTS PASSED'), ('prompt', 'Type help.'),
            ('controller', '[USB] ports=2'), ('keyboard', '[USB] HID keyboard'),
            ('mouse', '[USB] HID mouse')) if name != failure))
        self.stdin = SimpleNamespace(write=self.write, flush=lambda: None)
        self.qmp = SimpleNamespace(execute=self.execute, close=self.close_qmp)
        self.endpoint = SimpleNamespace(accept=lambda: self.qmp, close=self.close_endpoint)

    def emit(self, name, text):
        if self.failure != name:
            self.chunks.put(text.encode()+b'\n')

    def monotonic(self):
        self.clock += .1
        return self.clock

    def write(self, data):
        if data == b'\x1d':
            self.emit('gui-stop', 'GUI: stopped')
        else:
            self.command_bytes.extend(data)
            if data == b'\n':
                if self.command_bytes == b'gui\n':
                    self.emit('gui-start', 'GUI: started pid=9')
                    self.emit('framebuffer', 'WM: fb=800x600')
                elif self.command_bytes == b'shutdown\n':
                    self.emit('shutdown', 'SHUTDOWN OK')
                self.command_bytes.clear()

    def execute(self, name, arguments):
        if name == 'human-monitor-command':
            self.help_calls += 1
            self.emit('help-'+str(self.help_calls), 'help | login')
        elif name == 'input-send-event':
            self.emit('pointer', 'WM: pointer x=160 y=140')
        elif name == 'device_del':
            self.emit('removal', '[USB] device removed')
        elif name == 'device_add':
            self.emit('reenumeration', '[USB] HID keyboard')
        else:
            raise AssertionError('Unexpected mocked QMP command '+name)

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        if self.returncode is None:
            self.returncode = 2 if self.failure == 'exit' else 1
        return self.returncode

    def kill(self):
        self.returncode = -9

    def close_endpoint(self):
        self.endpoint_closed = True

    def close_qmp(self):
        self.qmp_closed = True
        if self.failure == 'cleanup':
            raise RuntimeError('Mocked cleanup failure')

    def run(self, folder):
        namespace = dict(ROOT=folder, QEMU=None if self.failure == 'prerequisite' else 'mock-qemu',
                         FIRMWARE='mock-firmware', VARS_SRC='mock-vars', json=json,
                         os=SimpleNamespace(chdir=lambda _: None),
                         shutil=SimpleNamespace(copyfile=lambda *_: None),
                         queue=SimpleNamespace(Queue=lambda: self.chunks, Empty=queue.Empty),
                         threading=SimpleNamespace(Thread=lambda target, daemon: SimpleNamespace(start=target)),
                         time=SimpleNamespace(monotonic=self.monotonic, sleep=lambda _: None),
                         QmpListener=lambda: self.endpoint,
                         command=lambda _: ['mock-qemu'],
                         subprocess=SimpleNamespace(PIPE=-1, STDOUT=-2, Popen=lambda *a, **k: self))
        exec(USB_MAIN, namespace)
        with redirect_stdout(io.StringIO()):
            namespace['main']()


class LinuxUsbEvidence(unittest.TestCase):
    def setUp(self):
        cache = ROOT/'.cache'
        cache.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='linux-usb-evidence-', dir=cache)
        self.folder = Path(self.temporary.name)
        (self.folder/'build').mkdir()
        self.report = self.folder/'build/usb-results.json'

    def tearDown(self):
        self.temporary.cleanup()

    def gates(self, result):
        # Read the same JSON types as package_release, without importing its
        # artifact/key/build dependencies or replacing the production checks.
        exec(GATES, {'result': json.loads(json.dumps(result))})

    def test_usb_publishes_observed_counts_after_successful_cleanup(self):
        transport = UsbMock()
        transport.run(self.folder)
        self.assertTrue(transport.endpoint_closed and transport.qmp_closed)
        self.assertEqual(transport.poll(), 1)
        evidence = json.loads(self.report.read_text(encoding='utf-8'))
        self.assertEqual(evidence, {'passed': True, 'evidence': 'E2-qemu', 'guest_boots': 1,
                                  'xhci_initialized': True, 'keyboard_enumerations': 2,
                                  'mouse_enumerations': 1, 'keyboard_shell_inputs': 2,
                                  'pointer_updates': 1, 'keyboard_removals': 1,
                                  'shutdown_exit_code': 1})
        self.gates({'linux_compat_qemu': True, 'linux_compat_qemu_boots': 1, 'usb_qemu': evidence})

    def test_failed_usb_checks_remove_stale_success(self):
        failures = {'prerequisite': 'Linux needs', 'boot': 'BOOT TESTS PASSED',
                    'prompt': 'Type help.', 'controller': '[USB] ports=',
                    'keyboard': '[USB] HID keyboard', 'mouse': '[USB] HID mouse',
                    'help-1': 'help | login', 'gui-start': 'GUI: started pid=',
                    'framebuffer': 'WM: fb=', 'pointer': 'USB mouse movement',
                    'removal': '[USB] device removed', 'reenumeration': '[USB] HID keyboard',
                    'gui-stop': 'GUI: stopped', 'help-2': 'help | login',
                    'shutdown': 'SHUTDOWN OK', 'exit': 'Unexpected QEMU exit code',
                    'cleanup': 'Mocked cleanup failure'}
        for failure, message in failures.items():
            with self.subTest(failure=failure):
                self.report.write_text('{"passed":true,"stale":true}', encoding='utf-8')
                with self.assertRaisesRegex(RuntimeError, re.escape(message)):
                    UsbMock(failure).run(self.folder)
                self.assertFalse(self.report.exists(), 'Failed USB run reused an old success')

    def test_release_refuses_missing_partial_or_host_only_evidence(self):
        UsbMock().run(self.folder)
        usb = json.loads(self.report.read_text(encoding='utf-8'))
        good = {'linux_compat_qemu': True, 'linux_compat_qemu_boots': 1, 'usb_qemu': usb}
        for key in good:
            result = dict(good)
            del result[key]
            with self.subTest(missing=key), self.assertRaises(SystemExit):
                self.gates(result)
        for key in usb:
            for bad in (None, False, '', 0, -1, 99):
                if key == 'pointer_updates' and bad == 99:
                    continue  # More than one actual pointer update is valid.
                result = dict(good, usb_qemu=dict(usb, **{key: bad}))
                with self.subTest(field=key, value=bad), self.assertRaises(SystemExit):
                    self.gates(result)
        for bad in (None, False, [], 'passed'):
            with self.subTest(report=bad), self.assertRaises(SystemExit):
                self.gates(dict(good, usb_qemu=bad))
        with self.assertRaises(SystemExit):
            self.gates(dict(good, linux_compat_qemu=False))
        with self.assertRaises(SystemExit):
            self.gates(dict(good, linux_compat_qemu_boots=0))
        with self.assertRaises(SystemExit):
            self.gates(dict(good, linux_compat_qemu_boots=True))
        self.gates(dict(good, usb_qemu=dict(usb, pointer_updates=3)))

    def test_summary_separates_host_only_from_actual_usb_report(self):
        for stale in (False, True):
            if stale:
                self.report.write_text('{"stale":true}', encoding='utf-8')
            result = {}
            exec(SUMMARY, dict(result=result, args=SimpleNamespace(host_only=True), ROOT=self.folder, json=json))
            self.assertEqual(result, {'linux_compat_qemu': False, 'linux_compat_qemu_boots': 0, 'usb_qemu': {}})
            with self.assertRaises(SystemExit):
                self.gates(result)
        UsbMock().run(self.folder)
        result = {}
        exec(SUMMARY, dict(result=result, args=SimpleNamespace(host_only=False), ROOT=self.folder, json=json))
        self.gates(result)
        self.report.unlink()
        with self.assertRaises(FileNotFoundError):
            exec(SUMMARY, dict(result={}, args=SimpleNamespace(host_only=False), ROOT=self.folder, json=json))

    def test_runner_registration_keeps_guest_tests_in_full_suite(self):
        full = next(n for n in test_nodes if isinstance(n, ast.If) and ast.unparse(n.test) == 'not args.host_only')
        scripts = [n.value for n in ast.walk(full) if isinstance(n, ast.Constant) and isinstance(n.value, str)]
        self.assertIn('tests/qemu/qemu_usb_test.py', scripts)
        self.assertIn('tests/qemu/qemu_linux_compat_test.py', scripts)


if __name__ == '__main__':
    unittest.main(verbosity=2)
