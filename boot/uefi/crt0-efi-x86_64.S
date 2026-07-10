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
    /* Align stack to 16 bytes */
    andq $-16, %rsp

    /* rcx = ImageHandle, rdx = SystemTable already in place */
    /* efi_main is ms_abi, so it expects (rcx, rdx) - just call it */
    call efi_main

    /* If efi_main returns, halt */
    cli
.Lhalt:
    hlt
    jmp .Lhalt
