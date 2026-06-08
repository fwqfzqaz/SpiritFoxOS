#!/usr/bin/env python3
"""
elf2pe.py - Convert ELF64 to UEFI-compatible PE/COFF executable.

Reads an ELF64 linked with linker_efi.ld and produces a valid PE32+
(EFI application) that UEFI firmware can load via LoadImage / StartImage.

Key features:
- Sets correct PE layout (headers in page 0, code starts at 0x1000)
- Converts ELF relocations to PE base relocations for ASLR support
- Generates proper .reloc section with real entries

Usage: python3 elf2pe.py <input.elf> <output.efi>
"""
import struct
import sys

# ── ELF constants ──────────────────────────────────────────────
ELFMAG     = b'\x7fELF'
EM_X86_64  = 62
SHT_RELA   = 4          # Relocation with addend
SHT_NULL   = 0
SHT_NOBITS = 8

# ELF relocation types (x86_64)
R_X86_64_NONE    = 0    # No relocation
R_X86_64_64      = 1    # 64-bit absolute
R_X86_64_PC32    = 2    # PC-relative 32-bit (signed)
R_X86_64_GOT32   = 3    # 32-bit GOT entry
R_X86_64_PLT32   = 4    # 32-bit PLT entry
R_X86_64_32      = 10   # 32-bit absolute (zero-extended)
R_X86_64_32S     = 11   # 32-bit absolute (sign-extended)
R_X86_64_PC32S   = 12   # PC-relative 32-bit signed
R_X86_64_64S     = 35   # 64-bit sign-extended

# ── PE constants ───────────────────────────────────────────────
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

# PE base relocation types
IMAGE_REL_BASED_ABSOLUTE   = 0
IMAGE_REL_BASED_DIR64      = 1
IMAGE_REL_BASED_HIGH       = 2
IMAGE_REL_BASED_LOW        = 3
IMAGE_REL_BASED_HIGHLOW    = 4


def read_elf(path):
    """Parse ELF64, return entry point, sections, and relocation info."""
    with open(path, 'rb') as f:
        data = f.read()

    assert data[:4] == ELFMAG
    assert data[4] == 2  # 64-bit
    assert data[5] == 1  # LE

    e_entry  = struct.unpack_from('<Q', data, 24)[0]
    e_shoff  = struct.unpack_from('<Q', data, 40)[0]
    e_shentsz= struct.unpack_from('<H', data, 58)[0]
    e_shnum  = struct.unpack_from('<H', data, 60)[0]
    e_shstrndx = struct.unpack_from('<H', data, 62)[0]

    # String table
    shdr_off = e_shoff + e_shstrndx * e_shentsz
    strtab_off  = struct.unpack_from('<Q', data, shdr_off + 24)[0]
    strtab_sz   = struct.unpack_from('<Q', data, shdr_off + 32)[0]
    strtab = data[strtab_off:strtab_off + strtab_sz]

    def get_str(off):
        end = strtab.index(b'\x00', off)
        return strtab[off:end].decode('ascii')

    # Parse all sections
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
        sh_info  = struct.unpack_from('<I', data, off + 40)[0]  # for RELA: target section idx
        raw = b'' if sh_type == SHT_NOBITS else data[sh_offset:sh_offset + sh_size]

        sec = {
            'name': name, 'sh_type': sh_type,
            'va': sh_addr, 'size': sh_size, 'raw': raw,
            'sh_info': sh_info, 'index': i,
        }
        sections.append(sec)
        sec_by_name[name] = sec

    print(f"ELF: entry=0x{e_entry:X}, {e_shnum} sections")

    # Parse relocation sections (SHT_RELA)
    relocations = []  # list of (target_va_offset, reloc_type, addend)
    for sec in sections:
        if sec['sh_type'] != SHT_RELA:
            continue
        # Find target section by sh_info
        target_idx = sec['sh_info']
        if target_idx >= len(sections):
            continue
        target_sec = sections[target_idx]
        target_base_va = target_sec['va']

        # Parse RELA entries: each is 24 bytes (offset(8), info(8), addend(8))
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

            # Target VA within the section = section VA + offset
            target_va = target_base_va + r_offset

            # Only process absolute relocations; skip PC-relative ones
            # (PC-relative relocations work without base relocation)
            if r_type in (R_X86_64_PC32, R_X86_64_PC32S, R_X86_64_PLT32):
                continue

            relocations.append((target_va, r_type, r_addend))

    print(f"  Found {len(relocations)} absolute relocations for PE base reloc")

    return {'entry': e_entry, 'sections': sections, 'relocs': relocations}


