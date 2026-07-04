/*
 * SpiritFoxOS User-Mode Process Support
 *
 * User-mode transitions, exec, and FD table helpers.
 * Extracted from process.c for modularity.
 * Refactored to use mmu_virt_to_phys() and serial module.
 */

#include "process.h"
#include "gdt.h"
#include "hal.h"
#include "memory.h"
#include "kmalloc.h"
#include "string.h"
#include "vfs.h"
#include "elf64.h"
#include "vga.h"
#include "serial.h"
#include "mmu.h"

extern process_t *current;

/* ========================================================================
 * Constants
 * ======================================================================== */

#define KERNEL_STACK_PAGES 2
#define DEFAULT_TIMESLICE  20

/* GDT selectors (must match gdt.h) */
#define USER_CS     0x18
#define USER_DS     0x20

/* RFLAGS with interrupt flag set */
#define RFLAGS_IF   0x0202

/* ========================================================================
 * write_user_mem() – write bytes to user virtual address via mmu
 *
 * The kernel uses identity mapping, so physical page addresses are also
 * valid virtual addresses.  We use mmu_virt_to_phys() to find the
 * physical page, then write through the identity mapping.
 * ======================================================================== */

int write_user_mem(process_t *proc, uint64_t vaddr,
                   const void *data, size_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    size_t off = 0;

    while (off < len) {
        uint64_t page_off = (vaddr + off) & (PAGE_SIZE - 1);
        size_t   chunk    = PAGE_SIZE - (size_t)page_off;
        if (chunk > len - off)
            chunk = len - off;

        uint64_t phys = mmu_virt_to_phys(proc->pml4, vaddr + off);
        if (phys == 0) return -1;

        memcpy((void *)(uintptr_t)phys, src + off, chunk);
        off += chunk;
    }
    return 0;
}

/* ========================================================================
 * process_exec() – replace the current process image with a new program
 *
 * 1. Copy path to kernel buffer (user address space will be replaced)
 * 2. Load ELF via elf_load_from_vfs() (creates new PML4, maps segments/stack)
 * 3. Switch to new address space (CR3)
 * 4. Reset signals, set up user stack with argc/argv/envp
 * 5. Build trap frame and iretq to user mode
 *
 * On failure the old address space is restored so the process can continue.
 * On success this function never returns – it jumps to user mode.
 * ======================================================================== */

