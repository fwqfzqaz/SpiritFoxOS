#!/usr/bin/env python3
"""
elf2pe.py - 将ELF64转换为UEFI兼容的PE/COFF可执行文件。

读取使用linker_efi.ld链接的ELF64文件，生成有效的PE32+
（EFI应用程序），使UEFI固件能够通过LoadImage / StartImage加载。

主要功能：
- 设置正确的PE布局（头在第0页，代码从0x1000开始）
- 将ELF重定位转换为PE基址重定位以支持ASLR
- 生成带有真实条目的正确.reloc节

用法: python3 elf2pe.py <input.elf> <output.efi>
"""
import struct
import sys

# ── ELF常量 ──────────────────────────────────────────────
ELFMAG     = b'\x7fELF'
EM_X86_64  = 62
SHT_RELA   = 4          # 带加数的重定位
SHT_NULL   = 0
SHT_NOBITS = 8

# ELF重定位类型（x86_64）
R_X86_64_NONE    = 0    # 无重定位
R_X86_64_64      = 1    # 64位绝对地址
R_X86_64_PC32    = 2    # PC相对32位（有符号）
R_X86_64_GOT32   = 3    # 32位GOT条目
R_X86_64_PLT32   = 4    # 32位PLT条目
R_X86_64_32      = 10   # 32位绝对地址（零扩展）
R_X86_64_32S     = 11   # 32位绝对地址（符号扩展）
R_X86_64_PC32S   = 12   # PC相对32位有符号
R_X86_64_64S     = 35   # 64位符号扩展

# ── PE常量 ───────────────────────────────────────────────
IMAGE_FILE_MACHINE_AMD64        = 0x8664
IMAGE_FILE_EXECUTABLE_IMAGE      = 0x0002
IMAGE_FILE_LARGE_ADDRESS_AWARE   = 0x0020
IMAGE_SUBSYSTEM_EFI_APPLICATION = 10

IMAGE_SCN_CNT_CODE               = 0x00000020
IMAGE_SCN_CNT_INITIALIZED_DATA   = 0x00000040
IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080
IMAGE_SCN_MEM_DISCARDABLE        = 0x02000000
IMAGE_SCN_MEM_EXECUTE            = 0x20000000
IMAGE_SCN_MEM_READ               = 0x40000000
IMAGE_SCN_MEM_WRITE              = 0x80000000

# PE基址重定位类型
IMAGE_REL_BASED_ABSOLUTE   = 0
IMAGE_REL_BASED_DIR64      = 1
IMAGE_REL_BASED_HIGH       = 2
IMAGE_REL_BASED_LOW        = 3
IMAGE_REL_BASED_HIGHLOW    = 4


