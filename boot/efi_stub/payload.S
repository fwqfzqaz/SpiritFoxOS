/*
 * payload.S - 内嵌内核 ELF payload
 *
 * SpiritFoxOS EFI Stub 采用 Linux bzImage 式的 "stub + payload" 模型：
 *   kernel.efi = [EFI stub] + [kernel.elf]
 *
 * 内嵌的 kernel.elf 是按 0x100000 链接的原始 ELF。stub 运行于 UEFI 加载
 * 地址，解析其 ELF 头，将 PT_LOAD 段拷贝到 p_paddr (0x100000) 并清零 BSS，
 * 再跳转 e_entry —— 因 payload 是原始未重定位字节，落在链接地址即正确。
 *
 * KERNEL_PAYLOAD_PATH 由 Makefile 通过 -D 注入实际路径 (指向 kernel.elf)。
 */
#ifndef KERNEL_PAYLOAD_PATH
#define KERNEL_PAYLOAD_PATH "kernel.elf"
#endif

    .section .rodata
    .balign 16
    .globl __kernel_elf_start
    .globl __kernel_elf_end
__kernel_elf_start:
    .incbin KERNEL_PAYLOAD_PATH
__kernel_elf_end:
