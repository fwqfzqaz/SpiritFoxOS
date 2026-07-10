; AP Trampoline - loaded at physical 0x8000
; Executed by APs after receiving SIPI from BSP
; Must switch from 16-bit real mode to 64-bit long mode
;
; This file is NOT compiled into the kernel ELF. Instead, it is
; assembled into a raw binary and embedded as a C array in smp.c.
; This avoids ELF relocation issues with mixed 16/64-bit code.

[bits 16]
org 0x8000

; Patchable data offsets:
;   0x8000: PML4 physical address (4 bytes)
;   0x8004: C entry point address (8 bytes)
;   0x800C: GDT limit (2 bytes) + GDT base (8 bytes)

align 4
ap_pml4:       dd 0          ; +0x00: PML4 physical address
ap_entry_ptr:  dq 0          ; +0x04: C entry point address
ap_gdt_ptr:    dw 0          ; +0x0C: GDT limit
               dq 0          ; +0x0E: GDT base address

_start:
    cli

    xor ax, ax
    mov ds, ax
    mov es, ax

    ; Enable A20
    in al, 0x92
    or al, 2
    out 0x92, al

    ; Load PML4
    mov eax, [ap_pml4]
    mov cr3, eax

    ; Enable PAE
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Enable long mode
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; Load GDT
    lgdt [ap_gdt_ptr]

    ; Enable paging
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; Far jump to 64-bit code
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

    mov rax, [0x8004]    ; ap_entry_ptr at fixed offset
    call rax

.halt:
    cli
    hlt
    jmp .halt
