#include "smp.h"
#include "apic.h"
#include "acpi.h"
#include "hal.h"
#include "kmalloc.h"
#include "string.h"
#include "vga.h"

/* ========================================================================
 * AP 引导代码的外部声明（在 ap_trampoline.S 中定义）
 * ======================================================================== */
/* ========================================================================
 * AP 引导代码的外部声明
 * 引导代码被汇编为原始二进制（ap_trampoline.bin）
 * 并在此处作为字节数组嵌入，以避免混合 16/64 位代码的
 * ELF 重定位问题。
 * ======================================================================== */

/* 引导代码可修补数据偏移量（必须与 ap_trampoline.S 匹配） */
#define TRAMP_PML4_OFF      0x00   /* 4 字节：PML4 物理地址 */
#define TRAMP_ENTRY_OFF     0x04   /* 8 字节：C 入口点地址 */
#define TRAMP_GDT_OFF       0x0C   /* 2+8 字节：GDT 界限 + 基地址 */

/* 引导代码二进制 - 通过以下方式从 ap_trampoline.S 汇编：
 *   nasm -f bin ap_trampoline.S -o ap_trampoline.bin
 *   xxd -i ap_trampoline.bin > ap_trampoline_bin.h
 * 目前直接作为静态数组嵌入。
 * 引导代码从 16 位实模式切换到 64 位长模式。 */
static const uint8_t ap_trampoline_code[] = {
    /* ap_pml4:   dd 0 */       0x00, 0x00, 0x00, 0x00,
    /* ap_entry:  dq 0 */       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* ap_gdt_ptr: dw 0 */      0x00, 0x00,
    /*            dq 0 */       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* cli */                   0xFA,
    /* xor ax, ax */            0x31, 0xC0,
    /* mov ds, ax */            0x8E, 0xD8,
    /* mov es, ax */            0x8E, 0xC0,
    /* in al, 0x92 */           0xE4, 0x92,
    /* or al, 2 */              0x0C, 0x02,
    /* out 0x92, al */          0xE6, 0x92,
    /* mov eax, [0x8000] */     0xA1, 0x00, 0x80, 0x00, 0x00,
    /* mov cr3, eax */          0x0F, 0x22, 0xD8,
    /* mov eax, cr4 */          0x0F, 0x20, 0xE0,
    /* or eax, 0x20 */          0x0D, 0x20, 0x00, 0x00, 0x00,
    /* mov cr4, eax */          0x0F, 0x22, 0xE0,
    /* mov ecx, 0xC0000080 */   0xB9, 0x80, 0x00, 0x00, 0xC0,
    /* rdmsr */                 0x0F, 0x32,
    /* or eax, 0x100 */         0x0D, 0x00, 0x01, 0x00, 0x00,
    /* wrmsr */                 0x0F, 0x30,
    /* lgdt [0x800C] */         0x0F, 0x01, 0x15, 0x0C, 0x80, 0x00, 0x00,
    /* mov eax, cr0 */          0x0F, 0x20, 0xC0,
    /* or eax, 0x80000000 */    0x0D, 0x00, 0x00, 0x00, 0x80,
    /* mov cr0, eax */          0x0F, 0x22, 0xC0,
    /* jmp 0x08:ap_long_mode（远跳转） */
    0xEA, 0x36, 0x80, 0x00, 0x00, 0x08, 0x00,
    /* --- 64 位模式 --- */
    /* mov ds, ax */            0x8E, 0xD8,
    /* mov es, ax */            0x8E, 0xC0,
    /* mov fs, ax */            0x8E, 0xE0,
    /* mov gs, ax */            0x8E, 0xE8,
    /* mov ss, ax */            0x8E, 0xD0,
    /* mov rsp, 0x90000 */      0x48, 0xC7, 0xC4, 0x00, 0x00, 0x09, 0x00,
    /* mov rax, [0x8004] */     0x48, 0xA1, 0x04, 0x80, 0x00, 0x00,
    /* call rax */              0xFF, 0xD0,
    /* cli */                   0xFA,
    /* hlt */                   0xF4,
    /* jmp .-2 */               0xEB, 0xFD,
};

static const size_t ap_trampoline_size = sizeof(ap_trampoline_code);

/* ========================================================================
 * 全局状态
 * ======================================================================== */
static cpu_info_t  cpu_infos[SMP_MAX_CPUS];
static cpu_local_t cpu_locals[SMP_MAX_CPUS];
static int         cpu_count = 0;

/* LAPIC MMIO 基地址（从 apic_get_lapic_base 缓存） */
static uintptr_t   lapic_base = 0;

/* ========================================================================
 * LAPIC ICR 辅助函数（写入 ICR 寄存器）
 * ======================================================================== */

