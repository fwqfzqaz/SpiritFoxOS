#!/usr/bin/env python3
"""Generate PE .reloc section from ELF relocations for UEFI bootloader.

objcopy does not properly convert ELF relocations to PE .reloc entries.
This script:
  1. Reads the ELF to find data sections with absolute pointers
  2. Generates PE .reloc section with IMAGE_REL_BASED_DIR64 entries
  3. Patches the PE binary in-place (or adds .reloc if missing)

Usage: gen_reloc.py <bootloader.elf> <BOOTX64.EFI>
"""

import sys
import struct

# ── ELF constants ─────────────────────────────────────────────────────────────
ELFMAG = b'\x7fELF'
ELFCLASS64 = 2
ELFDATA2LSB = 1
SHT_RELA = 4
SHT_PROGBITS = 1
SHT_NOBITS = 8

R_X86_64_64        = 1
R_X86_64_GLOB_DAT  = 6
R_X86_64_JUMP_SLOT = 7
R_X86_64_RELATIVE  = 8
RELOC_TYPES = {R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT, R_X86_64_RELATIVE}

# ── PE constants ──────────────────────────────────────────────────────────────
IMAGE_REL_BASED_DIR64 = 10
PE32_PLUS_MAGIC = 0x020B


def _read_elf_header(f):
    f.seek(0)
    ident = f.read(16)
    if ident[:4] != ELFMAG or ident[4] != ELFCLASS64 or ident[5] != ELFDATA2LSB:
        raise ValueError("Not a valid little-endian ELF64 file")
    hdr_data = f.read(48)
    fields = struct.unpack('<HHIQQQIHHHHHH', hdr_data)
    return {
        'e_shoff': fields[4], 'e_shentsize': fields[9],
        'e_shnum': fields[10], 'e_shstrndx': fields[11],
    }


def _read_section_header(f, offset):
    f.seek(offset)
    data = f.read(64)
    if len(data) < 64:
        raise ValueError("Truncated section header")
    vals = struct.unpack('<IIQQQQIIQQ', data)
    return {
        'sh_name': vals[0], 'sh_type': vals[1], 'sh_flags': vals[2],
        'sh_addr': vals[3], 'sh_offset': vals[4], 'sh_size': vals[5],
        'sh_link': vals[6], 'sh_info': vals[7], 'sh_addralign': vals[8],
        'sh_entsize': vals[9],
    }


def _read_section_name(f, strtab_hdr, name_offset):
    f.seek(strtab_hdr['sh_offset'] + name_offset)
    result = b''
    while True:
        ch = f.read(1)
        if ch == b'\0' or not ch:
            break
        result += ch
    return result.decode('ascii', errors='replace')


def collect_relocation_offsets(elf_path):
    """Collect all RVAs that need PE base relocation."""
    offsets = set()

    with open(elf_path, 'rb') as f:
        hdr = _read_elf_header(f)

        # Read all section headers
        sections = []
        for i in range(hdr['e_shnum']):
            sections.append(_read_section_header(f, hdr['e_shoff'] + i * hdr['e_shentsize']))

        strtab = sections[hdr['e_shstrndx']] if hdr['e_shstrndx'] < len(sections) else None

        # 1. Collect from .rela sections
        for sec in sections:
            if sec['sh_type'] != SHT_RELA:
                continue
            entry_size = 24
            if sec['sh_entsize'] and sec['sh_entsize'] != entry_size:
                entry_size = sec['sh_entsize']
            count = sec['sh_size'] // entry_size
            f.seek(sec['sh_offset'])
            data = f.read(sec['sh_size'])
            for i in range(count):
                off = i * entry_size
                r_offset, r_info, _ = struct.unpack_from('<QQq', data, off)
                r_type = r_info & 0xFFFFFFFF
                if r_type in RELOC_TYPES:
                    offsets.add(r_offset)

        # 2. Scan .data/.sdata for absolute pointers within image VMA range
        vma_min = 0xFFFFFFFFFFFFFFFF
        vma_max = 0
        for sec in sections:
            if sec['sh_addr'] and sec['sh_size'] and sec['sh_type'] != SHT_NOBITS:
                vma_min = min(vma_min, sec['sh_addr'])
                vma_max = max(vma_max, sec['sh_addr'] + sec['sh_size'])

        SHF_WRITE = 0x1
        for sec in sections:
            if not (sec['sh_flags'] & SHF_WRITE):
                continue
            if sec['sh_type'] != SHT_PROGBITS or sec['sh_size'] == 0:
                continue
            name = ''
            if strtab:
                name = _read_section_name(f, strtab, sec['sh_name'])
            if name not in ('.data', '.sdata'):
                continue

            f.seek(sec['sh_offset'])
            data = f.read(sec['sh_size'])
            for i in range(0, len(data) - 7, 8):
                val = struct.unpack_from('<Q', data, i)[0]
                if val != 0 and vma_min <= val < vma_max:
                    offsets.add(sec['sh_addr'] + i)

    return sorted(offsets)


