/* Progressive UEFI test - step 1: just return */
#include <stdint.h>

typedef unsigned long long UINTN;
typedef long long EFI_STATUS;
typedef void *EFI_HANDLE;

/* Minimal EFI_SYSTEM_TABLE matching our efi.h layout */
typedef struct {
    uint64_t sig;
    uint32_t rev;
    uint32_t hdr_sz;
    uint32_t crc32;
    uint32_t reserved;
    uint16_t *fw_vendor;
    uint32_t fw_rev;
    void *con_in;
    void *con_out;
    void *err_out;
    void *runtime;
    void *boot;
    uint64_t num_tables;
    void *config_table;
} EFI_SYSTEM_TABLE_T;

#define EFI_SUCCESS 0

/* Write character to serial port COM1 (0x3F8) */
static void serial_out(char c) {
    volatile unsigned char *port = (volatile unsigned char *)0x3F8;
    /* Wait for transmit buffer empty */
    while ((*port & 0x20) == 0);
    *(port + 0) = c;
}

static void serial_str(const char *s) {
    while (*s) serial_out(*s++);
}

/* Step 1: Entry, print to serial, return */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE_T *SystemTable) {
    (void)ImageHandle;
    serial_str("[TEST] efi_main entered\r\n");

    /* Test: can we access SystemTable? */
    if (SystemTable != 0) {
        serial_str("[TEST] SystemTable non-null\r\n");
        /* Try to access con_out at offset 0x30 */
        void **st = (void **)SystemTable;
        void *con_out = st[0x30 / 8];  /* offset 0x30 = 6th pointer */
        if (con_out != 0) {
            serial_str("[TEST] con_out non-null\r\n");
        } else {
            serial_str("[TEST] con_out is NULL\r\n");
        }
        /* Try to access boot at offset 0x48 */
        void *boot_srv = st[0x48 / 8];
        if (boot_srv != 0) {
            serial_str("[TEST] boot services non-null\r\n");
        } else {
            serial_str("[TEST] boot services is NULL\r\n");
        }
    } else {
        serial_str("[TEST] SystemTable is NULL!\r\n");
    }

    serial_str("[TEST] returning EFI_SUCCESS\r\n");
    return EFI_SUCCESS;
}