static void lapic_write(uint32_t offset, uint32_t value)
{
    hal_mmio_write32(lapic_base + offset, value);
}

static uint32_t lapic_read(uint32_t offset)
{
    return hal_mmio_read32(lapic_base + offset);
}

/* 等待待处理的 IPI 投递完成 */
static void lapic_wait_icr(void)
{
    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVERY_PENDING)
        ;
}

/* ========================================================================
 * 自旋锁实现（票据锁）
 * ======================================================================== */

void spinlock_init(spinlock_t *lock)
{
    lock->ticket = 0;
    lock->serving = 0;
}

void spinlock_acquire(spinlock_t *lock)
{
    uint64_t flags = hal_save_interrupts();

    /* 原子获取并递增票据号 */
    uint32_t my_ticket = __atomic_fetch_add(&lock->ticket, 1, __ATOMIC_ACQUIRE);

    /* 自旋直到轮到自己的票据号 */
    while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != my_ticket) {
        hal_restore_interrupts(flags);
        __asm__ volatile ("pause");
        flags = hal_save_interrupts();
    }

    /* 持有锁期间中断被禁用。
     * 标志保存在局部变量中，释放时恢复。 */
    /* 保存标志到锁中以便释放时恢复 */
    __asm__ volatile ("" : : : "memory");  /* 编译器屏障 */
    /* 我们通过简单的方法实现每 CPU 标志存储：
     * 由于我们禁用中断且在自旋期间保持禁用，
     * 释放时只需重新启用中断。这是安全的，因为
     * spinlock_release 必须由获取锁的同一 CPU 调用。 */
}

void spinlock_release(spinlock_t *lock)
{
    /* 推进服务计数器 */
    __atomic_add_fetch(&lock->serving, 1, __ATOMIC_RELEASE);

    /* 重新启用中断（在获取时被禁用） */
    hal_enable_interrupts();
}

/* ========================================================================
 * IPI 函数
 * ======================================================================== */

void smp_send_ipi(uint8_t dest_apic_id, uint8_t vector)
{
    lapic_wait_icr();

    /* 在 ICR 高位设置目标 APIC ID（第 24-31 位） */
    uint32_t icr_high = ((uint32_t)dest_apic_id << 24);
    lapic_write(LAPIC_ICR_HIGH, icr_high);

    /* 在 ICR 低位设置向量号和投递模式 */
    uint32_t icr_low = (uint32_t)vector
                      | LAPIC_ICR_DELIVERY_FIXED
                      | LAPIC_ICR_DEST_PHYSICAL
                      | LAPIC_ICR_LEVEL_ASSERT;
    lapic_write(LAPIC_ICR_LOW, icr_low);

    lapic_wait_icr();
}

void smp_broadcast_ipi(uint8_t vector)
{
    lapic_wait_icr();

    /* 简写模式无需设置 ICR 高位 */
    lapic_write(LAPIC_ICR_HIGH, 0);

    /* 设置向量号、固定投递、除自身外所有 CPU 简写 */
    uint32_t icr_low = (uint32_t)vector
                      | LAPIC_ICR_DELIVERY_FIXED
                      | LAPIC_ICR_LEVEL_ASSERT
                      | LAPIC_ICR_SHORTHAND_ALL_EXCLUDING;
    lapic_write(LAPIC_ICR_LOW, icr_low);

    lapic_wait_icr();
}

/* ========================================================================
 * 从 MADT 枚举 CPU
 * ======================================================================== */

static void smp_enumerate_cpus(void)
{
    void *madt_ptr = acpi_find_table("APIC");
    if (!madt_ptr) {
        printf("[SMP] No MADT table found, single CPU mode\n");
        cpu_count = 1;
        cpu_infos[0].processor_id = 0;
        cpu_infos[0].apic_id = (uint8_t)apic_get_lapic_id();
        cpu_infos[0].flags = 1; /* 已启用 */
        return;
    }

    acpi_sdt_header_t *madt_hdr = (acpi_sdt_header_t *)madt_ptr;
    uintptr_t entry_offset = sizeof(acpi_sdt_header_t) + 8; /* 跳过头部 + lapic_addr + flags */

    cpu_count = 0;

    while (entry_offset < madt_hdr->length && cpu_count < SMP_MAX_CPUS) {
        madt_entry_t *entry = (madt_entry_t *)((uintptr_t)madt_ptr + entry_offset);
        if (entry->length == 0)
            break;

        if (entry->type == MADT_TYPE_LAPIC) {
            madt_lapic_t *lapic = (madt_lapic_t *)entry;
            /* 仅添加已启用的 CPU */
            if (lapic->flags & 0x01) {
                cpu_infos[cpu_count].processor_id = lapic->processor_id;
                cpu_infos[cpu_count].apic_id = lapic->apic_id;
                cpu_infos[cpu_count].flags = lapic->flags;
                cpu_count++;
            }
        }

        entry_offset += entry->length;
    }

    if (cpu_count == 0) {
        /* 回退：至少有 BSP */
        printf("[SMP] No LAPIC entries in MADT, assuming single CPU\n");
        cpu_count = 1;
        cpu_infos[0].processor_id = 0;
        cpu_infos[0].apic_id = (uint8_t)apic_get_lapic_id();
        cpu_infos[0].flags = 1;
    }

    printf("[SMP] Found %d CPU(s)\n", cpu_count);
    for (int i = 0; i < cpu_count; i++) {
        printf("[SMP]   CPU %d: APIC ID=%u, Processor ID=%u\n",
               i, cpu_infos[i].apic_id, cpu_infos[i].processor_id);
    }
}

