#!/usr/bin/env python3
"""
使用标准CheckSumMappedFile算法计算PE校验和。
"""
import struct
import sys

def pe_checksum(data):
    """按照Microsoft/UEFI定义计算PE校验和。"""
    # CheckSum字段位于可选头偏移64处
    # 需要先找到它
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

    # 标准算法：对所有16位字求和，折叠进位，加上文件长度
    checksum = 0
    # 填充到偶数长度
    if len(data) % 2 != 0:
        data = data + b'\x00'

    for i in range(0, len(data), 2):
        word = struct.unpack_from('<H', data, i)[0]
        # 跳过校验和字段本身
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
        # 修复它
        struct.pack_into('<I', data, offset, cs)
        outpath = path + '.fixed'
        with open(outpath, 'wb') as f:
            f.write(data)
        print(f"已修复！已写入 {outpath}")
