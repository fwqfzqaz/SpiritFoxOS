#include "smp.h"
#include "apic.h"
#include "acpi.h"
#include "gdt.h"
#include "hal.h"
#include "idt.h"
#include "kmalloc.h"
#include "memory.h"
#include "process.h"
#include "string.h"
#include "timer.h"
#include "vga.h"

/* 内核栈页数（与 process.h 中 KERNEL_STACK_PAGES 一致） */
#ifndef KERNEL_STACK_PAGES
#define KERNEL_STACK_PAGES 2
#endif

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
    /* 由 NASM 从 ap_trampoline.S 汇编生成：
     *   nasm -f bin ap_trampoline.S -o ap_trampoline.bin
     *   xxd -i ap_trampoline.bin
     *
     * 可修补数据偏移量（与 ap_trampoline.S 匹配）：
     *   0x00-0x03: PML4 物理地址（4 字节）
     *   0x04-0x0B: C 入口点地址（8 字节）
     *   0x0C-0x0D: GDT 界限（2 字节）
     *   0x0E-0x15: GDT 基地址（8 字节） */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfa, 0x31,
    0xc0, 0x8e, 0xd8, 0x8e, 0xc0, 0xe4, 0x92, 0x0c, 0x02, 0xe6, 0x92, 0x66,
    0xa1, 0x00, 0x80, 0x0f, 0x22, 0xd8, 0x0f, 0x20, 0xe0, 0x66, 0x83, 0xc8,
    0x20, 0x0f, 0x22, 0xe0, 0x66, 0xb9, 0x80, 0x00, 0x00, 0xc0, 0x0f, 0x32,
    0x66, 0x0d, 0x00, 0x01, 0x00, 0x00, 0x0f, 0x30, 0x0f, 0x01, 0x16, 0x0c,
    0x80, 0x0f, 0x20, 0xc0, 0x66, 0x0d, 0x01, 0x00, 0x00, 0x80, 0x0f, 0x22,
    0xc0, 0xea, 0x5a, 0x80, 0x08, 0x00, 0x66, 0xb8, 0x10, 0x00, 0x8e, 0xd8,
    0x8e, 0xc0, 0x8e, 0xe0, 0x8e, 0xe8, 0x8e, 0xd0, 0xbc, 0x00, 0x00, 0x09,
    0x00, 0x48, 0x8b, 0x04, 0x25, 0x04, 0x80, 0x00, 0x00, 0xff, 0xd0, 0xfa,
    0xf4, 0xeb, 0xfc
};

static const size_t ap_trampoline_size = sizeof(ap_trampoline_code);

/* ========================================================================
 * 全局状态
 * ======================================================================== */
static cpu_info_t  cpu_infos[SMP_MAX_CPUS];
cpu_local_t cpu_locals[SMP_MAX_CPUS];
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
    lock->saved_flags = 0;
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
     * 保存标志到锁中以便释放时正确恢复。 */
    __asm__ volatile ("" : : : "memory");  /* 编译器屏障 */
    lock->saved_flags = flags;
}

