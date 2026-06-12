#!/usr/bin/env python3
"""
字节级比较：正常工作的mingw PE vs 有问题的elf2pe.py生成的PE
"""
import struct

def parse_pe(path):
    with open(path, 'rb') as f:
        data = f.read()

    e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
    coff_off = e_lfanew + 4
    opt_off = coff_off + 20
    magic = struct.unpack_from('<H', data, opt_off)[0]

    result = {'data': data, 'size': len(data), 'e_lfanew': e_lfanew}

    # COFF头字段（全部）
    result['coff_machine']  = struct.unpack_from('<H', data, coff_off)[0]
    result['coff_sections'] = struct.unpack_from('<H', data, coff_off + 2)[0]
    result['coff_timestamp'] = struct.unpack_from('<I', data, coff_off + 4)[0]
    result['coff_sym_ptr']  = struct.unpack_from('<I', data, coff_off + 8)[0]
    result['coff_sym_num']  = struct.unpack_from('<I', data, coff_off + 12)[0]
    result['coff_opt_size'] = struct.unpack_from('<H', data, coff_off + 16)[0]
    result['coff_chars']    = struct.unpack_from('<H', data, coff_off + 18)[0]

    # 可选头关键字段
    if magic == 0x20b:
        o = opt_off
        result['opt_magic']      = magic
        result['opt_major_link'] = struct.unpack_from('<B', data, o+2)[0]
        result['opt_minor_link'] = struct.unpack_from('<B', data, o+3)[0]
        result['opt_code_sz']    = struct.unpack_from('<I', data, o+4)[0]
        result['opt_idata_sz']   = struct.unpack_from('<I', data, o+8)[0]
        result['opt_udata_sz']   = struct.unpack_from('<I', data, o+12)[0]
        result['opt_entry']      = struct.unpack_from('<I', data, o+16)[0]
        result['opt_base_code']  = struct.unpack_from('<I', data, o+20)[0]
        result['opt_image_base'] = struct.unpack_from('<Q', data, o+24)[0]
        result['opt_sec_align']  = struct.unpack_from('<I', data, o+32)[0]
        result['opt_file_align'] = struct.unpack_from('<I', data, o+36)[0]
        result['opt_os_ver']     = struct.unpack_from('<H', data, o+40)[0] << 16 | struct.unpack_from('<H', data, o+42)[0]
        result['opt_img_ver']    = struct.unpack_from('<H', data, o+44)[0] << 16 | struct.unpack_from('<H', data, o+46)[0]
        result['opt_sub_ver']    = struct.unpack_from('<H', data, o+48)[0] << 16 | struct.unpack_from('<H', data, o+50)[0]
        result['opt_win32_val']  = struct.unpack_from('<I', data, o+64)[0]
        result['opt_img_sz']     = struct.unpack_from('<I', data, o+56)[0]
        result['opt_hdrs_sz']    = struct.unpack_from('<I', data, o+60)[0]
        result['opt_subsystem']  = struct.unpack_from('<H', data, o+68)[0]
        result['opt_dll_char']   = struct.unpack_from('<H', data, o+70)[0]
        result['opt_stack_rsv']  = struct.unpack_from('<Q', data, o+72)[0]
        result['opt_stack_com']  = struct.unpack_from('<Q', data, o+80)[0]
        result['opt_heap_rsv']   = struct.unpack_from('<Q', data, o+88)[0]
        result['opt_heap_com']   = struct.unpack_from('<Q', data, o+96)[0]
        result['opt_loader_fls'] = struct.unpack_from('<I', data, o+104)[0]
        result['opt_num_rva']    = struct.unpack_from('<I', data, o+108)[0]
        result['opt_checksum']   = struct.unpack_from('<I', data, opt_off + 64)[0]  # 与win32_val相同

    # 节头
    nsec = result['coff_sections']
    sect_off = opt_off + result['coff_opt_size']
    result['sections'] = []
    for i in range(nsec):
        s = sect_off + i * 40
        name = data[s:s+8].rstrip(b'\x00').decode('ascii')
        vsize, va, raw_size, raw_ptr = struct.unpack_from('<IIII', data, s+8)
        reloc_n, linenum_n, reloc_c, linenum_c = struct.unpack_from('<HHHH', data, s+24)
        chars = struct.unpack_from('<I', data, s+36)[0]
        result['sections'].append({
            'name': name, 'vsize': vsize, 'va': va,
            'raw_size': raw_size, 'raw_ptr': raw_ptr,
            'reloc_n': reloc_n, 'linenum_n': linenum_n,
            'reloc_c': reloc_c, 'linenum_c': linenum_c,
            'chars': chars,
        })

    return result


mingw = parse_pe('build/test_mingw2.EFI')
elf2pe = parse_pe('build/test_minimal.EFI.fixed')

print("=" * 80)
print("比较：mingw（正常） vs elf2pe.py（失败）")
print(f"{'字段':<30} {'MINGW':<25} {'ELF2PE':<25} {'差异?'}")
print("-" * 80)