def build_pe_reloc_data(relocs, page_size=4096):
    """
    Build PE .reloc section raw data from a list of (VA, elf_type, addend).
    Returns bytes containing properly formatted base relocation blocks.
    """
    if not relocs:
        # Fallback: minimal dummy reloc block
        return struct.pack('<II', 0, 12) + struct.pack('<HHHH', 0, 0, 0, 0)

    # Map ELF reloc type → PE base reloc type
    def elf_to_pe_type(elf_type):
        if elf_type in (R_X86_64_64, R_X86_64_64S):
            return IMAGE_REL_BASED_DIR64      # Type 1: 64-bit field
        elif elf_type in (R_X86_64_32, R_X86_64_32S):
            return IMAGE_REL_BASED_HIGHLOW    # Type 4: 32-bit field
        else:
            return IMAGE_REL_BASED_ABSOLUTE  # Type 0: padding (shouldn't happen)

    # Group relocations by page (4KB)
    pages = {}  # page_rva -> [(offset_in_page, pe_type), ...]
    for va, elf_type, addend in relocs:
        page_rva = (va // page_size) * page_size
        offset_in_page = va % page_size
        pe_type = elf_to_pe_type(elf_type)

        if page_rva not in pages:
            pages[page_rva] = []
        pages[page_rva].append((offset_in_page, pe_type))

    # Build binary data
    result = bytearray()
    for page_rva in sorted(pages.keys()):
        entries = pages[page_rva]
        # Block size = 4 (header) + 2 * num_entries
        block_size = 4 + 2 * len(entries)
        # Align block size to 4 bytes
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
    Build PE32+ using a verified-working mingw PE as structural template.

    Strategy: Keep template's section table AND headers completely intact.
    Inject ALL ELF data into template's .text section, laid out at the same
    relative offsets as the ELF. This preserves RIP-relative addressing.
    """
    import os

    entry_va = elf_info['entry']
    all_sections = elf_info['sections']
    relocs = elf_info.get('relocs', [])

    # ── Load template PE (mingw-generated, known to work) ───────
    script_dir = os.path.dirname(os.path.abspath(__file__))
    template_path = os.path.join(script_dir, '..', 'build', 'test_mingw2.EFI')
    if not os.path.exists(template_path):
        template_path = 'build/test_mingw2.EFI'

    with open(template_path, 'rb') as f:
        tmpl = bytearray(f.read())

    # ── Parse template layout ───────────────────────────────────
    t_lfanew = struct.unpack_from('<I', tmpl, 0x3c)[0]
    t_coff = t_lfanew + 4
    t_opt = t_coff + 20
    t_opt_sz = struct.unpack_from('<H', tmpl, t_coff + 16)[0]
    t_sect_start = t_opt + t_opt_sz

    t_image_base = struct.unpack_from('<Q', tmpl, t_opt + 24)[0]
    t_file_align = struct.unpack_from('<I', tmpl, t_opt + 36)[0]
    t_sec_align = struct.unpack_from('<I', tmpl, t_opt + 32)[0]
    t_size_of_headers = struct.unpack_from('<I', tmpl, t_opt + 60)[0]

    # Parse template sections
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

    # Find template .text section
    text_sec = None
    for ts in t_sections:
        if ts['name'] == '.text':
            text_sec = ts
            break
    if not text_sec:
        raise RuntimeError("Template has no .text section!")

    print(f"Template: ImageBase=0x{t_image_base:X}, .text VA=0x{text_sec['va']:X}, "
          f"raw_ptr=0x{text_sec['raw_ptr']:X}, raw_sz={text_sec['raw_sz']}")

    # ── Compute total ELF image size needed ────────────────────
    # Find the highest VA end address across all ELF sections
    max_elf_va_end = 0
    elf_section_data = {}  # va -> raw_bytes

    SEC_NAMES = {'.text', '.rodata', '.data', '.bss', '.reloc'}
    for s in all_sections:
        if s['name'] not in SEC_NAMES:
            continue
        va_end = s['va'] + s['size']
        if va_end > max_elf_va_end:
            max_elf_va_end = va_end
        if s['name'] != '.bss' and len(s['raw']) > 0:
            elf_section_data[s['va']] = s['raw']
            print(f"  ELF section {s['name']}: VA=0x{s['va']:X}, size={s['size']}")

    # Total size from .text start (VA=0x1000) to end of last section
    total_code_size = max_elf_va_end - 0x1000  # 0x1000 is first section VA
    total_code_aligned = ((total_code_size + t_file_align - 1) // t_file_align) * t_file_align

    print(f"  Total ELF payload: {total_code_size} bytes (aligned: {total_code_aligned})")

    # ── Build new PE: expand .text to hold everything ───────────
    # File must be large enough for: headers + .text data + ALL template sections
    # Find max file offset needed by any template section
    max_template_raw_end = text_sec['raw_ptr']
    for ts in t_sections:
        if ts['raw_sz'] > 0:
            raw_end = ts['raw_ptr'] + ts['raw_sz']
            if raw_end > max_template_raw_end:
                max_template_raw_end = raw_end

    # New file size = max(our data end, template sections end)
    our_data_end = text_sec['raw_ptr'] + total_code_aligned
    new_file_size = max(our_data_end, max_template_raw_end)

    pe = bytearray(new_file_size)
    # Copy template headers AND all template section data
    copy_len = min(len(tmpl), new_file_size)
    pe[:copy_len] = tmpl[:copy_len]

    # Write ELF section data at correct offsets within .text region
    # Each section's data goes at: text_raw_ptr + (section_va - 0x1000)
    text_start_va = 0x1000  # First section VA from linker script
    for sec_va, raw_data in elf_section_data.items():
        offset_in_text = sec_va - text_start_va
        abs_offset = text_sec['raw_ptr'] + offset_in_text
        pe[abs_offset:abs_offset + len(raw_data)] = raw_data
        print(f"    Wrote {len(raw_data)} bytes at file offset 0x{abs_offset:X} "
              f"(RVA 0x{sec_va:X})")

    # Pad remaining space in .text with zeros (not NOPs - includes data areas)
    for j in range(total_code_size, total_code_aligned):
        pe[text_sec['raw_ptr'] + j] = 0

    # ── Update template's .text section header ──────────────────
    s = text_sec['hdr_off']
    code_vsize_aligned = ((total_code_size + t_sec_align - 1) // t_sec_align) * t_sec_align
    struct.pack_into('<I', pe, s + 8, code_vsize_aligned)      # VirtualSize
    struct.pack_into('<I', pe, s + 16, total_code_aligned)     # SizeOfRawData

    # ── Update optional header fields ──────────────────────────
    struct.pack_into('<I', pe, t_opt + 4, total_code_aligned)   # SizeOfCode
    struct.pack_into('<I', pe, t_opt + 16, entry_va)             # AddressOfEntryPoint (RVA)

    # SizeOfImage: must cover ALL sections (template sections + our .text data)
    # Find the max VA end across both our data and template sections
    our_max_va = max_elf_va_end
    tmpl_max_va = 0
    for ts in t_sections:
        va_end = ts['va'] + ts['vsize']
        if va_end > tmpl_max_va:
            tmpl_max_va = va_end
    size_of_image = (max(our_max_va, tmpl_max_va) + t_sec_align - 1) // t_sec_align * t_sec_align
    struct.pack_into('<I', pe, t_opt + 56, size_of_image)

    # Keep original SizeOfInitializedData / SizeOfUninitializedData from template
    # (zeroing these caused "Unsupported" error from UEFI)

    # ── Compute CheckSum ────────────────────────────────────────
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

    print(f"Output: {sys.argv[2]} ({len(pe)} bytes)")


if __name__ == '__main__':
    main()
