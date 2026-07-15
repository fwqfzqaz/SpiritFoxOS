#!/usr/bin/env python3
"""Generate and patch PE .reloc section for UEFI bootloader.

objcopy does not properly set up the PE Data Directory for base relocations.
This script:
  1. Reads the ELF to find data sections with absolute pointers
  2. Generates PE .reloc section with IMAGE_REL_BASED_DIR64 entries
  3. Adds or patches .reloc section in the PE binary
  4. Updates Data Directory entry for Base Relocations
  5. Fixes PE optional header fields (Stack/Heap sizes, etc.)

Usage: gen_reloc.py <bootloader.elf> <BOOTX64.EFI>
"""

import sys
import struct
import os

# ELF constants
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

# PE constants
IMAGE_REL_BASED_DIR64 = 10
PE32_PLUS_MAGIC = 0x020B
EFI_APPLICATION = 10


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

        # 2. Scan .data/.sdata for absolute pointers
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
        # Empty .reloc: just a single block header with 0 pages
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
    """Patch the PE binary: fix header fields, add/patch .reloc, update Data Directory."""
    with open(pe_path, 'r+b') as f:
        pe_data = bytearray(f.read())

    # DOS header -> PE offset
    pe_offset = struct.unpack_from('<I', pe_data, 0x3C)[0]

    # PE signature check
    if pe_data[pe_offset:pe_offset+4] != b'PE\0\0':
        print("Error: Not a valid PE file", file=sys.stderr)
        return False

    # COFF header
    coff_offset = pe_offset + 4
    machine, num_sections = struct.unpack_from('<HH', pe_data, coff_offset)
    opt_header_size = struct.unpack_from('<H', pe_data, coff_offset + 16)[0]

    # Fix COFF Characteristics: clear IMAGE_FILE_RELOCS_STRIPPED (0x0001)
    characteristics = struct.unpack_from('<H', pe_data, coff_offset + 18)[0]
    if characteristics & 0x0001:
        characteristics &= ~0x0001
        struct.pack_into('<H', pe_data, coff_offset + 18, characteristics)
        print(f"Cleared RELOCS_STRIPPED flag in COFF header")

    # Optional header
    opt_offset = coff_offset + 20
    magic = struct.unpack_from('<H', pe_data, opt_offset)[0]
    is_pe32plus = (magic == PE32_PLUS_MAGIC)

    if not is_pe32plus:
        print("Error: Not a PE32+ file", file=sys.stderr)
        return False

    # --- Fix Optional Header fields (PE32+ layout) ---

    # Subsystem at opt_offset + 68
    subsystem = struct.unpack_from('<H', pe_data, opt_offset + 68)[0]
    if subsystem != EFI_APPLICATION:
        struct.pack_into('<H', pe_data, opt_offset + 68, EFI_APPLICATION)
        print(f"Fixed Subsystem: {subsystem} -> {EFI_APPLICATION} (EFI_APPLICATION)")

    # DllCharacteristics at opt_offset + 70
    struct.pack_into('<H', pe_data, opt_offset + 70, 0)

    # SizeOfStackReserve at opt_offset + 72 (8 bytes)
    struct.pack_into('<Q', pe_data, opt_offset + 72, 0x100000)   # 1 MB

    # SizeOfStackCommit at opt_offset + 80 (8 bytes)
    struct.pack_into('<Q', pe_data, opt_offset + 80, 0x10000)    # 64 KB

    # SizeOfHeapReserve at opt_offset + 88 (8 bytes)
    struct.pack_into('<Q', pe_data, opt_offset + 88, 0x100000)   # 1 MB

    # SizeOfHeapCommit at opt_offset + 96 (8 bytes)
    struct.pack_into('<Q', pe_data, opt_offset + 96, 0x1000)     # 4 KB

    # --- Find or add .reloc section ---
    section_start = opt_offset + opt_header_size

    found_reloc = False
    reloc_section_idx = -1

    for i in range(num_sections):
        sec_offset = section_start + i * 40
        name = pe_data[sec_offset:sec_offset+8].rstrip(b'\0').decode('ascii', errors='replace')
        if name == '.reloc':
            found_reloc = True
            reloc_section_idx = i
            break

    if found_reloc:
        # Patch existing .reloc section
        sec_offset = section_start + reloc_section_idx * 40
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from('<IIII', pe_data, sec_offset + 8)

        padded = reloc_data
        if len(padded) > raw_size:
            print(f"Warning: reloc data ({len(padded)}) > section size ({raw_size}), truncating",
                  file=sys.stderr)
            padded = padded[:raw_size]
        elif len(padded) < raw_size:
            padded = padded + b'\0' * (raw_size - len(padded))

        pe_data[raw_ptr:raw_ptr + len(padded)] = padded
        # Update virtual size
        struct.pack_into('<I', pe_data, sec_offset + 8, len(reloc_data))

        reloc_rva = vaddr
        reloc_size = len(reloc_data)
        print(f"Patched .reloc: {len(reloc_data)} bytes at RVA 0x{vaddr:x}")
    else:
        # Need to add .reloc section
        print("No .reloc section found, adding one...")

        # Calculate new section position
        last_sec_offset = section_start + (num_sections - 1) * 40
        last_vsize = struct.unpack_from('<I', pe_data, last_sec_offset + 8)[0]
        last_vaddr = struct.unpack_from('<I', pe_data, last_sec_offset + 12)[0]
        last_raw_size = struct.unpack_from('<I', pe_data, last_sec_offset + 16)[0]
        last_raw_ptr = struct.unpack_from('<I', pe_data, last_sec_offset + 20)[0]

        new_vaddr = (last_vaddr + last_vsize + 0xFFF) & ~0xFFF
        new_raw_ptr = (last_raw_ptr + last_raw_size + 0x1FF) & ~0x1FF
        new_vsize = len(reloc_data)
        new_raw_size = (len(reloc_data) + 0x1FF) & ~0x1FF

        # Pad PE data to accommodate new section
        needed_size = new_raw_ptr + new_raw_size
        if len(pe_data) < needed_size:
            pe_data.extend(b'\0' * (needed_size - len(pe_data)))

        # Write new section header (exactly 40 bytes)
        new_sec_offset = section_start + num_sections * 40
        sec_header = b'.reloc\0\0'                                    # Name (8 bytes)
        sec_header += struct.pack('<I', new_vsize)                    # VirtualSize
        sec_header += struct.pack('<I', new_vaddr)                    # VirtualAddress
        sec_header += struct.pack('<I', new_raw_size)                 # SizeOfRawData
        sec_header += struct.pack('<I', new_raw_ptr)                  # PointerToRawData
        sec_header += struct.pack('<I', 0)                            # PointerToRelocations
        sec_header += struct.pack('<I', 0)                            # PointerToLinenumbers
        sec_header += struct.pack('<H', 0)                            # NumberOfRelocations
        sec_header += struct.pack('<H', 0)                            # NumberOfLinenums
        sec_header += struct.pack('<I', 0x42000040)                   # Characteristics

        pe_data[new_sec_offset:new_sec_offset + 40] = sec_header

        # Write .reloc data
        reloc_padded = reloc_data + b'\0' * (new_raw_size - len(reloc_data))
        pe_data[new_raw_ptr:new_raw_ptr + len(reloc_padded)] = reloc_padded

        # Update COFF header: NumberOfSections
        struct.pack_into('<H', pe_data, coff_offset + 2, num_sections + 1)

        # Update SizeOfImage in Optional Header (offset 56 for PE32+)
        new_image_size = new_vaddr + ((new_vsize + 0xFFF) & ~0xFFF)
        struct.pack_into('<I', pe_data, opt_offset + 56, new_image_size)

        reloc_rva = new_vaddr
        reloc_size = new_vsize
        print(f"Added .reloc: {len(reloc_data)} bytes at RVA 0x{new_vaddr:x}")

    # --- Update Data Directory entry for Base Relocations ---
    # In PE32+, Data Directory starts at opt_offset + 112 + 8*8 = opt_offset + 176
    # Actually: DataDir starts at opt_offset + 112 for PE32+
    # Entry 5 (Base Relocation) = DataDir + 5*8
    # PE32+ Optional Header layout:
    #   0-1: Magic (2)
    #   ... many fields ...
    #   112: Data Directory start (NumberOfRvaAndSizes is at offset 108)
    num_data_dir = struct.unpack_from('<I', pe_data, opt_offset + 108)[0]
    datadir_offset = opt_offset + 112

    if num_data_dir >= 6:
        # Entry 5: Base Relocation Directory
        entry_offset = datadir_offset + 5 * 8
        old_rva, old_size = struct.unpack_from('<II', pe_data, entry_offset)
        struct.pack_into('<II', pe_data, entry_offset, reloc_rva, reloc_size)
        print(f"Data Directory[5] (Base Reloc): RVA=0x{reloc_rva:x} Size={reloc_size}")
    else:
        print(f"Warning: Data Directory only has {num_data_dir} entries, need >= 6", file=sys.stderr)

    # Write back
    with open(pe_path, 'wb') as f:
        f.write(pe_data)

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
