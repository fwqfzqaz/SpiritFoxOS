/*
 * SpiritFoxOS 用户态进程支持
 *
 * 用户态转换、exec 和 FD 表辅助函数。
 * 从 process.c 中提取以实现模块化。
 * 重构为使用 mmu_virt_to_phys() 和串口模块。
 */

#include "process.h"
#include "gdt.h"
#include "hal.h"
#include "memory.h"
#include "kmalloc.h"
#include "smp.h"
#include "string.h"
#include "vfs.h"
#include "elf64.h"
#include "vga.h"
#include "serial.h"
#include "mmu.h"

extern process_t *current;

/* ========================================================================
 * 常量
 * ======================================================================== */

#define KERNEL_STACK_PAGES 2
#define DEFAULT_TIMESLICE  20

/* GDT 选择器（必须与 gdt.h 匹配） */
#define USER_CS     0x18
#define USER_DS     0x20

/* 带中断标志的 RFLAGS */
#define RFLAGS_IF   0x0202

/* ========================================================================
 * write_user_mem() – 通过 mmu 向用户虚拟地址写入字节
 *
 * 内核使用恒等映射，因此物理页地址也是有效的虚拟地址。
 * 我们使用 mmu_virt_to_phys() 查找物理页，然后通过恒等映射写入。
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
 * process_exec() – 用新程序替换当前进程映像
 *
 * 1. 将路径复制到内核缓冲区（用户地址空间将被替换）
 * 2. 通过 elf_load_from_vfs() 加载 ELF（创建新 PML4，映射段/栈）
 * 3. 切换到新地址空间（CR3）
 * 4. 重置信号，设置带 argc/argv/envp 的用户栈
 * 5. 构建 trap frame 并 iretq 到用户态
 *
 * 失败时恢复旧地址空间，以便进程可以继续运行。
 * 成功时此函数不返回 – 它跳转到用户态。
 * ======================================================================== */

