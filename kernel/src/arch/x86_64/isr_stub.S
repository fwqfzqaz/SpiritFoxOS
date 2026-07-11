; isr_stub.S - x86_64 SpiritFoxOS 的 ISR/IRQ 存根
; NASM 语法

[bits 64]

; 外部 C 处理程序
extern isr_handler
extern irq_handler
extern spurious_handler

; ========================================
; 宏：通用 ISR 存根（无错误码）
; ========================================
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0              ; 压入虚拟错误码
    push %1             ; 压入中断号
    jmp isr_common
%endmacro

; ========================================
; 宏：带错误码的 ISR 存根
; ========================================
%macro ISR_ERR 1
global isr%1
isr%1:
    ; 错误码已由 CPU 压入
    push %1             ; 压入中断号
    jmp isr_common
%endmacro

; ========================================
; 宏：IRQ 存根
; ========================================
%macro IRQ_STUB 2
global irq%1
irq%1:
    push 0              ; 压入虚拟错误码
    push %2             ; 压入中断号（IRQ + 32）
    jmp irq_common
%endmacro

; ========================================
; CPU 异常 ISR 0-31
; ========================================
ISR_NOERR 0     ; 除以零
ISR_NOERR 1     ; 调试
ISR_NOERR 2     ; 不可屏蔽中断
ISR_NOERR 3     ; 断点
ISR_NOERR 4     ; 溢出
ISR_NOERR 5     ; 边界范围超出
ISR_NOERR 6     ; 无效操作码
ISR_NOERR 7     ; 设备不可用
ISR_ERR   8     ; 双重故障（错误码）
ISR_NOERR 9     ; 协处理器段越界
ISR_ERR   10    ; 无效 TSS（错误码）
ISR_ERR   11    ; 段不存在（错误码）
ISR_ERR   12    ; 栈段故障（错误码）
ISR_ERR   13    ; 一般保护故障（错误码）
ISR_ERR   14    ; 缺页异常（错误码）
ISR_NOERR 15    ; 保留
ISR_NOERR 16    ; x87 FPU 错误
ISR_ERR   17    ; 对齐检查（错误码）
ISR_NOERR 18    ; 机器检查
ISR_NOERR 19    ; SIMD 浮点异常
ISR_NOERR 20    ; 虚拟化
ISR_NOERR 21    ; 保留
ISR_NOERR 22    ; 保留
ISR_NOERR 23    ; 保留
ISR_NOERR 24    ; 保留
ISR_NOERR 25    ; 保留
ISR_NOERR 26    ; 保留
ISR_NOERR 27    ; 保留
ISR_NOERR 28    ; 保留
ISR_NOERR 29    ; 保留
ISR_ERR   30    ; 安全异常（错误码）
ISR_NOERR 31    ; 保留

; ========================================
; IRQ 存根（IRQ0 - IRQ15 映射到 INT 32-47）
; ========================================
IRQ_STUB 0,  32    ; 定时器（通过 IOAPIC 的 PIT）
IRQ_STUB 1,  33    ; 键盘（通过 IOAPIC 的 IRQ1）
IRQ_STUB 2,  34    ; 级联
IRQ_STUB 3,  35    ; COM2
IRQ_STUB 4,  36    ; COM1
IRQ_STUB 5,  37    ; LPT2
IRQ_STUB 6,  38    ; 软驱
IRQ_STUB 7,  39    ; LPT1
IRQ_STUB 8,  40    ; RTC
IRQ_STUB 9,  41    ; 空闲
IRQ_STUB 10, 42    ; 空闲
IRQ_STUB 11, 43    ; 空闲
IRQ_STUB 12, 44    ; PS/2 鼠标
IRQ_STUB 13, 45    ; FPU
IRQ_STUB 14, 46    ; 主 ATA
IRQ_STUB 15, 47    ; 从 ATA

; ========================================
; 伪中断存根（向量 255）
; ========================================
global irq_spurious
irq_spurious:
    push 0              ; 虚拟错误码
    push 255            ; 伪中断向量号
    jmp spurious_common

; ========================================
; ISR 公共处理程序
; ========================================
isr_common:
    ; 调试：向 COM1 输出 'E' 以确认异常到达
    mov dx, 0x3F9
    in al, dx
    test al, 0x20
    jz .skip_exc_debug
    mov dx, 0x3F8
    mov al, 'E'
    out dx, al
.skip_exc_debug:

    ; 保存所有通用寄存器
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

    ; 保存数据段
    mov ax, ds
    push rax

    ; 加载内核数据段
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 传递中断号和错误码作为参数
    mov rdi, [rsp+128]   ; 中断号
    mov rsi, [rsp+136]   ; 错误码
    cld
    call isr_handler

    ; 恢复数据段
    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 恢复通用寄存器
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

    ; 移除中断号和错误码
    add rsp, 16
    iretq

; ========================================
; IRQ 公共处理程序
; ========================================
irq_common:
    ; 保存所有通用寄存器
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

    ; 保存数据段
    mov ax, ds
    push rax

    ; 加载内核数据段
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 传递中断号和错误码作为参数
    mov rdi, [rsp+128]   ; 中断号
    mov rsi, [rsp+136]   ; 错误码
    cld
    call irq_handler

    ; 恢复数据段
    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 恢复通用寄存器
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

    ; 移除中断号和错误码
    add rsp, 16
    iretq

