; SpiritFoxOS - Multiboot2引导与长模式切换
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

; SpiritFoxOS - Multiboot2头与32位入口点
; GRUB在32位保护模式下将内核加载到1MB处，我们切换到长模式

bits 32

section .multiboot2_header progbits align=8

mb2_header_start:
    dd 0xE85250D6              ; magic
    dd 0                       ; architecture: 0 = i386
    dd mb2_header_end - mb2_header_start  ; header length
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start)) ; checksum

    ; 帧缓冲区标签（type=5，请求图形模式）
    dw 5                       ; type = MULTIBOOT_HEADER_TAG_FRAMEBUFFER
    dw 0                       ; 标志位
    dd 24                      ; 大小（填充到8字节对齐）
    dd 1024                    ; 宽度
    dd 768                     ; 高度
    dd 32                      ; 色深（32位真彩色）
    dd 0                       ; 填充到8字节边界

    ; 结束标签
    dw 0                       ; 类型
    dw 0                       ; 标志位
    dd 8                       ; 大小
mb2_header_end:


section .bss nobits align=4096

; 用于切换到长模式的临时页表
pml4: resb 4096
pdpt: resb 4096
pd0:  resb 4096
pd1:  resb 4096
pd2:  resb 4096
pd3:  resb 4096

; 32位模式栈
stack32: resb 16384
stack32_top:

section .data

; 保存的Multiboot2值（放在.data而非.bss中，以避免清零问题）
saved_magic: dd 0
saved_info:  dd 0


section .text

global _start
extern kernel_main

_start:
    ; 保存Multiboot2魔数和信息指针到固定内存位置
    ; EAX = 0x36D76289 (Multiboot2魔数), EBX = 信息结构体地址
    mov [saved_magic], eax
    mov [saved_info], ebx

    ; 设置32位栈
    mov esp, stack32_top

    ; 检查CPUID支持
    call check_cpuid
    ; 检查长模式支持
    call check_long_mode

    ; 设置页表
    call setup_page_tables

    ; 启用PAE（物理地址扩展）
    call enable_paging

    ; 加载64位GDT（全局描述符表）
    lgdt [gdt64.pointer]

    ; 远跳转到64位代码段
    jmp gdt64.code_seg:long_mode_entry


check_cpuid:
    ; 翻转EFLAGS中的ID位
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
    ; 检查扩展CPUID是否可用
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    ; 检查长模式位
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret
.no_long_mode:
    hlt


setup_page_tables:
    ; 映射 PML4[0] -> PDPT（低4GB恒等映射）
    mov eax, pdpt
    or  eax, 0x03              ; 存在 + 可写
    mov [pml4], eax

    ; 映射 PML4[256] -> PDPT（高半部分内核位于 0xFFFFFFFF80000000）
    mov [pml4 + 256 * 8], eax

    ; 映射 PDPT[0] -> PD0（第一个1GB: 0x00000000 - 0x3FFFFFFF）
    mov eax, pd0
    or  eax, 0x03
    mov [pdpt], eax

    ; 映射 PDPT[1] -> PD1（第二个1GB: 0x40000000 - 0x7FFFFFFF）
    mov eax, pd1
    or  eax, 0x03
    mov [pdpt + 8], eax

    ; 映射 PDPT[2] -> PD2（第三个1GB: 0x80000000 - 0xBFFFFFFF）
    mov eax, pd2
    or  eax, 0x03
    mov [pdpt + 16], eax

    ; 映射 PDPT[3] -> PD3（第四个1GB: 0xC0000000 - 0xFFFFFFFF）
    mov eax, pd3
    or  eax, 0x03
    mov [pdpt + 24], eax

    ; 用512个2MB页面填充PD0（映射 0 - 1GB）
    mov ecx, 0
    mov eax, 0x83              ; 存在 + 可写 + 巨页（2MB页面）
.fill_pd0:
    mov [pd0 + ecx * 8], eax
    add eax, 0x200000          ; 下一个2MB
    inc ecx
    cmp ecx, 512
    jne .fill_pd0

    ; 用512个2MB页面填充PD1（映射 1GB - 2GB）
    mov ecx, 0
    mov eax, 0x40000083        ; 从1GB开始
.fill_pd1:
    mov [pd1 + ecx * 8], eax
    add eax, 0x200000
    inc ecx
    cmp ecx, 512
    jne .fill_pd1

    ; 用512个2MB页面填充PD2（映射 2GB - 3GB）
    mov ecx, 0
    mov eax, 0x80000083        ; 从2GB开始
.fill_pd2:
    mov [pd2 + ecx * 8], eax
    add eax, 0x200000
    inc ecx
    cmp ecx, 512
    jne .fill_pd2

    ; 用512个2MB页面填充PD3（映射 3GB - 4GB）
    mov ecx, 0
    mov eax, 0xC0000083        ; 从3GB开始
.fill_pd3:
    mov [pd3 + ecx * 8], eax
    add eax, 0x200000
    inc ecx
    cmp ecx, 512
    jne .fill_pd3

    ret


enable_paging:
    ; 将PML4加载到CR3
    mov eax, pml4
    mov cr3, eax

    ; 启用PAE（CR4第5位）
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    ; 启用长模式（IA32_EFER.LME = 1，MSR 0xC0000080）
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8
    wrmsr

    ; 启用分页（CR0第31位）
    mov eax, cr0
    or  eax, 1 << 31
    mov cr0, eax

    ret


section .rodata progbits align=8

; 64位模式GDT（全局描述符表）
gdt64:
    dq 0                                    ; 空描述符
.code_seg: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; 代码段：可执行/可读，代码/数据，存在，长模式
.data_seg: equ $ - gdt64
    dq (1<<44) | (1<<47) | (1<<41)           ; 数据段：可读/可写，代码/数据，存在
.pointer:
    dw $ - gdt64 - 1                         ; 段限长
    dq gdt64                                 ; 基地址


bits 64

section .text

long_mode_entry:
    ; 加载64位数据段
    mov ax, gdt64.data_seg
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; 设置64位栈（使用32位栈的低区域，因为它位于低内存中）
    mov rsp, stack32_top

    ; 从栈中获取Multiboot2魔数和信息
    ; 它们在32位模式下被压入：push ebx, push eax
    ; 栈布局：[eax(魔数)] [ebx(信息)] <- stack32_top指向其上方
    ; 但我们将RSP重置为stack32_top，所以需要调整
    ; 实际上，这些压入操作在栈被用于其他用途之前就发生了，
    ; 所以值可能已丢失。我们改用BSS变量。

    ; 调用kernel_main，传入multiboot2魔数和信息
    mov edi, [rel saved_magic]
    mov esi, [rel saved_info]
    call kernel_main

    ; 如果内核返回则停机
.halt:
    cli
    hlt
    jmp .halt