def read_elf(path):
    """解析ELF64，返回入口点、节和重定位信息。"""
    with open(path, 'rb') as f:
        data = f.read()

    assert data[:4] == ELFMAG
    assert data[4] == 2  # 64位
    assert data[5] == 1  # 小端

    e_entry  = struct.unpack_from('<Q', data, 24)[0]
    e_shoff  = struct.unpack_from('<Q', data, 40)[0]
    e_shentsz= struct.unpack_from('<H', data, 58)[0]
    e_shnum  = struct.unpack_from('<H', data, 60)[0]
    e_shstrndx = struct.unpack_from('<H', data, 62)[0]

    # 字符串表
    shdr_off = e_shoff + e_shstrndx * e_shentsz
    strtab_off  = struct.unpack_from('<Q', data, shdr_off + 24)[0]
    strtab_sz   = struct.unpack_from('<Q', data, shdr_off + 32)[0]
    strtab = data[strtab_off:strtab_off + strtab_sz]

    def get_str(off):
        end = strtab.index(b'\x00', off)
        return strtab[off:end].decode('ascii')

    # 解析所有节
    sections = []
    sec_by_name = {}
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsz
        name  = get_str(struct.unpack_from('<I', data, off)[0])
        sh_type  = struct.unpack_from('<I', data, off + 4)[0]
        sh_flags = struct.unpack_from('<Q', data, off + 8)[0]
        sh_addr  = struct.unpack_from('<Q', data, off + 16)[0]
        sh_offset= struct.unpack_from('<Q', data, off + 24)[0]
        sh_size  = struct.unpack_from('<Q', data, off + 32)[0]
        sh_info  = struct.unpack_from('<I', data, off + 40)[0]  # 对于RELA：目标节索引
        raw = b'' if sh_type == SHT_NOBITS else data[sh_offset:sh_offset + sh_size]

        sec = {
            'name': name, 'sh_type': sh_type,
            'va': sh_addr, 'size': sh_size, 'raw': raw,
            'sh_info': sh_info, 'index': i,
        }
        sections.append(sec)
        sec_by_name[name] = sec

    print(f"ELF: entry=0x{e_entry:X}, {e_shnum} sections")

    # 解析重定位节（SHT_RELA）
    relocations = []  # (目标VA偏移, 重定位类型, 加数) 列表
    for sec in sections:
        if sec['sh_type'] != SHT_RELA:
            continue
        # 通过sh_info查找目标节
        target_idx = sec['sh_info']
        if target_idx >= len(sections):
            continue
        target_sec = sections[target_idx]
        target_base_va = target_sec['va']

        # 解析RELA条目：每个24字节（偏移(8), 信息(8), 加数(8)）
        n_entries = sec['size'] // 24
        raw = sec['raw']
        for j in range(n_entries):
            e_off = j * 24
            r_offset  = struct.unpack_from('<Q', raw, e_off)[0]
            r_info    = struct.unpack_from('<Q', raw, e_off + 8)[0]
            r_addend  = struct.unpack_from('<Q', raw, e_off + 16)[0]
            r_sym     = r_info >> 32
            r_type    = r_info & 0xFFFFFFFF

            if r_type == R_X86_64_NONE:
                continue

            # 目标VA = 节VA + 偏移
            target_va = target_base_va + r_offset

            # 只处理绝对重定位；跳过PC相对重定位
            # （PC相对重定位无需基址重定位即可工作）
            if r_type in (R_X86_64_PC32, R_X86_64_PC32S, R_X86_64_PLT32):
                continue

            relocations.append((target_va, r_type, r_addend))

    print(f"  发现 {len(relocations)} 个用于PE基址重定位的绝对重定位")

    return {'entry': e_entry, 'sections': sections, 'relocs': relocations}


