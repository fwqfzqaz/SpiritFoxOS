#!/usr/bin/env python3
import struct

with open('build/kernel_efi.elf', 'rb') as f:
    data = f.read()

e_shoff  = struct.unpack_from('<Q', data, 40)[0]
e_shentsz= struct.unpack_from('<H', data, 58)[0]
e_shnum  = struct.unpack_from('<H', data, 60)[0]
e_shstrndx = struct.unpack_from('<H', data, 62)[0]

shdr_off = e_shoff + e_shstrndx * e_shentsz
strtab_off  = struct.unpack_from('<Q', data, shdr_off + 24)[0]
strtab_sz   = struct.unpack_from('<Q', data, shdr_off + 32)[0]
strtab = data[strtab_off:strtab_off + strtab_sz]

def get_str(off):
    end = strtab.index(b'\x00', off)
    return strtab[off:end].decode('ascii')

print(f"ELF: {e_shnum} sections, shoff=0x{e_shoff:x}, shentsz={e_shentsz}")
type_names = {0:'NULL',1:'PROGBITS',2:'SYMTAB',3:'STRTAB',4:'RELA',
              7:'NOTE',8:'NOBITS',11:'DYNSYM'}

for i in range(e_shnum):
    off = e_shoff + i * e_shentsz
    name  = get_str(struct.unpack_from('<I', data, off)[0])
    sh_type  = struct.unpack_from('<I', data, off + 4)[0]
    sh_addr  = struct.unpack_from('<Q', data, off + 16)[0]
    sh_offset= struct.unpack_from('<Q', data, off + 24)[0]
    sh_size  = struct.unpack_from('<Q', data, off + 32)[0]

    if sh_type == 8:
        raw_len = 0
    else:
        raw_len = len(data[sh_offset:sh_offset+sh_size])

    tname = type_names.get(sh_type, str(sh_type))
    print(f"  {name:12s} {tname:8s} VA=0x{sh_addr:08x} Off=0x{sh_offset:08x} Size=0x{sh_size:06x} RawLen=0x{raw_len:06x}")