fields = [
    ('File Size', mingw['size'], elf2pe['size']),
    ('e_lfanew', mingw['e_lfanew'], elf2pe['e_lfanew']),
    ('COFF Machine', mingw['coff_machine'], elf2pe['coff_machine']),
    ('COFF Sections', mingw['coff_sections'], elf2pe['coff_sections']),
    ('COFF Characteristics', hex(mingw['coff_chars']), hex(elf2pe['coff_chars'])),
    ('Opt Header Size', mingw['coff_opt_size'], elf2pe['coff_opt_size']),
    ('Magic', hex(mingw['opt_magic']), hex(elf2pe['opt_magic'])),
    ('MajorLinkerVer', mingw['opt_major_link'], elf2pe['opt_major_link']),
    ('MinorLinkerVer', mingw['opt_minor_link'], elf2pe['opt_minor_link']),
    ('SizeOfCode', hex(mingw['opt_code_sz']), hex(elf2pe['opt_code_sz'])),
    ('SizeOfInitData', hex(mingw['opt_idata_sz']), hex(elf2pe['opt_idata_sz'])),
    ('SizeOfUninitData', hex(mingw['opt_udata_sz']), hex(elf2pe['opt_udata_sz'])),
    ('EntryPoint', hex(mingw['opt_entry']), hex(elf2pe['opt_entry'])),
    ('BaseOfCode', hex(mingw['opt_base_code']), hex(elf2pe['opt_base_code'])),
    ('ImageBase', hex(mingw['opt_image_base']), hex(elf2pe['opt_image_base'])),
    ('SectionAlign', hex(mingw['opt_sec_align']), hex(elf2pe['opt_sec_align'])),
    ('FileAlign', hex(mingw['opt_file_align']), hex(elf2pe['opt_file_align'])),
    ('OS Version', hex(mingw['opt_os_ver']), hex(elf2pe['opt_os_ver'])),
    ('ImageVersion', hex(mingw['opt_img_ver']), hex(elf2pe['opt_img_ver'])),
    ('SubsystemVer', hex(mingw['opt_sub_ver']), hex(elf2pe['opt_sub_ver'])),
    ('Win32Value', hex(mingw['opt_win32_val']), hex(elf2pe['opt_win32_val'])),
    ('SizeOfImage', hex(mingw['opt_img_sz']), hex(elf2pe['opt_img_sz'])),
    ('SizeOfHeaders', hex(mingw['opt_hdrs_sz']), hex(elf2pe['opt_hdrs_sz'])),
    ('Subsystem', mingw['opt_subsystem'], elf2pe['opt_subsystem']),
    ('DllChar', hex(mingw['opt_dll_char']), hex(elf2pe['opt_dll_char'])),
    ('StackReserve', hex(mingw['opt_stack_rsv']), hex(elf2pe['opt_stack_rsv'])),
    ('StackCommit', hex(mingw['opt_stack_com']), hex(elf2pe['opt_stack_com'])),
    ('HeapReserve', hex(mingw['opt_heap_rsv']), hex(elf2pe['opt_heap_com'])),
    ('HeapCommit', hex(mingw['opt_heap_com']), hex(elf2pe['opt_heap_com'])),
    ('LoaderFlags', hex(mingw['opt_loader_fls']), hex(elf2pe['opt_loader_fls'])),
    ('NumRvaAndSizes', mingw['opt_num_rva'], elf2pe['opt_num_rva']),
    ('CheckSum', hex(mingw['opt_checksum']), hex(elf2pe['opt_checksum'])),
]

for name, mv, ev in fields:
    diff = " ***" if mv != ev else ""
    print(f"{name:<30} {str(mv):<25} {str(ev):<25}{diff}")

# 比较各节
print("\n" + "=" * 80)
print("节比较")
print("-" * 80)
max_sec = max(len(mingw['sections']), len(elf2pe['sections']))
for i in range(max_sec):
    ms = mingw['sections'][i] if i < len(mingw['sections']) else None
    es = elf2pe['sections'][i] if i < len(elf2pe['sections']) else None
    if ms and es:
        diffs = []
        for k in ['vsize','va','raw_size','raw_ptr','chars']:
            if ms[k] != es[k]:
                diffs.append(k + ":" + hex(ms[k]) + "!=" + hex(es[k]))
        dstr = " *** " + ", ".join(diffs) if diffs else ""
        print(f"  [{i}] {ms['name']:8s} VA={ms['va']:08x} RSz={ms['raw_size']:06x} RP={ms['raw_ptr']:06x} Ch={ms['chars']:08x}{dstr}")
    elif ms:
        print(f"  [{i}] {ms['name']:8s} (仅存在于MINGW)")
    elif es:
        print(f"  [{i}] {es['name']:8s} (仅存在于ELF2PE)")

# 前512字节十六进制差异比较
print("\n" + "=" * 80)
print("前256字节十六进制差异")
md = mingw['data']
ed = elf2pe['data']
for off in range(0, min(len(md), len(ed)), 16):
    mb = md[off:off+16]
    eb = ed[off:off+16]
    if mb != eb:
        print(f"  {off:04x}: MINGW={mb.hex()}  ELF2PE={eb.hex()}")
