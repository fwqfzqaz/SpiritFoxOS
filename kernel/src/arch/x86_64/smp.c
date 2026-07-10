#include "smp.h"
#include "apic.h"
#include "acpi.h"
#include "hal.h"
#include "kmalloc.h"
#include "string.h"
#include "vga.h"

/* ========================================================================
 * External declarations for AP trampoline (defined in ap_trampoline.S)
 * ======================================================================== */
/* ========================================================================
 * External declarations for AP trampoline
 * The trampoline is assembled as a raw binary (ap_trampoline.bin)
 * and embedded here as a byte array to avoid ELF relocation issues
 * with mixed 16/64-bit code.
 * ======================================================================== */

/* Trampoline patchable data offsets (must match ap_trampoline.S) */
#define TRAMP_PML4_OFF      0x00   /* 4 bytes: PML4 physical address */
#define TRAMP_ENTRY_OFF     0x04   /* 8 bytes: C entry point address */
#define TRAMP_GDT_OFF       0x0C   /* 2+8 bytes: GDT limit + base */

/* Trampoline binary - assembled from ap_trampoline.S via:
 *   nasm -f bin ap_trampoline.S -o ap_trampoline.bin
 *   xxd -i ap_trampoline.bin > ap_trampoline_bin.h
 * For now we embed it directly as a static array.
 * The trampoline switches from 16-bit real mode to 64-bit long mode. */
static const uint8_t ap_trampoline_code[] = {
    /* ap_pml4:   dd 0 */       0x00, 0x00, 0x00, 0x00,
    /* ap_entry:  dq 0 */       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* ap_gdt_ptr: dw 0 */      0x00, 0x00,
    /*            dq 0 */       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* cli */                   0xFA,
    /* xor ax, ax */            0x31, 0xC0,
    /* mov ds, ax */            0x8E, 0xD8,
    /* mov es, ax */            0x8E, 0xC0,
    /* in al, 0x92 */           0xE4, 0x92,
    /* or al, 2 */              0x0C, 0x02,
    /* out 0x92, al */          0xE6, 0x92,
    /* mov eax, [0x8000] */     0xA1, 0x00, 0x80, 0x00, 0x00,
    /* mov cr3, eax */          0x0F, 0x22, 0xD8,
    /* mov eax, cr4 */          0x0F, 0x20, 0xE0,
    /* or eax, 0x20 */          0x0D, 0x20, 0x00, 0x00, 0x00,
    /* mov cr4, eax */          0x0F, 0x22, 0xE0,
    /* mov ecx, 0xC0000080 */   0xB9, 0x80, 0x00, 0x00, 0xC0,
    /* rdmsr */                 0x0F, 0x32,
    /* or eax, 0x100 */         0x0D, 0x00, 0x01, 0x00, 0x00,
    /* wrmsr */                 0x0F, 0x30,
    /* lgdt [0x800C] */         0x0F, 0x01, 0x15, 0x0C, 0x80, 0x00, 0x00,
    /* mov eax, cr0 */          0x0F, 0x20, 0xC0,
    /* or eax, 0x80000000 */    0x0D, 0x00, 0x00, 0x00, 0x80,
    /* mov cr0, eax */          0x0F, 0x22, 0xC0,
    /* jmp 0x08:ap_long_mode (far jump) */
    0xEA, 0x36, 0x80, 0x00, 0x00, 0x08, 0x00,
    /* --- 64-bit mode --- */
    /* mov ax, 0x10 */          0x66, 0xB8, 0x10, 0x00,
    /* mov ds, ax */            0x8E, 0xD8,
    /* mov es, ax */            0x8E, 0xC0,
    /* mov fs, ax */            0x8E, 0xE0,
    /* mov gs, ax */            0x8E, 0xE8,
    /* mov ss, ax */            0x8E, 0xD0,
    /* mov rsp, 0x90000 */      0x48, 0xC7, 0xC4, 0x00, 0x00, 0x09, 0x00,
    /* mov rax, [0x8004] */     0x48, 0xA1, 0x04, 0x80, 0x00, 0x00,
    /* call rax */              0xFF, 0xD0,
    /* cli */                   0xFA,
    /* hlt */                   0xF4,
    /* jmp .-2 */               0xEB, 0xFD,
};

static const size_t ap_trampoline_size = sizeof(ap_trampoline_code);

/* ========================================================================
 * Global state
 * ======================================================================== */
static cpu_info_t  cpu_infos[SMP_MAX_CPUS];
static cpu_local_t cpu_locals[SMP_MAX_CPUS];
static int         cpu_count = 0;

