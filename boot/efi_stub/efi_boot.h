#ifndef EFI_BOOT_H
#define EFI_BOOT_H

/*
 * SpiritFoxOS EFI Stub - 类型定义
 *
 * BootInfo 布局由 include/sf.h 统一定义，以便 stub 构造的
 * 启动信息能被内核直接消费。
 */

#include <stdint.h>
#include "sf.h"

/* =========================================
 * Boot Info (与 Core 0 共用 include/sf.h)
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

typedef SfBootInfo BootInfo;

/* =========================================
 * EFI Type Definitions (self-contained, 无 gnu-efi 依赖)
 * ========================================= */
typedef void *EFI_HANDLE;
typedef uint64_t EFI_STATUS;
typedef uint16_t CHAR16;
typedef uint64_t UINTN;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint8_t UINT8;

#define EFIERR(a)                 (0x8000000000000000ULL | (a))

#define EFI_SUCCESS               0
#define EFI_LOAD_ERROR            ((EFI_STATUS)EFIERR(1))
#define EFI_INVALID_PARAMETER     ((EFI_STATUS)EFIERR(2))
#define EFI_UNSUPPORTED           ((EFI_STATUS)EFIERR(3))
#define EFI_BAD_BUFFER_SIZE       ((EFI_STATUS)EFIERR(4))
#define EFI_BUFFER_TOO_SMALL      ((EFI_STATUS)EFIERR(5))
#define EFI_NOT_READY             ((EFI_STATUS)EFIERR(6))
#define EFI_DEVICE_ERROR          ((EFI_STATUS)EFIERR(7))
#define EFI_WRITE_PROTECTED       ((EFI_STATUS)EFIERR(8))
#define EFI_OUT_OF_RESOURCES      ((EFI_STATUS)EFIERR(9))
#define EFI_NOT_FOUND             ((EFI_STATUS)EFIERR(14))

#define EFI_ERROR(status)         (((EFI_STATUS)(status)) != EFI_SUCCESS)

/* 内存类型 */
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

#define EFI_FILE_MODE_READ        0x0000000000000001ULL

/* EFI GUID */
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* GOP 像素格式 */
typedef uint32_t EFI_GRAPHICS_PIXEL_FORMAT;
#define PixelRedGreenBlueReserved8BitPerColor 0
#define PixelBlueGreenRedReserved8BitPerColor 1
#define PixelBitMask  2
#define PixelBltOnly  3
#define PixelFormatMax 4

typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GRAPHICS_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t ModeNumber,
    UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GRAPHICS_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t ModeNumber);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_QUERY_MODE QueryMode;
    EFI_GRAPHICS_SET_MODE   SetMode;
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* EFI 内存描述符 */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* 已加载映像协议 - x86_64 精确布局 */
typedef struct {
    uint32_t Revision;                   /* 0x00 */
    EFI_HANDLE ParentHandle;             /* 0x08 */
    EFI_PHYSICAL_ADDRESS SystemTable;    /* 0x10 */
    EFI_HANDLE DeviceHandle;             /* 0x18 */
    void *FilePath;                      /* 0x20 */
    void *Reserved;                      /* 0x28 */
    uint32_t LoadOptionsSize;            /* 0x30 */
    void *LoadOptions;                   /* 0x38 */
    void *ImageBase;                     /* 0x40 */
    uint64_t ImageSize;                  /* 0x48 */
    int ImageCodeType;                   /* 0x50 */
    int ImageDataType;                   /* 0x54 */
    void *Unload;                        /* 0x58 */
} EFI_LOADED_IMAGE_PROTOCOL;

/* 引导服务函数指针类型 */
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

