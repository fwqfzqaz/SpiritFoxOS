#!/usr/bin/env python3
import struct

with open('build/BOOTX64.EFI', 'rb') as f:
    data = f.read()

# 检查RawPtr位置处的数据
print(f"文件大小: {len(data):#x}")
print(f"\n检查节RawPtr偏移处的原始数据:")

sections_info = [
    ('.text',   0x400,  0x18a27),
    ('.rodata', 0x19000, 0x82c91),
    ('.data',   0x9be00, 0x634),
    ('.reloc',  0x9c600, 0x10),
]

for name, expected_ptr, expected_size in sections_info:
    actual = data[expected_ptr:expected_ptr+min(expected_size, 16)]
    non_zero = any(b != 0 for b in actual)
    print(f"  {name:8s} @ 0x{expected_ptr:06x}: 前16字节 = {actual.hex()} {'(有数据!)' if non_zero else '(全零)'}")

# 检查节头中的大小字段
e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
opt_off = e_lfanew + 4 + 20
opt_hdr_size = struct.unpack_from('<H', data, opt_off + 16)[0]  # 错误！这不是opt_hdr_size
# 让我重新正确计算
coff_off = e_lfanew + 4
num_sections = struct.unpack_from('<H', data, coff_off + 2)[0]
opt_hdr_size = struct.unpack_from('<H', data, coff_off + 16)[0]
sect_off = opt_off + opt_hdr_size

print(f"\n节头位于 0x{sect_off:x}:")
for i in range(num_sections):
    s_off = sect_off + i * 40
    name = data[s_off:s_off+8].rstrip(b'\x00').decode('ascii')
    vsize = struct.unpack_from('<I', data, s_off + 8)[0]
    va = struct.unpack_from('<I', data, s_off + 12)[0]
    raw_size_hdr = struct.unpack_from('<I', data, s_off + 16)[0]  # 这是头中存储的值
    raw_ptr = struct.unpack_from('<I', data, s_off + 20)[0]
    print(f"  [{i}] {name:8s}: SizeOfRawData(头中)=0x{raw_size_hdr:04x}, RawPtr=0x{raw_ptr:06x}")
