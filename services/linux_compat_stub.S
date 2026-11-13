/* Linux 兼容启动 stub（S2）：注册兼容描述符、构造最小 Linux 栈、跳转 e_entry。
 * 数据页首 8 字节由 loader 写入绝对入口地址。仅使用 write/exit 两个翻译。
 * Frame 偏移：rdi=64 rsi=72 rdx=96 rax=112。 */
.section .text
.global _sf_linux_stub_start
_sf_linux_stub_start:
	lea	_sf_linux_stub_start(%rip),%rbx
	/* frame = mmap(0, 4096) */
	xor	%edi,%edi
	mov	$4096,%esi
	xor	%edx,%edx
	mov	$35,%eax
	int	$0x80
	test	%rax,%rax
	js	.Lstub_fail
	mov	%rax,%r12
	/* SYS_COMPAT_SET(handler, frame, 0) */
	lea	_sf_compat_handler(%rip),%rdi
	mov	%r12,%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x80
	test	%rax,%rax
	jnz	.Lstub_fail
	/* 最小初始栈：AT_NULL、envp NULL、argv NULL、argc=0；保持 16 字节对齐。 */
	mov	%rsp,%rax
	and	$-8,%rax
	mov	%rax,%rsp
	xor	%eax,%eax
	push	%rax
	push	%rax
	push	%rax
	push	%rax
	push	%rax
	mov	4096(%rbx),%rax
	jmp	*%rax
.Lstub_fail:
	mov	$1,%edi
	xor	%eax,%eax
	int	$0x80
	ud2
/* int 0x80 上行调用入口：rdi 指向帧页。 */
_sf_compat_handler:
	mov	112(%rdi),%rax
	cmp	$1,%rax
	je	.Lwrite
	cmp	$60,%rax
	je	.Lexit
	cmp	$231,%rax
	je	.Lexit
	mov	$-38,%rdi
	xor	%esi,%esi
	mov	$42,%eax
	int	$0x81
	ud2
.Lwrite:
	mov	64(%rdi),%r8
	mov	72(%rdi),%r9
	mov	96(%rdi),%r10
	cmp	$2,%r8
	ja	.Lbad
	cmp	$512,%r10
	jbe	1f
	mov	$512,%r10
1:	mov	%r9,%rdi
	mov	%r10,%rsi
	xor	%edx,%edx
	mov	$2,%eax
	int	$0x81
	mov	%rax,%rdi
	xor	%esi,%esi
	mov	$42,%eax
	int	$0x81
	ud2
.Lbad:
	mov	$-9,%rdi
	xor	%esi,%esi
	mov	$42,%eax
	int	$0x81
	ud2
.Lexit:
	mov	64(%rdi),%rdi
	xor	%eax,%eax
	int	$0x81
	ud2
.section .data
.space 4096
