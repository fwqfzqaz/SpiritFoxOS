#!/usr/bin/env python3
"""Build the deterministic, bounded SFPK initrd directory and ELF payloads."""
from pathlib import Path
import argparse
import re
import struct

parser = argparse.ArgumentParser()
parser.add_argument("--manifest", type=Path, default=Path("modules/manifest.txt"))
parser.add_argument("--build", type=Path, default=Path("build/modules"))
parser.add_argument("--output", type=Path, default=Path("build/modules/init.sfpack"))
args = parser.parse_args()
names = []
for line in args.manifest.read_text(encoding="utf-8").splitlines():
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    name, source = line.split()
    if not re.fullmatch(r"[a-z0-9_.-]{1,31}", name) or name in names:
        raise SystemExit(f"Invalid/duplicate module name: {name}")
    names.append(name)
if not 1 <= len(names) <= 16:
    raise SystemExit("A package requires 1..16 modules")
offset = 16 + len(names) * 48
directory, payload = bytearray(), bytearray()
for name in names:
    data = (args.build / (name + ".sfmod")).read_bytes()
    padding = (-offset) % 8
    payload.extend(bytes(padding))
    offset += padding
    directory.extend(struct.pack("<32sQQ", name.encode(), offset, len(data)))
    payload.extend(data)
    offset += len(data)
if offset > 16 * 1024 * 1024:
    raise SystemExit("Module package exceeds 16 MiB")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(struct.pack("<IIII", 0x4B504653, 0x10002, len(names), offset)
                        + directory + payload)
print(f"[SFMOD] {args.output}: {len(names)} modules, {offset} bytes")
