#include "idt.h"
#include "keyboard.h"
#include "mouse.h"
#include "apic.h"
#include "timer.h"
#include "hal.h"
#include "vga.h"
#include "process.h"
#include "rtl8139.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) IDTEntry;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) IDTPtr;

static IDTEntry idt[IDT_ENTRIES];
static IDTPtr   idt_ptr;

/* ISR 存根声明（在汇编中定义） */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

/* IRQ 存根（向量 32-47） */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

/* 伪中断存根（向量 255） */
extern void irq_spurious(void);

/* 系统调用入口点（在 isr_stub.S 中定义） */
extern void syscall_entry(void);

static const char* exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt",
    "Breakpoint", "Into Detected Overflow", "Out of Bounds",
    "Invalid Opcode", "No Coprocessor", "Double Fault",
    "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault",
    "Unknown Interrupt", "Coprocessor Fault", "Alignment Check",
    "Machine Check", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved"
};

/* ISR 处理程序 - 从汇编调用，用于异常（INT 0-31） */
void isr_handler(uint64_t int_num, uint64_t error_code)
{
    /* 对于缺页异常（向量 14），先尝试 COW 处理。
     * 错误码第 1 位 (W/R) = 1 表示写操作引起的缺页。
     * 错误码第 0 位 (P)   = 1 表示保护错误（页存在但不可写）。
     * 此组合正是 COW 产生的典型情况。 */
    if (int_num == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        /* 写操作缺页且页存在：(error_code & 0x3) == 0x3 */
        if ((error_code & 0x3) == 0x3) {
            if (process_cow_page_fault(cr2) == 0) {
                /* COW 处理成功 - 重试引起缺页的指令 */
                return;
            }
        }
    }

    serial_puts("\n!!! EXCEPTION: ");
    if (int_num < 32) {
        serial_puts(exception_messages[int_num]);
    } else {
        serial_puts("Unknown");
    }
    serial_puts(" [");
    serial_put_hex(int_num);
    serial_puts("] Error code: ");
    serial_put_hex(error_code);

    /* 对于缺页异常，打印 CR2（缺页地址） */
    if (int_num == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        serial_puts(" CR2: ");
        serial_put_hex(cr2);
    }

    serial_puts("\nSystem Halted.\n");

    /* 如果当前进程是用户进程，则终止它而不是停机 */
    process_t *cur = process_current();
    if (cur && !(cur->flags & PROC_FLAG_KERNEL) && cur->pid > 0) {
        serial_puts("Killing user process PID=");
        serial_put_dec((uint64_t)cur->pid);
        serial_puts("\n");
        cur->state = PROC_ZOMBIE;
        cur->exit_code = 128 + (int)int_num;
        /* 不停机 - 让调度器继续运行 */
        hal_enable_interrupts();
        for (;;) { __asm__ volatile ("hlt"); }
    }

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* IRQ 处理程序 - 从汇编调用，用于硬件中断 */
void irq_handler(uint64_t int_num, uint64_t error_code)
{
    (void)error_code;

    switch (int_num) {
    case 32:  /* 定时器（通过 IOAPIC 的 PIT 或 LAPIC 定时器） */
        timer_handler();
        break;
    case 33:  /* 键盘（通过 IOAPIC 的 IRQ1） */
        keyboard_handler();
        break;
    case 44:  /* PS/2 鼠标（通过 IOAPIC 的 IRQ12） */
        mouse_handler();
        break;
    case 43:  /* RTL8139 网卡（通过 IOAPIC 的 IRQ11） */
        rtl8139_irq_handler();
        break;
    default:
        /* 其他 IRQ - 当前无处理程序 */
        break;
    }

    /* 发送 EOI：如果可用则使用 LAPIC，否则回退到 PIC */
    apic_eoi();
    if (int_num >= 40) {
        /* 从 PIC EOI（用于传统兼容） */
        hal_outb(0xA0, 0x20);
    }

    /* 检查定时器滴答后是否需要重新调度 */
    if (int_num == 32 && need_reschedule_check()) {
        scheduler_schedule();
    }
}

/* 伪中断处理程序 */
void spurious_handler(uint64_t int_num, uint64_t error_code)
{
    (void)int_num;
    (void)error_code;
    /* 伪中断不能向 IOAPIC 发送 EOI，
     * 仅当是真正的 APIC 伪中断时才向 LAPIC 发送 */
    /* 对于 LAPIC 伪中断，不需要 EOI */
}

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t type_attr)
{
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].selector    = selector;
    idt[num].ist         = 0;
    idt[num].type_attr   = type_attr;
    idt[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].reserved    = 0;
}

static void idt_flush(uint64_t idt_ptr_addr)
{
    __asm__ volatile ("lidt (%0)" : : "r"(idt_ptr_addr));
}

