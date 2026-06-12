#!/usr/bin/env python3
"""
fix_pe.py - 后处理PE/COFF文件以实现UEFI兼容性。

修复objcopy生成的PE中导致UEFI的LoadImage服务
拒绝加载并返回"Unsupported"或"not an image"错误的常见问题。

用法: python3 fix_pe.py <pe_file.efi>
"""
import struct
import sys
import binascii


def fix_pe(filepath):
    with open(filepath, 'rb') as f:
        data = bytearray(f.read())

    if len(data) < 512:
        print(f"错误: 文件过小（{len(data)} 字节）")
        return False

    # 验证DOS头
    if data[0:2] != b'MZ':
        print("错误: 不是有效的DOS/PE文件（缺少MZ签名）")
        return False

    # 从DOS头获取PE偏移
    pe_offset = struct.unpack_from('<I', data, 0x3C)[0]
    if pe_offset >= len(data) or data[pe_offset:pe_offset+4] != b'PE\x00\x00':
        print(f"错误: 偏移 0x{pe_offset:X} 处的PE签名无效")
        return False

    coff_offset = pe_offset + 4  # 在"PE\0\0"之后

    # 解析COFF头
    machine = struct.unpack_from('<H', data, coff_offset)[0]
    num_sections = struct.unpack_from('<H', data, coff_offset + 2)[0]
    opt_hdr_size = struct.unpack_from('<H', data, coff_offset + 16)[0]
    characteristics = struct.unpack_from('<H', data, coff_offset + 18)[0]

    if machine != 0x8664:
        print(f"警告: 机器类型 0x{machine:04X}（期望AMD64为0x8664）")

    # 解析可选头（PE32+）
    opt_offset = coff_offset + 20
    magic = struct.unpack_from('<H', data, opt_offset)[0]

    if magic != 0x20B:
        print(f"错误: 期望PE32+魔数（0x20B），得到 0x{magic:04X}")
        return False

    # PE32+中从可选头开始算起的特定偏移
    # 子系统在PE32+中的偏移68处
    subsystem_off = opt_offset + 68
    subsystem = struct.unpack_from('<H', data, subsystem_off)[0]

    # DllCharacteristics在PE32+中的偏移70处
    dll_chars_off = opt_offset + 70
    dll_chars = struct.unpack_from('<H', data, dll_chars_off)[0]

    print(f"PE: {num_sections} sections, subsystem={subsystem}, "
          f"dll_chars=0x{dll_chars:04X}, chars=0x{characteristics:04X}")

    # 修复1：为UEFI兼容性设置DllCharacteristics
    # NX_COMPAT（0x0100）是某些UEFI实现所必需的
    new_dll_chars = dll_chars | 0x0160  # HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT
    struct.pack_into('<H', data, dll_chars_off, new_dll_chars)
    print(f"  Fixed DllCharacteristics: 0x{dll_chars:04X} -> 0x{new_dll_chars:04X}")

    # 修复2：确保COFF特征包含EXECUTABLE_IMAGE
    new_chars = characteristics | 0x0002  # EXECUTABLE_IMAGE
    struct.pack_into('<H', data, coff_offset + 18, new_chars)
    if new_chars != characteristics:
        print(f"  Fixed Characteristics: 0x{characteristics:04X} -> 0x{new_chars:04X}")

    # 解析节头以查找.reloc
    sec_table_offset = opt_offset + opt_hdr_size
    reloc_sec_idx = -1
    sec_info = []

    for i in range(num_sections):
        sec_off = sec_table_offset + i * 40
        name_bytes = data[sec_off:sec_off + 8]
        name = name_bytes.rstrip(b'\x00').decode('ascii', errors='replace')
        vsize = struct.unpack_from('<I', data, sec_off + 8)[0]
        va = struct.unpack_from('<I', data, sec_off + 12)[0]
        raw_size = struct.unpack_from('<I', data, sec_off + 16)[0]
        raw_ptr = struct.unpack_from('<I', data, sec_off + 20)[0]
        sec_chars = struct.unpack_from('<I', data, sec_off + 36)[0]

        sec_info.append({
            'name': name,
            'va': va,
            'vsize': vsize,
            'raw_size': raw_size,
            'raw_ptr': raw_ptr,
            'chars': sec_chars,
            'offset': sec_off
        })

        if name == '.reloc':
            reloc_sec_idx = i
            print(f"  Found .reloc: VA=0x{va:X}, RawSize={raw_size}, Chars=0x{sec_chars:08X}")

    # 修复3：确保.reloc具有正确的节特征
    if reloc_sec_idx >= 0:
        sec = sec_info[reloc_sec_idx]
        # 必需: INITIALIZED_DATA | MEM_DISCARDABLE | MEM_READ
        required_chars = 0x42000040
        if sec['chars'] != required_chars:
            print(f"  Fixing .reloc chars: 0x{sec['chars']:08X} -> 0x{required_chars:08X}")
            struct.pack_into('<I', data, sec['offset'] + 36, required_chars)

        # 确保.reloc具有有效内容（至少一个基址重定位块）
        if sec['raw_size'] > 0 and sec['raw_ptr'] > 0 and sec['raw_ptr'] < len(data):
            # 检查它是否看起来像一个有效的重定位目录
            page_rva = struct.unpack_from('<I', data, sec['raw_ptr'])[0]
            block_size = struct.unpack_from('<I', data, sec['raw_ptr'] + 4)[0]
            print(f"  .reloc 内容: PageRVA=0x{page_rva:X}, BlockSize={block_size}")

            if block_size == 0 or block_size > sec['raw_size']:
                # 修复：创建最小有效重定位块
                print(f"  修复.reloc内容（无效的块大小）")
                struct.pack_into('<I', data, sec['raw_ptr'], 0)     # 页面RVA = 0
                struct.pack_into('<I', data, sec['raw_ptr'] + 4, 12)  # 块大小 = 12
                # 4个IMAGE_REL_BASED_ABSOLUTE（类型0）条目 = 8字节
                struct.pack_into('<H', data, sec['raw_ptr'] + 8, 0)
                struct.pack_into('<H', data, sec['raw_ptr'] + 10, 0)
                struct.pack_into('<H', data, sec['raw_ptr'] + 12, 0)
                struct.pack_into('<H', data, sec['raw_ptr'] + 14, 0)

    # 修复4：重新计算SizeOfImage（必须对齐到SectionAlignment）
    # 找出所有节中最高的VA + VirtualSize
    section_alignment = struct.unpack_from('<I', data, opt_offset + 44)[0]
    if section_alignment == 0:
        section_alignment = 4096  # 默认值
    max_end = 0
    for sec in sec_info:
        end = sec['va'] + max(sec['vsize'], sec['raw_size'])
        if end > max_end:
            max_end = end

    # 向上对齐到节对齐值
    size_of_image = ((max_end + section_alignment - 1) // section_alignment) * section_alignment
    size_of_image_off = opt_offset + 56  # PE32+中的SizeOfImage
    current_soi = struct.unpack_from('<I', data, size_of_image_off)[0]
    if size_of_image != current_soi:
        print(f"  Fixing SizeOfImage: 0x{current_soi:X} -> 0x{size_of_image:X}")
        struct.pack_into('<I', data, size_of_image_off, size_of_image)

    # 修复5：重新计算SizeOfHeaders
    headers_end = sec_table_offset + num_sections * 40
    file_alignment = struct.unpack_from('<I', data, opt_offset + 48)[0]
    if file_alignment == 0:
        file_alignment = 512  # 默认值
    size_of_headers = ((headers_end + file_alignment - 1) // file_alignment) * file_alignment
    soh_off = opt_offset + 60  # PE32+中的SizeOfHeaders
    current_soh = struct.unpack_from('<I', data, soh_off)[0]
    if size_of_headers != current_soh:
        print(f"  Fixing SizeOfHeaders: 0x{current_soh:X} -> 0x{size_of_headers:X}")
        struct.pack_into('<I', data, soh_off, size_of_headers)

    # 写入修复后的文件
    with open(filepath, 'wb') as f:
        f.write(data)

    print(f"已修复: {filepath}")
    return True


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <pe_file.efi>")
        sys.exit(1)

    sys.exit(0 if fix_pe(sys.argv[1]) else 1)
