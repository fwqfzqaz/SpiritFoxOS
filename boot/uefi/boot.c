#include <efi.h>
#include <efilib.h>
#include <eficon.h>
#include <efidevp.h>
#include <efiapi.h>
#include <efiprot.h>
#include <elf.h>

#include "boot.h"

#define KERNEL_PATH     L"\\EFI\\SpiritFoxOS\\kernel.elf"
#define KERNEL_ENTRY    0x100000

typedef void (*kernel_entry_t)(BootInfo *boot_info);

static EFI_STATUS
get_memory_map(EFI_SYSTEM_TABLE *SystemTable,
               EFI_MEMORY_DESCRIPTOR **Map,
               UINTN *MapSize,
               UINTN *MapKey,
               UINTN *DescriptorSize,
               UINT32 *DescriptorVersion)
{
    EFI_STATUS status;

    *MapSize = 0;
    *Map = NULL;

    status = SystemTable->BootServices->GetMemoryMap(
        MapSize, *Map, MapKey, DescriptorSize, DescriptorVersion);
    if (status != EFI_BUFFER_TOO_SMALL)
        return status;

    /* Add extra descriptors in case the map grows between calls */
    *MapSize += 2 * (*DescriptorSize);

    status = SystemTable->BootServices->AllocatePool(
        EfiLoaderData, *MapSize, (void **)Map);
    if (EFI_ERROR(status))
        return status;

    status = SystemTable->BootServices->GetMemoryMap(
        MapSize, *Map, MapKey, DescriptorSize, DescriptorVersion);
    if (EFI_ERROR(status)) {
        SystemTable->BootServices->FreePool(*Map);
        *Map = NULL;
        return status;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS
setup_gop(EFI_SYSTEM_TABLE *SystemTable,
          EFI_GRAPHICS_OUTPUT_PROTOCOL **Gop)
{
    EFI_STATUS status;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    status = SystemTable->BootServices->LocateProtocol(
        &gop_guid, NULL, (void **)Gop);
    return status;
}

static EFI_STATUS
load_kernel_elf(EFI_SYSTEM_TABLE *SystemTable,
                EFI_HANDLE ImageHandle,
                void **kernel_entry)
{
    EFI_STATUS status;
    EFI_LOADED_IMAGE *loaded_image;
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_FILE_PROTOCOL *root;
    EFI_FILE_PROTOCOL *kernel_file;
    UINTN file_info_size;
    EFI_FILE_INFO *file_info;
    UINTN kernel_size;
    void *kernel_buffer;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    UINTN i;

    /* Get the loaded image protocol to access the device handle */
    status = SystemTable->BootServices->HandleProtocol(
        ImageHandle, &loaded_image_guid, (void **)&loaded_image);
    if (EFI_ERROR(status)) {
        Print(L"Failed to get loaded image protocol: %r\n", status);
        return status;
    }

    /* Get the file system protocol from the device */
    status = SystemTable->BootServices->HandleProtocol(
        loaded_image->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(status)) {
        Print(L"Failed to get file system protocol: %r\n", status);
        return status;
    }

    /* Open the root volume */
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        Print(L"Failed to open root volume: %r\n", status);
        return status;
    }

    /* Open the kernel ELF file */
    status = root->Open(root, &kernel_file, KERNEL_PATH,
                        EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"Failed to open kernel file: %r\n", status);
        return status;
    }

    /* Get file info to determine file size */
    file_info_size = 0;
    file_info = NULL;
    status = kernel_file->GetInfo(kernel_file, &gEfiFileInfoGuid,
                                  &file_info_size, file_info);
    if (status == EFI_BUFFER_TOO_SMALL) {
        status = SystemTable->BootServices->AllocatePool(
            EfiLoaderData, file_info_size, (void **)&file_info);
        if (EFI_ERROR(status)) {
            kernel_file->Close(kernel_file);
            return status;
        }
        status = kernel_file->GetInfo(kernel_file, &gEfiFileInfoGuid,
                                      &file_info_size, file_info);
    }
    if (EFI_ERROR(status)) {
        Print(L"Failed to get kernel file info: %r\n", status);
        kernel_file->Close(kernel_file);
        return status;
    }

    kernel_size = file_info->FileSize;
    SystemTable->BootServices->FreePool(file_info);

    /* Read the entire kernel file into a buffer */
    status = SystemTable->BootServices->AllocatePool(
        EfiLoaderData, kernel_size, &kernel_buffer);
    if (EFI_ERROR(status)) {
        Print(L"Failed to allocate kernel buffer: %r\n", status);
        kernel_file->Close(kernel_file);
        return status;
    }

    status = kernel_file->Read(kernel_file, &kernel_size, kernel_buffer);
    kernel_file->Close(kernel_file);
    if (EFI_ERROR(status)) {
        Print(L"Failed to read kernel file: %r\n", status);
        SystemTable->BootServices->FreePool(kernel_buffer);
        return status;
    }

    /* Validate ELF header */
    ehdr = (Elf64_Ehdr *)kernel_buffer;
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        Print(L"Kernel is not a valid ELF file\n");
        SystemTable->BootServices->FreePool(kernel_buffer);
        return EFI_LOAD_ERROR;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        Print(L"Kernel is not a 64-bit ELF\n");
        SystemTable->BootServices->FreePool(kernel_buffer);
        return EFI_LOAD_ERROR;
    }

    /* Load program segments into memory */
    phdr = (Elf64_Phdr *)((uint8_t *)kernel_buffer + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        /* Allocate pages at the desired physical address */
        UINTN pages = (phdr[i].p_memsz + 0xFFF) / 0x1000;
        EFI_PHYSICAL_ADDRESS segment_addr = phdr[i].p_paddr;

        status = SystemTable->BootServices->AllocatePages(
            AllocateAddress, EfiLoaderData, pages, &segment_addr);
        if (EFI_ERROR(status)) {
            Print(L"Failed to allocate segment at 0x%lx: %r\n",
                  phdr[i].p_paddr, status);
            SystemTable->BootServices->FreePool(kernel_buffer);
            return status;
        }

        /* Copy segment data */
        uint8_t *src = (uint8_t *)kernel_buffer + phdr[i].p_offset;
        uint8_t *dst = (uint8_t *)phdr[i].p_paddr;
        UINTN copy_size = phdr[i].p_filesz;

        SystemTable->BootServices->CopyMem(dst, src, copy_size);

        /* Zero out BSS (remaining bytes) */
        if (phdr[i].p_memsz > phdr[i].p_filesz) {
            UINTN bss_size = phdr[i].p_memsz - phdr[i].p_filesz;
            SystemTable->BootServices->SetMem(
                dst + phdr[i].p_filesz, bss_size, 0);
        }
    }

    /* Set kernel entry point */
    *kernel_entry = (void *)(uintptr_t)ehdr->e_entry;

    SystemTable->BootServices->FreePool(kernel_buffer);

    return EFI_SUCCESS;
}

