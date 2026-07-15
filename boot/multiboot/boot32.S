# boot32.S - SpiritFoxOS 长模式跳板
# Multiboot 入口点，设置长模式并跳转到 64 位内核

.section .text

.global _start

#define COM1 0x3F8

# Multiboot 入口点
# GRUB 传递: eax=0x2BADB002, ebx=multiboot_info 指针
_start:
    # 设置一个小栈（在 0x9000 的 BootInfo 下方）
    movl $0x8000, %esp

    # 为 C 函数压入参数: _start_c(magic, mbi)
    pushl %ebx          # mbi 指针（第二个参数）
    pushl %eax          # magic（第一个参数）
    call _start_c

    # _start_c 在 %eax 中返回 p4_table 物理地址
    # （实际上是 noreturn，但以防万一）
.halt:
    cli
    hlt
    jmp .halt


# ---- 启用长模式并跳转到 64 位内核 ----
# 从 C 调用: enable_long_mode(uint32_t p4_table_addr)
# cdecl: 参数在栈上 4(%esp) 处

.global enable_long_mode

# 64 位 GDT
.align 16
gdt64_start:
    .quad 0x0000000000000000              # 0x00: 空描述符
    .quad 0x00AF9A000000FFFF              # 0x08: 64 位代码段 (L=1, D=0)
    .quad 0x00CF92000000FFFF              # 0x10: 数据段
gdt64_end:

.align 4
gdt64_ptr:
    .word gdt64_end - gdt64_start - 1     # 段限
    .long gdt64_start                     # 基地址（32 位，恒等映射）

enable_long_mode:
    # 从栈中获取 p4_table 地址（cdecl 第一个参数）
    # 保存在 %ebx 中，因为串口输出会破坏 %eax/%al
    movl 4(%esp), %ebx

    # 调试: 'P' - 开始
    # 重要: 读取 COM1+5 (0x3FD) 的线路状态寄存器以检查发送就绪
    # 读取 COM1 (0x3F8) 是数据寄存器，不是状态寄存器！
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'P', %al
    outb %al, %dx

    # 步骤 1: 将 P4 加载到 CR3（使用保存的 %ebx，而非被破坏的 %eax）
    movl %ebx, %cr3

    # 调试: '3' - CR3 已加载
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'3', %al
    outb %al, %dx

    # 步骤 2: 在 CR4 中启用 PAE
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    # 调试: 'A' - PAE 已启用
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'A', %al
    outb %al, %dx

    # 步骤 3: 在 EFER MSR 中启用长模式
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    # 调试: 'E' - EFER.LME 已设置
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'E', %al
    outb %al, %dx

    # 步骤 4: 加载 64 位 GDT（在启用分页之前，仍处于 32 位模式）
    lgdt gdt64_ptr

    # 调试: 'G' - GDT 已加载
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'G', %al
    outb %al, %dx

    # 步骤 5: 在 CR0 中启用分页 - 关键时刻
    # 此后 CPU 处于长模式的兼容子模式
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0

    # 调试: 'S' - 分页已启用，仍处于兼容模式
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'S', %al
    outb %al, %dx

    # 步骤 6: 远跳转到 64 位代码段以进入真正的 64 位模式
    # 此指令在兼容模式（32 位解码）下执行，因此 ljmpl 可用
    ljmpl $0x08, $lm_entry

lm_entry:
    # 现在处于 64 位长模式！
    # 从这里开始的所有指令都在 64 位模式上下文中解码
    # 但它们是作为 32 位代码汇编的，所以必须注意
    # 寻址方式（不使用绝对内存引用，使用寄存器间接寻址）

    # 调试: '6' - 成功进入 64 位模式！
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'6', %al
    outb %al, %dx

    # 为 64 位模式设置数据段
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # 在 2MB 处设置栈（在恒等映射区域内）
    movl $0x200000, %esp

    # 在 edi 中传递 BootInfo 指针 (0x9000)（System V ABI 第一个参数）
    movl $0x9000, %edi

    # 调试: 'J' - 即将跳转到内核
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'J', %al
    outb %al, %dx

    # 跳转到 0x100000 处的 64 位内核
    # 使用寄存器间接跳转以避免 32 位与 64 位寻址不匹配
    # （jmpl *kernel_addr 使用 abs32 寻址，在 64 位模式下会变为 RIP 相对寻址）
    movl $0x100000, %eax
    jmp *%eax

    # 不应到达此处
.halt2:
    cli
    hlt
    jmp .halt2
