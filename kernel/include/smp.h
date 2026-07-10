#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stddef.h>
#include "process.h"

/* ========================================================================
 * Maximum CPUs supported
 * ======================================================================== */
#define SMP_MAX_CPUS  256

/* ========================================================================
 * AP trampoline location (physical address)
 * ======================================================================== */
#define AP_TRAMPOLINE_ADDR  0x8000

/* ========================================================================
 * Spinlock
 * ======================================================================== */
typedef struct {
    volatile uint32_t ticket;
    volatile uint32_t serving;
} spinlock_t;

void spinlock_init(spinlock_t *lock);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

/* ========================================================================
 * Per-CPU data
 * ======================================================================== */
typedef struct {
    uint32_t    cpu_id;           /* APIC ID */
    uint32_t    index;            /* Logical CPU index (0..cpu_count-1) */
    int         online;           /* Is this CPU online? */
    int         bsp;              /* Is this the BSP? */
    void       *kernel_stack;     /* This CPU's kernel stack */
    uint64_t    kernel_rsp;       /* Saved kernel RSP */
    process_t  *current_process;  /* Currently running process */
} cpu_local_t;

/* ========================================================================
 * CPU info (from MADT enumeration)
 * ======================================================================== */
typedef struct {
    uint8_t  processor_id;  /* ACPI processor ID */
    uint8_t  apic_id;       /* Local APIC ID */
    uint32_t flags;         /* MADT flags (bit 0 = enabled) */
} cpu_info_t;

/* ========================================================================
 * LAPIC ICR definitions
 * ======================================================================== */
#define LAPIC_ICR_LOW          0x300
#define LAPIC_ICR_HIGH         0x310

/* ICR delivery modes */
#define LAPIC_ICR_DELIVERY_FIXED    (0 << 8)
#define LAPIC_ICR_DELIVERY_INIT     (5 << 8)
#define LAPIC_ICR_DELIVERY_STARTUP  (6 << 8)

/* ICR destination mode */
#define LAPIC_ICR_DEST_PHYSICAL     (0 << 11)
#define LAPIC_ICR_DEST_LOGICAL      (1 << 11)

/* ICR delivery status */
#define LAPIC_ICR_DELIVERY_PENDING  (1 << 12)

/* ICR level */
#define LAPIC_ICR_LEVEL_ASSERT      (1 << 14)

/* ICR shorthand */
#define LAPIC_ICR_SHORTHAND_NONE             (0 << 18)
#define LAPIC_ICR_SHORTHAND_SELF             (1 << 18)
#define LAPIC_ICR_SHORTHAND_ALL_INCLUDING    (2 << 18)
#define LAPIC_ICR_SHORTHAND_ALL_EXCLUDING    (3 << 18)

/* ========================================================================
 * SMP initialization
 * ======================================================================== */

/* Initialize SMP: enumerate CPUs from MADT, start APs */
void smp_init(void);

/* AP C entry point (called from assembly trampoline) */
void ap_entry_c(void);

/* ========================================================================
 * IPI functions
 * ======================================================================== */

/* Send an IPI to a specific APIC ID */
void smp_send_ipi(uint8_t dest_apic_id, uint8_t vector);

/* Broadcast IPI to all CPUs except self */
void smp_broadcast_ipi(uint8_t vector);

/* ========================================================================
 * Accessors
 * ======================================================================== */

/* Get total CPU count (including BSP) */
int smp_get_cpu_count(void);

/* Get CPU info by logical index */
cpu_info_t *smp_get_cpu_info(int index);

/* Get per-CPU local data for current CPU (via GS base) */
cpu_local_t *smp_get_current_cpu(void);

#endif /* SMP_H */