void idt_init(void)
{
    idt_ptr.limit = sizeof(IDTEntry) * IDT_ENTRIES - 1;
    idt_ptr.base  = (uint64_t)&idt;

    /* 清零 IDT */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low  = 0;
        idt[i].selector    = 0;
        idt[i].ist         = 0;
        idt[i].type_attr   = 0;
        idt[i].offset_mid  = 0;
        idt[i].offset_high = 0;
        idt[i].reserved    = 0;
    }

    /* 重映射 PIC：主 IRQ 0-7 -> INT 32-39，从 IRQ 8-15 -> INT 40-47
     * 即使使用 APIC 也需要这样做，以防止伪 PIC 中断
     * 触发 CPU 异常（INT 8-15）。PIC 稍后会被
     * apic_init() 屏蔽。 */
    hal_outb(0x20, 0x11);  /* ICW1：主片，级联，需要 ICW4 */
    hal_outb(0xA0, 0x11);  /* ICW1：从片，级联，需要 ICW4 */
    hal_outb(0x21, 0x20);  /* ICW2：主片偏移 32 */
    hal_outb(0xA1, 0x28);  /* ICW2：从片偏移 40 */
    hal_outb(0x21, 0x04);  /* ICW3：主片级联 */
    hal_outb(0xA1, 0x02);  /* ICW3：从片级联 */
    hal_outb(0x21, 0x01);  /* ICW4：8086 模式 */
    hal_outb(0xA1, 0x01);  /* ICW4：8086 模式 */

    /* 初始屏蔽所有 PIC IRQ - APIC 将处理中断 */
    hal_outb(0x21, 0xFF);
    hal_outb(0xA1, 0xFF);

    /* 设置异常处理程序（ISR 0-31） */
    idt_set_gate(0,  (uint64_t)isr0,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(1,  (uint64_t)isr1,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(3,  (uint64_t)isr3,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(5,  (uint64_t)isr5,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(7,  (uint64_t)isr7,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(9,  (uint64_t)isr9,  0x08, IDT_TYPE_TRAP);
    idt_set_gate(10, (uint64_t)isr10, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(11, (uint64_t)isr11, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(12, (uint64_t)isr12, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(13, (uint64_t)isr13, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(14, (uint64_t)isr14, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(15, (uint64_t)isr15, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(16, (uint64_t)isr16, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(17, (uint64_t)isr17, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(18, (uint64_t)isr18, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(19, (uint64_t)isr19, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(20, (uint64_t)isr20, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(21, (uint64_t)isr21, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(22, (uint64_t)isr22, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(23, (uint64_t)isr23, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(24, (uint64_t)isr24, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(25, (uint64_t)isr25, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(26, (uint64_t)isr26, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(27, (uint64_t)isr27, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(28, (uint64_t)isr28, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(29, (uint64_t)isr29, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(30, (uint64_t)isr30, 0x08, IDT_TYPE_TRAP);
    idt_set_gate(31, (uint64_t)isr31, 0x08, IDT_TYPE_TRAP);

    /* 设置 IRQ 处理程序（向量 32-47） */
    idt_set_gate(32, (uint64_t)irq0,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(33, (uint64_t)irq1,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(34, (uint64_t)irq2,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(35, (uint64_t)irq3,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(36, (uint64_t)irq4,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(37, (uint64_t)irq5,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(38, (uint64_t)irq6,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(39, (uint64_t)irq7,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(40, (uint64_t)irq8,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(41, (uint64_t)irq9,  0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(42, (uint64_t)irq10, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(43, (uint64_t)irq11, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(44, (uint64_t)irq12, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(45, (uint64_t)irq13, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(46, (uint64_t)irq14, 0x08, IDT_TYPE_INTERRUPT);
    idt_set_gate(47, (uint64_t)irq15, 0x08, IDT_TYPE_INTERRUPT);

    /* 伪中断处理程序（向量 255） */
    idt_set_gate(255, (uint64_t)irq_spurious, 0x08, IDT_TYPE_INTERRUPT);

    /* 加载 IDT - 但还不要启用中断（sti）。
     * 中断将在 APIC 初始化后启用。 */
    idt_flush((uint64_t)&idt_ptr);

    /* 通过 MSR 设置系统调用接口 */
    /* 启用 SYSCALL/SYSRET */
    uint64_t efer = hal_read_msr(MSR_IA32_EFER);
    hal_write_msr(MSR_IA32_EFER, efer | 1);  /* SCE 位 */

    /* 设置 SYSCALL 入口点 */
    hal_write_msr(MSR_IA32_STAR, ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32));
    hal_write_msr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);
    hal_write_msr(MSR_IA32_FMASK, 0x200);  /* 系统调用时清除 IF */

    /* 每 CPU 暂存空间的分配推迟到 syscall_init()
     * 因为 alloc_page() 在 memory_init() 运行前不可用。
     * 目前先将 GS_BASE 设为 0。 */
    hal_write_msr(MSR_IA32_GS_BASE, 0);
    hal_write_msr(MSR_IA32_KERNEL_GS_BASE, 0);
}
