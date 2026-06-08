#!/usr/bin/env python3
"""
Compute PE CheckSum using the standard CheckSumMappedFile algorithm.
"""
import struct
import sys

def pe_checksum(data):
    """Compute PE checksum as defined by Microsoft/UEFI."""
    # The CheckSum field is at offset 64 in the Optional Header
    # We need to find it first
    e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
    coff_off = e_lfanew + 4
    opt_off = coff_off + 20
    magic = struct.unpack_from('<H', data, opt_off)[0]

    if magic == 0x20b:  # PE32+
        checksum_offset = opt_off + 64
    elif magic == 0x10b:  # PE32
        checksum_offset = opt_off + 56
    else:
        return None, -1

    # Standard algorithm: sum all 16-bit words, fold carry, add file length
    checksum = 0
    # Pad to even length
    if len(data) % 2 != 0:
        data = data + b'\x00'

    for i in range(0, len(data), 2):
        word = struct.unpack_from('<H', data, i)[0]
        # Skip the checksum field itself
        if i == checksum_offset:
            continue
        checksum += word
        if checksum > 0xFFFFFFFF:
            checksum = (checksum & 0xFFFFFFFF) + (checksum >> 32)

    checksum = (checksum & 0xFFFF) + (checksum >> 16)
    checksum = (checksum & 0xFFFF) + (checksum >> 16)

    return checksum, checksum_offset


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'build/BOOTX64.EFI'
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    cs, offset = pe_checksum(data)
    print(f"Computed checksum: 0x{cs:08X} at offset 0x{offset:X}")
    print(f"Current value:     0x{struct.unpack_from('<I', data, offset)[0]:08X}")

    if cs != struct.unpack_from('<I', data, offset)[0]:
        # Fix it
        struct.pack_into('<I', data, offset, cs)
        outpath = path + '.fixed'
        with open(outpath, 'wb') as f:
            f.write(data)
        print(f"Fixed! Written to {outpath}")
