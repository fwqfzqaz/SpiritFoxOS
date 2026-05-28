; SpiritFoxOS - Multiboot2 Header and 32-bit Entry Point
; GRUB loads kernel at 1MB in 32-bit protected mode, we switch to long mode

bits 32

section .multiboot2_header align(8)

mb2_header_start:
    dd 0xE85250D6              ; magic
    dd 0                       ; architecture: 0 = i386
    dd mb2_header_end - mb2_header_start  ; header length
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start)) ; checksum

    ; Framebuffer tag (type=5, request graphical mode)
    dw 5                       ; type = MULTIBOOT_HEADER_TAG_FRAMEBUFFER
    dw 0                       ; flags
    dd 24                      ; size (padded to 8-byte alignment)
    dd 1024                    ; width
    dd 768                     ; height
    dd 32                      ; bpp (32-bit true color)
    dd 0                       ; padding to 8-byte boundary

    ; End tag
    dw 0                       ; type
    dw 0                       ; flags
    dd 8                       ; size
mb2_header_end:


section .bss align(4096)

; Temporary page tables for switching to long mode
pml4: resb 4096
pdpt: resb 4096
pd0:  resb 4096
pd1:  resb 4096
pd2:  resb 4096
pd3:  resb 4096

; Stack for 32-bit mode
stack32: resb 16384
stack32_top:

section .data

; Saved Multiboot2 values (in .data, not .bss, to avoid zeroing issues)
saved_magic: dd 0
saved_info:  dd 0


section .text

global _start
extern kernel_main

_start:
    ; Save Multiboot2 magic and info pointer to fixed memory locations
    ; EAX = 0x36D76289 (Multiboot2 magic), EBX = info structure address
    mov [saved_magic], eax
    mov [saved_info], ebx

    ; Set up 32-bit stack
    mov esp, stack32_top

    ; Check for CPUID support
    call check_cpuid
    ; Check for long mode support
    call check_long_mode

    ; Set up page tables
    call setup_page_tables

    ; Enable PAE
    call enable_paging

    ; Load 64-bit GDT
    lgdt [gdt64.pointer]

    ; Far jump to 64-bit code segment
    jmp gdt64.code_seg:long_mode_entry


check_cpuid:
    ; Flip ID bit in EFLAGS
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    hlt


check_long_mode:
    ; Check if extended CPUID is available
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    ; Check long mode bit
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    hlt


setup_page_tables:
    ; Map PML4[0] -> PDPT (identity mapping low 4GB)
    mov eax, pdpt
    or  eax, 0x03              ; Present + Writable
    mov [pml4], eax

    ; Map PML4[256] -> PDPT (higher-half kernel at 0xFFFFFFFF80000000)
    mov [pml4 + 256 * 8], eax

    ; Map PDPT[0] -> PD0 (first 1GB: 0x00000000 - 0x3FFFFFFF)
    mov eax, pd0
    or  eax, 0x03
    mov [pdpt], eax

    ; Map PDPT[1] -> PD1 (second 1GB: 0x40000000 - 0x7FFFFFFF)
    mov eax, pd1
    or  eax, 0x03
    mov [pdpt + 8], eax

    ; Map PDPT[2] -> PD2 (third 1GB: 0x80000000 - 0xBFFFFFFF)
    mov eax, pd2
    or  eax, 0x03
    mov [pdpt + 16], eax

    ; Map PDPT[3] -> PD3 (fourth 1GB: 0xC0000000 - 0xFFFFFFFF)
    mov eax, pd3
    or  eax, 0x03
    mov [pdpt + 24], eax

    ; Fill PD0 with 512 x 2MB pages (maps 0 - 1GB)
    mov ecx, 0
    mov eax, 0x83              ; Present + Writable + Huge (2MB page)
.fill_pd0:
    mov [pd0 + ecx * 8], eax
    add eax, 0x200000          ; Next 2MB
    inc ecx
    cmp ecx, 512
    jne .fill_pd0

    ; Fill PD1 with 512 x 2MB pages (maps 1GB - 2GB)
    mov ecx, 0
    mov eax, 0x40000083        ; Start at 1GB
.fill_pd1:
    mov [pd1 + ecx * 8], eax
    add eax, 0x200000
    inc ecx
    cmp ecx, 512
    jne .fill_pd1

    ; Fill PD2 with 512 x 2MB pages (maps 2GB - 3GB)
    mov ecx, 0
    mov eax, 0x80000083        ; Start at 2GB
.fill_pd2:
    mov [pd2 + ecx * 8], eax
    add eax, 0x200000
    inc ecx
    cmp ecx, 512
    jne .fill_pd2

    ; Fill PD3 with 512 x 2MB pages (maps 3GB - 4GB)
    mov ecx, 0
    mov eax, 0xC0000083        ; Start at 3GB
.fill_pd3:
    mov [pd3 + ecx * 8], eax
    add eax, 0x200000
    inc ecx
    cmp ecx, 512
    jne .fill_pd3

    ret


enable_paging:
    ; Load PML4 into CR3
    mov eax, pml4
    mov cr3, eax

    ; Enable PAE (CR4 bit 5)
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    ; Enable long mode (IA32_EFER.LME = 1, MSR 0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8
    wrmsr

    ; Enable paging (CR0 bit 31)
    mov eax, cr0
    or  eax, 1 << 31
    mov cr0, eax

    ret


section .rodata align(8)

; GDT for 64-bit mode
gdt64:
    dq 0                                    ; Null descriptor
.code_seg: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; Code segment: Execute/Read, Code/Data, Present, Long mode
.data_seg: equ $ - gdt64
    dq (1<<44) | (1<<47) | (1<<41)           ; Data segment: Read/Write, Code/Data, Present
.pointer:
    dw $ - gdt64 - 1                         ; Limit
    dq gdt64                                 ; Base address


bits 64

section .text

long_mode_entry:
    ; Load 64-bit data segments
    mov ax, gdt64.data_seg
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up 64-bit stack (use lower area of 32-bit stack since it's in low memory)
    mov rsp, stack32_top

    ; Retrieve Multiboot2 magic and info from the stack
    ; They were pushed in 32-bit mode: push ebx, push eax
    ; Stack layout: [eax(magic)] [ebx(info)] <- stack32_top points above
    ; But we reset RSP to stack32_top, so we need to adjust
    ; Actually, the pushes happened before the stack was used for other things,
    ; so the values might be lost. Let's use the BSS variables instead.

    ; Call kernel main with multiboot2 magic and info
    mov edi, [rel saved_magic]
    mov esi, [rel saved_info]
    call kernel_main

    ; Halt if kernel returns
.halt:
    cli
    hlt
    jmp .halt
