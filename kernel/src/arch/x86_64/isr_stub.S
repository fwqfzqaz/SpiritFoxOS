; isr_stub.S - ISR/IRQ stubs for x86_64 SpiritFoxOS
; NASM syntax

[bits 64]

; External C handlers
extern isr_handler
extern irq_handler
extern spurious_handler

; ========================================
; Macro: Generic ISR stub (no error code)
; ========================================
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0              ; push dummy error code
    push %1             ; push interrupt number
    jmp isr_common
%endmacro

; ========================================
; Macro: ISR stub with error code
; ========================================
%macro ISR_ERR 1
global isr%1
isr%1:
    ; error code already pushed by CPU
    push %1             ; push interrupt number
    jmp isr_common
%endmacro

; ========================================
; Macro: IRQ stub
; ========================================
%macro IRQ_STUB 2
global irq%1
irq%1:
    push 0              ; push dummy error code
    push %2             ; push interrupt number (IRQ + 32)
    jmp irq_common
%endmacro

; ========================================
; CPU Exception ISR 0-31
; ========================================
ISR_NOERR 0     ; Divide by zero
ISR_NOERR 1     ; Debug
ISR_NOERR 2     ; NMI
ISR_NOERR 3     ; Breakpoint
ISR_NOERR 4     ; Overflow
ISR_NOERR 5     ; Bound range exceeded
ISR_NOERR 6     ; Invalid opcode
ISR_NOERR 7     ; Device not available
ISR_ERR   8     ; Double fault (error code)
ISR_NOERR 9     ; Coprocessor segment overrun
ISR_ERR   10    ; Invalid TSS (error code)
ISR_ERR   11    ; Segment not present (error code)
ISR_ERR   12    ; Stack segment fault (error code)
ISR_ERR   13    ; General protection fault (error code)
ISR_ERR   14    ; Page fault (error code)
ISR_NOERR 15    ; Reserved
ISR_NOERR 16    ; x87 FPU error
ISR_ERR   17    ; Alignment check (error code)
ISR_NOERR 18    ; Machine check
ISR_NOERR 19    ; SIMD floating point
ISR_NOERR 20    ; Virtualization
ISR_NOERR 21    ; Reserved
ISR_NOERR 22    ; Reserved
ISR_NOERR 23    ; Reserved
ISR_NOERR 24    ; Reserved
ISR_NOERR 25    ; Reserved
ISR_NOERR 26    ; Reserved
ISR_NOERR 27    ; Reserved
ISR_NOERR 28    ; Reserved
ISR_NOERR 29    ; Reserved
ISR_ERR   30    ; Security (error code)
ISR_NOERR 31    ; Reserved

; ========================================
; IRQ stubs (IRQ0 - IRQ15 mapped to INT 32-47)
; ========================================
IRQ_STUB 0,  32    ; Timer (PIT via IOAPIC)
IRQ_STUB 1,  33    ; Keyboard (IRQ1 via IOAPIC)
IRQ_STUB 2,  34    ; Cascade
IRQ_STUB 3,  35    ; COM2
IRQ_STUB 4,  36    ; COM1
IRQ_STUB 5,  37    ; LPT2
IRQ_STUB 6,  38    ; Floppy
IRQ_STUB 7,  39    ; LPT1
IRQ_STUB 8,  40    ; RTC
IRQ_STUB 9,  41    ; Free
IRQ_STUB 10, 42    ; Free
IRQ_STUB 11, 43    ; Free
IRQ_STUB 12, 44    ; PS/2 Mouse
IRQ_STUB 13, 45    ; FPU
IRQ_STUB 14, 46    ; Primary ATA
IRQ_STUB 15, 47    ; Secondary ATA

; ========================================
; Spurious interrupt stub (vector 255)
; ========================================
global irq_spurious
irq_spurious:
    push 0              ; dummy error code
    push 255            ; spurious vector number
    jmp spurious_common

; ========================================
; Common ISR handler
; ========================================
isr_common:
    ; Debug: output 'E' to COM1 to confirm exception reached
    mov dx, 0x3F9
    in al, dx
    test al, 0x20
    jz .skip_exc_debug
    mov dx, 0x3F8
    mov al, 'E'
    out dx, al
.skip_exc_debug:

    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
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

    ; Pass interrupt number and error code as arguments
    mov rdi, [rsp+128]   ; int_num
    mov rsi, [rsp+136]   ; error_code
    cld
    call isr_handler

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
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove interrupt number and error code
    add rsp, 16
    iretq

; ========================================
; Common IRQ handler
; ========================================
irq_common:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
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

    ; Pass interrupt number and error code as arguments
    mov rdi, [rsp+128]   ; int_num
    mov rsi, [rsp+136]   ; error_code
    cld
    call irq_handler

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
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove interrupt number and error code
    add rsp, 16
    iretq

