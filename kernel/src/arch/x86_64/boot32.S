; boot32.S - 32-bit multiboot boot stub for SpiritFoxOS
; GRUB loads this in 32-bit protected mode, then we switch to 64-bit
; After switching to long mode, we jump to the 64-bit kernel at 0x102000

[bits 32]

; Multiboot1 header
MB1_MAGIC  equ 0x1BADB002
MB1_FLAGS  equ 0x00000003   ; align + meminfo
MB1_CHECK  equ -(MB1_MAGIC + MB1_FLAGS)

section .multiboot align=4
dd MB1_MAGIC
dd MB1_FLAGS
dd MB1_CHECK

section .text align=16
global _start

_start:
    ; GRUB puts multiboot info in EBX, magic in EAX
    ; Save them on stack
    push ebx
    push eax

    ; Set up stack (use a fixed address in low memory that won't conflict)
    mov esp, 0x90000

    ; Set up page tables at 0x70000 (below our stack)
    ; P4 at 0x70000, P3 at 0x71000, P2 at 0x72000

    ; Zero out page tables (3 pages = 12288 dwords)
    mov edi, 0x70000
    xor eax, eax
    mov ecx, 3072
    rep stosd

    ; P4[0] -> P3
    mov dword [0x70000], 0x71003   ; present + writable
    ; P3[0] -> P2
    mov dword [0x71000], 0x72003   ; present + writable

    ; P2: map first 512 entries as 2MB pages (1GB identity mapped)
    mov ecx, 0
    mov edi, 0x72000
.map_p2:
    mov eax, ecx
    shl eax, 21           ; * 2MB
    or eax, 0x83          ; present + writable + huge (2MB)
    mov [edi], eax
    mov dword [edi+4], 0
    add edi, 8
    inc ecx
    cmp ecx, 512
    jne .map_p2

    ; Load P4 into CR3
    mov eax, 0x70000
    mov cr3, eax

    ; Enable PAE in CR4
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Enable long mode in EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; Enable paging (and keep protection enabled)
    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)
    mov cr0, eax

    ; Load 64-bit GDT
    lgdt [gdt64_pointer]

    ; Far jump to 64-bit code segment
    jmp 0x08:long_mode_entry

[bits 64]
long_mode_entry:
    ; Set up 64-bit segment registers
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up 64-bit stack
    mov rsp, 0x90000

    ; Pass BootInfo pointer in rdi (System V ABI)
    lea rdi, [rel boot_info_data]

    ; Jump to 64-bit kernel entry at 0x102000
    mov rax, 0x102000
    jmp rax

    ; Should never reach here
.halt:
    cli
    hlt
    jmp .halt

section .rodata align=16
gdt64:
    dq 0                                    ; null
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)  ; code seg (0x08)
    dq (1<<44) | (1<<47) | (1<<41)             ; data seg (0x10)
gdt64_pointer:
    dw $ - gdt64 - 1       ; limit
    dd gdt64               ; base (lower 32 bits)
    dd 0                   ; base (upper 32 bits, always 0 for low addresses)

section .data align=16
; BootInfo struct - must match C struct layout (with proper alignment)
boot_info_data:
    dd 0x5F1F0F05           ; magic (uint32_t)
    dd 0                    ; padding for 8-byte alignment
    dq 0xE0000000           ; framebuffer_base (uint64_t) - QEMU std VGA LFB
    dq 1024 * 768 * 4       ; framebuffer_size (uint64_t)
    dd 1024                 ; width (uint32_t)
    dd 768                  ; height (uint32_t)
    dd 4096                 ; pitch (uint32_t) - 1024 * 4
    dd 32                   ; bpp (uint32_t)
    dq 0                    ; memory_map (uint64_t) - null
    dq 0                    ; memory_map_size (uint64_t)
    dq 0                    ; memory_map_descriptor_size (uint64_t)
    dq 0                    ; memory_map_entry_count (uint64_t)
