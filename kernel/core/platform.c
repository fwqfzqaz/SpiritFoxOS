/* 内核平台适配；宿主单元测试提供另一个适配实现。 */
#include "internal.h"
#include "kmalloc.h"
#include "hal.h"
#include "serial.h"

void *sf_alloc(size_t size) { return kmalloc(size); }
void sf_free(void *address) { if (address) kfree(address); }
void sf_log(const char *text) { serial_puts(text); }
uint64_t sf_irq_save(void) { return hal_save_interrupts(); }
void sf_irq_restore(uint64_t flags) { hal_restore_interrupts(flags); }

extern const uint8_t sfm_package_start[], sfm_package_end[];
extern void kernel_panic(const char *message);
extern int sfm_mount_compat(void);

void sfm_boot(void)
{
    int result = sfm_package_load(sfm_package_start, sfm_package_end - sfm_package_start);
    if (result) kernel_panic("sfmod package validation/init failed");
    if (sfm_selftest()) kernel_panic("sfmod message/file self-test failed");
    sf_log("[sfmod] self-test passed: IPC queue, RPC, VFS/tmpfs read/write\n");
    if (sfm_mount_compat()) kernel_panic("sfmod VFS bridge failed");
}
