/* Minimal UEFI test - returns immediately */
void efi_main(void) {
    /* Just return; entry point will do RET = return to caller */
    __asm__ volatile("movl $0x80000003, %eax");  /* EFI_SUCCESS-ish, actually just ret */
}