int process_exec(const char *path, const char *const argv[],
                 const char *const envp[])
{
    if (!current || !path)
        return -1;

    /* ---- 1. Copy path to kernel buffer ---- */
    char kpath[VFS_MAX_PATH];
    {
        size_t i;
        for (i = 0; i < VFS_MAX_PATH - 1 && path[i]; i++)
            kpath[i] = path[i];
        kpath[i] = '\0';
    }

    /* ---- 2. Save old address-space state for rollback ---- */
    uint64_t old_pml4      = current->pml4;
    uint64_t old_entry     = current->entry_point;
    uint64_t old_stack_top = current->stack_top;
    uint64_t old_brk       = current->brk;
    uint64_t old_mmap_base = current->mmap_base;

    /* Track whether the binary has a PT_INTERP (dynamic linker) */
    int has_interp = 0;

    /* ---- 3. Load ELF (creates new PML4, loads segments, maps stack) ---- */
    {
        int fd = vfs_open(kpath, VFS_O_RDONLY, 0);
        if (fd < 0) {
            current->pml4       = old_pml4;
            current->entry_point = old_entry;
            current->stack_top  = old_stack_top;
            current->brk        = old_brk;
            current->mmap_base  = old_mmap_base;
            return -1;
        }

        int64_t file_size = vfs_seek(fd, 0, VFS_SEEK_END);
        if (file_size <= 0) {
            vfs_close(fd);
            current->pml4       = old_pml4;
            current->entry_point = old_entry;
            current->stack_top  = old_stack_top;
            current->brk        = old_brk;
            current->mmap_base  = old_mmap_base;
            return -1;
        }
        vfs_seek(fd, 0, VFS_SEEK_SET);

        void *buf = kmalloc((size_t)file_size);
        if (!buf) {
            vfs_close(fd);
            current->pml4       = old_pml4;
            current->entry_point = old_entry;
            current->stack_top  = old_stack_top;
            current->brk        = old_brk;
            current->mmap_base  = old_mmap_base;
            return -1;
        }

        int bytes_read = vfs_read(fd, buf, (size_t)file_size);
        vfs_close(fd);

        if (bytes_read != (int)file_size) {
            kfree(buf);
            current->pml4       = old_pml4;
            current->entry_point = old_entry;
            current->stack_top  = old_stack_top;
            current->brk        = old_brk;
            current->mmap_base  = old_mmap_base;
            return -1;
        }

        /* Parse ELF header to check type and find PT_INTERP */
        const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)buf;
        char interp_path[256] = {0};

        if ((size_t)file_size >= sizeof(Elf64_Ehdr) &&
            ehdr->e_ident[0] == ELFMAG0 && ehdr->e_ident[1] == ELFMAG1 &&
            ehdr->e_ident[2] == ELFMAG2 && ehdr->e_ident[3] == ELFMAG3 &&
            ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize <= (uint64_t)file_size) {

            const Elf64_Phdr *phdr = (const Elf64_Phdr *)
                ((const uint8_t *)buf + ehdr->e_phoff);

            for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_INTERP) {
                    has_interp = 1;
                    uint64_t off = phdr[i].p_offset;
                    uint64_t sz  = phdr[i].p_filesz;
                    if (sz > 255) sz = 255;
                    memcpy(interp_path, (const uint8_t *)buf + off, sz);
                    interp_path[sz] = '\0';
                    /* Remove trailing newline/null from path */
                    for (size_t j = 0; j < sz; j++) {
                        if (interp_path[j] == '\n' || interp_path[j] == '\0') {
                            interp_path[j] = '\0';
                            break;
                        }
                    }
                    break;
                }
            }
        }

        /* Load the main binary */
        int load_ret;
        if (ehdr->e_type == ET_DYN) {
            /* PIE executable – load at a fixed base address */
            uint64_t pie_base = 0x400000ULL;
            load_ret = elf_load_with_base(current, buf, (size_t)file_size,
                                          pie_base);
        } else {
            /* Regular ET_EXEC – load at original virtual addresses */
            load_ret = elf_load_with_base(current, buf, (size_t)file_size, 0);
        }

        if (load_ret < 0) {
            kfree(buf);
            current->pml4       = old_pml4;
            current->entry_point = old_entry;
            current->stack_top  = old_stack_top;
            current->brk        = old_brk;
            current->mmap_base  = old_mmap_base;
            return -1;
        }

        /* Try to load the dynamic linker (interpreter) if PT_INTERP exists */
        if (has_interp && interp_path[0]) {
            int interp_fd = vfs_open(interp_path, VFS_O_RDONLY, 0);
            if (interp_fd >= 0) {
                int64_t interp_size = vfs_seek(interp_fd, 0, VFS_SEEK_END);
                vfs_seek(interp_fd, 0, VFS_SEEK_SET);

                if (interp_size > 0) {
                    void *interp_buf = kmalloc((size_t)interp_size);
                    if (interp_buf) {
                        int ib = vfs_read(interp_fd, interp_buf,
                                          (size_t)interp_size);
                        vfs_close(interp_fd);

                        if (ib == (int)interp_size) {
                            /* Load interpreter at a high address in the
                             * mmap region, well away from the main binary */
                            uint64_t interp_base = 0x7FF000000000ULL;
                            int iret = elf_load_with_base(current, interp_buf,
                                                           (size_t)interp_size,
                                                           interp_base);
                            if (iret == 0) {
                                /* Interpreter loaded – set entry point to
                                 * the interpreter's entry point so it can
                                 * perform relocations and then jump to the
                                 * main program. */
                                const Elf64_Ehdr *iehdr =
                                    (const Elf64_Ehdr *)interp_buf;
                                uint64_t interp_entry = iehdr->e_entry;
                                if (iehdr->e_type == ET_DYN)
                                    interp_entry += interp_base;
                                current->entry_point = interp_entry;
                            }
                            /* If interpreter loading failed, proceed with
                             * the main binary's entry point anyway. */
                        }
                        kfree(interp_buf);
                    } else {
                        vfs_close(interp_fd);
                    }
                } else {
                    vfs_close(interp_fd);
                }
            }
            /* If the interpreter file doesn't exist, proceed without it */
        }

        kfree(buf);
    }

    /* ---- 4. Switch to new address space ---- */
    hal_write_cr3(current->pml4);

    /* ---- 5. Reset signal state (POSIX: caught signals → default on exec) ---- */
    current->pending_signals = 0;
    memset(current->signal_handlers, 0, sizeof(current->signal_handlers));
    memset(current->signal_flags, 0, sizeof(current->signal_flags));
    current->signal_restorer = 0;
    current->mmap_current = current->mmap_base;

    /* ---- 6. Build user-stack: argc, argv[], envp[], auxv[] ---- */

    /* Collect ELF info for auxiliary vector before building the stack.
     * We need the original ELF header info for the main binary. */
    const Elf64_Ehdr *main_ehdr = (const Elf64_Ehdr *)NULL; /* already freed */
    uint64_t main_phdr_addr = 0;    /* AT_PHDR: address of phdr table in memory */
    uint64_t main_phnum = 0;        /* AT_PHNUM */
    uint64_t main_phent = 0;        /* AT_PHENT */
    uint64_t main_base = 0;         /* AT_BASE: interpreter base addr (0 if static) */
    uint64_t main_entry = current->entry_point; /* AT_ENTRY */
    uint64_t main_flags = 0;        /* AT_FLAGS */

    /* Re-read the ELF to get phdr/program header info for auxv.
     * We need to know where the PT_LOAD segments were mapped to compute AT_PHDR. */
    {
        int fd2 = vfs_open(kpath, VFS_O_RDONLY, 0);
        if (fd2 >= 0) {
            int64_t fsz = vfs_seek(fd2, 0, VFS_SEEK_END);
            vfs_seek(fd2, 0, VFS_SEEK_SET);
            if (fsz > 0 && fsz <= 64 * 1024 * 1024) {
                void *ebuf = kmalloc((size_t)fsz);
                if (ebuf) {
                    if (vfs_read(fd2, ebuf, (size_t)fsz) == (int)fsz) {
                        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)ebuf;
                        main_phnum = eh->e_phnum;
                        main_phent = eh->e_phentsize;

                        /* Find the base address where the first PT_LOAD was mapped */
                        if (eh->e_type == ET_DYN)
                            main_base = 0; /* PIE base - we'll find it from PT_PHDR */
                        else
                            main_base = 0; /* ET_EXEC uses original addresses */

                        /* Find PT_PHDR to get the program header table address */
                        const Elf64_Phdr *ph = (const Elf64_Phdr *)
                            ((const uint8_t *)ebuf + eh->e_phoff);
                        for (uint16_t i = 0; i < eh->e_phnum; i++) {
                            if (ph[i].p_type == PT_PHDR) {
                                /* For ET_EXEC: phdr is at p_vaddr directly.
                                 * For ET_DYN (PIE): phdr is at pie_base + p_vaddr. */
                                if (eh->e_type == ET_DYN)
                                    main_phdr_addr = 0x400000ULL + ph[i].p_vaddr;
                                else
                                    main_phdr_addr = ph[i].p_vaddr;
                                break;
                            }
                        }
                        /* If no PT_PHDR, compute from first PT_LOAD */
                        if (main_phdr_addr == 0) {
                            for (uint16_t i = 0; i < eh->e_phnum; i++) {
                                if (ph[i].p_type == PT_LOAD) {
                                    uint64_t load_base = (eh->e_type == ET_DYN)
                                        ? 0x400000ULL : 0;
                                    /* phdr table is typically in the first LOAD segment */
                                    main_phdr_addr = load_base + eh->e_phoff;
                                    break;
                                }
                            }
                        }
                    }
                    kfree(ebuf);
                }
            }
            vfs_close(fd2);
        }
    }

    /* If interpreter was loaded, set AT_BASE to its load address */
    if (has_interp)
        main_base = 0x7FF000000000ULL;

    /* Count argv and copy strings to kernel space */
    int    argc_val = 0;
    char   kargv_buf[PROC_MAX_ARGS][VFS_MAX_PATH];
    size_t kargv_len[PROC_MAX_ARGS];

    if (argv) {
        while (argv[argc_val] && argc_val < PROC_MAX_ARGS) {
            size_t len = 0;
            while (argv[argc_val][len] && len < VFS_MAX_PATH - 1)
                len++;
            memcpy(kargv_buf[argc_val], argv[argc_val], len);
            kargv_buf[argc_val][len] = '\0';
            kargv_len[argc_val] = len + 1;   /* include NUL */
            argc_val++;
        }
    }

    /* Count envp and copy strings to kernel space */
    int    envc_val = 0;
    char   kenvp_buf[PROC_MAX_ENV][VFS_MAX_PATH];
    size_t kenvp_len[PROC_MAX_ENV];

    if (envp) {
        while (envp[envc_val] && envc_val < PROC_MAX_ENV) {
            size_t len = 0;
            while (envp[envc_val][len] && len < VFS_MAX_PATH - 1)
                len++;
            memcpy(kenvp_buf[envc_val], envp[envc_val], len);
            kenvp_buf[envc_val][len] = '\0';
            kenvp_len[envc_val] = len + 1;
            envc_val++;
        }
    }

    uint64_t sp = current->stack_top;

    /* Push string data (argv strings, then envp strings) at the bottom */
    uint64_t kargv_str_addr[PROC_MAX_ARGS];
    for (int j = argc_val - 1; j >= 0; j--) {
        sp -= kargv_len[j];
        kargv_str_addr[j] = sp;
        write_user_mem(current, sp, kargv_buf[j], kargv_len[j]);
    }

    uint64_t kenvp_str_addr[PROC_MAX_ENV];
    for (int j = envc_val - 1; j >= 0; j--) {
        sp -= kenvp_len[j];
        kenvp_str_addr[j] = sp;
        write_user_mem(current, sp, kenvp_buf[j], kenvp_len[j]);
    }

    /* Align sp to 16 bytes (ABI requirement) */
    sp &= ~0xFULL;

    /* Auxiliary vector (AT_NULL terminated, pairs of type+value) */
    /* Push in reverse order since stack grows down */
    uint64_t auxv[] = {
        0, 0,                   /* AT_NULL terminator */
        3, main_phdr_addr,      /* AT_PHDR */
        4, main_phent,          /* AT_PHENT */
        5, main_phnum,          /* AT_PHNUM */
        6, 4096,                /* AT_PAGESZ */
        7, main_base,           /* AT_BASE (interpreter base) */
        8, 0,                   /* AT_FLAGS */
        9, main_entry,          /* AT_ENTRY */
        11, 0,                  /* AT_UID */
        12, 0,                  /* AT_EUID */
        13, 0,                  /* AT_GID */
        14, 0,                  /* AT_EGID */
        15, 0x0000,             /* AT_HWCAP */
        16, 0,                  /* AT_CLKTCK (100 Hz) */
        23, 0,                  /* AT_SECURE = 0 */
        25, 0,                  /* AT_RANDOM (placeholder) */
        26, 0,                  /* AT_HWCAP2 */
    };
    int nauxv = sizeof(auxv) / (2 * sizeof(uint64_t));
    for (int i = nauxv - 1; i >= 0; i--) {
        sp -= 8;
        write_user_mem(current, sp, &auxv[i * 2 + 1], 8); /* value */
        sp -= 8;
        write_user_mem(current, sp, &auxv[i * 2], 8);     /* type */
    }

    /* envp NULL terminator */
    sp -= 8;
    write_user_mem(current, sp, (uint64_t[]){0}, 8);

    /* envp pointers (reverse order) */
    for (int j = envc_val - 1; j >= 0; j--) {
        sp -= 8;
        write_user_mem(current, sp, &kenvp_str_addr[j], 8);
    }

    /* argv NULL terminator */
    sp -= 8;
    write_user_mem(current, sp, (uint64_t[]){0}, 8);

    /* argv pointers (reverse order) */
    for (int j = argc_val - 1; j >= 0; j--) {
        sp -= 8;
        write_user_mem(current, sp, &kargv_str_addr[j], 8);
    }

    /* argc */
    sp -= 8;
    uint64_t argc64 = (uint64_t)argc_val;
    write_user_mem(current, sp, &argc64, 8);

    /* ---- 7. Build trap frame on kernel stack ---- */
    process_setup_frame(current, current->entry_point, sp, 0);

    /* Update TSS rsp0 for future interrupts from user mode */
    if (current->kernel_stack) {
        tss.rsp0 = (uint64_t)current->kernel_stack +
                    (KERNEL_STACK_PAGES * PAGE_SIZE);
    }

    /* ---- 8. Jump to user mode – does not return ---- */
    process_enter_user(current->trap_frame);

    __builtin_unreachable();
}

