#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>

/* =========================================
 * Boot Info (shared with kernel)
 * ========================================= */
#define BOOT_INFO_MAGIC  0x5F1F0F05
#define BOOT_TYPE_LEGACY 0
#define BOOT_TYPE_UEFI   1

typedef struct MemoryMapEntry {
    uint64_t    physical_start;
    uint64_t    virtual_start;
    uint64_t    number_of_pages;
    uint64_t    attribute;
    uint32_t    type;
    uint32_t    reserved;
} MemoryMapEntry;

typedef struct BootInfo {
    uint32_t    magic;
    uint32_t    boot_type;

    uint64_t    framebuffer_base;
    uint64_t    framebuffer_size;
    uint32_t    width;
    uint32_t    height;
    uint32_t    pitch;
    uint32_t    bpp;

    uint64_t    memory_map;
    uint64_t    memory_map_size;
    uint64_t    memory_map_descriptor_size;
    uint64_t    memory_map_entry_count;

    uint64_t    acpi_rsdp;
    uint64_t    efi_runtime_services;
    uint64_t    total_memory;
} BootInfo;

/* =========================================
 * EFI Type Definitions (self-contained)
 * ========================================= */
typedef void *EFI_HANDLE;
typedef unsigned long EFI_STATUS;
typedef uint16_t CHAR16;
typedef unsigned long UINTN;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint8_t UINT8;

#define EFI_SUCCESS               0
#define EFI_LOAD_ERROR            ((EFI_STATUS)(1))
#define EFI_INVALID_PARAMETER     ((EFI_STATUS)(2))
#define EFI_UNSUPPORTED           ((EFI_STATUS)(3))
#define EFI_BAD_BUFFER_SIZE       ((EFI_STATUS)(4))
#define EFI_BUFFER_TOO_SMALL      ((EFI_STATUS)(5))
#define EFI_NOT_READY             ((EFI_STATUS)(6))
#define EFI_DEVICE_ERROR          ((EFI_STATUS)(7))
#define EFI_WRITE_PROTECTED       ((EFI_STATUS)(8))
#define EFI_OUT_OF_RESOURCES      ((EFI_STATUS)(9))
#define EFI_NOT_FOUND             ((EFI_STATUS)(14))

#define EFI_ERROR(status)         (((EFI_STATUS)(status)) != EFI_SUCCESS)

/* Memory types */
#define EfiReservedMemoryType     0
#define EfiLoaderCode             1
#define EfiLoaderData             2
#define EfiBootServicesCode       3
#define EfiBootServicesData       4
#define EfiRuntimeServicesCode    5
#define EfiRuntimeServicesData    6
#define EfiConventionalMemory     7
#define EfiUnusableMemory         8
#define EfiACPIReclaimMemory      9
#define EfiACPIMemoryNVS          10
#define EfiMemoryMappedIO         11
#define EfiMemoryMappedIOPortSpace 12
#define EfiPalCode                13

/* File modes */
#define EFI_FILE_MODE_READ        0x0000000000000001

/* EFI GUID */
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* GOP pixel format */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

/* GOP Mode Info */
typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* GOP Mode */
typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* GOP Protocol */
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GRAPHICS_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t ModeNumber,
    UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GRAPHICS_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t ModeNumber);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_QUERY_MODE QueryMode;
    EFI_GRAPHICS_SET_MODE   SetMode;
    /* Blt omitted - not needed */
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* EFI Memory Descriptor */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* EFI File Info */
typedef struct {
    uint64_t Size;
    uint64_t FileSize;
    uint64_t PhysicalSize;
    uint32_t Attribute;
    uint32_t Pad;
    /* Time fields omitted for simplicity */
    uint8_t  FileName[1]; /* Variable length */
} EFI_FILE_INFO;

/* EFI File Protocol */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_OPEN_FN)(
    EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_CLOSE_FN)(
    EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_READ_FN)(
    EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_GET_INFO_FN)(
    EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
    UINTN *BufferSize, void *Buffer);

struct _EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_FILE_OPEN_FN      FileOpen;
    EFI_FILE_CLOSE_FN     FileClose;
    void *Delete;
    EFI_FILE_READ_FN      FileRead;
    void *Write;
    void *GetPosition;
    void *SetPosition;
    EFI_FILE_GET_INFO_FN  GetInfo;
    void *SetInfo;
    void *Flush;
};

/* Simple File System Protocol */
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_SIMPLE_FILE_OPEN_VOLUME)(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_SIMPLE_FILE_OPEN_VOLUME OpenVolume;
};

/* Loaded Image Protocol - exact UEFI spec layout for x86_64 */
typedef struct {
    uint32_t Revision;                   /* 0x00 */
    EFI_HANDLE ParentHandle;             /* 0x08 */
    EFI_PHYSICAL_ADDRESS SystemTable;    /* 0x10 */
    EFI_HANDLE DeviceHandle;             /* 0x18 - offset 0x18 on x86_64 */
    void *FilePath;                      /* 0x20 */
    void *Reserved;                      /* 0x28 */
    uint32_t LoadOptionsSize;            /* 0x30 */
    void *LoadOptions;                   /* 0x38 */
    void *ImageBase;                     /* 0x40 */
    uint64_t ImageSize;                  /* 0x48 */
    int ImageCodeType;                   /* 0x50 - EFI_MEMORY_TYPE is int */
    int ImageDataType;                   /* 0x54 */
    void *Unload;                        /* 0x58 */
} EFI_LOADED_IMAGE_PROTOCOL;