/* ========================================================================
 * 延时循环（近似毫秒）
 * ======================================================================== */

static void delay_ms(unsigned int ms)
{
    /* 使用 hal_halt_no_cli 配合简单循环的粗略延时。
     * 每次迭代在现代 CPU 速度下带 io_wait 大约 ~1us。 */
    for (unsigned int i = 0; i < ms * 1000; i++) {
        hal_outb(0x80, 0); /* 通过端口 0x80 延时约 1us */
    }
}

/* ========================================================================
 * AP 启动序列
 * ======================================================================== */

static void smp_start_aps(void)
{
    uint32_t bsp_apic_id = apic_get_lapic_id();

    printf("[SMP] BSP APIC ID: %u\n", bsp_apic_id);

    /* 初始化所有 CPU 的 cpu_locals */
    for (int i = 0; i < cpu_count; i++) {
        cpu_locals[i].cpu_id = cpu_infos[i].apic_id;
        cpu_locals[i].index = (uint32_t)i;
        cpu_locals[i].online = 0;
        cpu_locals[i].bsp = (cpu_infos[i].apic_id == bsp_apic_id) ? 1 : 0;
        cpu_locals[i].current_process = NULL;
        cpu_locals[i].kernel_rsp = 0;

        /* 为 AP 分配内核栈（BSP 已有） */
        if (!cpu_locals[i].bsp) {
            void *stack = kmalloc(PROC_KERNEL_STACK);
            if (!stack) {
                printf("[SMP] ERROR: Failed to allocate stack for AP %u\n",
                       cpu_infos[i].apic_id);
                continue;
            }
            memset(stack, 0, PROC_KERNEL_STACK);
            /* 栈向下增长：指向已分配区域的顶部 */
            cpu_locals[i].kernel_stack = stack;
        } else {
            cpu_locals[i].kernel_stack = NULL; /* BSP 使用现有栈 */
        }
    }

    /* 标记 BSP 为在线 */
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_locals[i].bsp) {
            cpu_locals[i].online = 1;
            /* 设置 BSP 的 GS 基地址 */
            hal_write_msr(MSR_IA32_GS_BASE, (uint64_t)&cpu_locals[i]);
            break;
        }
    }

    /* 将 AP 引导代码复制到 0x8000（恒等映射区域） */
    hal_ensure_mapped(AP_TRAMPOLINE_ADDR, ap_trampoline_size);
    memcpy((void *)AP_TRAMPOLINE_ADDR, ap_trampoline_code, ap_trampoline_size);

    /* 用运行时值修补引导数据 */
    uint8_t *trampoline_base = (uint8_t *)AP_TRAMPOLINE_ADDR;

    /* 设置 PML4 地址 */
    *(uint32_t *)(trampoline_base + TRAMP_PML4_OFF) = (uint32_t)(hal_read_cr3() & PTE_ADDR_MASK);

    /* 设置 C 入口点 */
    *(uint64_t *)(trampoline_base + TRAMP_ENTRY_OFF) = (uint64_t)(uintptr_t)ap_entry_c;

    /* 设置 GDT 指针（从当前 GDTR 获取） */
    uint8_t gdt_buffer[10];
    __asm__ volatile ("sgdt %0" : "=m"(gdt_buffer) : : "memory");
    memcpy(trampoline_base + TRAMP_GDT_OFF, gdt_buffer, 10);

    printf("[SMP] Trampoline copied to 0x%x (%lu bytes)\n",
           AP_TRAMPOLINE_ADDR, (unsigned long)ap_trampoline_size);

    /* 启动每个 AP */
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_locals[i].bsp)
            continue;

        uint8_t apic_id = cpu_infos[i].apic_id;
        printf("[SMP] Starting AP with APIC ID %u...\n", apic_id);

        /* 步骤 1：发送 INIT IPI */
        lapic_wait_icr();
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
        lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_LEVEL_ASSERT);
        lapic_wait_icr();

        /* 步骤 2：等待 10ms */
        delay_ms(10);

        /* 步骤 3：发送 SIPI（启动 IPI），向量为 0x08（页面 0x8000 / 4096） */
        lapic_wait_icr();
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
        lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_DELIVERY_STARTUP | 0x08);
        lapic_wait_icr();

        /* 步骤 4：等待 200us */
        delay_ms(1);

        /* 步骤 5：发送第二次 SIPI（按 Intel 规范建议） */
        lapic_wait_icr();
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
        lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_DELIVERY_STARTUP | 0x08);
        lapic_wait_icr();

        /* 步骤 6：等待 AP 上线（带超时） */
        int timeout = 5000; /* 约 5 秒 */
        while (!cpu_locals[i].online && timeout > 0) {
            hal_halt_no_cli();
            delay_ms(1);
            timeout--;
        }

        if (cpu_locals[i].online) {
            printf("[SMP] AP %u is online\n", apic_id);
        } else {
            printf("[SMP] WARNING: AP %u did not come online (timeout)\n", apic_id);
        }
    }
}

