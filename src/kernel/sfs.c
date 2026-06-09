/* SpiritFoxOS - SpiritFox文件系统
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "sfs.h"
#include "ata.h"
#include "log.h"
#include "pmm.h"
#include "vmm.h"
#include "../include/string.h"

static ata_device_t *sfs_disk = NULL;
static sfs_superblock_t sfs_sb;
static int sfs_loaded = 0;
static uint32_t sfs_start_lba = 0;

static uint32_t sfs_checksum(const void *data, uint32_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size - 4; i++) {
        sum += p[i];
    }
    return sum;
}

static int sfs_read_sector(uint64_t lba, void *buf) {
    if (!sfs_disk) return -1;
    return ata_read_sectors(sfs_disk, lba, 1, buf);
}

static int sfs_write_sector(uint64_t lba, const void *buf) {
    if (!sfs_disk) return -1;
    return ata_write_sectors(sfs_disk, lba, 1, buf);
}

static int sfs_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    if (!sfs_disk) return -1;
    return ata_read_sectors(sfs_disk, lba, count, buf);
}

static int sfs_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    if (!sfs_disk) return -1;
    return ata_write_sectors(sfs_disk, lba, count, buf);
}

static uint32_t sfs_detect_iso_end(void) {
    uint8_t sector[512];

    /* ISO9660 primary volume descriptor is at LBA 16 */
    if (sfs_read_sector(16, sector) != 0) return 2048;

    /* Check for ISO9660 magic "CD001" at offset 1 */
    if (sector[1] != 'C' || sector[2] != 'D' ||
        sector[3] != '0' || sector[4] != '0' || sector[5] != '1') {
        /* No ISO found, use a safe default */
        return 2048;
    }

    /* Volume Space Size is at offset 80 (both-endian, 32-bit LE at 80, 32-bit BE at 84) */
    uint32_t volume_sectors = *(uint32_t *)&sector[80];

    /* Add 1MB safety margin (2048 sectors) */
    uint32_t safe_start = volume_sectors + 2048;

    /* Align to 2048-sector boundary (1MB alignment for safety) */
    safe_start = (safe_start + 2047) & ~2047;

    LOG_I("sfs", "ISO detected: %u sectors, safe data area starts at LBA %u",
          volume_sectors, safe_start);

    return safe_start;
}

static int sfs_scan_for_superblock(void) {
    /* Try known locations: ISO end area, then fixed fallback positions */
    uint32_t candidates[4];
    int count = 0;

    /* First: detect ISO and use area after it */
    uint32_t iso_end = sfs_detect_iso_end();
    candidates[count++] = iso_end;

    /* Fallback: try fixed positions */
    candidates[count++] = 65536;    /* 32MB offset */
    candidates[count++] = 131072;   /* 64MB offset */
    candidates[count++] = 2048;     /* Original position */

    for (int i = 0; i < count; i++) {
        if (sfs_read_sector(candidates[i], &sfs_sb) != 0) continue;
        if (sfs_sb.magic == SFS_MAGIC && sfs_sb.version == SFS_VERSION) {
            sfs_start_lba = candidates[i];
            return 1;
        }
    }

    return 0;
}

void sfs_init(void) {
    sfs_disk = ata_get_device(0);
    if (!sfs_disk) {
        LOG_W("sfs", "No ATA disk found for filesystem");
        return;
    }

    /* Try to find existing filesystem */
    if (sfs_scan_for_superblock()) {
        sfs_loaded = 1;
        LOG_I("sfs", "Filesystem found at LBA %u (v%u, %u files, data at LBA %u)",
              sfs_start_lba, sfs_sb.version, sfs_sb.file_table_count,
              sfs_sb.data_area_sector);
        return;
    }

    /* No filesystem found - auto-format in safe area after ISO */
    LOG_I("sfs", "No filesystem found, auto-formatting in safe area...");

    sfs_start_lba = sfs_detect_iso_end();
    if (sfs_format() != 0) {
        LOG_E("sfs", "Auto-format failed");
    }
}

int sfs_is_formatted(void) {
    return sfs_loaded;
}