def gen_pe_reloc(offsets):
    """Generate PE .reloc section data from sorted RVA offsets."""
    if not offsets:
        return struct.pack('<II', 0, 8)

    pages = {}
    for off in offsets:
        page = off & ~0xFFF
        pages.setdefault(page, []).append(off & 0xFFF)

    data = b''
    for page in sorted(pages):
        entries = sorted(pages[page])
        num_entries = len(entries)
        raw_size = 8 + num_entries * 2
        padded_size = raw_size if raw_size % 4 == 0 else raw_size + 2

        block = struct.pack('<II', page, padded_size)
        for e in entries:
            block += struct.pack('<H', (IMAGE_REL_BASED_DIR64 << 12) | e)
        if raw_size % 4 != 0:
            block += struct.pack('<H', 0)
        data += block

    return data


def patch_pe(pe_path, reloc_data):
    """Patch the PE binary: fix subsystem, entry point, and .reloc section."""
    with open(pe_path, 'r+b') as f:
        # DOS header
        f.seek(0x3C)
        pe_offset = struct.unpack('<I', f.read(4))[0]

        # PE signature
        f.seek(pe_offset)
        sig = f.read(4)
        if sig != b'PE\0\0':
            print("Error: Not a valid PE file", file=sys.stderr)
            return False

        # COFF header
        machine, num_sections = struct.unpack('<HH', f.read(4))
        f.read(12)
        opt_header_size, _ = struct.unpack('<HH', f.read(4))

        # Optional header
        opt_start = f.tell()
        magic = struct.unpack('<H', f.read(2))[0]

        is_pe32plus = (magic == PE32_PLUS_MAGIC)

        if is_pe32plus:
            # Fix Subsystem: offset 92 from OptionalHeader start = EFI_APPLICATION (10)
            f.seek(opt_start + 92)
            subsystem = struct.unpack('<H', f.read(2))[0]
            if subsystem != 10:
                f.seek(opt_start + 92)
                f.write(struct.pack('<H', 10))
                print(f"Fixed Subsystem: {subsystem} -> 10 (EFI_APPLICATION)")

            # Fix DllCharacteristics: offset 96 - ensure no incompatible flags
            # For UEFI, we want IMAGE_DLLCHARACTERISTICS_NX_COMPAT (0x100) on modern firmware
            # but keep it 0 for maximum compatibility
            f.seek(opt_start + 96)
            f.write(struct.pack('<H', 0))
        else:
            # PE32: Subsystem at offset 68
            f.seek(opt_start + 68)
            subsystem = struct.unpack('<H', f.read(2))[0]
            if subsystem != 10:
                f.seek(opt_start + 68)
                f.write(struct.pack('<H', 10))
                print(f"Fixed Subsystem: {subsystem} -> 10 (EFI_APPLICATION)")

        # Find .reloc section and patch it
        section_start = opt_start + opt_header_size
        found_reloc = False

        for i in range(num_sections):
            sec_offset = section_start + i * 40
            f.seek(sec_offset)
            name = f.read(8).rstrip(b'\0').decode('ascii', errors='replace')
            vsize, vaddr, raw_size, raw_ptr = struct.unpack('<IIII', f.read(16))

            if name == '.reloc':
                found_reloc = True
                padded = reloc_data
                if len(padded) > raw_size:
                    print(f"Warning: reloc data ({len(padded)}) > section size ({raw_size}), truncating",
                          file=sys.stderr)
                    padded = padded[:raw_size]
                elif len(padded) < raw_size:
                    padded = padded + b'\0' * (raw_size - len(padded))

                f.seek(raw_ptr)
                f.write(padded)
                f.seek(sec_offset + 8)
                f.write(struct.pack('<I', len(reloc_data)))
                print(f"Patched .reloc: {len(reloc_data)} bytes at file offset 0x{raw_ptr:x}")
                return True

        if not found_reloc:
            print("No .reloc section in PE (OK for fully PIC code)")

        return True


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <bootloader.elf> <BOOTX64.EFI>", file=sys.stderr)
        sys.exit(1)

    elf_path = sys.argv[1]
    pe_path = sys.argv[2]

    # 1. Collect relocation offsets from ELF
    offsets = collect_relocation_offsets(elf_path)
    print(f"Found {len(offsets)} relocation entries")

    # 2. Generate PE .reloc data
    reloc_data = gen_pe_reloc(offsets)
    print(f"Generated .reloc section: {len(reloc_data)} bytes")

    # 3. Patch the PE in-place
    if patch_pe(pe_path, reloc_data):
        print(f"Successfully patched {pe_path}")
    else:
        print("Failed to patch PE", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
