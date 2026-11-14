/* S3 翻译器入口：建立帧页与翻译器栈、注册兼容描述符、构造最小 Linux 栈，
 * 然后跳转 Linux 入口。数据页首 qword 由 sf-init 写入入口绝对地址。
 * 数据页位于固定偏移 __sf_linux_translator_data（链接脚本 0x10000）。 */
/* 入口必须落在 blob 偏移 0：sf-init 以追加段基址作为 plan.entry，
 * 因此使用独立段并由链接脚本排在最前。 */
.section .text.start,"ax"
.global _sf_linux_translator_start
_sf_linux_translator_start:
	lea	__sf_linux_translator_data(%rip),%rbx
	/* frame = mmap(NULL, 4096, 0, 0) */
	xor	%edi,%edi
	mov	$4096,%esi
	xor	%edx,%edx
	mov	$35,%eax
	int	$0x80
	test	%rax,%rax
	js	.Lfail
	mov	%rax,%r12
	/* translator stack = mmap(NULL, 64 KiB) */
	xor	%edi,%edi
	mov	$65536,%esi
	xor	%edx,%edx
	mov	$35,%eax
	int	$0x80
	test	%rax,%rax
	js	.Lfail
	add	$65536,%rax
	mov	%rax,8(%rbx)
	/* Linux initial stack = mmap(NULL, 64 KiB) */
	xor	%edi,%edi
	mov	$65536,%esi
	xor	%edx,%edx
	mov	$35,%eax
	int	$0x80
	test	%rax,%rax
	js	.Lfail
	add	$65536,%rax
	mov	%rax,%r13
	/* SYS_COMPAT_SET(handler, frame, 0) */
	lea	_sf_compat_handler(%rip),%rdi
	mov	%r12,%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x80
	test	%rax,%rax
	jnz	.Lfail
	/* argc=1、argv[0]="app"、envp=NULL、最小 auxv；保持 16 字节对齐。 */
	mov	%r13,%rsp
	lea	.Lapp(%rip),%r14
	xor	%eax,%eax
	push	%rax			/* AT_NULL value */
	push	%rax			/* AT_NULL type */
	push	$4096			/* AT_PAGESZ value */
	push	$6			/* AT_PAGESZ type */
	push	(%rbx)			/* AT_ENTRY value */
	push	$9			/* AT_ENTRY type */
	push	%rax			/* AT_PHDR value */
	push	$3			/* AT_PHDR type */
	push	%rax			/* envp[0] = NULL */
	push	%r14			/* argv[0] */
	push	$1			/* argc */
	mov	(%rbx),%rax
	jmp	*%rax
.Lfail:
	mov	$1,%edi
	xor	%eax,%eax
	int	$0x80
	ud2
/* int 0x80 上行入口：rdi 指向帧页，先切换到翻译器栈再调用 C 分派器。 */
.section .text
_sf_compat_handler:
	lea	__sf_linux_translator_data(%rip),%rax
	mov	8(%rax),%rsp
	call	sf_linux_dispatch
	mov	%rax,%rdi
	xor	%esi,%esi
	mov	$42,%eax
	int	$0x81
	ud2
.section .rodata
.Lapp:
.asciz "app"