/* Boot Services function pointer types */
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_ALLOCATE_PAGES)(
    int Type, int MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_ALLOCATE_POOL)(
    int PoolType, UINTN Size, void **Buffer);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FREE_POOL)(void *Buffer);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol, void *Registration, void **Interface);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle, UINTN MapKey);
typedef void (__attribute__((ms_abi)) *EFI_COPY_MEM)(void *Destination, const void *Source, UINTN Length);
typedef void (__attribute__((ms_abi)) *EFI_SET_MEM)(void *Buffer, UINTN Size, UINT8 Value);

/* Boot Services Table - exact UEFI spec layout for x86_64 */
typedef struct {
    /* Header (24 bytes) */
    uint64_t TableSignature;        /* 0x000 */
    uint32_t TableRevision;         /* 0x008 */
    uint32_t HeaderSize;            /* 0x00C */
    uint32_t Crc32;                 /* 0x010 */
    uint32_t Reserved;              /* 0x014 */

    /* Task priority functions */
    void *RaiseTPL;                 /* 0x018 */
    void *RestoreTPL;               /* 0x020 */

    /* Memory functions */
    EFI_ALLOCATE_PAGES   AllocatePages;   /* 0x028 */
    EFI_FREE_PAGES       FreePages;       /* 0x030 */
    EFI_ALLOCATE_POOL    AllocatePool;    /* 0x038 */
    EFI_FREE_POOL        FreePool;        /* 0x040 */

    EFI_COPY_MEM         CopyMem;         /* 0x048 */
    EFI_SET_MEM          SetMem;          /* 0x050 */

    EFI_GET_MEMORY_MAP   GetMemoryMap;    /* 0x058 */

    /* Protocol handler functions */
    EFI_HANDLE_PROTOCOL  HandleProtocol;  /* 0x060 */
    void *Reserved1;                      /* 0x068 */
    void *RegisterProtocolNotify;         /* 0x070 */
    void *LocateHandle;                   /* 0x078 */
    void *LocateDevicePath;               /* 0x080 */
    void *InstallConfigurationTable;      /* 0x088 */

    /* Image functions */
    void *LoadImage;                      /* 0x090 */
    void *StartImage;                     /* 0x098 */
    void *Exit;                           /* 0x0A0 */
    void *UnloadImage;                    /* 0x0A8 */

    EFI_EXIT_BOOT_SERVICES ExitBootServices; /* 0x0B0 */

    /* Misc functions */
    void *GetNextMonotonicCount;          /* 0x0B8 */
    void *Stall;                          /* 0x0C0 */
    void *SetWatchdogTimer;               /* 0x0C8 */

    /* Driver support functions */
    void *ConnectController;              /* 0x0D0 */
    void *DisconnectController;           /* 0x0D8 */

    /* Protocol open/close functions */
    void *OpenProtocol;                   /* 0x0E0 */
    void *CloseProtocol;                  /* 0x0E8 */
    void *OpenProtocolInformation;        /* 0x0F0 */

    void *ProtocolsPerHandle;             /* 0x0F8 */
    void *LocateHandleBuffer;             /* 0x100 */

    EFI_LOCATE_PROTOCOL  LocateProtocol;  /* 0x108 */

    void *InstallMultipleProtocolInterfaces;    /* 0x110 */
    void *UninstallMultipleProtocolInterfaces;  /* 0x118 */
    void *CalculateCrc32;                       /* 0x120 */
} EFI_BOOT_SERVICES;

/* Runtime Services - we just need the pointer */
typedef struct {
    uint64_t TableSignature;
    uint32_t TableRevision;
    uint32_t HeaderSize;
    uint32_t Crc32;
    uint32_t Reserved;
    /* Rest omitted - kernel will access via efi_runtime_services pointer */
} EFI_RUNTIME_SERVICES;

/* Configuration Table Entry */
typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* Simple Text Output Protocol */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_STRING OutputString;
    /* More fields omitted */
};

/* System Table - exact UEFI spec layout for x86_64 */
typedef struct {
    uint64_t TableSignature;                        /* 0x00 */
    uint32_t TableRevision;                         /* 0x08 */
    uint32_t HeaderSize;                            /* 0x0C */
    uint32_t Crc32;                                 /* 0x10 */
    uint32_t Reserved;                              /* 0x14 */
    CHAR16 *FirmwareVendor;                         /* 0x18 */
    UINTN   FirmwareRevision;                       /* 0x20 - UINTN is 8 bytes on x86_64 */
    EFI_HANDLE ConsoleInHandle;                     /* 0x28 */
    void *ConIn;                                    /* 0x30 */
    EFI_HANDLE ConsoleOutHandle;                    /* 0x38 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;        /* 0x40 */
    EFI_HANDLE StandardErrorHandle;                 /* 0x48 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;        /* 0x50 */
    EFI_RUNTIME_SERVICES *RuntimeServices;          /* 0x58 */
    EFI_BOOT_SERVICES    *BootServices;             /* 0x60 */
    UINTN NumberOfTableEntries;                     /* 0x68 */
    EFI_CONFIGURATION_TABLE *ConfigurationTable;    /* 0x70 */
} EFI_SYSTEM_TABLE;

/* Well-known GUIDs */
#define EFI_GOP_GUID \
    {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}}

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_FILE_INFO_GUID \
    {0x09576E92, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define ACPI_TABLE_GUID \
    {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}

#define ACPI_20_TABLE_GUID \
    {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}

/* ELF definitions for kernel loading */
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define ELFMAG0     0x7F
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'
#define EI_CLASS    4
#define ELFCLASS64  2
#define PT_LOAD     1

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* Allocate type */
#define AllocateAddress 2

#endif /* BOOT_H */
