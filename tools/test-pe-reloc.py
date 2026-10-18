#!/usr/bin/env python3
"""Regression: adding a PE section must preserve the executable payload."""
from pathlib import Path
import contextlib
import importlib.util
import io
import struct
import tempfile
import unittest

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gen_reloc", root / "boot/efi_stub/gen_reloc.py")
reloc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reloc)

class PeRelocationTest(unittest.TestCase):
    def test_header_growth_preserves_sections_and_symbols(self):
        data = bytearray(1536)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 128)
        data[128:132] = b"PE\0\0"
        coff, opt, sections = 132, 152, 392
        struct.pack_into("<HHIIIHH", data, coff, 0x8664, 3, 0, 1536, 0, 240, 0x207)
        struct.pack_into("<H", data, opt, 0x20B)
        struct.pack_into("<II", data, opt + 32, 4096, 512)
        struct.pack_into("<II", data, opt + 56, 16384, 512)
        struct.pack_into("<I", data, opt + 108, 16)
        for index, (name, size, address, raw_size, raw_ptr) in enumerate([
            (b".text", 512, 4096, 512, 512),
            (b".rodata", 512, 8192, 512, 1024),
            (b".bss", 4096, 12288, 0, 0),
        ]):
            offset = sections + index * 40
            data[offset:offset + 8] = name.ljust(8, b"\0")
            struct.pack_into("<IIII", data, offset + 8, size, address, raw_size, raw_ptr)
        payload = bytes(range(256)) * 4
        data[512:1536] = payload
        data.extend(b"SYMBOL-STRING-TABLE")
        (root / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root / "build") as directory:
            path = Path(directory) / "sample.efi"
            path.write_bytes(data)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(reloc.patch_pe(path, reloc.gen_pe_reloc([])))
            patched = path.read_bytes()
            self.assertEqual(struct.unpack_from("<H", patched, coff + 2)[0], 4)
            self.assertGreaterEqual(struct.unpack_from("<I", patched, opt + 60)[0], sections + 160)
            text_pointer = struct.unpack_from("<I", patched, sections + 20)[0]
            self.assertEqual(patched[text_pointer:text_pointer + 1024], payload)
            symbol_pointer = struct.unpack_from("<I", patched, coff + 8)[0]
            self.assertEqual(patched[symbol_pointer:symbol_pointer + 19], b"SYMBOL-STRING-TABLE")
            reloc_pointer = struct.unpack_from("<I", patched, sections + 120 + 20)[0]
            self.assertGreaterEqual(reloc_pointer, symbol_pointer + 19)
            self.assertEqual(reloc_pointer % 512, 0)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertTrue(reloc.patch_pe(path, reloc.gen_pe_reloc([])))
            self.assertEqual(path.read_bytes(), patched)

if __name__ == "__main__":
    unittest.main()
