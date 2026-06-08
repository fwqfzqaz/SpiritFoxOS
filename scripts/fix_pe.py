#!/usr/bin/env python3
"""
fix_pe.py - Post-process PE/COFF file for UEFI compatibility.

Fixes common issues in objcopy-generated PEs that cause UEFI's
LoadImage service to reject them with "Unsupported" or "not an image".

Usage: python3 fix_pe.py <pe_file.efi>
"""
import struct
import sys
import binascii


def fix_pe(filepath):
    with open(filepath, 'rb') as f:
        data = bytearray(f.read())

    if len(data) < 512:
        print(f"Error: File too small ({len(data)} bytes)")
        return False

    # Verify DOS header
    if data[0:2] != b'MZ':
        print("Error: Not a valid DOS/PE file (missing MZ signature)")
        return False

    # Get PE offset from DOS header
    pe_offset = struct.unpack_from('<I', data, 0x3C)[0]
    if pe_offset >= len(data) or data[pe_offset:pe_offset+4] != b'PE\x00\x00':
        print(f"Error: Invalid PE signature at offset 0x{pe_offset:X}")
        return False

    coff_offset = pe_offset + 4  # After "PE\0\0"

    # Parse COFF header
    machine = struct.unpack_from('<H', data, coff_offset)[0]
    num_sections = struct.unpack_from('<H', data, coff_offset + 2)[0]
    opt_hdr_size = struct.unpack_from('<H', data, coff_offset + 16)[0]
    characteristics = struct.unpack_from('<H', data, coff_offset + 18)[0]

    if machine != 0x8664:
        print(f"Warning: Machine type 0x{machine:04X} (expected 0x8664 for AMD64)")

    # Parse Optional Header (PE32+)
    opt_offset = coff_offset + 20
    magic = struct.unpack_from('<H', data, opt_offset)[0]

    if magic != 0x20B:
        print(f"Error: Expected PE32+ magic (0x20B), got 0x{magic:04X}")
        return False

    # PE32+ specific offsets from start of optional header
    # Subsystem at offset 68 in PE32+
    subsystem_off = opt_offset + 68
    subsystem = struct.unpack_from('<H', data, subsystem_off)[0]

    # DllCharacteristics at offset 70 in PE32+
    dll_chars_off = opt_offset + 70
    dll_chars = struct.unpack_from('<H', data, dll_chars_off)[0]

    print(f"PE: {num_sections} sections, subsystem={subsystem}, "
          f"dll_chars=0x{dll_chars:04X}, chars=0x{characteristics:04X}")

    # Fix 1: Set DllCharacteristics for UEFI compatibility
    # NX_COMPAT (0x0100) is required by some UEFI implementations
    new_dll_chars = dll_chars | 0x0160  # HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT
    struct.pack_into('<H', data, dll_chars_off, new_dll_chars)
    print(f"  Fixed DllCharacteristics: 0x{dll_chars:04X} -> 0x{new_dll_chars:04X}")

    # Fix 2: Ensure COFF characteristics include EXECUTABLE_IMAGE
    new_chars = characteristics | 0x0002  # EXECUTABLE_IMAGE
    struct.pack_into('<H', data, coff_offset + 18, new_chars)
    if new_chars != characteristics:
        print(f"  Fixed Characteristics: 0x{characteristics:04X} -> 0x{new_chars:04X}")

    # Parse section headers to find .reloc
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

    # Fix 3: Ensure .reloc has correct section characteristics
    if reloc_sec_idx >= 0:
        sec = sec_info[reloc_sec_idx]
        # Required: INITIALIZED_DATA | MEM_DISCARDABLE | MEM_READ
        required_chars = 0x42000040
        if sec['chars'] != required_chars:
            print(f"  Fixing .reloc chars: 0x{sec['chars']:08X} -> 0x{required_chars:08X}")
            struct.pack_into('<I', data, sec['offset'] + 36, required_chars)

        # Ensure .reloc has valid content (at least one base relocation block)
        if sec['raw_size'] > 0 and sec['raw_ptr'] > 0 and sec['raw_ptr'] < len(data):
            # Check if it looks like a valid relocation directory
            page_rva = struct.unpack_from('<I', data, sec['raw_ptr'])[0]
            block_size = struct.unpack_from('<I', data, sec['raw_ptr'] + 4)[0]
            print(f"  .reloc content: PageRVA=0x{page_rva:X}, BlockSize={block_size}")

            if block_size == 0 or block_size > sec['raw_size']:
                # Fix: create minimal valid relocation block
                print(f"  Fixing .reloc content (invalid block size)")
                struct.pack_into('<I', data, sec['raw_ptr'], 0)     # Page RVA = 0
                struct.pack_into('<I', data, sec['raw_ptr'] + 4, 12)  # Block size = 12
                # 4 x IMAGE_REL_BASED_ABSOLUTE (type 0) entries = 8 bytes
                struct.pack_into('<H', data, sec['raw_ptr'] + 8, 0)
                struct.pack_into('<H', data, sec['raw_ptr'] + 10, 0)
                struct.pack_into('<H', data, sec['raw_ptr'] + 12, 0)
                struct.pack_into('<H', data, sec['raw_ptr'] + 14, 0)

    # Fix 4: Recalculate SizeOfImage (must be aligned to SectionAlignment)
    # Find the highest VA + VirtualSize among all sections
    section_alignment = struct.unpack_from('<I', data, opt_offset + 44)[0]
    if section_alignment == 0:
        section_alignment = 4096  # Default
    max_end = 0
    for sec in sec_info:
        end = sec['va'] + max(sec['vsize'], sec['raw_size'])
        if end > max_end:
            max_end = end

    # Align up to section alignment
    size_of_image = ((max_end + section_alignment - 1) // section_alignment) * section_alignment
    size_of_image_off = opt_offset + 56  # SizeOfImage in PE32+
    current_soi = struct.unpack_from('<I', data, size_of_image_off)[0]
    if size_of_image != current_soi:
        print(f"  Fixing SizeOfImage: 0x{current_soi:X} -> 0x{size_of_image:X}")
        struct.pack_into('<I', data, size_of_image_off, size_of_image)

    # Fix 5: Recalculate SizeOfHeaders
    headers_end = sec_table_offset + num_sections * 40
    file_alignment = struct.unpack_from('<I', data, opt_offset + 48)[0]
    if file_alignment == 0:
        file_alignment = 512  # Default
    size_of_headers = ((headers_end + file_alignment - 1) // file_alignment) * file_alignment
    soh_off = opt_offset + 60  # SizeOfHeaders in PE32+
    current_soh = struct.unpack_from('<I', data, soh_off)[0]
    if size_of_headers != current_soh:
        print(f"  Fixing SizeOfHeaders: 0x{current_soh:X} -> 0x{size_of_headers:X}")
        struct.pack_into('<I', data, soh_off, size_of_headers)

    # Write fixed file
    with open(filepath, 'wb') as f:
        f.write(data)

    print(f"Fixed: {filepath}")
    return True


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <pe_file.efi>")
        sys.exit(1)

    sys.exit(0 if fix_pe(sys.argv[1]) else 1)
