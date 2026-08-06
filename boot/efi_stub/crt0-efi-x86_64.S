/*
 * SpiritFoxOS EFI Stub 入口 (x86_64)
 * 自包含 - 不依赖 gnu-efi
 *
 * UEFI 以 MS x64 调用约定调用本入口:
 *   rcx = ImageHandle
 *   rdx = SystemTable
 *
 * efi_main 声明为 __attribute__((ms_abi))，GCC 会按 (rcx, rdx) 生成代码，
 * 因此这里直接透传调用。
 */
    .text
    .globl _start
_start:
    /* 栈对齐到 16 字节 */
    andq $-16, %rsp

    /* rcx = ImageHandle, rdx = SystemTable 已就位 */
    call efi_main

    /* efi_main 不应返回；若返回则停机 */
    cli
.Lhalt:
    hlt
    jmp .Lhalt
