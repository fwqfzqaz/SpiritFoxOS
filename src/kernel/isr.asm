; SpiritFoxOS - Interrupt Service Routines
; Common ISR/IRQ stubs that save context and call C handler

bits 64

default rel   ; Use RIP-relative addressing for PIC compatibility

section .text

extern interrupt_handler

; Macro for CPU exception handlers (some push error code, some don't)
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0                 ; Push dummy error code
    push %1                ; Push interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    ; Error code already pushed by CPU
    push %1                ; Push interrupt number
    jmp isr_common
%endmacro

; CPU Exception handlers (0-31)
ISR_NOERR 0    ; #DE Divide by Zero
ISR_NOERR 1    ; #DB Debug
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; #BP Breakpoint
ISR_NOERR 4    ; #OF Overflow
ISR_NOERR 5    ; #BR BOUND Range Exceeded
ISR_NOERR 6    ; #UD Invalid Opcode
ISR_NOERR 7    ; #NM Device Not Available
ISR_ERR   8    ; #DF Double Fault
ISR_NOERR 9    ; Coprocessor Segment Overrun
ISR_ERR   10   ; #TS Invalid TSS
ISR_ERR   11   ; #NP Segment Not Present
ISR_ERR   12   ; #SS Stack Segment Fault
ISR_ERR   13   ; #GP General Protection Fault
ISR_ERR   14   ; #PF Page Fault
ISR_NOERR 15   ; Reserved
ISR_NOERR 16   ; #MF x87 FPU Error
ISR_ERR   17   ; #AC Alignment Check
ISR_NOERR 18   ; #MC Machine Check
ISR_NOERR 19   ; #XM SIMD Floating-Point
ISR_NOERR 20   ; #VE Virtualization Exception
ISR_NOERR 21   ; #CP Control Protection
ISR_NOERR 22   ; Reserved
ISR_NOERR 23   ; Reserved
ISR_NOERR 24   ; Reserved
ISR_NOERR 25   ; Reserved
ISR_NOERR 26   ; Reserved
ISR_NOERR 27   ; Reserved
ISR_NOERR 28   ; Reserved
ISR_NOERR 29   ; #VC VMM Communication
ISR_ERR   30   ; #HX Security Exception
ISR_NOERR 31   ; Reserved

; IRQ handlers (32-47)
ISR_NOERR 32   ; IRQ0  - PIT Timer
ISR_NOERR 33   ; IRQ1  - Keyboard
ISR_NOERR 34   ; IRQ2  - Cascade
ISR_NOERR 35   ; IRQ3  - COM2
ISR_NOERR 36   ; IRQ4  - COM1
ISR_NOERR 37   ; IRQ5  - LPT2
ISR_NOERR 38   ; IRQ6  - Floppy
ISR_NOERR 39   ; IRQ7  - LPT1
ISR_NOERR 40   ; IRQ8  - RTC
ISR_NOERR 41   ; IRQ9  - Free
ISR_NOERR 42   ; IRQ10 - Free
ISR_NOERR 43   ; IRQ11 - Free
ISR_NOERR 44   ; IRQ12 - Mouse
ISR_NOERR 45   ; IRQ13 - FPU
ISR_NOERR 46   ; IRQ14 - Primary ATA
ISR_NOERR 47   ; IRQ15 - Secondary ATA

; Common ISR handler
isr_common:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Save data segment
    mov ax, ds
    push rax

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Pass pointer to interrupt frame as argument
    mov rdi, rsp

    ; Call C handler (RIP-relative for PIC)
    lea rax, [rel interrupt_handler]
    call rax

    ; Restore data segment
    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Restore general-purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove interrupt number and error code
    add rsp, 16

    ; Return from interrupt
    iretq

; ISR table for IDT initialization
section .data
global isr_table
isr_table:
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    dq isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
    dq isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
