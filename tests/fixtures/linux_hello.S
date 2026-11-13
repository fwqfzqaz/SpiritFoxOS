/* S2 验收夹具：x86_64 静态 PIE（ET_DYN、无解释器、无动态依赖）的 Linux hello。
 * 直接使用 Linux syscall ABI，装载期 0F 05 会被改写为 CD 80。 */
.section .rodata
msg:
	.ascii "LINUX COMPAT HELLO\n"
.set msg_len, . - msg
.section .text
.global _start
_start:
	mov	$1,%eax
	mov	$1,%edi
	lea	msg(%rip),%rsi
	mov	$msg_len,%edx
	syscall
	mov	$60,%eax
	xor	%edi,%edi
	syscall
	ud2