void spinlock_release(spinlock_t *lock)
{
    /* 推进服务计数器 */
    __atomic_add_fetch(&lock->serving, 1, __ATOMIC_RELEASE);

    /* 恢复获取锁时保存的中断标志（而非无条件启用中断） */
    hal_restore_interrupts(lock->saved_flags);
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
 * 延时循环（基于 PIT 定时器的毫秒延时）
 * ======================================================================== */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void delay_ms(unsigned int ms)
{
    /* 简单忙等待：在 QEMU 中端口 0x80 写入约 1us。
     * 不精确但足够用于 AP 启动延时。 */
    for (unsigned int i = 0; i < ms * 2000; i++) {
        hal_outb(0x80, 0);
    }
}

/* ========================================================================
 * AP 启动序列
 * ======================================================================== */

static void start_aps(void)
{
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

        /* 步骤 2：等待 10ms（QEMU 中减少为 1ms 以加速启动） */
        delay_ms(1);

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
        int timeout = 10000000; /* 约 1 秒（忙等待） */
        while (!cpu_locals[i].online && timeout > 0) {
            __asm__ volatile ("pause");
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

    /* 为本 AP 分配 Per-CPU syscall 暂存区。
     * 每个核心需要独立暂存区，因为 syscall 入口通过 swapgs + gs: 访问。
     * 布局与 BSP 相同：gs:0=用户RSP, gs:8=内核RSP, gs:16=PML4, gs:24=内核CR3 */
    local->syscall_cpu_area = alloc_page();
    if (local->syscall_cpu_area) {
        memset(local->syscall_cpu_area, 0, PAGE_SIZE);
        uint64_t *cpu_area = (uint64_t *)local->syscall_cpu_area;
        cpu_area[3] = hal_read_cr3();   /* 偏移 24 = 内核 CR3 */
        hal_write_msr(MSR_IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)local->syscall_cpu_area);
        printf("[SMP] AP %u syscall_cpu_area at %p\n", my_apic_id, local->syscall_cpu_area);
    } else {
        printf("[SMP] WARNING: AP %u failed to allocate syscall_cpu_area\n", my_apic_id);
    }

    /* 初始化本 AP 的 GDT 和 TSS */
    gdt_init_cpu(local->index);
    gdt_load_cpu(local->index);

    /* 加载内核 IDT（AP 的 IDTR 仍指向实模式 IVT，
     * 不重新加载会导致中断时三重故障！） */
    idt_reload();

    /* 设置 cpu_local 中的 TSS 指针和选择子 */
    local->tss = &cpu_gdts[local->index].tss;
    local->tss_selector = GDT_TSS_SEL;

    /* 初始化本 AP 的 LAPIC 定时器 */
    lapic_timer_init(APIC_VECTOR_TIMER, TIMER_HZ);

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
    smp_enumerate_cpus_only();
    smp_start_aps();
}

/* 仅枚举 CPU（可在 process_init 之前调用） */
void smp_enumerate_cpus_only(void)
{
    /* 缓存 LAPIC 基地址用于 ICR 访问 */
    lapic_base = apic_get_lapic_base();

    printf("[SMP] Initializing SMP subsystem...\n");

    /* 从 MADT 枚举 CPU */
    smp_enumerate_cpus();

    /* 初始化 BSP 的 cpu_locals 并设置 GS 基地址。
     * 这必须在 process_init() 和 syscall_init() 之前完成，
     * 因为它们使用 this_cpu() 访问 Per-CPU 数据。 */
    uint32_t bsp_apic_id = apic_get_lapic_id();
    memset(&cpu_locals[0], 0, sizeof(cpu_local_t));
    cpu_locals[0].cpu_id = bsp_apic_id;
    cpu_locals[0].index = 0;
    cpu_locals[0].online = 1;
    cpu_locals[0].bsp = 1;
    cpu_locals[0].current_process = NULL;
    cpu_locals[0].need_reschedule = 0;
    spinlock_init(&cpu_locals[0].runqueue_lock);

    /* 设置 BSP 的 GS 基地址 */
    hal_write_msr(MSR_IA32_GS_BASE, (uint64_t)&cpu_locals[0]);
    printf("[SMP] BSP GS base set to %p\n", (void *)&cpu_locals[0]);

    printf("[SMP] Enumeration complete: %d CPU(s) found\n", cpu_count);
}

/* 启动 AP（必须在 process_init 之后调用） */
void smp_start_aps(void)
{
    if (lapic_base == 0) {
        printf("[SMP] ERROR: LAPIC base not available, cannot start APs\n");
        return;
    }

    /* 初始化 AP 的 cpu_locals（BSP 已在 smp_enumerate_cpus_only() 中初始化） */
    uint32_t bsp_apic_id = apic_get_lapic_id();

    for (int i = 0; i < cpu_count; i++) {
        /* 跳过 BSP——已在 smp_enumerate_cpus_only() 中初始化 */
        if (cpu_infos[i].apic_id == bsp_apic_id) {
            /* 更新 BSP 的 TSS 指针（此时 GDT 已初始化） */
            cpu_locals[i].tss = &cpu_gdts[0].tss;
            cpu_locals[i].tss_selector = GDT_TSS_SEL;
            continue;
        }

        cpu_locals[i].cpu_id = cpu_infos[i].apic_id;
        cpu_locals[i].index = (uint32_t)i;
        cpu_locals[i].online = 0;
        cpu_locals[i].bsp = 0;
        cpu_locals[i].current_process = NULL;
        cpu_locals[i].kernel_rsp = 0;
        cpu_locals[i].need_reschedule = 0;
        cpu_locals[i].tss = NULL;
        cpu_locals[i].syscall_cpu_area = NULL;
        cpu_locals[i].lapic_timer_count = 0;
        cpu_locals[i].runqueue_head = NULL;
        cpu_locals[i].runqueue_tail = NULL;
        cpu_locals[i].runqueue_count = 0;
        spinlock_init(&cpu_locals[i].runqueue_lock);

        /* 为 AP 分配内核栈 */
        void *stack = alloc_pages(KERNEL_STACK_PAGES);
        if (!stack) {
            printf("[SMP] ERROR: Failed to allocate stack for AP %u\n",
                   cpu_infos[i].apic_id);
            continue;
        }
        memset(stack, 0, KERNEL_STACK_PAGES * PAGE_SIZE);
        cpu_locals[i].kernel_stack = stack;
    }

    /* 启动 AP */
    if (cpu_count > 1) {
        start_aps();
    }

    /* 为 BSP 初始化 LAPIC 定时器（取代 PIT 周期中断） */
    lapic_timer_init(APIC_VECTOR_TIMER, TIMER_HZ);

    /* LAPIC 定时器接管后，禁用 PIT 周期中断避免双重中断 */
    timer_disable_pit();

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

/* ========================================================================
 * TLB 刷新（多核安全）
 * ======================================================================== */

void smp_flush_tlb_all(void)
{
    /* 先刷新本地 TLB */
    hal_flush_tlb();

    /* 如果有多核在线，向其他 CPU 广播 TLB 刷新 IPI */
    if (cpu_count > 1) {
        smp_broadcast_ipi(APIC_VECTOR_TLB_SHOOTDOWN);
    }
}
