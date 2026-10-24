"""Format a new 2 MiB prototype volume; never modify an existing user disk."""
import hashlib
import struct
from mkreadfs import create as readfs

HEADER, BANK, BANK_SECTORS = 2048, 2051, 256


def seal(sector):
    sector[480:] = hashlib.sha256(sector[:480]).digest()
    return sector


def create(files):
    image = bytearray(readfs(files, 4096*512))
    for i in range(len(files)):
        off, size = struct.unpack_from('<QQ', image, 512+i*128+64)
        if off+size > HEADER*512:
            raise ValueError('Immutable files overlap transaction arena')
    volume = hashlib.sha256(image[:HEADER*512]).digest()[:16]
    header = bytearray(512)
    struct.pack_into('<8sII16s', header, 0, b'SFTXN1\0\0', 1, 4096, volume)
    image[HEADER*512:(HEADER+1)*512] = seal(header)
    bank = bytearray(BANK_SECTORS*512)
    struct.pack_into('<8sQI', bank, 0, b'SFBANK1\0', 1, 0)
    image[BANK*512:(BANK+BANK_SECTORS)*512] = bank
    record = bytearray(512)
    struct.pack_into('<8sQII16s32s', record, 0, b'SFCMT1\0\0', 1, 0, 0, volume, hashlib.sha256(bank).digest())
    image[(HEADER+1)*512:(HEADER+2)*512] = seal(record)
    return bytes(image)