; ========================================
; 伪中断公共处理程序
; ========================================
spurious_common:
    ; 保存所有通用寄存器
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

    ; 保存数据段
    mov ax, ds
    push rax

    ; 加载内核数据段
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 传递中断号和错误码作为参数
    mov rdi, [rsp+128]   ; 中断号
    mov rsi, [rsp+136]   ; 错误码
    cld
    call spurious_handler

    ; 恢复数据段
    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 恢复通用寄存器
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

    ; 移除中断号和错误码
    add rsp, 16
    iretq

; ========================================
; 传统键盘中断 ISR 入口点（保留用于兼容）
; ========================================
global keyboard_isr
keyboard_isr:
    push 0              ; 虚拟错误码
    push 33             ; IRQ1 的中断号
    jmp irq_common

; ========================================
; 系统调用入口点（syscall/sysret）
; ========================================
global syscall_entry
extern syscall_handler_c

syscall_entry:
    ; syscall 指令将 RIP 保存到 RCX，RFLAGS 保存到 R11
    ; 需要通过 TSS.rsp0 切换到内核栈
    swapgs                          ; 加载内核 GS 基地址
    mov gs:0, rsp                   ; 保存用户 RSP（使用 GS:0 处的暂存空间）
    mov rsp, gs:8                   ; 从 GS:8 加载内核 RSP

    ; 压入与 trap_frame_t 布局匹配的陷阱帧：
    ;   r15, r14, r13, r12, r11, r10, r9, r8,
    ;   rdi, rsi, rdx, rcx, rbx, rbp, rax,
    ;   int_num, error_code,
    ;   rip, cs, rflags, rsp, ss

    ; 首先压入 SS, RSP, RFLAGS, CS, RIP（iretq 帧）
    ; syscall 提供给我们：RCX=用户_rip，R11=用户_rflags
    ; 用户 RSP 已保存到 gs:0
    push 0x23                       ; SS = 用户数据选择子（0x20 | 3 = 0x23）
    push qword [gs:0]               ; 用户 RSP
    push r11                        ; RFLAGS（由 syscall 保存在 r11 中）
    push 0x1B                       ; CS = 用户代码选择子（0x18 | 3 = 0x1B）
    push rcx                        ; 用户 RIP（由 syscall 保存在 rcx 中）

    ; 压入错误码和中断号
    push 0                          ; 虚拟错误码
    push 0x80                       ; 系统调用向量号

    ; 按 trap_frame_t 顺序压入通用寄存器
    push rax                        ; 偏移 112
    push rbp                        ; 偏移 104
    push rbx                        ; 偏移 96
    push rcx                        ; 偏移 88
    push rdx                        ; 偏移 80
    push rsi                        ; 偏移 72
    push rdi                        ; 偏移 64
    push r8                         ; 偏移 56
    push r9                         ; 偏移 48
    push r10                        ; 偏移 40
    push r11                        ; 偏移 32
    push r12                        ; 偏移 24
    push r13                        ; 偏移 16
    push r14                        ; 偏移 8
    push r15                        ; 偏移 0

    ; 加载内核数据段
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 调用 C 处理程序，传入陷阱帧指针
    mov rdi, rsp
    cld
    call syscall_handler_c

    ; 返回路径 - 恢复并 iretq
    ; 内核数据段已设置，无需恢复

    ; 按 trap_frame_t 顺序恢复通用寄存器
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

    ; 移除中断号和错误码
    add rsp, 16

    ; 设置 GS 以便 swapgs 并恢复用户 RSP
    swapgs                          ; 恢复用户 GS

    ; iretq 将恢复 RIP, CS, RFLAGS, RSP, SS
    iretq

; ========================================
; 上下文切换函数（纯汇编）
;
; void arch_switch_to(uint64_t *old_rsp_ptr, uint64_t new_rsp);
;
; 保存被调用者保存的寄存器，切换 RSP，恢复寄存器，
; 然后返回。首次切换到新的内核线程时，
; 新栈必须包含：
;   [栈顶] arg        <- 弹出到 rsi
;   entry            <- 弹出到 rdi
;   kthread_trampoline <- 返回地址（由 ret 弹出）
;   rbp (0)
;   rbx (0)
;   r12 (0)
;   r13 (0)
;   r14 (0)
;   r15 (0)
;   <- kernel_rsp 指向此处
; ========================================
global arch_switch_to
arch_switch_to:
    ; 保存被调用者保存的寄存器
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; 保存旧 RSP
    mov rax, rdi            ; rax = old_rsp_ptr
    mov [rax], rsp          ; *old_rsp_ptr = 当前 RSP

    ; 加载新 RSP
    mov rsp, rsi            ; rsp = new_rsp

    ; 从新栈恢复被调用者保存的寄存器
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; 返回 - 将从新栈弹出返回地址
    ret

; ========================================
; 内核线程跳板（纯汇编）
;
; arch_switch_to 返回后（通过 ret），执行到达此处。
; 栈中包含：entry, arg（在线程创建时压入）。
; 将它们弹出到 rdi/rsi 并调用 C 封装函数 kthread_entry。
; ========================================
extern kthread_entry

global kthread_trampoline_asm
kthread_trampoline_asm:
    pop rdi             ; 入口函数指针
    pop rsi             ; 参数
    call kthread_entry  ; kthread_entry(entry, arg)
    ; 不应返回，但以防万一
.halt:
    cli
    hlt
    jmp .halt