static EFI_STATUS
build_boot_info(EFI_SYSTEM_TABLE *SystemTable,
                EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                EFI_MEMORY_DESCRIPTOR *Map,
                UINTN MapSize,
                UINTN DescriptorSize,
                BootInfo *info)
{
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;
    UINTN info_size;

    info->magic = BOOT_INFO_MAGIC;

    /* Framebuffer info from GOP */
    if (Gop != NULL) {
        Gop->QueryMode(Gop, Gop->Mode->Mode, &info_size, &mode_info);
        info->framebuffer_base = Gop->Mode->FrameBufferBase;
        info->framebuffer_size = Gop->Mode->FrameBufferSize;
        info->width = mode_info->HorizontalResolution;
        info->height = mode_info->VerticalResolution;
        info->pitch = mode_info->PixelsPerScanLine * 4;

        switch (mode_info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
        case PixelBlueGreenRedReserved8BitPerColor:
            info->bpp = 32;
            break;
        case PixelBitMask:
            info->bpp = 32;
            break;
        default:
            info->bpp = 32;
            break;
        }
    } else {
        info->framebuffer_base = 0;
        info->framebuffer_size = 0;
        info->width = 0;
        info->height = 0;
        info->pitch = 0;
        info->bpp = 0;
    }

    /* Memory map info */
    info->memory_map = (uint64_t)(uintptr_t)Map;
    info->memory_map_size = MapSize;
    info->memory_map_descriptor_size = DescriptorSize;
    info->memory_map_entry_count = MapSize / DescriptorSize;

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    void *kernel_entry = NULL;
    BootInfo boot_info;
    kernel_entry_t kernel;

    /* Initialize GNU-EFI */
    InitializeLib(ImageHandle, SystemTable);

    Print(L"SpiritFoxOS UEFI Bootloader\n");
    Print(L"===========================\n");

    /* Step 1: Set up GOP (framebuffer) */
    Print(L"Setting up GOP...\n");
    status = setup_gop(SystemTable, &gop);
    if (EFI_ERROR(status)) {
        Print(L"Warning: GOP setup failed: %r (no framebuffer)\n", status);
        gop = NULL;
    } else {
        Print(L"GOP: %dx%d, BPP: 32, FB: 0x%lx\n",
              gop->Mode->Info->HorizontalResolution,
              gop->Mode->Info->VerticalResolution,
              gop->Mode->FrameBufferBase);
    }

    /* Step 2: Load the kernel ELF */
    Print(L"Loading kernel from %s...\n", KERNEL_PATH);
    status = load_kernel_elf(SystemTable, ImageHandle, &kernel_entry);
    if (EFI_ERROR(status)) {
        Print(L"Failed to load kernel: %r\n", status);
        return status;
    }
    Print(L"Kernel loaded, entry point: 0x%lx\n", (UINTN)kernel_entry);

    /* Step 3: Get the memory map (must be done right before ExitBootServices) */
    Print(L"Getting memory map...\n");
    status = get_memory_map(SystemTable, &memory_map, &map_size,
                            &map_key, &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) {
        Print(L"Failed to get memory map: %r\n", status);
        return status;
    }
    Print(L"Memory map: %d bytes, %d entries\n",
          map_size, map_size / descriptor_size);

    /* Step 4: Build boot info struct */
    status = build_boot_info(SystemTable, gop, memory_map, map_size,
                             descriptor_size, &boot_info);
    if (EFI_ERROR(status)) {
        Print(L"Failed to build boot info: %r\n", status);
        return status;
    }

    /* Step 5: Exit boot services
     * The memory map may change, so we may need to retry. */
    Print(L"Exiting boot services...\n");
    status = SystemTable->BootServices->ExitBootServices(
        ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        /* Map key is stale, get a fresh memory map and retry */
        SystemTable->BootServices->FreePool(memory_map);
        status = get_memory_map(SystemTable, &memory_map, &map_size,
                                &map_key, &descriptor_size,
                                &descriptor_version);
        if (EFI_ERROR(status)) {
            /* Can't print anymore, just halt */
            while (1) __asm__ volatile("pause");
        }

        /* Rebuild boot info with new map */
        boot_info.memory_map = (uint64_t)(uintptr_t)memory_map;
        boot_info.memory_map_size = map_size;
        boot_info.memory_map_descriptor_size = descriptor_size;
        boot_info.memory_map_entry_count = map_size / descriptor_size;

        status = SystemTable->BootServices->ExitBootServices(
            ImageHandle, map_key);
        if (EFI_ERROR(status)) {
            /* Fatal: cannot exit boot services */
            while (1) __asm__ volatile("pause");
        }
    }

    /* Step 6: Jump to the kernel entry point */
    kernel = (kernel_entry_t)kernel_entry;
    kernel(&boot_info);

    /* Should never reach here */
    while (1) __asm__ volatile("pause");

    return EFI_SUCCESS;
}
