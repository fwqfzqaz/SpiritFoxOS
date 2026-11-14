/* 静态 PIE 夹具入口：SysV 初始栈布局为 argc、argv、envp、auxv。
 * 对齐后调用 sf_tool_main，最后用 exit_group(60/231) 结束进程。 */
.section .text
.global _start
_start:
	mov	(%rsp),%rdi
	lea	8(%rsp),%rsi
	and	$-16,%rsp
	call	sf_tool_main
	mov	%eax,%edi
	mov	$231,%eax
	syscall
	ud2
