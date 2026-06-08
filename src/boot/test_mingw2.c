/* Minimal UEFI app for mingw - no external dependencies */
typedef unsigned long long UINTN;
typedef unsigned long long EFI_STATUS;
typedef void *EFI_HANDLE;
typedef struct _EFI_SYSTEM_TABLE { void *dummy; } EFI_SYSTEM_TABLE;

#define EFI_SUCCESS 0

/* UEFI image entry point convention for mingw */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    return EFI_SUCCESS;
}
