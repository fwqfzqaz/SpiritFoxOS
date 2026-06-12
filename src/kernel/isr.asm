; SpiritFoxOS - 中断服务例程汇编存根
; Copyright (C) 2025 SpiritFoxOS Contributors
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program.  If not, see <http://www.gnu.org/licenses/>.

; SpiritFoxOS - 中断服务例程
; 通用ISR/IRQ存根，保存上下文并调用C语言处理函数

bits 64

default rel   ; 使用RIP相对寻址以兼容PIC

section .text

extern interrupt_handler

; CPU异常处理宏（部分会压入错误码，部分不会）
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0                 ; 压入虚拟错误码
    push %1                ; 压入中断号
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    ; CPU已压入错误码
    push %1                ; 压入中断号
    jmp isr_common
%endmacro

; CPU异常处理函数 (0-31)
ISR_NOERR 0    ; #DE 除零错误
ISR_NOERR 1    ; #DB 调试异常
ISR_NOERR 2    ; NMI 不可屏蔽中断
ISR_NOERR 3    ; #BP 断点陷阱
ISR_NOERR 4    ; #OF 溢出
ISR_NOERR 5    ; #BR BOUND范围越界
ISR_NOERR 6    ; #UD 无效操作码
ISR_NOERR 7    ; #NM 设备不可用（x87/FPU）
ISR_ERR   8    ; #DF 双重故障
ISR_NOERR 9    ; 协处理器段越界
ISR_ERR   10   ; #TS 无效TSS
ISR_ERR   11   ; #NP 段不存在
ISR_ERR   12   ; #SS 栈段错误
ISR_ERR   13   ; #GP 一般保护故障
ISR_ERR   14   ; #PF 页错误
ISR_NOERR 15   ; 保留
ISR_NOERR 16   ; #MF x87 FPU浮点错误
ISR_ERR   17   ; #AC 对齐检查
ISR_NOERR 18   ; #MC 机器检查
ISR_NOERR 19   ; #XM SIMD浮点异常
ISR_NOERR 20   ; #VE 虚拟化异常
ISR_NOERR 21   ; #CP 控制保护异常
ISR_NOERR 22   ; 保留
ISR_NOERR 23   ; 保留
ISR_NOERR 24   ; 保留
ISR_NOERR 25   ; 保留
ISR_NOERR 26   ; 保留
ISR_NOERR 27   ; 保留
ISR_NOERR 28   ; 保留
ISR_NOERR 29   ; #VC VMM通信
ISR_ERR   30   ; #HX 安全异常
ISR_NOERR 31   ; 保留

; IRQ中断处理函数 (32-47)
ISR_NOERR 32   ; IRQ0  - PIT定时器
ISR_NOERR 33   ; IRQ1  - 键盘
ISR_NOERR 34   ; IRQ2  - 级联（从片连接）
ISR_NOERR 35   ; IRQ3  - COM2串口
ISR_NOERR 36   ; IRQ4  - COM1串口
ISR_NOERR 37   ; IRQ5  - LPT2并口
ISR_NOERR 38   ; IRQ6  - 软盘
ISR_NOERR 39   ; IRQ7  - LPT1并口
ISR_NOERR 40   ; IRQ8  - RTC实时时钟
ISR_NOERR 41   ; IRQ9  - 空闲/重定向
ISR_NOERR 42   ; IRQ10 - 空闲
ISR_NOERR 43   ; IRQ11 - 空闲
ISR_NOERR 44   ; IRQ12 - 鼠标
ISR_NOERR 45   ; IRQ13 - FPU/Coprocessor
ISR_NOERR 46   ; IRQ14 - 主ATA通道
ISR_NOERR 47   ; IRQ15 - 从ATA通道

; 通用ISR处理函数
isr_common:
    ; 保存所有通用寄存器
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

    ; 保存数据段
    mov ax, ds
    push rax

    ; 加载内核数据段
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 将中断帧指针作为参数传递
    mov rdi, rsp

    ; 调用C语言处理函数（使用RIP相对寻址以兼容PIC）
    lea rax, [rel interrupt_handler]
    call rax

    ; 恢复数据段
    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 恢复所有通用寄存器
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

    ; 弹出中断号和错误码
    add rsp, 16

    ; 中断返回
    iretq

; 用于IDT初始化的ISR表
section .data
global isr_table
isr_table:
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    dq isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
    dq isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