/* LAPIC MMIO base (cached from apic_get_lapic_base) */
static uintptr_t   lapic_base = 0;

/* ========================================================================
 * LAPIC ICR helpers (write to ICR registers)
 * ======================================================================== */

static void lapic_write(uint32_t offset, uint32_t value)
{
    hal_mmio_write32(lapic_base + offset, value);
}

static uint32_t lapic_read(uint32_t offset)
{
    return hal_mmio_read32(lapic_base + offset);
}

/* Wait for pending IPI delivery to complete */
static void lapic_wait_icr(void)
{
    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVERY_PENDING)
        ;
}

/* ========================================================================
 * Spinlock implementation (ticket lock)
 * ======================================================================== */

void spinlock_init(spinlock_t *lock)
{
    lock->ticket = 0;
    lock->serving = 0;
}

void spinlock_acquire(spinlock_t *lock)
{
    uint64_t flags = hal_save_interrupts();

    /* Atomically fetch and increment ticket */
    uint32_t my_ticket = __atomic_fetch_add(&lock->ticket, 1, __ATOMIC_ACQUIRE);

    /* Spin until our ticket is being served */
    while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != my_ticket) {
        hal_restore_interrupts(flags);
        __asm__ volatile ("pause");
        flags = hal_save_interrupts();
    }

    /* Interrupts disabled while lock is held.
     * Flags are saved in local variable, restored on release. */
    /* Store flags in the lock for release to restore */
    __asm__ volatile ("" : : : "memory");  /* compiler barrier */
    /* We keep a per-CPU flags storage via a simple approach:
     * Since we disable interrupts and they stay disabled while spinning,
     * on release we just re-enable interrupts. This is safe because
     * spinlock_release must be called from the same CPU that acquired it. */
}

void spinlock_release(spinlock_t *lock)
{
    /* Advance serving counter */
    __atomic_add_fetch(&lock->serving, 1, __ATOMIC_RELEASE);

    /* Re-enable interrupts (they were disabled in acquire) */
    hal_enable_interrupts();
}

/* ========================================================================
 * IPI functions
 * ======================================================================== */

void smp_send_ipi(uint8_t dest_apic_id, uint8_t vector)
{
    lapic_wait_icr();

    /* Set destination APIC ID in ICR high (bits 24-31) */
    uint32_t icr_high = ((uint32_t)dest_apic_id << 24);
    lapic_write(LAPIC_ICR_HIGH, icr_high);

    /* Set vector and delivery mode in ICR low */
    uint32_t icr_low = (uint32_t)vector
                      | LAPIC_ICR_DELIVERY_FIXED
                      | LAPIC_ICR_DEST_PHYSICAL
                      | LAPIC_ICR_LEVEL_ASSERT;
    lapic_write(LAPIC_ICR_LOW, icr_low);

    lapic_wait_icr();
}

void smp_broadcast_ipi(uint8_t vector)
{
    lapic_wait_icr();

    /* No need to set ICR high for shorthand */
    lapic_write(LAPIC_ICR_HIGH, 0);

    /* Set vector, fixed delivery, all-excluding-self shorthand */
    uint32_t icr_low = (uint32_t)vector
                      | LAPIC_ICR_DELIVERY_FIXED
                      | LAPIC_ICR_LEVEL_ASSERT
                      | LAPIC_ICR_SHORTHAND_ALL_EXCLUDING;
    lapic_write(LAPIC_ICR_LOW, icr_low);

    lapic_wait_icr();
}

/* ========================================================================
 * CPU enumeration from MADT
 * ======================================================================== */

