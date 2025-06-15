[BITS 16]
switch_to_pm:
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SEG:init_pm

[BITS 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    lidt [idtr]
    sti

    call main
    jmp $

; GDT 定义
gdt_start:
null_descriptor:
    dd 0x0
    dd 0x0

code_descriptor:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10011010b
    db 11001111b
    db 0x0

data_descriptor:
    dw 0xffff
    dw 0x0
    db 0x0
    db 10010010b
    db 11001111b
    db 0x0

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start
    ; IDT 定义（简单初始化为 0）
idt:
    times 256 dq 0

idtr:
    dw 256 * 8 - 1
    dd idt

; 在 init_pm 中启用 IDT
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, 0x90000
    mov esp, ebp

    lidt [idtr]         ; 加载中断描述符表
    sti                 ; 开启中断

    call main
    jmp $