"""Create the bounded, read-only SFRFS1 disk format. No host disk access."""
import hashlib
from pathlib import Path
import struct

MAX_FILE = 128*1024
MAX_IMAGE = 8*1024*1024


def valid_path(path):
    return (1 < len(path.encode('ascii')) < 64 and path.startswith('/') and
            not path.endswith('/') and '\\' not in path and
            all(33 <= ord(c) <= 126 for c in path) and
            all(part not in ('', '.', '..') for part in path[1:].split('/')))


def create(files, image_bytes=1024*1024):
    if not 1024 <= image_bytes <= MAX_IMAGE or image_bytes % 512 or not 1 <= len(files) <= 16:
        raise ValueError('Invalid image size or file count')
    entries = sorted(files.items())
    result = bytearray(image_bytes)
    table = bytearray(128*len(entries))
    position = (512+len(table)+511)//512*512
    for i, (path, data) in enumerate(entries):
        if not valid_path(path) or not 0 < len(data) <= MAX_FILE or position+len(data) > image_bytes:
            raise ValueError('Invalid path or file size: '+path)
        if any(path.startswith(previous+'/') for previous, _ in entries[:i]):
            raise ValueError('File used as directory')
        struct.pack_into('<64sQQ32sI12s', table, i*128, path.encode('ascii'), position, len(data), hashlib.sha256(data).digest(), 1, bytes(12))
        result[position:position+len(data)] = data
        position = (position+len(data)+511)//512*512
    struct.pack_into('<8sIIQIIQQ32s', result, 0, b'SFRFS1\0\0', 1, 512, image_bytes, len(entries), 128, 512, len(table), hashlib.sha256(table).digest())
    result[512:512+len(table)] = table
    return bytes(result)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('files', nargs='+', help='/disk/path=local/file')
    args = parser.parse_args()
    files = {}
    for item in args.files:
        name, source = item.split('=', 1)
        if name in files:
            raise ValueError('Duplicate path')
        files[name] = Path(source).read_bytes()
    args.out.write_bytes(create(files))