static void smp_enumerate_cpus(void)
{
    void *madt_ptr = acpi_find_table("APIC");
    if (!madt_ptr) {
        printf("[SMP] No MADT table found, single CPU mode\n");
        cpu_count = 1;
        cpu_infos[0].processor_id = 0;
        cpu_infos[0].apic_id = (uint8_t)apic_get_lapic_id();
        cpu_infos[0].flags = 1; /* enabled */
        return;
    }

    acpi_sdt_header_t *madt_hdr = (acpi_sdt_header_t *)madt_ptr;
    uintptr_t entry_offset = sizeof(acpi_sdt_header_t) + 8; /* skip header + lapic_addr + flags */

    cpu_count = 0;

    while (entry_offset < madt_hdr->length && cpu_count < SMP_MAX_CPUS) {
        madt_entry_t *entry = (madt_entry_t *)((uintptr_t)madt_ptr + entry_offset);
        if (entry->length == 0)
            break;

        if (entry->type == MADT_TYPE_LAPIC) {
            madt_lapic_t *lapic = (madt_lapic_t *)entry;
            /* Only add enabled CPUs */
            if (lapic->flags & 0x01) {
                cpu_infos[cpu_count].processor_id = lapic->processor_id;
                cpu_infos[cpu_count].apic_id = lapic->apic_id;
                cpu_infos[cpu_count].flags = lapic->flags;
                cpu_count++;
            }
        }

        entry_offset += entry->length;
    }

    if (cpu_count == 0) {
        /* Fallback: at least the BSP */
        printf("[SMP] No LAPIC entries in MADT, assuming single CPU\n");
        cpu_count = 1;
        cpu_infos[0].processor_id = 0;
        cpu_infos[0].apic_id = (uint8_t)apic_get_lapic_id();
        cpu_infos[0].flags = 1;
    }

    printf("[SMP] Found %d CPU(s)\n", cpu_count);
    for (int i = 0; i < cpu_count; i++) {
        printf("[SMP]   CPU %d: APIC ID=%u, Processor ID=%u\n",
               i, cpu_infos[i].apic_id, cpu_infos[i].processor_id);
    }
}

/* ========================================================================
 * Delay loop (approximate milliseconds)
 * ======================================================================== */

static void delay_ms(unsigned int ms)
{
    /* Rough delay using hal_halt_no_cli with a simple loop.
     * Each iteration is roughly ~1us at modern CPU speeds with io_wait. */
    for (unsigned int i = 0; i < ms * 1000; i++) {
        hal_outb(0x80, 0); /* ~1us delay via port 0x80 */
    }
}

/* ========================================================================
 * AP startup sequence
 * ======================================================================== */

static void smp_start_aps(void)
{
    uint32_t bsp_apic_id = apic_get_lapic_id();

    printf("[SMP] BSP APIC ID: %u\n", bsp_apic_id);

    /* Initialize cpu_locals for all CPUs */
    for (int i = 0; i < cpu_count; i++) {
        cpu_locals[i].cpu_id = cpu_infos[i].apic_id;
        cpu_locals[i].index = (uint32_t)i;
        cpu_locals[i].online = 0;
        cpu_locals[i].bsp = (cpu_infos[i].apic_id == bsp_apic_id) ? 1 : 0;
        cpu_locals[i].current_process = NULL;
        cpu_locals[i].kernel_rsp = 0;

        /* Allocate kernel stack for APs (BSP already has one) */
        if (!cpu_locals[i].bsp) {
            void *stack = kmalloc(PROC_KERNEL_STACK);
            if (!stack) {
                printf("[SMP] ERROR: Failed to allocate stack for AP %u\n",
                       cpu_infos[i].apic_id);
                continue;
            }
            memset(stack, 0, PROC_KERNEL_STACK);
            /* Stack grows downward: point to top of allocated region */
            cpu_locals[i].kernel_stack = stack;
        } else {
            cpu_locals[i].kernel_stack = NULL; /* BSP uses existing stack */
        }
    }

    /* Mark BSP as online */
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_locals[i].bsp) {
            cpu_locals[i].online = 1;
            /* Set GS base for BSP */
            hal_write_msr(MSR_IA32_GS_BASE, (uint64_t)&cpu_locals[i]);
            break;
        }
    }

    /* Copy AP trampoline code to 0x8000 (identity-mapped region) */
    hal_ensure_mapped(AP_TRAMPOLINE_ADDR, ap_trampoline_size);
    memcpy((void *)AP_TRAMPOLINE_ADDR, ap_trampoline_code, ap_trampoline_size);

    /* Patch trampoline data with runtime values */
    uint8_t *trampoline_base = (uint8_t *)AP_TRAMPOLINE_ADDR;

    /* Set PML4 address */
    *(uint32_t *)(trampoline_base + TRAMP_PML4_OFF) = (uint32_t)(hal_read_cr3() & PTE_ADDR_MASK);

    /* Set C entry point */
    *(uint64_t *)(trampoline_base + TRAMP_ENTRY_OFF) = (uint64_t)(uintptr_t)ap_entry_c;

    /* Set GDT pointer (from current GDTR) */
    uint8_t gdt_buffer[10];
    __asm__ volatile ("sgdt %0" : "=m"(gdt_buffer) : : "memory");
    memcpy(trampoline_base + TRAMP_GDT_OFF, gdt_buffer, 10);

    printf("[SMP] Trampoline copied to 0x%x (%lu bytes)\n",
           AP_TRAMPOLINE_ADDR, (unsigned long)ap_trampoline_size);

    /* Start each AP */
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_locals[i].bsp)
            continue;

        uint8_t apic_id = cpu_infos[i].apic_id;
        printf("[SMP] Starting AP with APIC ID %u...\n", apic_id);

        /* Step 1: Send INIT IPI */
        lapic_wait_icr();
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
        lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_LEVEL_ASSERT);
        lapic_wait_icr();

        /* Step 2: Wait 10ms */
        delay_ms(10);

        /* Step 3: Send SIPI (Startup IPI) with vector 0x08 (page 0x8000 / 4096) */
        lapic_wait_icr();
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
        lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_DELIVERY_STARTUP | 0x08);
        lapic_wait_icr();

        /* Step 4: Wait 200us */
        delay_ms(1);

        /* Step 5: Send second SIPI (per Intel spec recommendation) */
        lapic_wait_icr();
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)apic_id << 24);
        lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_DELIVERY_STARTUP | 0x08);
        lapic_wait_icr();

        /* Step 6: Wait for AP to come online (with timeout) */
        int timeout = 5000; /* ~5 seconds */
        while (!cpu_locals[i].online && timeout > 0) {
            hal_halt_no_cli();
            delay_ms(1);
            timeout--;
        }

        if (cpu_locals[i].online) {
            printf("[SMP] AP %u is online\n", apic_id);
        } else {
            printf("[SMP] WARNING: AP %u did not come online (timeout)\n", apic_id);
        }
    }
}

