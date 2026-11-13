/* S2 兼容负例探针：数据页 [0]=mode、[8]=外来进程页地址。
 * mode 0 校验注册边界；mode 1 未注册即执行 int 0x81；mode 2 触发嵌套上行调用。
 * 退出码 1..7 表示对应检查失败，0 表示通过。 */
.section .text
.global _sf_linux_probe_start
_sf_linux_probe_start:
	lea	_sf_linux_probe_start(%rip),%rbx
	mov	4096(%rbx),%rax
	test	%rax,%rax
	jnz	.Lmode
	/* frame = mmap(0, 4096) */
	xor	%edi,%edi
	mov	$4096,%esi
	xor	%edx,%edx
	mov	$35,%eax
	int	$0x80
	test	%rax,%rax
	js	.Lfail_pre
	mov	%rax,%r12
	/* 1: handler 在 RW/NX 帧页 -> SF_DENIED */
	mov	%r12,%rdi
	mov	%r12,%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x80
	cmp	$-2,%rax
	jne	.Lfail1
	/* 2: 帧页未对齐 -> SF_INVALID */
	lea	_sf_linux_probe_start(%rip),%rdi
	lea	1(%r12),%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x80
	cmp	$-1,%rax
	jne	.Lfail2
	/* 3: 帧页为 RX 代码页（不可写） -> SF_DENIED */
	lea	_sf_linux_probe_start(%rip),%rdi
	lea	_sf_linux_probe_start(%rip),%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x80
	cmp	$-2,%rax
	jne	.Lfail3
	/* 4: 外来进程帧页 -> SF_DENIED */
	lea	_sf_linux_probe_start(%rip),%rdi
	mov	4104(%rbx),%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x80
	cmp	$-2,%rax
	jne	.Lfail4
	/* 5: flags 非零 -> SF_INVALID */
	lea	_sf_linux_probe_start(%rip),%rdi
	mov	%r12,%rsi
	mov	$1,%edx
	mov	$41,%eax
	int	$0x80
	cmp	$-1,%rax
	jne	.Lfail5
	/* 6: 合法注册 */
	lea	_sf_linux_probe_start(%rip),%rdi
	mov	%r12,%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x80
	test	%rax,%rax
	jnz	.Lfail6
	/* 7: 重复注册（经原生入口） -> SF_BUSY */
	lea	_sf_linux_probe_start(%rip),%rdi
	mov	%r12,%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x81
	cmp	$-7,%rax
	jne	.Lfail7
	xor	%edi,%edi
	xor	%eax,%eax
	int	$0x81
	ud2
.Lmode:
	cmp	$1,%rax
	je	.Lmode81
	/* mode 2: 注册后 int 0x80 进入 handler，handler 再次 int 0x80 触发嵌套。 */
	xor	%edi,%edi
	mov	$4096,%esi
	xor	%edx,%edx
	mov	$35,%eax
	int	$0x80
	test	%rax,%rax
	js	.Lfail_pre
	mov	%rax,%r12
	lea	_sf_probe_handler(%rip),%rdi
	mov	%r12,%rsi
	xor	%edx,%edx
	mov	$41,%eax
	int	$0x80
	test	%rax,%rax
	jnz	.Lfail_pre
	xor	%eax,%eax
	int	$0x80
	ud2
.Lmode81:
	/* 非兼容任务执行 int 0x81：应得到 SF_DENIED 而不是被终止。 */
	int	$0x81
	cmp	$-2,%rax
	jne	.Lfail1
	xor	%edi,%edi
	xor	%eax,%eax
	int	$0x80
	ud2
_sf_probe_handler:
	int	$0x80
	ud2
.Lfail1:	mov	$1,%edi
	jmp	.Lfail_pre
.Lfail2:	mov	$2,%edi
	jmp	.Lfail_pre
.Lfail3:	mov	$3,%edi
	jmp	.Lfail_pre
.Lfail4:	mov	$4,%edi
	jmp	.Lfail_pre
.Lfail5:	mov	$5,%edi
	jmp	.Lfail_pre
.Lfail6:	mov	$6,%edi
	jmp	.Lfail_pre
.Lfail7:	mov	$7,%edi
	jmp	.Lfail_post
.Lfail_pre:
	xor	%eax,%eax
	int	$0x80
	ud2
.Lfail_post:
	mov	$42,%eax
	int	$0x81
	ud2
.section .data
.space 4096