int process_exec(const char *path, const char *const argv[],
                 const char *const envp[])
{
    if (!current || !path)
        return -1;

    /* ---- 1. 将路径复制到内核缓冲区 ---- */
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

    /* 清除 pml4 和 stack_top，强制 elf_load_with_base() 创建新的用户页表和映射栈。
     * 对于从内核线程调用 process_exec() 的情况，proc->pml4 是内核页表，
     * elf_load_with_base() 检查 !proc->pml4 来决定是否创建新页表。
     * 如果不清零，用户段会被映射到内核页表中，导致严重错误。 */
    current->pml4      = 0;
    current->stack_top = 0;

    /* ---- 3. 加载 ELF（创建新 PML4，加载段，映射栈） ---- */
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
            /* 常规 ET_EXEC – 加载到原始虚拟地址 */
            load_ret = elf_load_with_base(current, buf, (size_t)file_size, 0);
        }

        serial_puts("[exec] elf_load returned ");
        serial_put_hex((uint64_t)load_ret);
        serial_puts(" pml4=0x");
        serial_put_hex(current->pml4);
        serial_puts(" entry=0x");
        serial_put_hex(current->entry_point);
        serial_puts("\n");

        if (load_ret < 0) {
            kfree(buf);
            current->pml4       = old_pml4;
            current->entry_point = old_entry;
            current->stack_top  = old_stack_top;
            current->brk        = old_brk;
            current->mmap_base  = old_mmap_base;
            return -1;
        }

        /* 如果存在 PT_INTERP，尝试加载动态链接器（解释器） */
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
            /* 如果解释器文件不存在，不使用它继续 */
        }

        kfree(buf);
    }

    /* ---- 4. Address space ready (CR3 switch deferred to iretq) ---- */
    /* 注意：不在此处切换 CR3！所有内核操作必须使用内核页表。
     * CR3 切换在 process_enter_user() 的 iretq 之前完成，
     * 以避免内核在进程页表下运行时数据被意外破坏。 */

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

    /* 重新读取 ELF 以获取 phdr/程序头信息用于 auxv。
     * 我们需要知道 PT_LOAD 段被映射到哪里以计算 AT_PHDR。 */
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

                        /* 查找第一个 PT_LOAD 被映射的基址 */
                        if (eh->e_type == ET_DYN)
                            main_base = 0; /* PIE 基址 - 我们将从 PT_PHDR 找到 */
                        else
                            main_base = 0; /* ET_EXEC 使用原始地址 */

                        /* 查找 PT_PHDR 以获取程序头表地址 */
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
                        /* 如果没有 PT_PHDR，从第一个 PT_LOAD 计算 */
                        if (main_phdr_addr == 0) {
                            for (uint16_t i = 0; i < eh->e_phnum; i++) {
                                if (ph[i].p_type == PT_LOAD) {
                                    uint64_t load_base = (eh->e_type == ET_DYN)
                                        ? 0x400000ULL : 0;
                                    /* phdr 表通常在第一个 LOAD 段中 */
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

    /* 如果已加载解释器，设置 AT_BASE 为其加载地址 */
    if (has_interp)
        main_base = 0x7FF000000000ULL;

    /* 计算 argv 数量并将字符串复制到内核空间 */
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
            kargv_len[argc_val] = len + 1;   /* 包含 NUL */
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

    /* 将 sp 对齐到 16 字节（ABI 要求） */
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
        write_user_mem(current, sp, &auxv[i * 2 + 1], 8); /* 值 */
        sp -= 8;
        write_user_mem(current, sp, &auxv[i * 2], 8);     /* 类型 */
    }

    /* envp NULL 终止符 */
    sp -= 8;
    write_user_mem(current, sp, (uint64_t[]){0}, 8);

    /* envp 指针（逆序） */
    for (int j = envc_val - 1; j >= 0; j--) {
        sp -= 8;
        write_user_mem(current, sp, &kenvp_str_addr[j], 8);
    }

    /* argv NULL 终止符 */
    sp -= 8;
    write_user_mem(current, sp, (uint64_t[]){0}, 8);

    /* argv 指针（逆序） */
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

    /* ---- 7.5 Clear kernel flag – this is now a user process ---- */
    /* process_create_kthread() sets PROC_FLAG_KERNEL, but after exec
     * the process runs in user mode.  Keeping the flag set causes
     * isr_handler() to treat user-mode CPU faults as kernel panics
     * instead of killing the offending user process. */
    current->flags &= ~PROC_FLAG_KERNEL;

    /* Update TSS rsp0 for future interrupts from user mode */
    if (current->kernel_stack) {
        gdt_set_tss_rsp0(this_cpu()->index,
            (uint64_t)current->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE));
    }

    /* ---- 8. 跳转到用户态 – 不返回 ---- */
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

    /* 将 trap_frame 与内核栈分开分配。
     * 之前它被放在内核栈的顶部，但 user_proc_launcher 中
     * 后续的函数调用会覆盖它，因为栈向下增长到同一区域。 */
    trap_frame_t *frame = kmalloc(sizeof(trap_frame_t));
    if (!frame) {
        printf("[setup_frame] ERROR: failed to allocate trap_frame\n");
        return;
    }

    printf("[setup_frame] frame=%p (heap-allocated) sizeof=%lu\n",
           (void *)frame, (unsigned long)sizeof(trap_frame_t));

    memset(frame, 0, sizeof(trap_frame_t));

    /* 用户态寄存器状态 */
    frame->rip    = entry;
    frame->cs     = USER_CS | 0x03;    /* 0x1B – 用户代码段，RPL 3 */
    frame->rflags = RFLAGS_IF;
    frame->rsp    = stack;
    frame->ss     = USER_DS | 0x03;    /* 0x23 – 用户数据段，RPL 3 */
    frame->rdi    = arg;

    proc->trap_frame  = frame;
    proc->entry_point = entry;
    proc->stack_top   = stack;

    printf("[setup_frame] done: proc->trap_frame=%p frame->rip=%llx frame->cs=%llx\n",
           (void *)proc->trap_frame, (unsigned long long)frame->rip,
           (unsigned long long)frame->cs);
}

/* ========================================================================
 * process_enter_user() – 通过 iretq 跳转到用户态
 * ======================================================================== */

