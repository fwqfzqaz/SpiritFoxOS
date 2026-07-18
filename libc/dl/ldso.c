/* SpiritFoxOS Dynamic Linker (ld.so) - Skeleton
 * 
 * This is a minimal dynamic linker that can load ELF shared objects.
 * It is loaded as the ELF interpreter (PT_INTERP) and is responsible
 * for loading shared libraries, resolving relocations, and calling
 * the program's entry point.
 *
 * Currently a skeleton - full implementation will be completed later.
 */

#include <stdint.h>
#include <stddef.h>

/* ELF definitions */
#define EI_NIDENT 16
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3

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
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* Dynamic section tags */
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_PLTGOT   3
#define DT_JMPREL   23
#define DT_PLTRELSZ 2
#define DT_PLTREL   20
#define DT_R_X86_64_NONE     0
#define DT_R_X86_64_64       1
#define DT_R_X86_64_GLOB_DAT 6
#define DT_R_X86_64_JUMP_SLOT 7
#define DT_R_X86_64_RELATIVE 8

/* Loaded shared object */
typedef struct so_info {
    const char       *name;
    void             *base;         /* Base address where loaded */
    Elf64_Ehdr       *ehdr;
    Elf64_Phdr       *phdr;
    struct so_info   *next;
    int               ref_count;
} so_info_t;

/* Global list of loaded shared objects */
static so_info_t *loaded_libs = NULL;

/* The dynamic linker entry point.
 * The kernel loads ld.so and passes it an auxiliary vector on the stack.
 * For now, this is a skeleton. */
void _dl_start(void *stack)
{
    /* TODO: Parse auxiliary vector from kernel
     * TODO: Load PT_INTERP libraries
     * TODO: Resolve relocations
     * TODO: Call _init of each library
     * TODO: Jump to program entry point */
    
    /* Infinite loop for now */
    while (1) {
        __asm__ volatile("hlt");
    }
}
