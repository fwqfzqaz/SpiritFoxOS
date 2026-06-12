#!/usr/bin/env python3
"""
混合方案：使用mingw PE头结构，注入我们自己的.text代码。
这将隔离问题是在头中还是在节数据中。
"""
import struct
import sys

def main():
    template_path = 'build/test_mingw2.EFI'   # 正常工作的mingw PE
    elf_pe_path = 'build/test_minimal.EFI'     # 我们有问题的PE
    output_path = 'build/test_hybrid.EFI'

    with open(template_path, 'rb') as f:
        tmpl = bytearray(f.read())
    with open(elf_pe_path, 'rb') as f:
        ours = f.read()

    # 解析我们的PE以查找.text节原始数据
    e_lfanew = struct.unpack_from('<I', ours, 0x3c)[0]
    coff_off = e_lfanew + 4
    opt_off = coff_off + 20
    opt_hdr_size = struct.unpack_from('<H', ours, coff_off + 16)[0]
    nsec_ours = struct.unpack_from('<H', ours, coff_off + 2)[0]
    sect_off_ours = opt_off + opt_hdr_size

    print(f"Our PE: {nsec_ours} sections, opt_hdr at 0x{opt_off:x}")

    # 查找我们的.text节原始数据
    our_text_data = None
    for i in range(nsec_ours):
        s = sect_off_ours + i * 40
        name = ours[s:s+8].rstrip(b'\x00').decode('ascii')
        va = struct.unpack_from('<I', ours, s + 8)[0]
        raw_ptr = struct.unpack_from('<I', ours, s + 20)[0]
        raw_sz = struct.unpack_from('<I', ours, s + 16)[0]
        print(f"  我们的节 [{i}] {name}: VA=0x{va:x} RawPtr=0x{raw_ptr:x} Size=0x{raw_sz:x}")
        if name == '.text' and raw_sz > 0:
            our_text_data = ours[raw_ptr:raw_ptr + raw_sz]
            print(f"    -> 提取了 {len(our_text_data)} 字节代码")

    if not our_text_data:
        print("错误: 未找到.text数据！")
        return

    # 解析mingw模板以查找其.text节
    t_lfanew = struct.unpack_from('<I', tmpl, 0x3c)[0]
    t_coff = t_lfanew + 4
    t_opt = t_coff + 20
    t_opt_sz = struct.unpack_from('<H', tmpl, t_coff + 16)[0]
    t_nsec = struct.unpack_from('<H', tmpl, t_coff + 2)[0]
    t_sect = t_opt + t_opt_sz

    print(f"\nTemplate: {t_nsec} sections")
    for i in range(t_nsec):
        s = t_sect + i * 40
        name = tmpl[s:s+8].rstrip(b'\x00').decode('ascii')
        va = struct.unpack_from('<I', tmpl, s + 8)[0]
        vsize = struct.unpack_from('<I', tmpl, s + 8)[0]
        raw_ptr = struct.unpack_from('<I', tmpl, s + 20)[0]
        raw_sz = struct.unpack_from('<I', tmpl, s + 16)[0]
        print(f"  Template [{i}] {name}: VA=0x{va:x} VSize=0x{vsize:x} RawPtr=0x{raw_ptr:x} Size=0x{raw_sz:x}")

        if name == '.text':
            # 替换.text内容（填充或截断以匹配）
            replace_sz = min(len(our_text_data), raw_sz)
            tmpl[raw_ptr:raw_ptr + replace_sz] = our_text_data[:replace_sz]
            if len(our_text_data) < raw_sz:
                # 用NOP（0x90）填充
                for j in range(replace_sz, raw_sz):
                    tmpl[raw_ptr + j] = 0x90
            print(f"    -> 在偏移 0x{raw_ptr:x} 处替换了 {replace_sz} 字节")

    # 写入混合PE
    with open(output_path, 'wb') as f:
        f.write(tmpl)
    print(f"\n已写入: {output_path} ({len(tmpl)} 字节)")

if __name__ == '__main__':
    main()