void process_enter_user(trap_frame_t *frame)
{
    if (!frame)
        return;

    /* 设置 TSS rsp0 以便中断时可以返回内核态 */
    if (current && current->kernel_stack) {
        gdt_set_tss_rsp0(this_cpu()->index,
            (uint64_t)current->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE));
    }

    /* Set up GS:8 (kernel RSP) and GS:16 (process PML4) for syscall entry point */
    {
        void *cpu_area_ptr = this_cpu()->syscall_cpu_area;
        if (cpu_area_ptr) {
            uint64_t kstack_top = (current && current->kernel_stack)
                ? (uint64_t)current->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE)
                : cpu_gdts[this_cpu()->index].tss.rsp0;
            uint64_t *cpu_area = (uint64_t *)cpu_area_ptr;
            cpu_area[1] = kstack_top;          /* gs:8 = 内核栈顶 */
            cpu_area[2] = current->pml4;       /* gs:16 = 进程 PML4 */
        }
    }

    serial_puts("[enter_user] rip=0x");
    serial_put_hex(frame->rip);
    serial_puts(" rsp=0x");
    serial_put_hex(frame->rsp);
    serial_puts(" pml4=0x");
    serial_put_hex(current->pml4);
    serial_puts("\n");

    /*
     * 切换到用户态 - CR3 切换在 iretq 之前的内联汇编中完成。
     *
     * 关键设计：所有内核操作（write_user_mem, 页表构建等）
     * 都在内核 CR3 下完成。CR3 切换发生在最后时刻，
     * 与 iretq 在同一指令序列中，确保内核不会在
     * 进程页表下执行任何可能破坏数据的操作。
     *
     * CR3 值传递方式：将 pml4 存入 frame->rcx，通过
     * 寄存器恢复加载到 rcx，再写入 CR3。这避免了
     * 编译器使用被内联汇编覆盖的寄存器来寻址的问题
     *（之前的 "m"(current->pml4) 约束导致编译器用
     * rdx 寻址，但 rdx 被寄存器恢复覆盖为 0）。
     */
    frame->rcx = current->pml4;

    __asm__ volatile (
        "cli\n\t"
        /* 将帧指针加载到 r8 */
        "mov %[frame], %%r8\n\t"

        /* 注意：不做 swapgs！内核态运行时 GS_BASE=0，
         * KERNEL_GS_BASE=per-CPU。iretq 后用户态也保持 GS_BASE=0。
         * 当用户态执行 syscall 时，syscall_entry 中的 swapgs 会
         * 正确地将 GS_BASE 换为 per-CPU（KERNEL_GS_BASE 的值）。 */

        /* 将数据段设置为用户态（USER_DS | RPL 3 = 0x23） */
        "mov $0x23, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"

        /* 压入 iretq 帧：ss, rsp, rflags, cs, rip */
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

        /* 从 trap_frame 恢复通用寄存器。
         * 注意：rcx 恢复的是 frame->rcx = current->pml4，
         * 用于后续的 CR3 切换。*/
        "mov 0(%%r8), %%r15\n\t"
        "mov 8(%%r8), %%r14\n\t"
        "mov 16(%%r8), %%r13\n\t"
        "mov 24(%%r8), %%r12\n\t"
        "mov 32(%%r8), %%r11\n\t"
        "mov 40(%%r8), %%r10\n\t"
        "mov 48(%%r8), %%r9\n\t"
        "mov 64(%%r8), %%rdi\n\t"
        "mov 72(%%r8), %%rsi\n\t"
        "mov 80(%%r8), %%rdx\n\t"
        "mov 88(%%r8), %%rcx\n\t"     /* rcx = frame->rcx = pml4 */
        "mov 96(%%r8), %%rbx\n\t"
        "mov 104(%%r8), %%rbp\n\t"
        "mov 112(%%r8), %%rax\n\t"

        /* CR3 切换：rcx 已包含 pml4 值 */
        "mov %%rcx, %%cr3\n\t"

        "mov 56(%%r8), %%r8\n\t"

        "iretq\n\t"
        :
        : [frame] "r"(frame)
        : "memory"
    );

    __builtin_unreachable();
}

/* ========================================================================
 * FD 表辅助函数
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
