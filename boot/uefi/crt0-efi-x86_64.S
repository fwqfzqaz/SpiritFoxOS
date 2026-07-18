/*
 * UEFI bootloader entry point (x86_64)
 * Self-contained - no gnu-efi dependency.
 *
 * UEFI calls us with MS x64 calling convention:
 *   rcx = ImageHandle
 *   rdx = SystemTable
 *
 * efi_main is declared __attribute__((ms_abi)), so GCC generates
 * code expecting args in rcx, rdx.  We just pass them through.
 */
    .text
    .globl _start
_start:
    /* 将栈对齐到 16 字节 */
    andq $-16, %rsp

    /* rcx = ImageHandle, rdx = SystemTable 已经就位 */
    /* efi_main 是 ms_abi，所以它期望 (rcx, rdx) - 直接调用 */
    call efi_main

    /* 如果 efi_main 返回，则停机 */
    cli
.Lhalt:
    hlt
    jmp .Lhalt
