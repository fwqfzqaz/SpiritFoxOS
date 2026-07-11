#include "apic.h"
#include "acpi.h"
#include "hal.h"
#include "vga.h"

static uintptr_t lapic_base = 0;
static uintptr_t ioapic_base = 0;
static uint32_t  ioapic_gsi_base = 0;
static int       apic_available = 0;

/* 重定向表从 IOAPIC 寄存器 0x10 开始 */
#define IOAPIC_REG_REDIRECTION 0x10

/* ========================================================================
 * 本地 APIC 寄存器访问（MMIO）
 * ======================================================================== */

static void lapic_write(uint32_t offset, uint32_t value)
{
    hal_mmio_write32(lapic_base + offset, value);
}

static uint32_t lapic_read(uint32_t offset)
{
    return hal_mmio_read32(lapic_base + offset);
}

/* ========================================================================
 * IOAPIC 寄存器访问（MMIO，间接方式）
 * ======================================================================== */

static void ioapic_write(uint32_t reg, uint32_t value)
{
    hal_mmio_write32(ioapic_base + IOAPIC_IOREGSEL, reg);
    hal_mmio_write32(ioapic_base + IOAPIC_IOWIN, value);
}

static uint32_t ioapic_read(uint32_t reg)
{
    hal_mmio_write32(ioapic_base + IOAPIC_IOREGSEL, reg);
    return hal_mmio_read32(ioapic_base + IOAPIC_IOWIN);
}

/* ========================================================================
 * IOAPIC 重定向条目辅助函数
 * ======================================================================== */

static void ioapic_set_redirection(uint8_t gsi, uint64_t entry)
{
    ioapic_write(IOAPIC_REG_REDIRECTION + gsi * 2, (uint32_t)(entry & 0xFFFFFFFF));
    ioapic_write(IOAPIC_REG_REDIRECTION + gsi * 2 + 1, (uint32_t)(entry >> 32));
}

static uint64_t ioapic_get_redirection(uint8_t gsi)
{
    uint32_t low  = ioapic_read(IOAPIC_REG_REDIRECTION + gsi * 2);
    uint32_t high = ioapic_read(IOAPIC_REG_REDIRECTION + gsi * 2 + 1);
    return ((uint64_t)high << 32) | low;
}

/* ========================================================================
 * 将 ISA IRQ 映射到 GSI（考虑 ACPI 覆盖）
 * ======================================================================== */

static uint32_t irq_to_gsi(uint8_t isa_irq)
{
    uint32_t gsi;
    uint16_t flags;
    if (acpi_get_irq_override(isa_irq, &gsi, &flags) == 0) {
        return gsi;
    }
    /* 无覆盖：直接映射 */
    return (uint32_t)isa_irq;
}

/* ========================================================================
 * 公共 API
 * ======================================================================== */

void apic_eoi(void)
{
    if (apic_available) {
        lapic_write(LAPIC_EOI, 0);
    }
}

uint32_t apic_get_lapic_id(void)
{
    if (!apic_available) return 0;
    return (lapic_read(LAPIC_ID) >> 24) & 0xFF;
}

uintptr_t apic_get_lapic_base(void)
{
    return lapic_base;
}

void apic_enable_irq(uint8_t isa_irq, uint8_t vector)
{
    if (!apic_available || !ioapic_base) return;

    uint32_t gsi = irq_to_gsi(isa_irq);

    /* 从 ACPI 覆盖确定极性和触发模式 */
    uint16_t override_flags = 0;
    uint32_t tmp_gsi;
    acpi_get_irq_override(isa_irq, &tmp_gsi, &override_flags);

    uint64_t entry = (uint64_t)vector;                    /* 向量号 */
    entry |= IOAPIC_DELMOD_FIXED;                         /* 固定投递模式 */
    entry |= IOAPIC_DESTMOD_PHYS;                         /* 物理目标模式 */

    /* 极性：标志位第 1 位（0 = 高电平有效，1 = 低电平有效） */
    if (override_flags & 0x02) {
        entry |= IOAPIC_POLARITY_LOW;
    }

    /* 触发模式：标志位第 0 位（0 = 边沿触发，1 = 电平触发） */
    if (override_flags & 0x01) {
        entry |= IOAPIC_TRIGGER_LEVEL;
    }

    /* 目标：BSP APIC ID */
    uint32_t dest_id = apic_get_lapic_id();
    entry |= ((uint64_t)dest_id << 56);

    ioapic_set_redirection(gsi - ioapic_gsi_base, entry);
}

void apic_disable_irq(uint8_t isa_irq)
{
    if (!apic_available || !ioapic_base) return;

    uint32_t gsi = irq_to_gsi(isa_irq);
    uint64_t entry = ioapic_get_redirection(gsi - ioapic_gsi_base);
    entry |= IOAPIC_MASK;
    ioapic_set_redirection(gsi - ioapic_gsi_base, entry);
}

void apic_init(void)
{
    /* 步骤 1：从 MSR 获取 LAPIC 基地址 */
    uint64_t msr_val = hal_read_msr(MSR_IA32_APIC_BASE);
    lapic_base = (uintptr_t)(msr_val & APIC_BASE_ADDR_MASK);

    /* 首先尝试从 ACPI 获取 LAPIC 地址 */
    uintptr_t acpi_lapic = acpi_get_lapic_addr();
    if (acpi_lapic != 0) {
        lapic_base = acpi_lapic;
    }

    if (lapic_base == 0) {
        printf("[APIC] ERROR: No LAPIC address found!\n");
        return;
    }

    printf("[APIC] LAPIC base: %p\n", (void*)lapic_base);

    /* 步骤 2：映射 LAPIC MMIO 区域 */
    hal_ensure_mapped(lapic_base, 0x1000);

    /* 步骤 3：通过 MSR 全局启用 LAPIC */
    if (!(msr_val & APIC_BASE_GLOBAL_ENABLE)) {
        msr_val |= APIC_BASE_GLOBAL_ENABLE;
        hal_write_msr(MSR_IA32_APIC_BASE, msr_val);
    }

    /* 步骤 4：设置伪中断向量并启用 LAPIC */
    lapic_write(LAPIC_SVR, APIC_VECTOR_SPURIOUS | LAPIC_SVR_ENABLE);

    /* 步骤 5：清除错误状态寄存器 */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* 步骤 6：初始屏蔽所有 LVT 条目 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);

    /* 步骤 7：设置任务优先级为 0（接受所有中断） */
    lapic_write(LAPIC_TPR, 0);

    apic_available = 1;

    printf("[APIC] LAPIC ID: %u, Version: 0x%x\n",
           apic_get_lapic_id(), lapic_read(LAPIC_VERSION));

    /* 步骤 8：初始化 IOAPIC */
    ioapic_base = acpi_get_ioapic_addr();
    ioapic_gsi_base = acpi_get_ioapic_gsi_base();

    if (ioapic_base == 0) {
        printf("[APIC] WARNING: No IOAPIC found, using LAPIC only\n");
        return;
    }

    /* 映射 IOAPIC MMIO 区域 */
    hal_ensure_mapped(ioapic_base, 0x1000);

    uint32_t ioapic_ver = ioapic_read(IOAPIC_REG_VER);
    int max_redir = (ioapic_ver >> 16) & 0xFF;
    printf("[APIC] IOAPIC base: %p, Version: 0x%x, Max redirections: %d\n",
           (void*)ioapic_base, ioapic_ver & 0xFF, max_redir);

    /* 步骤 9：屏蔽所有 IOAPIC 重定向条目 */
    for (int i = 0; i < max_redir; i++) {
        uint64_t entry = ioapic_get_redirection(i);
        entry |= IOAPIC_MASK;
        ioapic_set_redirection(i, entry);
    }

    /* 步骤 10：屏蔽所有 8259 PIC 中断 */
    hal_outb(0x21, 0xFF);  /* 屏蔽所有主 PIC 中断 */
    hal_outb(0xA1, 0xFF);  /* 屏蔽所有从 PIC 中断 */

    /* 步骤 11：通过 IOAPIC 路由关键 IRQ。
     * 注意：我们先设置键盘 IRQ 的路由并保持屏蔽位，
     * 然后在 PS/2 键盘控制器完全重新初始化后取消屏蔽。
     * 这样可以防止 UEFI 固件残留的 IRQ1 边沿被 IOAPIC 锁存。 */
    apic_enable_irq(0, APIC_VECTOR_TIMER);      /* PIT 定时器 -> GSI 2 -> 向量 32 */
    /* 键盘：先设置路由但保持屏蔽 */
    {
        uint32_t gsi = irq_to_gsi(1);
        uint16_t override_flags = 0;
        uint32_t tmp_gsi;
        acpi_get_irq_override(1, &tmp_gsi, &override_flags);
        uint64_t entry = (uint64_t)APIC_VECTOR_KEYBOARD;
        entry |= IOAPIC_DELMOD_FIXED;
        entry |= IOAPIC_DESTMOD_PHYS;
        if (override_flags & 0x02) entry |= IOAPIC_POLARITY_LOW;
        if (override_flags & 0x01) entry |= IOAPIC_TRIGGER_LEVEL;
        entry |= ((uint64_t)apic_get_lapic_id() << 56);
        entry |= IOAPIC_MASK;  /* 保持屏蔽直到 PS/2 初始化完成 */
        ioapic_set_redirection(gsi - ioapic_gsi_base, entry);
    }
    apic_enable_irq(4, APIC_VECTOR_COM1);       /* COM1 -> 向量 36 */
    apic_enable_irq(12, APIC_VECTOR_MOUSE);     /* PS/2 鼠标 -> 向量 44 */
    apic_enable_irq(14, APIC_VECTOR_ATA_PRI);   /* 主 ATA -> 向量 46 */
    apic_enable_irq(15, APIC_VECTOR_ATA_SEC);   /* 从 ATA -> 向量 47 */

    /* 重新初始化 PS/2 键盘控制器为中断模式。
     * UEFI 可能将键盘留在以下状态：
     * - PS/2 控制器有待处理的输出数据
     * - 键盘扫描被禁用
     * 必须在取消 IOAPIC 中键盘 IRQ 的屏蔽之前
     * 清除所有待处理的数据并启用扫描。
     */
    {
        int timeout;

        /* 步骤 A：清除 PS/2 控制器中所有待处理的数据 */
        for (timeout = 0; timeout < 100; timeout++) {
            if (hal_inb(0x64) & 0x01) {
                hal_inb(0x60); /* 丢弃 */
                hal_io_wait();
            } else {
                break;
            }
        }

        /* 步骤 B：确保 PS/2 CCR 中键盘 IRQ 已启用 */
        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x02))
            hal_io_wait();
        hal_outb(0x64, 0x20);
        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x01) == 0)
            hal_io_wait();
        uint8_t cmd_byte = hal_inb(0x60);

        cmd_byte |= 0x01;  /* 启用键盘中断（IRQ1） */
        cmd_byte |= 0x40;  /* 将扫描码集 2 转换为集 1 */
        cmd_byte &= ~0x10; /* 禁用键盘锁定 */

        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x02))
            hal_io_wait();
        hal_outb(0x64, 0x60);
        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x02))
            hal_io_wait();
        hal_outb(0x60, cmd_byte);

        /* 步骤 C：启用键盘扫描（0xF4）。
         * 应答（0xFA）通过轮询消费，而不是中断，
         * 因为 IOAPIC 键盘条目仍处于屏蔽状态。 */
        timeout = 10000;
        while (timeout-- > 0 && (hal_inb(0x64) & 0x02))
            hal_io_wait();
        hal_outb(0x60, 0xF4);

        timeout = 10000;
        while (timeout-- > 0) {
            if (hal_inb(0x64) & 0x01) {
                hal_inb(0x60); /* 消费应答 */
                break;
            }
            hal_io_wait();
        }

        /* 步骤 D：清除所有剩余数据（例如第二个应答字节） */
        hal_io_wait();
        for (timeout = 0; timeout < 100; timeout++) {
            if (hal_inb(0x64) & 0x01) {
                hal_inb(0x60);
                hal_io_wait();
            } else {
                break;
            }
        }

        /* 步骤 E：现在取消 IOAPIC 中键盘 IRQ 的屏蔽。
         * 此时 PS/2 控制器已清洁，IRQ1 信号线已撤销，
         * 因此不会有残留的边沿。 */
        {
            uint32_t gsi = irq_to_gsi(1);
            uint64_t entry = ioapic_get_redirection(gsi - ioapic_gsi_base);
            entry &= ~IOAPIC_MASK;
            ioapic_set_redirection(gsi - ioapic_gsi_base, entry);
        }
    }

    printf("[APIC] Initialized: PIC disabled, IOAPIC routing active\n");
}
