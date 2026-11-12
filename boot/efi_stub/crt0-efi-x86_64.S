.text
.global _start
.global EfiMain
_start:
EfiMain:
 andq $-16,%rsp
 subq $32,%rsp
 call efi_main
 cli
1: hlt
 jmp 1b
/* 低内存交接跳板：被复制到 1 GiB 以下的 EfiLoaderCode 页后执行。
 * 真实固件可能把 EFI 镜像装在 4 GiB 以上，而内核页表只直映 0..4 GiB；
 * 切换 CR3 后原镜像地址不可见，因此 CR3 与栈的切换必须在低内存完成。
 * ms_abi 参数：rcx=入口，rdx=BootInfo，r8=CR3，r9=栈顶。 */
.global efi_handoff_low_start
.global efi_handoff_low_end
efi_handoff_low_start:
 cli
 mov %r8,%cr3
 mov %rdx,%rdi
 mov %r9,%rsp
 jmp *%rcx
efi_handoff_low_end:
.data
.global _tls_index
_tls_index: .long 0
