#include "acpi.h"
#include "hal.h"
#include "string.h"
#include "boot.h"

extern BootInfo *g_boot_info;

static acpi_rsdp_t *rsdp = NULL;
static acpi_sdt_header_t *rsdt = NULL;
static acpi_sdt_header_t *xsdt = NULL;
static uint32_t rsdt_entry_count = 0;
static int use_xsdt = 0;

/* MADT 解析结果 */
static uintptr_t lapic_addr = 0;
static uintptr_t ioapic_addr = 0;
static uint32_t  ioapic_gsi_base = 0;
static void     *madt_start = NULL;
static uint32_t  madt_length = 0;

/* 多 IOAPIC 支持 - 实体机可能有多个 IOAPIC */
#define MAX_IOAPICS  8
typedef struct {
    uintptr_t addr;
    uint32_t  gsi_base;
    uint32_t  gsi_end;     /* gsi_base + max_redirections */
} ioapic_info_t;

static ioapic_info_t ioapic_list[MAX_IOAPICS];
static int ioapic_count = 0;

static int acpi_checksum(const void *table, size_t length)
{
    const uint8_t *p = (const uint8_t *)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += p[i];
    return sum == 0;
}

static acpi_rsdp_t *acpi_scan_for_rsdp(uintptr_t start, uintptr_t end)
{
    for (uintptr_t addr = start; addr < end; addr += 16) {
        hal_ensure_mapped(addr, 16);
        const char *sig = (const char *)addr;
        if (memcmp(sig, "RSD PTR ", 8) != 0)
            continue;
        /* 验证前 20 字节（ACPI 1.0 校验和） */
        if (!acpi_checksum((const void *)addr, 20))
            continue;
        /* 如果修订版 >= 2，还需验证完整长度的扩展校验和 */
        acpi_rsdp_t *candidate = (acpi_rsdp_t *)addr;
        if (candidate->revision >= 2) {
            hal_ensure_mapped(addr, candidate->length);
            if (!acpi_checksum((const void *)addr, candidate->length))
                continue;
        }
        return candidate;
    }
    return NULL;
}

static acpi_rsdp_t *acpi_find_rsdp(void)
{
    /* 步骤 1：搜索 EBDA */
    hal_ensure_mapped(0x40E, 2);
    uint16_t ebda_segment = *(volatile uint16_t *)0x40E;
    uintptr_t ebda_addr = (uintptr_t)ebda_segment << 4;
    if (ebda_addr >= 0x100000)
        ebda_addr = 0; /* 合理性检查 */
    if (ebda_addr != 0) {
        acpi_rsdp_t *found = acpi_scan_for_rsdp(ebda_addr, ebda_addr + 0x400);
        if (found)
            return found;
    }

    /* 步骤 2：搜索 0xE0000 - 0xFFFFF */
    return acpi_scan_for_rsdp(0xE0000, 0x100000);
}

void acpi_init(void)
{
    /* 首先尝试从启动信息获取 RSDP（UEFI 直接提供） */
    if (g_boot_info && g_boot_info->acpi_rsdp != 0) {
        uintptr_t rsdp_addr = (uintptr_t)g_boot_info->acpi_rsdp;
        hal_ensure_mapped(rsdp_addr, sizeof(acpi_rsdp_t));
        acpi_rsdp_t *candidate = (acpi_rsdp_t *)rsdp_addr;
        if (memcmp(candidate->signature, "RSD PTR ", 8) == 0) {
            if (candidate->revision >= 2) {
                if (acpi_checksum(candidate, candidate->length)) {
                    rsdp = candidate;
                }
            } else {
                if (acpi_checksum(candidate, 20)) {
                    rsdp = candidate;
                }
            }
        }
    }

    /* 如果通过启动信息未找到 RSDP，则回退到传统扫描 */
    if (!rsdp) {
        rsdp = acpi_find_rsdp();
    }

    if (!rsdp)
        return;

    /* 优先使用 XSDT 用于 ACPI 2.0+（UEFI 提供修订版 >= 2） */
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        hal_ensure_mapped(rsdp->xsdt_address, sizeof(acpi_sdt_header_t));
        xsdt = (acpi_sdt_header_t *)(uintptr_t)rsdp->xsdt_address;

        hal_ensure_mapped(rsdp->xsdt_address, xsdt->length);
        if (memcmp(xsdt->signature, "XSDT", 4) == 0 && acpi_checksum(xsdt, xsdt->length)) {
            use_xsdt = 1;
            rsdt_entry_count = (xsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);
        } else {
            xsdt = NULL;
        }
    }

    /* 回退到 RSDT */
    if (!use_xsdt) {
        hal_ensure_mapped(rsdp->rsdt_address, sizeof(acpi_sdt_header_t));
        rsdt = (acpi_sdt_header_t *)(uintptr_t)rsdp->rsdt_address;

        hal_ensure_mapped(rsdp->rsdt_address, rsdt->length);
        if (!acpi_checksum(rsdt, rsdt->length)) {
            rsdt = NULL;
            return;
        }

        rsdt_entry_count = (rsdt->length - sizeof(acpi_sdt_header_t)) / sizeof(uint32_t);
    }

    /* 解析 MADT */
    void *madt_ptr = acpi_find_table("APIC");
    if (madt_ptr) {
        acpi_sdt_header_t *madt_hdr = (acpi_sdt_header_t *)madt_ptr;
        madt_start = madt_ptr;
        madt_length = madt_hdr->length;

        /* MADT 布局：头部（36 字节）+ 4 字节本地 APIC 地址 + 4 字节标志 + 条目 */
        if (madt_hdr->length >= sizeof(acpi_sdt_header_t) + 8) {
            uint32_t *madt_fields = (uint32_t *)((uintptr_t)madt_ptr + sizeof(acpi_sdt_header_t));
            lapic_addr = (uintptr_t)madt_fields[0];
        }

        /* 遍历 MADT 条目 */
        uintptr_t entry_offset = sizeof(acpi_sdt_header_t) + 8;
        while (entry_offset < madt_hdr->length) {
            madt_entry_t *entry = (madt_entry_t *)((uintptr_t)madt_ptr + entry_offset);
            if (entry->length == 0)
                break; /* 防止畸形数据导致的无限循环 */

            if (entry->type == MADT_TYPE_IOAPIC) {
                madt_ioapic_t *ioapic = (madt_ioapic_t *)entry;
                ioapic_addr = (uintptr_t)ioapic->ioapic_addr;
                ioapic_gsi_base = ioapic->gsi_base;

                /* 添加到多 IOAPIC 列表 */
                if (ioapic_count < MAX_IOAPICS) {
                    ioapic_list[ioapic_count].addr = ioapic_addr;
                    ioapic_list[ioapic_count].gsi_base = ioapic->gsi_base;
                    /* gsi_end 暂时设为 0，由 apic_init() 中读取 IOAPIC 版本后更新 */
                    ioapic_list[ioapic_count].gsi_end = 0;
                    ioapic_count++;
                }
            }

            entry_offset += entry->length;
        }
    }
}

