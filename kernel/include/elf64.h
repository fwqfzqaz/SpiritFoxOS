#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>

/* ELF magic */
#define EI_NIDENT  16
#define ELFMAG0    0x7F
#define ELFMAG1    'E'
#define ELFMAG2    'L'
#define ELFMAG3    'F'

/* ELF class */
#define ELFCLASS32  1
#define ELFCLASS64  2

/* Data encoding */
#define ELFDATA2LSB 1

/* OS/ABI */
#define ELFOSABI_NONE   0
#define ELFOSABI_LINUX  3

/* Object file types */
#define ET_NONE  0
#define ET_REL   1
#define ET_EXEC  2
#define ET_DYN   3
#define ET_CORE  4

/* Machine types */
#define EM_X86_64  62

/* Program header types */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_GNU_EH_FRAME 0x6474E550
#define PT_GNU_STACK     0x6474E551

/* Program header flags */
#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4

/* Dynamic entry tags */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ       10
#define DT_SYMENT      11
#define DT_INIT        12
#define DT_FINI        13
#define DT_SONAME      14
#define DT_RPATH       15
#define DT_SYMBOLIC    16
#define DT_REL         17
#define DT_RELSZ       18
#define DT_RELENT      19
#define DT_PLTREL      20
#define DT_DEBUG       21
#define DT_TEXTREL     22
#define DT_JMPREL      23
#define DT_BIND_NOW    24
#define DT_INIT_ARRAY  25
#define DT_FINI_ARRAY  26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_FLAGS       30

/* ========================================================================
 * ELF64 structures
 * ======================================================================== */

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

typedef struct {
    uint32_t d_tag;
    uint64_t d_val;
    uint64_t d_ptr;
} __attribute__((packed)) Elf64_Dyn;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) Elf64_Sym;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) Elf64_Rela;

/* Relocation macros */
#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i) ((uint32_t)(i))
#define R_X86_64_NONE    0
#define R_X86_64_64      1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

/* ========================================================================
 * ELF Loader API
 * ======================================================================== */

/* Load an ELF executable into a new process's address space.
 * Returns entry point address, or 0 on failure. */
uint64_t elf_load(process_t *proc, const void *data, size_t size);

/* Load an ELF with a base address offset (for PIE executables and shared
 * libraries).  Maps PT_LOAD segments at base_addr + p_vaddr.
 * Reuses proc->pml4 if already set (for loading interpreter after main).
 * Returns 0 on success, negative on error. */
int elf_load_with_base(process_t *proc, const void *data, size_t size,
                       uint64_t base_addr);

/* Load an ELF from VFS path.
 * Returns 0 on success, negative on error. */
int elf_load_from_vfs(process_t *proc, const char *path);

#endif /* ELF64_H */
