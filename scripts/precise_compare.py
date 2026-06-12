#!/usr/bin/env python3
"""
精确头比较：正常工作的mingw vs 有问题的elf2pe.py
重点关注从COFF头到节头结束的部分
"""
import struct

def hexdump(data, start=0, end=None):
    if end is None:
        end = len(data)
    for off in range(start, end, 16):
        hex_str = ' '.join(f'{data[i]:02x}' for i in range(off, min(off+16, end)))
        ascii_str = ''.join(chr(data[i]) if 32 <= data[i] < 127 else '.' for i in range(off, min(off+16, end)))
        print(f"  {off:04x}: {hex_str:<48s} {ascii_str}")

with open('build/test_mingw2.EFI', 'rb') as f:
    mingw = f.read()
with open('build/test_minimal.EFI', 'rb') as f:
    elf2pe = f.read()

m_lfanew = struct.unpack_from('<I', mingw, 0x3c)[0]
e_lfanew = struct.unpack_from('<I', elf2pe, 0x3c)[0]

print("=== MINGW 头（DOS -> PE签名 -> COFF -> 可选头 -> 节） ===")
hexdump(mingw, 0, 0x400)

print("\n=== ELF2PE 头 ===")
hexdump(elf2pe, 0, 0x200)

# PE部分的逐字段详细比较
print("\n=== 详细PE头比较 ===")
print(f"{'Offset':<8s} {'Size':<6s} {'Field':<35s} {'MINGW':<20s} {'ELF2PE':<20s}")
print("-" * 95)

# 从e_lfanew到节头结束进行比较
def cmp(m_off, e_off, size, name):
    m_val = m_data[m_off:m_off+size] if m_off+size <= len(mingw) else b'(越界)'
    e_val = e_data[e_off:e_off+size] if e_off+size <= len(elf2pe) else b'(越界)'
    match = "OK" if m_val == e_val else "*** 差异 ***"
    m_disp = m_val.hex() if len(m_val) <= 8 else f"({len(m_val)} bytes)"
    e_disp = e_val.hex() if len(e_val) <= 8 else f"({len(e_val)} bytes)"
    print(f"{m_off:<8x} {size:<6d} {name:<35s} {m_disp:<20s} {e_disp:<20s} {match}")

m_coff = m_lfanew + 4
e_coff = e_lfanew + 4
m_opt = m_coff + 20
e_opt = e_coff + 20
m_opt_sz = struct.unpack_from('<H', mingw, m_coff + 16)[0]
e_opt_sz = struct.unpack_from('<H', elf2pe, e_coff + 16)[0]
m_nsec = struct.unpack_from('<H', mingw, m_coff + 2)[0]
e_nsec = struct.unpack_from('<H', elf2pe, e_coff + 2)[0]
m_sect = m_opt + m_opt_sz
e_sect = e_opt + e_opt_sz

m_data = mingw
e_data = elf2pe

# DOS头关键字段（相对偏移）
cmp(0x00, 0x00, 2, "e_magic")
cmp(0x3c, 0x3c, 4, "e_lfanew")

# PE签名
cmp(m_lfanew, e_lfanew, 4, "PE签名")

# COFF头（20字节）
cmp(m_coff+0, e_coff+0, 2, "机器类型")
cmp(m_coff+2, e_coff+2, 2, "节数量")
cmp(m_coff+4, e_coff+4, 4, "时间戳")
cmp(m_coff+8, e_coff+8, 4, "符号表指针")
cmp(m_coff+12, e_coff+12, 4, "符号数量")
cmp(m_coff+16, e_coff+16, 2, "可选头大小")
cmp(m_coff+18, e_coff+18, 2, "特征")

# 可选头 PE32+
cmp(m_opt+0, e_opt+0, 2, "魔数")
cmp(m_opt+2, e_opt+2, 1, "主链接器版本")
cmp(m_opt+3, e_opt+3, 1, "次链接器版本")
cmp(m_opt+4, e_opt+4, 4, "代码大小")
cmp(m_opt+8, e_opt+8, 4, "已初始化数据大小")
cmp(m_opt+12, e_opt+12, 4, "未初始化数据大小")
cmp(m_opt+16, e_opt+16, 4, "入口点地址")
cmp(m_opt+20, e_opt+20, 4, "代码基址")
cmp(m_opt+24, e_opt+24, 8, "镜像基址")
cmp(m_opt+32, e_opt+32, 4, "节对齐")
cmp(m_opt+36, e_opt+36, 4, "文件对齐")
cmp(m_opt+40, e_opt+40, 2, "主操作系统版本")
cmp(m_opt+42, e_opt+42, 2, "次操作系统版本")
cmp(m_opt+44, e_opt+44, 2, "主镜像版本")
cmp(m_opt+46, e_opt+46, 2, "次镜像版本")
cmp(m_opt+48, e_opt+48, 2, "主子系统版本")
cmp(m_opt+50, e_opt+50, 2, "次子系统版本")
cmp(m_opt+52, e_opt+52, 4, "Win32版本值")
cmp(m_opt+56, e_opt+56, 4, "镜像大小")
cmp(m_opt+60, e_opt+60, 4, "头大小")
cmp(m_opt+64, e_opt+64, 4, "校验和")
cmp(m_opt+68, e_opt+68, 2, "子系统")
cmp(m_opt+70, e_opt+70, 2, "DLL特征")
cmp(m_opt+72, e_opt+72, 8, "栈保留大小")
cmp(m_opt+80, e_opt+80, 8, "栈提交大小")
cmp(m_opt+88, e_opt+88, 8, "堆保留大小")
cmp(m_opt+96, e_opt+96, 8, "堆提交大小")
cmp(m_opt+104, e_opt+104, 4, "加载器标志")
cmp(m_opt+108, e_opt+108, 4, "RVA和大小数量")

# 数据目录（16 x 8字节 = 128字节）
for i in range(16):
    names = ['导出','导入','资源','异常','证书','基址重定位',
             '调试','架构','全局指针','TLS','加载配置','绑定导入',
             '导入地址表','延迟导入','CLR','保留']
    dd_name = names[i] if i < len(names) else f'目录[{i}]'
    cmp(m_opt+112+i*8, e_opt+112+i*8, 8, f"数据目录[{i}] {dd_name}")

# 节头
print(f"\n=== 节头（mingw: {m_nsec}, elf2pe: {e_nsec}) ===")
max_n = max(m_nsec, e_nsec)
for i in range(max_n):
    ms = m_sect + i * 40 if i < m_nsec else None
    es = e_sect + i * 40 if i < e_nsec else None
    prefix = f"  Sec[{i}]"
    if ms and es:
        cmp(ms, es, 8, prefix + " Name")
        cmp(ms+8, es+8, 4, prefix + " VirtualSize")
        cmp(ms+12, es+12, 4, prefix + " VA")
        cmp(ms+16, es+16, 4, prefix + " SizeOfRawData")
        cmp(ms+20, es+20, 4, prefix + " PtrToRawData")
        cmp(ms+24, es+24, 2, prefix + " PtrToRelocations")
        cmp(ms+26, es+26, 2, prefix + " PtrToLinenumbers")
        cmp(ms+28, es+28, 2, prefix + " NumRelocations")
        cmp(ms+30, es+30, 2, prefix + " NumLinenumbers")
        cmp(ms+36, es+36, 4, prefix + " Characteristics")
    elif ms:
        print(f"  Sec[{i}] 仅存在于MINGW")
    elif es:
        print(f"  Sec[{i}] 仅存在于ELF2PE")