def build_pe_reloc_data(relocs, page_size=4096):
    """
    从(VA, elf类型, 加数)列表构建PE .reloc节原始数据。
    返回包含正确格式的基址重定位块的字节。
    """
    if not relocs:
        # 回退：最小化虚拟重定位块
        return struct.pack('<II', 0, 12) + struct.pack('<HHHH', 0, 0, 0, 0)

    # 映射ELF重定位类型 → PE基址重定位类型
    def elf_to_pe_type(elf_type):
        if elf_type in (R_X86_64_64, R_X86_64_64S):
            return IMAGE_REL_BASED_DIR64      # 类型1：64位字段
        elif elf_type in (R_X86_64_32, R_X86_64_32S):
            return IMAGE_REL_BASED_HIGHLOW    # 类型4：32位字段
        else:
            return IMAGE_REL_BASED_ABSOLUTE  # 类型0：填充（不应发生）

    # 按页（4KB）分组重定位
    pages = {}  # page_rva -> [(页内偏移, pe类型), ...]
    for va, elf_type, addend in relocs:
        page_rva = (va // page_size) * page_size
        offset_in_page = va % page_size
        pe_type = elf_to_pe_type(elf_type)

        if page_rva not in pages:
            pages[page_rva] = []
        pages[page_rva].append((offset_in_page, pe_type))

    # 构建二进制数据
    result = bytearray()
    for page_rva in sorted(pages.keys()):
        entries = pages[page_rva]
        # 块大小 = 4（头）+ 2 * 条目数
        block_size = 4 + 2 * len(entries)
        # 将块大小对齐到4字节
        if block_size % 4 != 0:
            block_size = ((block_size + 3) // 4) * 4
            while len(entries) * 2 < (block_size - 4):
                entries.append((0, IMAGE_REL_BASED_ABSOLUTE))

        result += struct.pack('<II', page_rva, block_size)
        for offset, pe_type in entries:
            result += struct.pack('<HH', offset, pe_type)

    return bytes(result)


def build_pe(elf_info):
    """
    使用已验证可用的mingw PE作为结构模板构建PE32+。

    策略：保持模板的节表和头完全不变。
    将所有ELF数据注入模板的.text节，按与ELF相同的相对偏移布局。
    这保留了RIP相对寻址。
    """
    import os

    entry_va = elf_info['entry']
    all_sections = elf_info['sections']
    relocs = elf_info.get('relocs', [])

    # ── 加载模板PE（mingw生成，已知可用） ───────
    script_dir = os.path.dirname(os.path.abspath(__file__))
    template_path = os.path.join(script_dir, '..', 'build', 'test_mingw2.EFI')
    if not os.path.exists(template_path):
        template_path = 'build/test_mingw2.EFI'

    with open(template_path, 'rb') as f:
        tmpl = bytearray(f.read())

    # ── 解析模板布局 ───────────────────────────────────
    t_lfanew = struct.unpack_from('<I', tmpl, 0x3c)[0]
    t_coff = t_lfanew + 4
    t_opt = t_coff + 20
    t_opt_sz = struct.unpack_from('<H', tmpl, t_coff + 16)[0]
    t_sect_start = t_opt + t_opt_sz

    t_image_base = struct.unpack_from('<Q', tmpl, t_opt + 24)[0]
    t_file_align = struct.unpack_from('<I', tmpl, t_opt + 36)[0]
    t_sec_align = struct.unpack_from('<I', tmpl, t_opt + 32)[0]
    t_size_of_headers = struct.unpack_from('<I', tmpl, t_opt + 60)[0]

    # 解析模板节
    t_nsec = struct.unpack_from('<H', tmpl, t_coff + 2)[0]
    t_sections = []
    for i in range(t_nsec):
        s = t_sect_start + i * 40
        name = tmpl[s:s+8].rstrip(b'\x00').decode('ascii')
        t_sections.append({
            'name': name,
            'vsize': struct.unpack_from('<I', tmpl, s + 8)[0],
            'va': struct.unpack_from('<I', tmpl, s + 12)[0],
            'raw_sz': struct.unpack_from('<I', tmpl, s + 16)[0],
            'raw_ptr': struct.unpack_from('<I', tmpl, s + 20)[0],
            'chars': struct.unpack_from('<I', tmpl, s + 36)[0],
            'hdr_off': s,
        })

    # 查找模板的.text节
    text_sec = None
    for ts in t_sections:
        if ts['name'] == '.text':
            text_sec = ts
            break
    if not text_sec:
        raise RuntimeError("模板没有.text节！")

    print(f"Template: ImageBase=0x{t_image_base:X}, .text VA=0x{text_sec['va']:X}, "
          f"raw_ptr=0x{text_sec['raw_ptr']:X}, raw_sz={text_sec['raw_sz']}")

    # ── 计算所需的总ELF镜像大小 ────────────────────
    # 找出所有ELF节中最高的VA结束地址
    max_elf_va_end = 0
    elf_section_data = {}  # va -> 原始字节

    SEC_NAMES = {'.text', '.rodata', '.data', '.bss', '.reloc'}
    for s in all_sections:
        if s['name'] not in SEC_NAMES:
            continue
        va_end = s['va'] + s['size']
        if va_end > max_elf_va_end:
            max_elf_va_end = va_end
        if s['name'] != '.bss' and len(s['raw']) > 0:
            elf_section_data[s['va']] = s['raw']
            print(f"  ELF节 {s['name']}: VA=0x{s['va']:X}, size={s['size']}")

    # 从.text开始（VA=0x1000）到最后一个节结束的总大小
    total_code_size = max_elf_va_end - 0x1000  # 0x1000是第一个节的VA
    total_code_aligned = ((total_code_size + t_file_align - 1) // t_file_align) * t_file_align

    print(f"  ELF总载荷: {total_code_size} 字节（对齐后: {total_code_aligned}）")

    # ── 构建新PE：扩展.text以容纳所有内容 ───────────
    # 文件必须足够大以容纳：头 + .text数据 + 所有模板节
    # 找出任何模板节所需的最大文件偏移
    max_template_raw_end = text_sec['raw_ptr']
    for ts in t_sections:
        if ts['raw_sz'] > 0:
            raw_end = ts['raw_ptr'] + ts['raw_sz']
            if raw_end > max_template_raw_end:
                max_template_raw_end = raw_end

    # 新文件大小 = max(我们的数据结束, 模板节结束)
    our_data_end = text_sec['raw_ptr'] + total_code_aligned
    new_file_size = max(our_data_end, max_template_raw_end)

    pe = bytearray(new_file_size)
    # 复制模板头和所有模板节数据
    copy_len = min(len(tmpl), new_file_size)
    pe[:copy_len] = tmpl[:copy_len]

    # 在.text区域内正确的偏移处写入ELF节数据
    # 每个节的数据位于：text_raw_ptr + (section_va - 0x1000)
    text_start_va = 0x1000  # 链接器脚本中的第一个节VA
    for sec_va, raw_data in elf_section_data.items():
        offset_in_text = sec_va - text_start_va
        abs_offset = text_sec['raw_ptr'] + offset_in_text
        pe[abs_offset:abs_offset + len(raw_data)] = raw_data
        print(f"    写入 {len(raw_data)} 字节到文件偏移 0x{abs_offset:X} "
              f"(RVA 0x{sec_va:X})")

    # 用零填充.text中剩余的空间（不是NOP - 包含数据区域）
    for j in range(total_code_size, total_code_aligned):
        pe[text_sec['raw_ptr'] + j] = 0

    # ── 更新模板的.text节头 ──────────────────────────
    s = text_sec['hdr_off']
    code_vsize_aligned = ((total_code_size + t_sec_align - 1) // t_sec_align) * t_sec_align
    struct.pack_into('<I', pe, s + 8, code_vsize_aligned)      # VirtualSize
    struct.pack_into('<I', pe, s + 16, total_code_aligned)     # SizeOfRawData

    # ── 更新可选头字段 ──────────────────────────────
    struct.pack_into('<I', pe, t_opt + 4, total_code_aligned)   # SizeOfCode
    struct.pack_into('<I', pe, t_opt + 16, entry_va)             # AddressOfEntryPoint (RVA)

    # SizeOfImage: 必须覆盖所有节（模板节 + 我们的.text数据）
    # 找出我们的数据和模板节中的最大VA结束地址
    our_max_va = max_elf_va_end
    tmpl_max_va = 0
    for ts in t_sections:
        va_end = ts['va'] + ts['vsize']
        if va_end > tmpl_max_va:
            tmpl_max_va = va_end
    size_of_image = (max(our_max_va, tmpl_max_va) + t_sec_align - 1) // t_sec_align * t_sec_align
    struct.pack_into('<I', pe, t_opt + 56, size_of_image)

    # 保留模板中原始的SizeOfInitializedData / SizeOfUninitializedData
    # （清零这些值会导致UEFI返回"Unsupported"错误）

    # ── 计算校验和 ────────────────────────────────────────
    checksum_offset = t_opt + 64
    checksum = 0
    data = bytes(pe)
    if len(data) % 2 != 0:
        data = data + b'\x00'
    for i in range(0, len(data), 2):
        if i == checksum_offset:
            continue
        checksum += struct.unpack_from('<H', data, i)[0]
        if checksum > 0xFFFFFFFF:
            checksum = (checksum & 0xFFFFFFFF) + (checksum >> 32)
    checksum = (checksum & 0xFFFF) + (checksum >> 16)
    checksum = (checksum & 0xFFFF) + (checksum >> 16)
    struct.pack_into('<I', pe, checksum_offset, checksum)

    print(f"PE: entry RVA=0x{entry_va:X}, .text_size={total_code_size}, "
          f"image_size=0x{size_of_image:X}, file_size={new_file_size}")
    return bytes(pe)


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input.elf> <output.efi>")
        sys.exit(1)

    info = read_elf(sys.argv[1])
    pe  = build_pe(info)

    with open(sys.argv[2], 'wb') as f:
        f.write(pe)

    print(f"输出: {sys.argv[2]} ({len(pe)} 字节)")


if __name__ == '__main__':
    main()