/* ========================================================================
 * AP C 入口点
 * ======================================================================== */

void ap_entry_c(void)
{
    /* 获取本 CPU 的 LAPIC ID */
    uint32_t my_apic_id = apic_get_lapic_id();

    /* 查找本 CPU 的 cpu_local_t */
    cpu_local_t *local = NULL;
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_locals[i].cpu_id == my_apic_id) {
            local = &cpu_locals[i];
            break;
        }
    }

    if (!local) {
        /* 未知 CPU - 停机 */
        hal_halt();
    }

    /* 设置 GS 基地址用于每 CPU 数据访问 */
    hal_write_msr(MSR_IA32_GS_BASE, (uint64_t)local);

    /* 设置内核栈 */
    if (local->kernel_stack) {
        uint64_t stack_top = (uint64_t)local->kernel_stack + PROC_KERNEL_STACK;
        __asm__ volatile ("mov %0, %%rsp" : : "r"(stack_top));
    }

    /* 初始化本 AP 的 LAPIC */
    /* 通过 MSR 启用 LAPIC */
    uint64_t msr_val = hal_read_msr(MSR_IA32_APIC_BASE);
    if (!(msr_val & APIC_BASE_GLOBAL_ENABLE)) {
        msr_val |= APIC_BASE_GLOBAL_ENABLE;
        hal_write_msr(MSR_IA32_APIC_BASE, msr_val);
    }

    /* 设置伪中断向量并启用 */
    lapic_write(LAPIC_SVR, APIC_VECTOR_SPURIOUS | LAPIC_SVR_ENABLE);

    /* 清除错误状态 */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* 屏蔽所有 LVT 条目 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);

    /* 接受所有中断 */
    lapic_write(LAPIC_TPR, 0);

    /* 标记自身为在线 */
    __atomic_store_n(&local->online, 1, __ATOMIC_RELEASE);

    printf("[SMP] AP %u initialized\n", my_apic_id);

    /* 进入空闲循环并启用中断 */
    hal_enable_interrupts();
    while (1) {
        hal_halt_no_cli();
    }
}

/* ========================================================================
 * SMP 初始化（由 BSP 调用）
 * ======================================================================== */

void smp_init(void)
{
    /* 缓存 LAPIC 基地址用于 ICR 访问 */
    lapic_base = apic_get_lapic_base();
    if (lapic_base == 0) {
        printf("[SMP] ERROR: LAPIC base not available\n");
        return;
    }

    printf("[SMP] Initializing SMP subsystem...\n");

    /* 步骤 1：从 MADT 枚举 CPU */
    smp_enumerate_cpus();

    /* 步骤 2：启动 AP（仅当 CPU 数量大于 1 时） */
    if (cpu_count > 1) {
        smp_start_aps();
    }

    printf("[SMP] Initialization complete: %d CPU(s) online\n", cpu_count);
}

/* ========================================================================
 * 访问器
 * ======================================================================== */

int smp_get_cpu_count(void)
{
    return cpu_count;
}

cpu_info_t *smp_get_cpu_info(int index)
{
    if (index < 0 || index >= cpu_count)
        return NULL;
    return &cpu_infos[index];
}

cpu_local_t *smp_get_current_cpu(void)
{
    uint64_t gs_base = hal_read_msr(MSR_IA32_GS_BASE);
    return (cpu_local_t *)gs_base;
}