/* ========================================================================
 * AP C entry point
 * ======================================================================== */

void ap_entry_c(void)
{
    /* Get our LAPIC ID */
    uint32_t my_apic_id = apic_get_lapic_id();

    /* Find our cpu_local_t */
    cpu_local_t *local = NULL;
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_locals[i].cpu_id == my_apic_id) {
            local = &cpu_locals[i];
            break;
        }
    }

    if (!local) {
        /* Unknown CPU - halt */
        hal_halt();
    }

    /* Set up GS base for per-CPU data access */
    hal_write_msr(MSR_IA32_GS_BASE, (uint64_t)local);

    /* Set up kernel stack */
    if (local->kernel_stack) {
        uint64_t stack_top = (uint64_t)local->kernel_stack + PROC_KERNEL_STACK;
        __asm__ volatile ("mov %0, %%rsp" : : "r"(stack_top));
    }

    /* Initialize LAPIC on this AP */
    /* Enable LAPIC via MSR */
    uint64_t msr_val = hal_read_msr(MSR_IA32_APIC_BASE);
    if (!(msr_val & APIC_BASE_GLOBAL_ENABLE)) {
        msr_val |= APIC_BASE_GLOBAL_ENABLE;
        hal_write_msr(MSR_IA32_APIC_BASE, msr_val);
    }

    /* Set spurious interrupt vector and enable */
    lapic_write(LAPIC_SVR, APIC_VECTOR_SPURIOUS | LAPIC_SVR_ENABLE);

    /* Clear error status */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    /* Mask all LVT entries */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);

    /* Accept all interrupts */
    lapic_write(LAPIC_TPR, 0);

    /* Mark self as online */
    __atomic_store_n(&local->online, 1, __ATOMIC_RELEASE);

    printf("[SMP] AP %u initialized\n", my_apic_id);

    /* Enter idle loop with interrupts enabled */
    hal_enable_interrupts();
    while (1) {
        hal_halt_no_cli();
    }
}

/* ========================================================================
 * SMP initialization (called by BSP)
 * ======================================================================== */

void smp_init(void)
{
    /* Cache the LAPIC base for ICR access */
    lapic_base = apic_get_lapic_base();
    if (lapic_base == 0) {
        printf("[SMP] ERROR: LAPIC base not available\n");
        return;
    }

    printf("[SMP] Initializing SMP subsystem...\n");

    /* Step 1: Enumerate CPUs from MADT */
    smp_enumerate_cpus();

    /* Step 2: Start APs (only if more than 1 CPU) */
    if (cpu_count > 1) {
        smp_start_aps();
    }

    printf("[SMP] Initialization complete: %d CPU(s) online\n", cpu_count);
}

/* ========================================================================
 * Accessors
 * ======================================================================== */

int smp_get_cpu_count(void)
{
    return cpu_count;
}

cpu_info_t *smp_get_cpu_info(int index)
{
    if (index < 0 || index >= cpu_count)
        return NULL;
    return &cpu_infos[index];
}

cpu_local_t *smp_get_current_cpu(void)
{
    uint64_t gs_base = hal_read_msr(MSR_IA32_GS_BASE);
    return (cpu_local_t *)gs_base;
}
