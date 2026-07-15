/*
 * Minimal UEFI test application - just prints a message and returns.
 * Used to verify PE loading works correctly.
 */
#include "boot.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_TEXT_STRING)(
    void *This, CHAR16 *String);

typedef struct {
    void *Reset;
    EFI_TEXT_STRING OutputString;
} SIMPLE_OUTPUT;

typedef struct {
    uint64_t TableSignature;
    uint32_t TableRevision;
    uint32_t HeaderSize;
    uint32_t Crc32;
    uint32_t Reserved;
    CHAR16 *FirmwareVendor;
    UINTN   FirmwareRevision;
    void *ConsoleInHandle;
    void *ConIn;
    void *ConsoleOutHandle;
    SIMPLE_OUTPUT *ConOut;
    void *StandardErrorHandle;
    SIMPLE_OUTPUT *StdErr;
    void *RuntimeServices;
    void *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} TEST_EFI_SYSTEM_TABLE;

EFI_STATUS __attribute__((ms_abi)) efi_main(
    void *image_handle, TEST_EFI_SYSTEM_TABLE *system_table)
{
    if (system_table && system_table->ConOut) {
        system_table->ConOut->OutputString(system_table->ConOut,
            L"UEFI Test: Hello from BOOTX64.EFI!\r\n");
    }
    return 0;
}
