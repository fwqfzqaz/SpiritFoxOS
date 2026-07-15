#ifndef HAL_H
#define HAL_H

/**
 * Hardware Abstraction Layer for x86_64
 * Unified interface for all hardware operations.
 * All drivers must use these functions, never inline asm directly.
 */

#include <stdint.h>
#include <stddef.h>

/* ========================================================================
 * Port I/O
 * ======================================================================== */

static inline uint8_t hal_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t hal_inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t hal_inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void hal_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void hal_outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void hal_outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline void hal_io_wait(void) {
    hal_outb(0x80, 0);
}

/* ========================================================================
 * Model-Specific Registers (MSR)
 * ======================================================================== */

static inline uint64_t hal_read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void hal_write_msr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

/* ========================================================================
 * Control Registers
 * ======================================================================== */

static inline uint64_t hal_read_cr0(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void hal_write_cr0(uint64_t val) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val));
}

static inline uint64_t hal_read_cr2(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t hal_read_cr3(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void hal_write_cr3(uint64_t val) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val));
}

static inline uint64_t hal_read_cr4(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void hal_write_cr4(uint64_t val) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val));
}

/* ========================================================================
 * CPUID
 * ======================================================================== */

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} hal_cpuid_t;

static inline void hal_cpuid(uint32_t leaf, uint32_t subleaf, hal_cpuid_t *result) {
    __asm__ volatile ("cpuid"
        : "=a"(result->eax), "=b"(result->ebx),
          "=c"(result->ecx), "=d"(result->edx)
        : "a"(leaf), "c"(subleaf));
}

/* ========================================================================
 * TLB & Cache
 * ======================================================================== */

static inline void hal_flush_tlb(void) {
    uint64_t cr3 = hal_read_cr3();
    hal_write_cr3(cr3);
}

static inline void hal_flush_tlb_page(uintptr_t addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline void hal_invalidate_cache(void) {
    __asm__ volatile ("wbinvd");
}

/* ========================================================================
 * Memory-Mapped I/O (MMIO)
 * ======================================================================== */

static inline uint8_t hal_mmio_read8(uintptr_t addr) {
    return *(volatile uint8_t *)addr;
}

static inline uint16_t hal_mmio_read16(uintptr_t addr) {
    return *(volatile uint16_t *)addr;
}

static inline uint32_t hal_mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline uint64_t hal_mmio_read64(uintptr_t addr) {
    return *(volatile uint64_t *)addr;
}

static inline void hal_mmio_write8(uintptr_t addr, uint8_t val) {
    *(volatile uint8_t *)addr = val;
}

static inline void hal_mmio_write16(uintptr_t addr, uint16_t val) {
    *(volatile uint16_t *)addr = val;
}

static inline void hal_mmio_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}

static inline void hal_mmio_write64(uintptr_t addr, uint64_t val) {
    *(volatile uint64_t *)addr = val;
}

/* ========================================================================
 * Interrupt Control
 * ======================================================================== */

static inline void hal_enable_interrupts(void) {
    __asm__ volatile ("sti");
}

static inline void hal_disable_interrupts(void) {
    __asm__ volatile ("cli");
}

static inline int hal_interrupts_enabled(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

static inline uint64_t hal_save_interrupts(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags));
    return flags;
}

static inline void hal_restore_interrupts(uint64_t flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags));
}

/* ========================================================================
 * Halt
 * ======================================================================== */

static inline void hal_halt(void) {
    while (1) {
        __asm__ volatile ("cli; hlt");
    }
    __builtin_unreachable();
}

static inline void hal_halt_no_cli(void) {
    __asm__ volatile ("hlt");
}

/* ========================================================================
 * MSR Addresses
 * ======================================================================== */

#define MSR_IA32_APIC_BASE      0x0000001B
#define MSR_IA32_EFER           0xC0000080
#define MSR_IA32_STAR           0xC0000081
#define MSR_IA32_LSTAR          0xC0000082
#define MSR_IA32_CSTAR          0xC0000083
#define MSR_IA32_FMASK          0xC0000084
#define MSR_IA32_FS_BASE        0xC0000100
#define MSR_IA32_GS_BASE        0xC0000101
#define MSR_IA32_KERNEL_GS_BASE 0xC0000102

/* APIC base flags */
#define APIC_BASE_ADDR_MASK     0xFFFFF000
#define APIC_BASE_GLOBAL_ENABLE (1 << 11)
#define APIC_BASE_BSP           (1 << 8)

/* ========================================================================
 * Paging helpers
 * ======================================================================== */

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PCD       (1ULL << 4)  /* Page-level Cache Disable */
#define PTE_PWT       (1ULL << 3)  /* Page-level Write-Through */
#define PTE_HUGE      (1ULL << 7)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Map a 2MB page at virtual address to physical address */
void hal_map_2mb(uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Map a 4KB page at virtual address to physical address */
void hal_map_4kb(uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Map a 1GB page at virtual address to physical address */
void hal_map_1gb(uintptr_t virt, uintptr_t phys, uint64_t flags);

/* Ensure a virtual address range is mapped (identity mapping) */
void hal_ensure_mapped(uintptr_t phys, size_t size);

/* Ensure MMIO range is mapped with cache disabled (identity mapping) */
void hal_ensure_mapped_mmio(uintptr_t phys, size_t size);

#endif /* HAL_H */