/* ========================================================================
 * process_setup_frame() – build trap frame for new user process
 * ======================================================================== */

void process_setup_frame(process_t *proc, uint64_t entry, uint64_t stack,
                          uint64_t arg)
{
    if (!proc || !proc->kernel_stack) {
        printf("[setup_frame] ERROR: proc=%p kstack=%p\n", (void *)proc, proc ? proc->kernel_stack : NULL);
        return;
    }

    /* Allocate trap_frame separately from the kernel stack.
     * Previously this was placed at the top of the kernel stack,
     * but subsequent function calls in user_proc_launcher would
     * overwrite it since the stack grows downward into the same area. */
    trap_frame_t *frame = kmalloc(sizeof(trap_frame_t));
    if (!frame) {
        printf("[setup_frame] ERROR: failed to allocate trap_frame\n");
        return;
    }

    printf("[setup_frame] frame=%p (heap-allocated) sizeof=%lu\n",
           (void *)frame, (unsigned long)sizeof(trap_frame_t));

    memset(frame, 0, sizeof(trap_frame_t));

    /* User mode register state */
    frame->rip    = entry;
    frame->cs     = USER_CS | 0x03;    /* 0x1B – user code with RPL 3 */
    frame->rflags = RFLAGS_IF;
    frame->rsp    = stack;
    frame->ss     = USER_DS | 0x03;    /* 0x23 – user data with RPL 3 */
    frame->rdi    = arg;

    proc->trap_frame  = frame;
    proc->entry_point = entry;
    proc->stack_top   = stack;

    printf("[setup_frame] done: proc->trap_frame=%p frame->rip=%llx frame->cs=%llx\n",
           (void *)proc->trap_frame, (unsigned long long)frame->rip,
           (unsigned long long)frame->cs);
}

