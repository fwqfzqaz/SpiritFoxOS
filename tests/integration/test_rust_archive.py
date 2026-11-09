"""Use real COFF archives to guard the narrow Rust/Zig stack-probe adapter."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build
from rust_archive import stack_probe_member


class RustArchive(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='rust-archive-', dir=ROOT / 'build')
        self.folder = Path(self.temp.name).resolve()
        self.assertTrue(self.folder.is_relative_to(ROOT / 'build'))

    def tearDown(self):
        assert self.folder.is_relative_to(ROOT / 'build')
        self.temp.cleanup()

    def archive(self, source, name='compiler_builtins-fixture.stack_probe.rcgu.o'):
        c = self.folder / 'probe.c'
        obj = self.folder / name
        archive = self.folder / 'probe.a'
        c.write_text(source, encoding='utf-8')
        build.run(['-target', 'x86_64-windows-gnu', '-c', str(c), '-o', str(obj)])
        subprocess.run([build.ZIG, 'ar', 'rcs', str(archive), str(obj)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return archive, name

    def test_real_coff_index_and_removal(self):
        archive, name = self.archive('void ___chkstk_ms(void) {}')
        original = archive.read_bytes()
        self.assertEqual(stack_probe_member(original), name)
        # Remove the exact indexed object using the same real archiver;
        # a second, independent member must remain usable and indexed.
        c = self.folder / 'other.c'
        obj = self.folder / 'other.o'
        c.write_text('int retain_me(void) { return 42; }', encoding='utf-8')
        build.run(['-target', 'x86_64-windows-gnu', '-c', str(c), '-o', str(obj)])
        subprocess.run([build.ZIG, 'ar', 'r', str(archive), str(obj)], check=True)
        subprocess.run([build.ZIG, 'ar', 'd', str(archive), name], check=True)
        self.assertIsNone(stack_probe_member(archive.read_bytes()))
        listing = subprocess.check_output([build.ZIG, 'ar', 't', str(archive)], text=True)
        self.assertEqual(listing.strip(), 'other.o')

    def test_no_probe_is_unchanged(self):
        archive, _ = self.archive('int something_else(void) { return 7; }')
        self.assertIsNone(stack_probe_member(archive.read_bytes()))

    def test_combined_definition_is_rejected(self):
        archive, _ = self.archive('void ___chkstk_ms(void) {} int retain_me(void) { return 7; }')
        with self.assertRaisesRegex(ValueError, 'other symbols'):
            stack_probe_member(archive.read_bytes())

    def test_non_builtin_definition_is_rejected(self):
        archive, _ = self.archive('void ___chkstk_ms(void) {}', 'application.o')
        with self.assertRaisesRegex(ValueError, 'compiler_builtins'):
            stack_probe_member(archive.read_bytes())

    def test_truncated_and_non_archive_rejected(self):
        archive, _ = self.archive('void ___chkstk_ms(void) {}')
        for value in (b'not an archive', archive.read_bytes()[:-10]):
            with self.subTest(size=len(value)), self.assertRaises(ValueError):
                stack_probe_member(value)


if __name__ == '__main__':
    unittest.main()