; ========================================
; Common spurious handler
; ========================================
spurious_common:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
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

    ; Pass interrupt number and error code as arguments
    mov rdi, [rsp+128]   ; int_num
    mov rsi, [rsp+136]   ; error_code
    cld
    call spurious_handler

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
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Remove interrupt number and error code
    add rsp, 16
    iretq

; ========================================
; Legacy keyboard ISR entry point (kept for compatibility)
; ========================================
global keyboard_isr
keyboard_isr:
    push 0              ; dummy error code
    push 33             ; interrupt number for IRQ1
    jmp irq_common

; ========================================
; Syscall entry point (syscall/sysret)
; ========================================
global syscall_entry
extern syscall_handler_c

syscall_entry:
    ; syscall instruction saves RIP to RCX and RFLAGS to R11
    ; We need to swap to kernel stack via TSS.rsp0
    swapgs                          ; Load kernel GS base
    mov gs:0, rsp                   ; Save user RSP (use scratch space at GS:0)
    mov rsp, gs:8                   ; Load kernel RSP from GS:8

    ; Push trap frame matching trap_frame_t layout:
    ;   r15, r14, r13, r12, r11, r10, r9, r8,
    ;   rdi, rsi, rdx, rcx, rbx, rbp, rax,
    ;   int_num, error_code,
    ;   rip, cs, rflags, rsp, ss

    ; First push SS, RSP, RFLAGS, CS, RIP (iretq frame)
    ; syscall gives us: RCX=user_rip, R11=user_rflags
    ; User RSP was saved to gs:0
    push 0x23                       ; SS = user data selector (0x20 | 3 = 0x23)
    push qword [gs:0]               ; User RSP
    push r11                        ; RFLAGS (saved by syscall in r11)
    push 0x1B                       ; CS = user code selector (0x18 | 3 = 0x1B)
    push rcx                        ; User RIP (saved by syscall in rcx)

    ; Push error code and interrupt number
    push 0                          ; Dummy error code
    push 0x80                       ; Syscall vector number

    ; Push general purpose registers IN trap_frame_t ORDER
    push rax                        ; offset 112
    push rbp                        ; offset 104
    push rbx                        ; offset 96
    push rcx                        ; offset 88
    push rdx                        ; offset 80
    push rsi                        ; offset 72
    push rdi                        ; offset 64
    push r8                         ; offset 56
    push r9                         ; offset 48
    push r10                        ; offset 40
    push r11                        ; offset 32
    push r12                        ; offset 24
    push r13                        ; offset 16
    push r14                        ; offset 8
    push r15                        ; offset 0

    ; Load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Call C handler with pointer to trap frame
    mov rdi, rsp
    cld
    call syscall_handler_c

    ; Return path - restore and iretq
    ; Restore kernel data segment is already set

    ; Restore GPRs in trap_frame_t order
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
    pop rdx
    pop rcx
    pop rbx
    pop rbp
    pop rax

    ; Remove int_num and error_code
    add rsp, 16

    ; Set GS for swapgs and restore user RSP
    swapgs                          ; Restore user GS

    ; iretq will restore RIP, CS, RFLAGS, RSP, SS
    iretq

; ========================================
; Context switch function (pure assembly)
;
; void arch_switch_to(uint64_t *old_rsp_ptr, uint64_t new_rsp);
;
; Saves callee-saved registers, switches RSP, restores registers,
; and returns.  When first switching to a new kernel thread,
; the new stack must contain:
;   [top] arg        <- popped into rsi
;   entry            <- popped into rdi
;   kthread_trampoline <- return address (popped by ret)
;   rbp (0)
;   rbx (0)
;   r12 (0)
;   r13 (0)
;   r14 (0)
;   r15 (0)
;   <- kernel_rsp points here
; ========================================
global arch_switch_to
arch_switch_to:
    ; Save callee-saved registers
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save old RSP
    mov rax, rdi            ; rax = old_rsp_ptr
    mov [rax], rsp          ; *old_rsp_ptr = current RSP

    ; Load new RSP
    mov rsp, rsi            ; rsp = new_rsp

    ; Restore callee-saved registers from new stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Return - will pop return address from new stack
    ret

; ========================================
; Kernel thread trampoline (pure assembly)
;
; After arch_switch_to returns (via ret), execution lands here.
; The stack contains: entry, arg (pushed during thread creation).
; We pop them into rdi/rsi and call the C wrapper kthread_entry.
; ========================================
extern kthread_entry

global kthread_trampoline_asm
kthread_trampoline_asm:
    pop rdi             ; entry function pointer
    pop rsi             ; argument
    call kthread_entry  ; kthread_entry(entry, arg)
    ; Should never return, but just in case
.halt:
    cli
    hlt
    jmp .halt