int sfs_format(void) {
    if (!sfs_disk) {
        LOG_E("sfs", "No disk available");
        return -1;
    }

    /* If no LBA set yet, detect safe area */
    if (sfs_start_lba == 0) {
        sfs_start_lba = sfs_detect_iso_end();
    }

    memset(&sfs_sb, 0, sizeof(sfs_sb));
    sfs_sb.magic = SFS_MAGIC;
    sfs_sb.version = SFS_VERSION;
    sfs_sb.total_sectors = sfs_disk->lba48 ? (uint32_t)sfs_disk->sectors_48 : sfs_disk->sectors_28;
    sfs_sb.file_table_sector = sfs_start_lba + 1;
    sfs_sb.file_table_count = SFS_MAX_FILES;
    sfs_sb.data_area_sector = sfs_start_lba + 1 + ((SFS_MAX_FILES * sizeof(sfs_file_entry_t) + 511) / 512);
    sfs_sb.sector_count = sfs_sb.total_sectors - sfs_sb.data_area_sector;
    sfs_sb.checksum = sfs_checksum(&sfs_sb, sizeof(sfs_sb));

    if (sfs_write_sector(sfs_start_lba, &sfs_sb) != 0) {
        LOG_E("sfs", "Failed to write superblock at LBA %u", sfs_start_lba);
        return -1;
    }

    /* Clear file table */
    uint8_t zero_buf[512];
    memset(zero_buf, 0, 512);
    uint32_t ft_sectors = (SFS_MAX_FILES * sizeof(sfs_file_entry_t) + 511) / 512;
    for (uint32_t i = 0; i < ft_sectors; i++) {
        if (sfs_write_sector(sfs_sb.file_table_sector + i, zero_buf) != 0) {
            LOG_E("sfs", "Failed to clear file table sector %u", i);
            return -1;
        }
    }

    sfs_loaded = 1;
    LOG_I("sfs", "Filesystem formatted at LBA %u: table at LBA %u, data at LBA %u (%u sectors)",
          sfs_start_lba, sfs_sb.file_table_sector, sfs_sb.data_area_sector, sfs_sb.sector_count);
    return 0;
}

static int sfs_read_file_table(sfs_file_entry_t *table) {
    if (!sfs_loaded) return -1;
    uint32_t ft_sectors = (SFS_MAX_FILES * sizeof(sfs_file_entry_t) + 511) / 512;
    return sfs_read_sectors(sfs_sb.file_table_sector, ft_sectors, table);
}

static int sfs_write_file_table(sfs_file_entry_t *table) {
    if (!sfs_loaded) return -1;
    uint32_t ft_sectors = (SFS_MAX_FILES * sizeof(sfs_file_entry_t) + 511) / 512;
    return sfs_write_sectors(sfs_sb.file_table_sector, ft_sectors, table);
}