/* 引导服务表 - x86_64 精确布局 */
typedef struct {
    uint64_t TableSignature;        /* 0x000 */
    uint32_t TableRevision;         /* 0x008 */
    uint32_t HeaderSize;            /* 0x00C */
    uint32_t Crc32;                 /* 0x010 */
    uint32_t Reserved;              /* 0x014 */

    void *RaiseTPL;                 /* 0x018 */
    void *RestoreTPL;               /* 0x020 */

    EFI_ALLOCATE_PAGES   AllocatePages;      /* 0x028 */
    EFI_FREE_PAGES       FreePages;          /* 0x030 */
    EFI_GET_MEMORY_MAP   GetMemoryMap;       /* 0x038 */
    EFI_ALLOCATE_POOL    AllocatePool;       /* 0x040 */
    EFI_FREE_POOL        FreePool;           /* 0x048 */

    void *CreateEvent;                         /* 0x050 */
    void *SetTimer;                            /* 0x058 */
    void *WaitForEvent;                        /* 0x060 */
    void *SignalEvent;                         /* 0x068 */
    void *CloseEvent;                          /* 0x070 */
    void *CheckEvent;                          /* 0x078 */

    void *InstallProtocolInterface;            /* 0x080 */
    void *ReinstallProtocolInterface;          /* 0x088 */
    void *UninstallProtocolInterface;          /* 0x090 */
    EFI_HANDLE_PROTOCOL  HandleProtocol;       /* 0x098 */
    void *PCHandleProtocol;                    /* 0x0A0 */
    void *RegisterProtocolNotify;              /* 0x0A8 */
    void *LocateHandle;                        /* 0x0B0 */
    void *LocateDevicePath;                    /* 0x0B8 */
    void *InstallConfigurationTable;           /* 0x0C0 */

    void *LoadImage;                           /* 0x0C8 */
    void *StartImage;                          /* 0x0D0 */
    void *Exit;                                /* 0x0D8 */
    void *UnloadImage;                         /* 0x0E0 */

    EFI_EXIT_BOOT_SERVICES ExitBootServices;   /* 0x0E8 */

    void *GetNextMonotonicCount;               /* 0x0F0 */
    void *Stall;                               /* 0x0F8 */
    void *SetWatchdogTimer;                    /* 0x100 */

    void *ConnectController;                   /* 0x108 */
    void *DisconnectController;                /* 0x110 */

    void *OpenProtocol;                        /* 0x118 */
    void *CloseProtocol;                       /* 0x120 */
    void *OpenProtocolInformation;             /* 0x128 */

    void *ProtocolsPerHandle;                  /* 0x130 */
    void *LocateHandleBuffer;                  /* 0x138 */

    EFI_LOCATE_PROTOCOL  LocateProtocol;       /* 0x140 */

    void *InstallMultipleProtocolInterfaces;   /* 0x148 */
    void *UninstallMultipleProtocolInterfaces; /* 0x150 */

    void *CalculateCrc32;                      /* 0x158 */

    EFI_COPY_MEM         CopyMem;              /* 0x160 */
    EFI_SET_MEM          SetMem;               /* 0x168 */

    void *CreateEventEx;                       /* 0x170 */
} EFI_BOOT_SERVICES;

/* 运行时服务 - 仅需头部 */
typedef struct {
    uint64_t TableSignature;
    uint32_t TableRevision;
    uint32_t HeaderSize;
    uint32_t Crc32;
    uint32_t Reserved;
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_STRING OutputString;
};

/* 系统表 - x86_64 精确布局 */
typedef struct {
    uint64_t TableSignature;                        /* 0x00 */
    uint32_t TableRevision;                         /* 0x08 */
    uint32_t HeaderSize;                            /* 0x0C */
    uint32_t Crc32;                                 /* 0x10 */
    uint32_t Reserved;                              /* 0x14 */
    CHAR16 *FirmwareVendor;                         /* 0x18 */
    UINTN   FirmwareRevision;                       /* 0x20 */
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

#define ACPI_TABLE_GUID \
    {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}

#define ACPI_20_TABLE_GUID \
    {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}

/* AllocatePages 分配类型 */
#define AllocateAddress 2

#endif /* EFI_BOOT_H */
