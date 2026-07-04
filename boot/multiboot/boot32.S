# boot32.S - Long mode trampoline for SpiritFoxOS
# Entry point for multiboot, sets up long mode and jumps to 64-bit kernel

.section .text

.global _start

#define COM1 0x3F8

# Multiboot entry point
# GRUB passes: eax=0x2BADB002, ebx=multiboot_info pointer
_start:
    # Set up a small stack (below BootInfo at 0x9000)
    movl $0x8000, %esp

    # Push args for C function: _start_c(magic, mbi)
    pushl %ebx          # mbi pointer (second arg)
    pushl %eax          # magic (first arg)
    call _start_c

    # _start_c returns p4_table physical address in %eax
    # (it's actually noreturn, but just in case)
.halt:
    cli
    hlt
    jmp .halt


# ---- Enable long mode and jump to 64-bit kernel ----
# Called from C with: enable_long_mode(uint32_t p4_table_addr)
# cdecl: argument on stack at 4(%esp)

.global enable_long_mode

# 64-bit GDT
.align 16
gdt64_start:
    .quad 0x0000000000000000              # 0x00: null descriptor
    .quad 0x00AF9A000000FFFF              # 0x08: 64-bit code segment (L=1, D=0)
    .quad 0x00CF92000000FFFF              # 0x10: data segment
gdt64_end:

.align 4
gdt64_ptr:
    .word gdt64_end - gdt64_start - 1     # limit
    .long gdt64_start                     # base (32-bit, identity mapped)

enable_long_mode:
    # Get p4_table address from stack (cdecl first arg)
    # Save in %ebx because serial output clobbers %eax/%al
    movl 4(%esp), %ebx

    # Debug: 'P' - starting
    # IMPORTANT: Read Line Status Register at COM1+5 (0x3FD) to check TX ready
    # Reading from COM1 (0x3F8) is the DATA register, not the status register!
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'P', %al
    outb %al, %dx

    # Step 1: Load P4 into CR3 (use saved %ebx, not clobbered %eax)
    movl %ebx, %cr3

    # Debug: '3' - CR3 loaded
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'3', %al
    outb %al, %dx

    # Step 2: Enable PAE in CR4
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    # Debug: 'A' - PAE enabled
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'A', %al
    outb %al, %dx

    # Step 3: Enable long mode in EFER MSR
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    # Debug: 'E' - EFER.LME set
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'E', %al
    outb %al, %dx

    # Step 4: Load 64-bit GDT (before enabling paging, still in 32-bit mode)
    lgdt gdt64_ptr

    # Debug: 'G' - GDT loaded
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'G', %al
    outb %al, %dx

    # Step 5: Enable paging in CR0 - CRITICAL MOMENT
    # After this, CPU is in compatibility sub-mode of long mode
    movl %cr0, %eax
    orl $(1 << 31), %eax
    movl %eax, %cr0

    # Debug: 'S' - paging enabled, still in compat mode
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'S', %al
    outb %al, %dx

    # Step 6: Far jump to 64-bit code segment to enter true 64-bit mode
    # This is executed in compatibility mode (32-bit decoding), so ljmpl works
    ljmpl $0x08, $lm_entry

lm_entry:
    # Now in 64-bit long mode!
    # All instructions from here are decoded in 64-bit mode context
    # but were assembled as 32-bit code, so we must be careful with
    # addressing modes (no absolute memory references, use register-indirect)

    # Debug: '6' - we made it to 64-bit mode!
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'6', %al
    outb %al, %dx

    # Set up data segments for 64-bit mode
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Set up stack at 2MB (well within identity-mapped region)
    movl $0x200000, %esp

    # Pass BootInfo pointer (0x9000) in edi (System V ABI first arg)
    movl $0x9000, %edi

    # Debug: 'J' - about to jump to kernel
    movw $(COM1 + 5), %dx
1:  inb %dx, %al
    test $0x20, %al
    jz 1b
    movw $COM1, %dx
    movb $'J', %al
    outb %al, %dx

    # Jump to the 64-bit kernel at 0x100000
    # Use register-indirect jump to avoid 32-bit vs 64-bit addressing mismatch
    # (jmpl *kernel_addr uses abs32 addressing which becomes RIP-relative in 64-bit mode)
    movl $0x100000, %eax
    jmp *%eax

    # Should never reach here
.halt2:
    cli
    hlt
    jmp .halt2