acpi_rsdp_t *acpi_get_rsdp(void)
{
    return rsdp;
}

void *acpi_find_table(const char *signature)
{
    if (rsdt_entry_count == 0)
        return NULL;

    if (use_xsdt) {
        /* XSDT：64 位条目 */
        uint64_t *entries = (uint64_t *)((uintptr_t)xsdt + sizeof(acpi_sdt_header_t));
        for (uint32_t i = 0; i < rsdt_entry_count; i++) {
            uintptr_t entry_addr = (uintptr_t)entries[i];
            if (entry_addr == 0) continue;
            hal_ensure_mapped(entry_addr, sizeof(acpi_sdt_header_t));
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)entry_addr;
            if (memcmp(hdr->signature, signature, 4) == 0) {
                hal_ensure_mapped(entry_addr, hdr->length);
                if (acpi_checksum(hdr, hdr->length))
                    return (void *)hdr;
            }
        }
    } else {
        /* RSDT：32 位条目 */
        if (!rsdt) return NULL;
        for (uint32_t i = 0; i < rsdt_entry_count; i++) {
            uint32_t entry_addr = ((uint32_t *)((uintptr_t)rsdt + sizeof(acpi_sdt_header_t)))[i];
            hal_ensure_mapped(entry_addr, sizeof(acpi_sdt_header_t));
            acpi_sdt_header_t *hdr = (acpi_sdt_header_t *)(uintptr_t)entry_addr;
            if (memcmp(hdr->signature, signature, 4) == 0) {
                hal_ensure_mapped(entry_addr, hdr->length);
                if (acpi_checksum(hdr, hdr->length))
                    return (void *)hdr;
            }
        }
    }
    return NULL;
}

uintptr_t acpi_get_lapic_addr(void)
{
    return lapic_addr;
}

uintptr_t acpi_get_ioapic_addr(void)
{
    return ioapic_addr;
}

uint32_t acpi_get_ioapic_gsi_base(void)
{
    return ioapic_gsi_base;
}

int acpi_get_irq_override(uint8_t isa_irq, uint32_t *gsi, uint16_t *flags)
{
    if (!madt_start || madt_length == 0)
        return -1;

    uintptr_t entry_offset = sizeof(acpi_sdt_header_t) + 8;
    while (entry_offset < madt_length) {
        madt_entry_t *entry = (madt_entry_t *)((uintptr_t)madt_start + entry_offset);
        if (entry->length == 0)
            break;

        if (entry->type == MADT_TYPE_OVERRIDE) {
            madt_override_t *ovr = (madt_override_t *)entry;
            if (ovr->irq_source == isa_irq) {
                if (gsi)
                    *gsi = ovr->gsi_target;
                if (flags)
                    *flags = ovr->flags;
                return 0;
            }
        }

        entry_offset += entry->length;
    }

    return -1;
}

int acpi_get_ioapic_for_gsi(uint32_t gsi, uintptr_t *ioapic_addr_out,
                             uint32_t *pin_offset_out)
{
    /* 在多 IOAPIC 列表中查找负责此 GSI 的 IOAPIC */
    for (int i = 0; i < ioapic_count; i++) {
        if (ioapic_list[i].gsi_end > 0) {
            /* gsi_end 已被 apic_init 更新 */
            if (gsi >= ioapic_list[i].gsi_base && gsi < ioapic_list[i].gsi_end) {
                if (ioapic_addr_out) *ioapic_addr_out = ioapic_list[i].addr;
                if (pin_offset_out) *pin_offset_out = gsi - ioapic_list[i].gsi_base;
                return 0;
            }
        } else {
            /* 单 IOAPIC 回退：假设所有 GSI 都路由到此 IOAPIC */
            if (gsi >= ioapic_list[i].gsi_base) {
                if (ioapic_addr_out) *ioapic_addr_out = ioapic_list[i].addr;
                if (pin_offset_out) *pin_offset_out = gsi - ioapic_list[i].gsi_base;
                return 0;
            }
        }
    }

    /* 回退到默认 IOAPIC */
    if (ioapic_addr != 0) {
        if (ioapic_addr_out) *ioapic_addr_out = ioapic_addr;
        if (pin_offset_out) *pin_offset_out = gsi - ioapic_gsi_base;
        return 0;
    }

    return -1;
}

int acpi_get_ioapic_count(void)
{
    return ioapic_count;
}
