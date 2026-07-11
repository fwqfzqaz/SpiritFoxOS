; AP 引导代码 - 加载到物理地址 0x8000
; 由 AP 在收到 BSP 的 SIPI 后执行
; 必须从 16 位实模式切换到 64 位长模式
;
; 此文件不会编译到内核 ELF 中，而是汇编为原始二进制
; 并作为 C 数组嵌入到 smp.c 中。
; 这样可以避免混合 16/64 位代码的 ELF 重定位问题。

[bits 16]
org 0x8000

; 可修补的数据偏移量：
;   0x8000: PML4 物理地址（4 字节）
;   0x8004: C 入口点地址（8 字节）
;   0x800C: GDT 界限（2 字节）+ GDT 基地址（8 字节）

align 4
ap_pml4:       dd 0          ; +0x00: PML4 物理地址
ap_entry_ptr:  dq 0          ; +0x04: C 入口点地址
ap_gdt_ptr:    dw 0          ; +0x0C: GDT 界限
               dq 0          ; +0x0E: GDT 基地址

_start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax

    ; 启用 A20
    in al, 0x92
    or al, 2
    out 0x92, al

    ; 加载 PML4
    mov eax, [ap_pml4]
    mov cr3, eax

    ; 启用 PAE
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; 启用长模式
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; 加载 GDT
    lgdt [ap_gdt_ptr]

    ; 启用分页
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; 远跳转到 64 位代码
    jmp 0x08:ap_long_mode

[bits 64]
ap_long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, 0x90000

    mov rax, [0x8004]    ; ap_entry_ptr 固定偏移量处
    call rax

.halt:
    cli
    hlt
    jmp .halt
