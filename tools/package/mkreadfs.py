"""Create the bounded, read-only SFRFS1/SFRFS2 disk images. No host disk access.

SFRFS1 is the original fixed profile (<=16 files, <=128 KiB each).
SFRFS2 is the generic nested-tree profile (<=48 files, <=1 MiB each, empty files).
Both share the same 512-byte header, 128-byte entry layout and table hash.
"""
import hashlib
from pathlib import Path
import struct

MAX_FILE = 128*1024
MAX_IMAGE = 8*1024*1024
V2_MAX_FILE = 1024*1024
V2_MAX_IMAGE = 32*1024*1024
V2_MAX_ENTRIES = 48


def valid_path(path):
    return (1 < len(path.encode('ascii')) < 64 and path.startswith('/') and
            not path.endswith('/') and '\\' not in path and
            all(33 <= ord(c) <= 126 for c in path) and
            all(part not in ('', '.', '..') for part in path[1:].split('/')))


def create(files, image_bytes=1024*1024, format=1):
    if format == 1:
        max_entries, max_file, max_image, magic, allow_empty = 16, MAX_FILE, MAX_IMAGE, b'SFRFS1\0\0', False
    elif format == 2:
        max_entries, max_file, max_image, magic, allow_empty = V2_MAX_ENTRIES, V2_MAX_FILE, V2_MAX_IMAGE, b'SFRFS2\0\0', True
    else:
        raise ValueError('Unsupported disk format')
    if not 1024 <= image_bytes <= max_image or image_bytes % 512 or not 0 <= len(files) <= max_entries:
        raise ValueError('Invalid image size or file count')
    if not allow_empty and not files:
        raise ValueError('SFRFS1 requires at least one file')
    entries = sorted(files.items())
    result = bytearray(image_bytes)
    table = bytearray(128*len(entries))
    position = (512+len(table)+511)//512*512
    for i, (path, data) in enumerate(entries):
        minimum = 0 if allow_empty else 1
        if not valid_path(path) or not minimum <= len(data) <= max_file or position+len(data) > image_bytes:
            raise ValueError('Invalid path or file size: '+path)
        if any(path.startswith(previous+'/') for previous, _ in entries[:i]):
            raise ValueError('File used as directory')
        struct.pack_into('<64sQQ32sI12s', table, i*128, path.encode('ascii'), position, len(data), hashlib.sha256(data).digest(), 1, bytes(12))
        result[position:position+len(data)] = data
        position = (position+len(data)+511)//512*512
    struct.pack_into('<8sIIQIIQQ32s', result, 0, magic, 1, 512, image_bytes, len(entries), 128, 512, len(table), hashlib.sha256(table).digest())
    result[512:512+len(table)] = table
    return bytes(result)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--format', type=int, choices=(1, 2), default=1)
    parser.add_argument('files', nargs='+', help='/disk/path=local/file')
    args = parser.parse_args()
    files = {}
    for item in args.files:
        name, source = item.split('=', 1)
        if name in files:
            raise ValueError('Duplicate path')
        files[name] = Path(source).read_bytes()
    args.out.write_bytes(create(files, format=args.format))