static int sfs_find_entry(sfs_file_entry_t *table, const char *name) {
    for (int i = 0; i < SFS_MAX_FILES; i++) {
        if (table[i].flags == 0 && strcmp(table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int sfs_find_free_entry(sfs_file_entry_t *table) {
    for (int i = 0; i < SFS_MAX_FILES; i++) {
        if (table[i].flags == 1 || table[i].name[0] == '\0') {
            return i;
        }
    }
    return -1;
}

static uint32_t sfs_find_free_sector(sfs_file_entry_t *table, uint32_t count) {
    uint32_t needed = (count + 511) / 512;
    uint32_t max_start = sfs_sb.sector_count - needed;

    for (uint32_t start = 0; start <= max_start; ) {
        int conflict = 0;
        for (int i = 0; i < SFS_MAX_FILES; i++) {
            if (table[i].flags != 0 || table[i].name[0] == '\0') continue;
            uint32_t fstart = table[i].start_sector;
            uint32_t fsize = (table[i].size + 511) / 512;
            if (start < fstart + fsize && start + needed > fstart) {
                start = fstart + fsize;
                conflict = 1;
                break;
            }
        }
        if (!conflict) return start;
    }
    return 0xFFFFFFFF;
}

int sfs_create_file(const char *name) {
    if (!sfs_loaded || !name) return -1;
    int len = 0;
    while (name[len]) len++;
    if (len >= SFS_FILENAME_MAX) return -1;

    sfs_file_entry_t table[SFS_MAX_FILES];
    if (sfs_read_file_table(table) != 0) return -1;

    if (sfs_find_entry(table, name) >= 0) return -1;

    int slot = sfs_find_free_entry(table);
    if (slot < 0) return -1;

    memset(&table[slot], 0, sizeof(sfs_file_entry_t));
    for (int i = 0; i <= len; i++) table[slot].name[i] = name[i];
    table[slot].size = 0;
    table[slot].start_sector = 0;
    table[slot].flags = 0;

    extern volatile uint64_t timer_ticks;
    table[slot].create_time = (uint32_t)(timer_ticks / 100);
    table[slot].modify_time = table[slot].create_time;
    table[slot].checksum = sfs_checksum(&table[slot], sizeof(sfs_file_entry_t));

    return sfs_write_file_table(table);
}

int sfs_write_file(const char *name, const void *data, uint32_t size) {
    if (!sfs_loaded || !name || !data) return -1;

    sfs_file_entry_t table[SFS_MAX_FILES];
    if (sfs_read_file_table(table) != 0) return -1;

    int idx = sfs_find_entry(table, name);
    if (idx < 0) {
        int slot = sfs_find_free_entry(table);
        if (slot < 0) {
            LOG_E("sfs", "No free file table entry");
            return -1;
        }
        memset(&table[slot], 0, sizeof(sfs_file_entry_t));
        int len = 0;
        while (name[len]) len++;
        for (int i = 0; i <= len; i++) table[slot].name[i] = name[i];
        table[slot].flags = 0;
        idx = slot;
    }

    uint32_t sectors_needed = (size + 511) / 512;
    uint32_t old_sectors = (table[idx].size + 511) / 512;

    if (sectors_needed > old_sectors || table[idx].start_sector == 0) {
        uint32_t start = sfs_find_free_sector(table, size);
        if (start == 0xFFFFFFFF) {
            LOG_E("sfs", "No free disk space for %u bytes", size);
            return -1;
        }
        table[idx].start_sector = start;
    }

    uint64_t data_lba = sfs_sb.data_area_sector + table[idx].start_sector;
    if (sfs_write_sectors(data_lba, sectors_needed, data) != 0) {
        LOG_E("sfs", "Failed to write data to LBA %u", (uint32_t)data_lba);
        return -1;
    }

    table[idx].size = size;
    extern volatile uint64_t timer_ticks;
    table[idx].modify_time = (uint32_t)(timer_ticks / 100);
    table[idx].checksum = sfs_checksum(&table[idx], sizeof(sfs_file_entry_t));

    if (sfs_write_file_table(table) != 0) {
        LOG_E("sfs", "Failed to update file table");
        return -1;
    }

    LOG_D("sfs", "Wrote '%s' (%u bytes, %u sectors at LBA %u)",
          name, size, sectors_needed, (uint32_t)data_lba);
    return 0;
}

int sfs_read_file(const char *name, void *buf, uint32_t buf_size, uint32_t *out_size) {
    if (!sfs_loaded || !name || !buf) return -1;

    sfs_file_entry_t table[SFS_MAX_FILES];
    if (sfs_read_file_table(table) != 0) return -1;

    int idx = sfs_find_entry(table, name);
    if (idx < 0) return -1;

    uint32_t read_size = table[idx].size;
    if (read_size > buf_size) read_size = buf_size;

    uint32_t sectors = (table[idx].size + 511) / 512;
    uint64_t data_lba = sfs_sb.data_area_sector + table[idx].start_sector;

    uint32_t alloc_pages = (sectors * 512 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *tmp = (uint8_t *)pmm_alloc_pages(alloc_pages);
    if (!tmp) return -1;
    uint64_t tmp_virt = phys_to_virt((uint64_t)tmp);

    if (sfs_read_sectors(data_lba, sectors, (void *)tmp_virt) != 0) {
        for (uint32_t p = 0; p < alloc_pages; p++) pmm_free_page((uint64_t)tmp + p * PAGE_SIZE);
        return -1;
    }

    memcpy(buf, (void *)tmp_virt, read_size);
    for (uint32_t p = 0; p < alloc_pages; p++) pmm_free_page((uint64_t)tmp + p * PAGE_SIZE);

    if (out_size) *out_size = read_size;
    return 0;
}

int sfs_delete_file(const char *name) {
    if (!sfs_loaded || !name) return -1;

    sfs_file_entry_t table[SFS_MAX_FILES];
    if (sfs_read_file_table(table) != 0) return -1;

    int idx = sfs_find_entry(table, name);
    if (idx < 0) return -1;

    table[idx].flags = 1;
    table[idx].name[0] = '\0';
    table[idx].size = 0;
    table[idx].checksum = 0;

    return sfs_write_file_table(table);
}

int sfs_list_files(sfs_file_entry_t *entries, int max_entries) {
    if (!sfs_loaded) return 0;

    sfs_file_entry_t table[SFS_MAX_FILES];
    if (sfs_read_file_table(table) != 0) return 0;

    int count = 0;
    for (int i = 0; i < SFS_MAX_FILES && count < max_entries; i++) {
        if (table[i].flags == 0 && table[i].name[0] != '\0') {
            entries[count++] = table[i];
        }
    }
    return count;
}

int sfs_file_exists(const char *name) {
    if (!sfs_loaded) return 0;
    sfs_file_entry_t table[SFS_MAX_FILES];
    if (sfs_read_file_table(table) != 0) return 0;
    return sfs_find_entry(table, name) >= 0;
}

int sfs_get_file_size(const char *name, uint32_t *size) {
    if (!sfs_loaded || !name || !size) return -1;
    sfs_file_entry_t table[SFS_MAX_FILES];
    if (sfs_read_file_table(table) != 0) return -1;
    int idx = sfs_find_entry(table, name);
    if (idx < 0) return -1;
    *size = table[idx].size;
    return 0;
}