/* ========================================================================
 * process_enter_user() – jump to user mode via iretq
 * ======================================================================== */

void process_enter_user(trap_frame_t *frame)
{
    if (!frame)
        return;

    /* Set TSS rsp0 so we can return to kernel mode on interrupts */
    if (current && current->kernel_stack) {
        tss.rsp0 = (uint64_t)current->kernel_stack +
                    (KERNEL_STACK_PAGES * PAGE_SIZE);
    }

    /* Debug: dump entry point bytes BEFORE CR3 switch */
    {
        uint64_t entry = frame->rip;
        uint64_t phys = mmu_virt_to_phys(current->pml4, entry);
        if (phys) {
            uint8_t *code = (uint8_t *)(uintptr_t)phys;
            serial_puts("[enter_user] code@");
            serial_put_hex(entry);
            serial_puts(":");
            for (int i = 0; i < 16; i++) {
                serial_putchar(' ');
                serial_putchar("0123456789abcdef"[(code[i] >> 4) & 0xF]);
                serial_putchar("0123456789abcdef"[code[i] & 0xF]);
            }
            serial_puts("\n");
        } else {
            serial_puts("[enter_user] entry point not mapped!\n");
        }
    }

    serial_puts("[enter_user] entry=");
    serial_put_hex(frame->rip);
    serial_puts(" stack=");
    serial_put_hex(frame->rsp);
    serial_puts(" cs=");
    serial_put_hex(frame->cs);
    serial_puts(" ss=");
    serial_put_hex(frame->ss);
    serial_puts("\n");

    /* Debug: Read GS MSRs BEFORE CR3 switch */
    printf("[enter_user] PRE-CR3: KERNEL_GS_BASE=%llx GS_BASE=%llx\n",
           (unsigned long long)hal_read_msr(MSR_IA32_KERNEL_GS_BASE),
           (unsigned long long)hal_read_msr(MSR_IA32_GS_BASE));

    /* Switch to the process's page tables */
    if (current && current->pml4) {
        printf("[enter_user] Switching CR3: old=%llx new=%llx\n",
               (unsigned long long)hal_read_cr3(),
               (unsigned long long)current->pml4);
        hal_write_cr3(current->pml4);
        printf("[enter_user] CR3 after switch=%llx\n",
               (unsigned long long)hal_read_cr3());

        /* Debug: verify kernel mapping still works after CR3 switch */
        {
            uint64_t entry = frame->rip;
            uint64_t phys = mmu_virt_to_phys(current->pml4, entry);
            if (phys) {
                uint8_t *code = (uint8_t *)(uintptr_t)phys;
                serial_puts("[enter_user] after CR3: code@");
                serial_put_hex(entry);
                serial_puts(":");
                for (int i = 0; i < 16; i++) {
                    serial_putchar(' ');
                    serial_putchar("0123456789abcdef"[(code[i] >> 4) & 0xF]);
                    serial_putchar("0123456789abcdef"[code[i] & 0xF]);
                }
                serial_puts("\n");
            }
        }
    }

    /* Set up GS:8 (kernel RSP) for syscall entry point */
    {
        uint64_t gs_base_kernel = hal_read_msr(MSR_IA32_KERNEL_GS_BASE);
        uint64_t gs_base_user = hal_read_msr(MSR_IA32_GS_BASE);
        printf("[enter_user] GS: KERNEL_GS_BASE=%llx GS_BASE=%llx\n",
               (unsigned long long)gs_base_kernel, (unsigned long long)gs_base_user);
        if (gs_base_kernel) {
            uint64_t kstack_top = (current && current->kernel_stack)
                ? (uint64_t)current->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE)
                : tss.rsp0;
            *(uint64_t *)(gs_base_kernel + 8) = kstack_top;
            printf("[enter_user] wrote kstack_top=%llx to gs_area+8 (gs_area=%p, val@%p=%llx)\n",
                   (unsigned long long)kstack_top,
                   (void *)gs_base_kernel,
                   (void *)(gs_base_kernel + 8),
                   (unsigned long long)*(uint64_t *)(gs_base_kernel + 8));
        } else {
            printf("[enter_user] WARNING: KERNEL_GS_BASE is 0!\n");
        }
    }

    /* Switch to user mode using iretq */
    printf("[enter_user] iretq frame: rip=%llx cs=%llx rflags=%llx rsp=%llx ss=%llx\n",
           (unsigned long long)frame->rip,
           (unsigned long long)frame->cs,
           (unsigned long long)frame->rflags,
           (unsigned long long)frame->rsp,
           (unsigned long long)frame->ss);

    __asm__ volatile (
        "cli\n\t"
        /* Load frame pointer into r8 (callee-saved, won't be clobbered) */
        "mov %[frame], %%r8\n\t"

        /* Set data segments to user mode (USER_DS | RPL 3 = 0x23) */
        "mov $0x23, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"

        /* Push iretq frame: ss, rsp, rflags, cs, rip */
        "mov 168(%%r8), %%rax\n\t"    /* frame->ss */
        "pushq %%rax\n\t"
        "mov 160(%%r8), %%rax\n\t"    /* frame->rsp */
        "pushq %%rax\n\t"
        "mov 152(%%r8), %%rax\n\t"    /* frame->rflags */
        "pushq %%rax\n\t"
        "mov 144(%%r8), %%rax\n\t"    /* frame->cs */
        "pushq %%rax\n\t"
        "mov 136(%%r8), %%rax\n\t"    /* frame->rip */
        "pushq %%rax\n\t"

        /* Restore general-purpose registers from trap_frame */
        "mov 0(%%r8), %%r15\n\t"
        "mov 8(%%r8), %%r14\n\t"
        "mov 16(%%r8), %%r13\n\t"
        "mov 24(%%r8), %%r12\n\t"
        "mov 32(%%r8), %%r11\n\t"
        "mov 40(%%r8), %%r10\n\t"
        "mov 48(%%r8), %%r9\n\t"
        /* skip r8 – we're still using it */
        "mov 64(%%r8), %%rdi\n\t"
        "mov 72(%%r8), %%rsi\n\t"
        "mov 80(%%r8), %%rdx\n\t"
        "mov 88(%%r8), %%rcx\n\t"
        "mov 96(%%r8), %%rbx\n\t"
        "mov 104(%%r8), %%rbp\n\t"
        /* Now load rax and r8 */
        "mov 112(%%r8), %%rax\n\t"
        "mov 56(%%r8), %%r8\n\t"

        "iretq\n\t"
        :
        : [frame] "r"(frame)
        : "memory"
    );

    __builtin_unreachable();
}

/* ========================================================================
 * FD table helpers
 * ======================================================================== */

vfs_file_t **process_get_fd_table(void)
{
    if (current)
        return current->fd_table;
    return NULL;
}

int process_alloc_fd(void)
{
    if (!current)
        return -1;
    for (int i = 0; i < PROC_MAX_FD; i++) {
        if (current->fd_table[i] == NULL)
            return i;
    }
    return -1;
}
