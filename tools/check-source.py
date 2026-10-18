#!/usr/bin/env python3
"""Check source portability. May also run in a git-archive extraction."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
if (root / ".git").exists():
    names = subprocess.check_output(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=root).decode().split("\0")
    paths = [root / name for name in names if name]
else:
    paths = [p for p in root.rglob("*") if p.is_file() and "build" not in p.relative_to(root).parts and ".git" not in p.relative_to(root).parts]
errors = []
seen = {}
text_suffixes = {".txt", ".c", ".h", ".S", ".s", ".ld", ".sh", ".py", ".md", ".yml", ".yaml", ".xml", ".lock", ".ps1"}
for path in paths:
    if not path.is_file():
        continue
    relative = path.relative_to(root).as_posix()
    previous = seen.setdefault(relative.casefold(), relative)
    if previous != relative:
        errors.append(f"case collision: {previous} / {relative}")
    data = path.read_bytes()
    if path.suffix in text_suffixes or path.name in {"Makefile", ".gitattributes", ".editorconfig"} or data.startswith(b"#!"):
        if b"\r\n" in data:
            errors.append(f"CRLF line endings: {relative}")
        if data.startswith(b"\xef\xbb\xbf"):
            errors.append(f"UTF-8 BOM: {relative}")
        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            errors.append(f"not UTF-8: {relative}")
        # Build/test entry points must not depend on one developer's home/mount.
        if relative.startswith("tools/") or path.name in {"run_tests.py", "run_tests.sh", "demo.py"}:
            if re.search(rb"[\"'](?:/media/|/home/)[A-Za-z0-9_.-]+/|[\"'][A-Za-z]:[\\/]", data):
                errors.append(f"machine-specific absolute path: {relative}")
if errors:
    print("\n".join(errors), file=sys.stderr)
    sys.exit(1)
print(f"Source portability checks passed ({len(paths)} files)")
