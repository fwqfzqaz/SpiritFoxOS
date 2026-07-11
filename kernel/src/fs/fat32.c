#include "fat32.h"
#include "blkdev.h"
#include "string.h"
#include "memory.h"
#include "vga.h"

/* ========================================================================
 * fat32 - 支持读写的 FAT32 文件系统驱动
 *
 * 与 VFS 层对接，提供 FAT32 格式块设备上的目录列表、文件读取、
 * 文件写入、文件/目录创建和删除功能。
 * ======================================================================== */

/* 下一个 inode 编号 */
static uint64_t fat32_next_ino = 1;

/* ========================================================================
 * 底层辅助函数
 * ======================================================================== */

/* 从磁盘读取完整簇到 buf。
 * buf 至少需要 (sectors_per_cluster * bytes_per_sector) 字节。
 * 成功返回 0，失败返回负数。 */
static int fat32_read_cluster(fat32_sb_data_t *sb_data, uint32_t cluster, void *buf)
{
    if (cluster < 2)
        return -1;

    uint64_t lba = sb_data->data_start_sector +
                   (uint64_t)(cluster - 2) * sb_data->sectors_per_cluster;
    uint32_t count = sb_data->sectors_per_cluster;

    return blkdev_read(sb_data->blkdev_id, lba, count, buf);
}

/* 将 buf 中的完整簇写入磁盘。
 * buf 至少需要 (sectors_per_cluster * bytes_per_sector) 字节。
 * 成功返回 0，失败返回负数。 */
static int fat32_write_cluster(fat32_sb_data_t *sb_data, uint32_t cluster, const void *buf)
{
    if (cluster < 2)
        return -1;

    uint64_t lba = sb_data->data_start_sector +
                   (uint64_t)(cluster - 2) * sb_data->sectors_per_cluster;
    uint32_t count = sb_data->sectors_per_cluster;

    return blkdev_write(sb_data->blkdev_id, lba, count, buf);
}

/* 读取给定簇的 FAT 表项。
 * 返回下一个簇号，或 >= FAT32_EOC_MIN 的值表示链结束。 */
static uint32_t fat32_get_fat_entry(fat32_sb_data_t *sb_data, uint32_t cluster)
{
    /* 每个 FAT 表项为 4 字节 */
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t fat_sector = sb_data->fat_start_sector + fat_offset / sb_data->bytes_per_sector;
    uint32_t entry_offset = fat_offset % sb_data->bytes_per_sector;

    /* 读取包含 FAT 表项的扇区 */
    /* 使用完整页面作为临时缓冲区 */
    void *sector_buf = alloc_page();
    if (!sector_buf)
        return 0x0FFFFFFF; /* 分配失败时返回 EOC */

    int ret = blkdev_read(sb_data->blkdev_id, fat_sector, 1, sector_buf);
    if (ret != 0) {
        free_page(sector_buf);
        return 0x0FFFFFFF;
    }

    uint32_t entry = *(uint32_t *)((uint8_t *)sector_buf + entry_offset);
    free_page(sector_buf);

    /* FAT32 仅使用低 28 位 */
    return entry & 0x0FFFFFFF;
}

/* 写入给定簇的 FAT 表项。
 * 保留现有表项的高 4 位。
 * 成功返回 0，失败返回负数。 */
static int fat32_set_fat_entry(fat32_sb_data_t *sb_data, uint32_t cluster, uint32_t value)
{
    /* 每个 FAT 表项为 4 字节 */
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t fat_sector = sb_data->fat_start_sector + fat_offset / sb_data->bytes_per_sector;
    uint32_t entry_offset = fat_offset % sb_data->bytes_per_sector;

    /* 读取包含 FAT 表项的扇区 */
    void *sector_buf = alloc_page();
    if (!sector_buf)
        return -1;

    int ret = blkdev_read(sb_data->blkdev_id, fat_sector, 1, sector_buf);
    if (ret != 0) {
        free_page(sector_buf);
        return -2;
    }

    /* 修改表项，保留高 4 位 */
    uint32_t *entry_ptr = (uint32_t *)((uint8_t *)sector_buf + entry_offset);
    *entry_ptr = (*entry_ptr & 0xF0000000) | (value & 0x0FFFFFFF);

    /* 将扇区写回 */
    ret = blkdev_write(sb_data->blkdev_id, fat_sector, 1, sector_buf);
    free_page(sector_buf);

    return ret;
}

/* 从 FAT 表中分配一个空闲簇。
 * 从 next_free_cluster 提示位置开始扫描。
 * 将分配的簇标记为链结束。
 * 成功返回 0，失败返回负数。 */
static int fat32_alloc_cluster(fat32_sb_data_t *sb_data, uint32_t *out_cluster)
{
    uint32_t start = sb_data->next_free_cluster;
    if (start < 2) start = 2;

    /* 扫描 FAT 表项寻找空闲项（值 == 0） */
    for (uint32_t i = start; i < sb_data->total_clusters + 2; i++) {
        uint32_t val = fat32_get_fat_entry(sb_data, i);
        if (val == 0) {
            /* 找到空闲簇 - 标记为链结束 */
            int ret = fat32_set_fat_entry(sb_data, i, 0x0FFFFFFF);
            if (ret != 0)
                return ret;
            *out_cluster = i;
            sb_data->next_free_cluster = i + 1;
            if (sb_data->next_free_cluster >= sb_data->total_clusters + 2)
                sb_data->next_free_cluster = 2;
            return 0;
        }
    }

    /* 回绕并从簇 2 开始尝试 */
    for (uint32_t i = 2; i < start; i++) {
        uint32_t val = fat32_get_fat_entry(sb_data, i);
        if (val == 0) {
            int ret = fat32_set_fat_entry(sb_data, i, 0x0FFFFFFF);
            if (ret != 0)
                return ret;
            *out_cluster = i;
            sb_data->next_free_cluster = i + 1;
            if (sb_data->next_free_cluster >= sb_data->total_clusters + 2)
                sb_data->next_free_cluster = 2;
            return 0;
        }
    }

    return -1;  /* 没有空闲簇 */
}

/* 在簇链末尾追加新簇。
 * last_cluster 是链中当前的最后一个簇。
 * 成功返回 0，失败返回负数。 */
static int fat32_append_cluster(fat32_sb_data_t *sb_data, uint32_t last_cluster, uint32_t *out_new_cluster)
{
    uint32_t new_cluster;
    int ret = fat32_alloc_cluster(sb_data, &new_cluster);
    if (ret != 0)
        return ret;

    /* 更新最后一个簇的 FAT 表项，指向新簇 */
    ret = fat32_set_fat_entry(sb_data, last_cluster, new_cluster);
    if (ret != 0) {
        /* 失败时释放已分配的簇 */
        fat32_set_fat_entry(sb_data, new_cluster, 0);
        return ret;
    }

    /* 将新簇清零，避免泄漏旧数据 */
    void *zero_buf = alloc_page();
    if (zero_buf) {
        memset(zero_buf, 0, PAGE_SIZE);
        fat32_write_cluster(sb_data, new_cluster, zero_buf);
        free_page(zero_buf);
    }

    *out_new_cluster = new_cluster;
    return 0;
}

/* 将 8.3 格式文件名转换为以空字符结尾的字符串。
 * 去除尾部空格，在文件名和扩展名之间插入 '.'。 */
static void fat32_format_83_name(const uint8_t *name83, char *out, int out_len)
{
    int i = 0;
    int pos = 0;

    /* 复制文件名部分（8 字节），去除尾部空格 */
    for (i = 7; i >= 0 && name83[i] == ' '; i--) {}
    int name_end = i;

    for (i = 0; i <= name_end && pos < out_len - 1; i++) {
        char c = (char)name83[i];
        /* 转换为小写显示（FAT32 以大写存储） */
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        out[pos++] = c;
    }

    /* 扩展名部分（3 字节），去除尾部空格 */
    for (i = 2; i >= 0 && name83[8 + i] == ' '; i--) {}
    int ext_end = i;

    if (ext_end >= 0) {
        if (pos < out_len - 1)
            out[pos++] = '.';
        for (i = 0; i <= ext_end && pos < out_len - 1; i++) {
            char c = (char)name83[8 + i];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            out[pos++] = c;
        }
    }

    out[pos] = '\0';
}

/* 将 8.3 FAT 文件名与以空字符结尾的搜索名进行比较。
 * 搜索名不区分大小写。 */
static int fat32_name_match(const uint8_t *name83, const char *search)
{
    char formatted[VFS_MAX_NAME];
    fat32_format_83_name(name83, formatted, VFS_MAX_NAME);
    return strcmp(formatted, search) == 0;
}

/* 将以空字符结尾的文件名转换为 8.3 FAT 格式。
 * 转换为大写，用空格填充。
 * out83 至少需要 11 字节。 */
static void fat32_name_to_83(const char *name, uint8_t *out83)
{
    /* 用空格初始化 */
    memset(out83, ' ', 11);

    int i = 0;
    int pos = 0;

    /* 复制文件名部分（最多 8 个字符，遇到 '.' 停止） */
    while (name[i] && name[i] != '.' && pos < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        out83[pos++] = (uint8_t)c;
        i++;
    }

    /* 跳到扩展名 */
    if (name[i] == '.') {
        i++;
        pos = 8;
        while (name[i] && pos < 11) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            out83[pos++] = (uint8_t)c;
            i++;
        }
    }
}

/* ========================================================================
 * 目录遍历与路径解析
 * ======================================================================== */

/* 遍历簇链中的目录项。
 * 成功时，用找到的目录项填充 entry，
 * 并将 *out_cluster 设为包含该条目的簇。
 * 找到返回 0，未找到返回负数。 */
static int fat32_dir_lookup(fat32_sb_data_t *sb_data, uint32_t dir_cluster,
                            const char *name, fat32_dir_entry_t *entry)
{
    uint32_t cluster = dir_cluster;
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -1;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -2;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first_byte = entries[i].name[0];

            /* 目录结束 */
            if (first_byte == 0x00) {
                free_page(cluster_buf);
                return -1;
            }

            /* 已删除的条目 - 跳过 */
            if (first_byte == 0xE5)
                continue;

            /* 跳过 LFN 条目 */
            if (entries[i].attr == FAT32_ATTR_LFN)
                continue;

            /* 跳过卷标识 */
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                continue;

            /* 检查名称是否匹配 */
            if (fat32_name_match(entries[i].name, name)) {
                if (entry)
                    memcpy(entry, &entries[i], sizeof(fat32_dir_entry_t));
                free_page(cluster_buf);
                return 0;
            }
        }

        /* 沿簇链继续 */
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    return -1;  /* 未找到 */
}

/* 从根目录解析路径。
 * 用结果填充 start_cluster 和 file_size。
 * 成功返回 0，失败返回负数。 */
static int __attribute__((unused)) fat32_resolve(fat32_sb_data_t *sb_data, const char *path,
                         uint32_t *start_cluster, uint32_t *file_size, uint32_t *is_dir)
{
    if (!path || path[0] == '\0')
        return -1;

    /* 跳过前导斜杠 */
    const char *p = path;
    while (*p == '/') p++;

    /* 如果斜杠后为空路径，返回根目录 */
    if (*p == '\0') {
        *start_cluster = sb_data->root_dir_cluster;
        *file_size = 0;
        *is_dir = 1;
        return 0;
    }

    /* 遍历路径组件 */
    uint32_t current_cluster = sb_data->root_dir_cluster;

    while (*p) {
        /* 提取下一个路径组件 */
        char component[VFS_MAX_NAME];
        int ci = 0;
        while (*p && *p != '/' && ci < VFS_MAX_NAME - 1) {
            component[ci++] = *p++;
        }
        component[ci] = '\0';

        /* 跳过斜杠 */
        while (*p == '/') p++;

        /* 在当前目录中查找此组件 */
        fat32_dir_entry_t entry;
        int ret = fat32_dir_lookup(sb_data, current_cluster, component, &entry);
        if (ret != 0)
            return -2;  /* 未找到 */

        /* 从条目获取簇号 */
        uint32_t entry_cluster = (uint32_t)entry.cluster_hi << 16 | entry.cluster_lo;

        /* 这是最后一个组件吗？ */
        if (*p == '\0') {
            *start_cluster = entry_cluster;
            *file_size = entry.file_size;
            *is_dir = (entry.attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
            return 0;
        }

        /* 中间组件必须是目录 */
        if (!(entry.attr & FAT32_ATTR_DIRECTORY))
            return -3;  /* 不是目录 */

        current_cluster = entry_cluster;
    }

    return -1;
}

/* ========================================================================
 * FAT32 inode 读取操作
 * ======================================================================== */

static int fat32_inode_read(vfs_file_t *file, void *buf, size_t count)
{
    vfs_inode_t *inode = file->inode;
    if (!inode || !inode->fs_data || !inode->sb || !inode->sb->fs_data)
        return -1;

    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)inode->sb->fs_data;

    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint64_t file_size = inode_data->file_size;

    /* 计算剩余字节数 */
    uint64_t remaining = file_size - file->offset;
    if (remaining > count)
        remaining = count;
    if (remaining == 0)
        return 0;

    /* 遍历簇链，找到包含当前偏移的簇 */
    uint32_t cluster = inode_data->start_cluster;
    uint64_t offset = file->offset;

    /* 跳过整个簇以到达正确的簇 */
    while (offset >= cluster_size && cluster >= 2 && cluster < FAT32_EOC_MIN) {
        offset -= cluster_size;
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    if (cluster < 2 || cluster >= FAT32_EOC_MIN)
        return 0;

    /* 逐簇读取数据 */
    size_t done = 0;
    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -2;

    while (done < remaining && cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            break;
        }

        /* 从此簇复制多少数据 */
        uint32_t in_cluster = cluster_size - (uint32_t)offset;
        size_t to_copy = remaining - done;
        if (to_copy > in_cluster)
            to_copy = in_cluster;

        memcpy((uint8_t *)buf + done,
               (uint8_t *)cluster_buf + offset, to_copy);

        done += to_copy;
        offset = 0;  /* 后续簇从偏移 0 开始 */

        /* 沿链继续 */
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    file->offset += done;
    return (int)done;
}

/* ========================================================================
 * 目录项写入辅助函数
 * ======================================================================== */

/* 更新磁盘上已有的目录项（如文件大小变更）。
 * 搜索 parent_cluster 目录项中匹配 name83 的条目，
 * 然后更新 file_size 字段并写回。
 * 成功返回 0，失败返回负数。 */
static int fat32_update_direntry(fat32_sb_data_t *sb_data, uint32_t parent_cluster,
                                  const uint8_t *name83, uint32_t new_start_cluster,
                                  uint32_t new_file_size)
{
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -1;

    uint32_t cluster = parent_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -2;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first_byte = entries[i].name[0];
            if (first_byte == 0x00) {
                free_page(cluster_buf);
                return -1;  /* 未找到 */
            }
            if (first_byte == 0xE5)
                continue;
            if (entries[i].attr == FAT32_ATTR_LFN)
                continue;
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                continue;

            /* 比较 8.3 文件名 */
            if (memcmp(entries[i].name, name83, 11) == 0) {
                /* 找到 - 更新条目 */
                entries[i].file_size = new_file_size;
                entries[i].cluster_hi = (uint16_t)(new_start_cluster >> 16);
                entries[i].cluster_lo = (uint16_t)(new_start_cluster & 0xFFFF);

                /* 将簇写回 */
                ret = fat32_write_cluster(sb_data, cluster, cluster_buf);
                free_page(cluster_buf);
                return ret;
            }
        }

        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    return -1;  /* 未找到 */
}

/* 在父目录的簇链中创建新的目录项。
 * 查找空闲槽位（已删除条目或条目末尾），或扩展链。
 * 成功返回 0，失败返回负数。 */
static int fat32_create_direntry(fat32_sb_data_t *sb_data, uint32_t parent_cluster,
                                  const char *name, uint32_t start_cluster,
                                  uint32_t file_size, uint8_t attr)
{
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    /* 构建 8.3 文件名 */
    uint8_t name83[11];
    fat32_name_to_83(name, name83);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -1;

    uint32_t cluster = parent_cluster;
    uint32_t prev_cluster = 0;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -2;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            /* 查找空闲槽位：已删除条目（0xE5）或结束标记（0x00） */
            if (entries[i].name[0] == 0xE5 || entries[i].name[0] == 0x00) {
                int was_end = (entries[i].name[0] == 0x00);

                /* 填充目录项 */
                memcpy(entries[i].name, name83, 11);
                entries[i].attr = attr;
                entries[i].nt_reserved = 0;
                entries[i].create_time_tenth = 0;
                entries[i].create_time = 0;
                entries[i].create_date = 0;
                entries[i].access_date = 0;
                entries[i].cluster_hi = (uint16_t)(start_cluster >> 16);
                entries[i].modify_time = 0;
                entries[i].modify_date = 0;
                entries[i].cluster_lo = (uint16_t)(start_cluster & 0xFFFF);
                entries[i].file_size = file_size;

                /* 如果使用了条目末尾槽位，确保下一个条目
                 * 也标记为条目末尾（如果有空间） */
                if (was_end && i + 1 < entries_per_cluster) {
                    entries[i + 1].name[0] = 0x00;
                }

                /* 将簇写回 */
                ret = fat32_write_cluster(sb_data, cluster, cluster_buf);
                free_page(cluster_buf);
                return ret;
            }
        }

        prev_cluster = cluster;
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    /* 未找到空闲槽位 - 扩展目录的簇链 */
    if (prev_cluster < 2 || prev_cluster >= FAT32_EOC_MIN) {
        free_page(cluster_buf);
        return -3;  /* 无法扩展 */
    }

    uint32_t new_cluster;
    int ret = fat32_append_cluster(sb_data, prev_cluster, &new_cluster);
    if (ret != 0) {
        free_page(cluster_buf);
        return ret;
    }

    /* 将新簇清零（已由 fat32_append_cluster 完成）并写入条目 */
    memset(cluster_buf, 0, cluster_size);
    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
    memcpy(entries[0].name, name83, 11);
    entries[0].attr = attr;
    entries[0].nt_reserved = 0;
    entries[0].create_time_tenth = 0;
    entries[0].create_time = 0;
    entries[0].create_date = 0;
    entries[0].access_date = 0;
    entries[0].cluster_hi = (uint16_t)(start_cluster >> 16);
    entries[0].modify_time = 0;
    entries[0].modify_date = 0;
    entries[0].cluster_lo = (uint16_t)(start_cluster & 0xFFFF);
    entries[0].file_size = file_size;
    /* 将下一个条目标记为条目末尾 */
    entries[1].name[0] = 0x00;

    ret = fat32_write_cluster(sb_data, new_cluster, cluster_buf);
    free_page(cluster_buf);
    return ret;
}

/* ========================================================================
 * FAT32 inode 写入操作
 * ======================================================================== */

static int fat32_inode_write(vfs_file_t *file, const void *buf, size_t count)
{
    vfs_inode_t *inode = file->inode;
    if (!inode || !inode->fs_data || !inode->sb || !inode->sb->fs_data)
        return -1;

    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)inode->sb->fs_data;

    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;

    /* 如果文件还没有簇，分配第一个 */
    if (inode_data->start_cluster == 0 || inode_data->start_cluster < 2) {
        uint32_t first_cluster;
        int ret = fat32_alloc_cluster(sb_data, &first_cluster);
        if (ret != 0)
            return -2;
        inode_data->start_cluster = first_cluster;
    }

    /* 遍历簇链，找到包含当前偏移的簇。
     * 如果偏移超出当前链，则扩展链。 */
    uint64_t offset = file->offset;
    uint32_t cluster = inode_data->start_cluster;
    uint64_t offset_remaining = offset;

    while (offset_remaining >= cluster_size) {
        uint32_t next = fat32_get_fat_entry(sb_data, cluster);
        if (next >= FAT32_EOC_MIN) {
            /* 需要扩展链 */
            uint32_t new_cluster;
            int ret = fat32_append_cluster(sb_data, cluster, &new_cluster);
            if (ret != 0)
                return -3;
            cluster = new_cluster;
        } else {
            cluster = next;
        }
        offset_remaining -= cluster_size;
    }

    /* 现在 cluster 是包含写入偏移的簇，
     * offset_remaining 是该簇内的偏移。 */

    /* 读-改-写：读取簇，覆盖新数据，写回 */
    size_t done = 0;
    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -4;

    while (done < count) {
        /* 读取当前簇 */
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            break;
        }

        /* 在此簇中写入多少数据 */
        uint32_t in_cluster = cluster_size - (uint32_t)offset_remaining;
        size_t to_copy = count - done;
        if (to_copy > in_cluster)
            to_copy = in_cluster;

        /* 覆盖新数据 */
        memcpy((uint8_t *)cluster_buf + offset_remaining,
               (const uint8_t *)buf + done, to_copy);

        /* 将簇写回 */
        ret = fat32_write_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            break;
        }

        done += to_copy;
        offset_remaining = 0;  /* 后续簇从偏移 0 开始 */

        /* 如果需要写入更多数据，沿链继续或扩展 */
        if (done < count) {
            uint32_t next = fat32_get_fat_entry(sb_data, cluster);
            if (next >= FAT32_EOC_MIN) {
                uint32_t new_cluster;
                ret = fat32_append_cluster(sb_data, cluster, &new_cluster);
                if (ret != 0)
                    break;
                cluster = new_cluster;
            } else {
                cluster = next;
            }
        }
    }

    free_page(cluster_buf);

    /* 更新文件偏移和大小 */
    file->offset += done;
    uint64_t new_size = file->offset;
    if (new_size > inode_data->file_size)
        inode_data->file_size = (uint32_t)new_size;
    if (new_size > inode->size)
        inode->size = new_size;

    /* 更新磁盘上的目录项 */
    fat32_update_direntry(sb_data, inode_data->parent_cluster,
                          inode_data->name83, inode_data->start_cluster,
                          inode_data->file_size);

    return (int)done;
}

/* ========================================================================
 * FAT32 VFS 创建/创建目录/取消链接回调
 * ======================================================================== */

static int fat32_create(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
    if (!parent || !parent->inode || !parent->inode->fs_data || !parent->inode->sb)
        return -1;

    fat32_inode_data_t *parent_data = (fat32_inode_data_t *)parent->inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)parent->inode->sb->fs_data;
    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)inode->fs_data;

    /* 在磁盘上创建目录项，start_cluster=0（尚无数据） */
    int ret = fat32_create_direntry(sb_data, parent_data->start_cluster,
                                     name, 0, 0, FAT32_ATTR_ARCHIVE);
    if (ret != 0)
        return ret;

    /* 存储 8.3 文件名和父簇号，用于后续更新 */
    fat32_name_to_83(name, inode_data->name83);
    inode_data->parent_cluster = parent_data->start_cluster;
    inode_data->start_cluster = 0;
    inode_data->file_size = 0;

    /* 设置 inode 操作 */
    inode->read = fat32_inode_read;
    inode->write = fat32_inode_write;

    return 0;
}

static int fat32_mkdir_cb(vfs_dentry_t *parent, const char *name, vfs_inode_t *inode)
{
    if (!parent || !parent->inode || !parent->inode->fs_data || !parent->inode->sb)
        return -1;

    fat32_inode_data_t *parent_data = (fat32_inode_data_t *)parent->inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)parent->inode->sb->fs_data;
    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)inode->fs_data;

    /* 为新目录分配一个簇 */
    uint32_t dir_cluster;
    int ret = fat32_alloc_cluster(sb_data, &dir_cluster);
    if (ret != 0)
        return ret;

    /* 将新簇清零并写入 "." 和 ".." 条目 */
    void *cluster_buf = alloc_page();
    if (!cluster_buf) {
        fat32_set_fat_entry(sb_data, dir_cluster, 0);
        return -2;
    }
    memset(cluster_buf, 0, PAGE_SIZE);

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;

    /* "." 条目 */
    memset(entries[0].name, ' ', 11);
    entries[0].name[0] = '.';
    entries[0].attr = FAT32_ATTR_DIRECTORY;
    entries[0].cluster_hi = (uint16_t)(dir_cluster >> 16);
    entries[0].cluster_lo = (uint16_t)(dir_cluster & 0xFFFF);
    entries[0].file_size = 0;

    /* ".." 条目 */
    memset(entries[1].name, ' ', 11);
    entries[1].name[0] = '.';
    entries[1].name[1] = '.';
    entries[1].attr = FAT32_ATTR_DIRECTORY;
    entries[1].cluster_hi = (uint16_t)(parent_data->start_cluster >> 16);
    entries[1].cluster_lo = (uint16_t)(parent_data->start_cluster & 0xFFFF);
    entries[1].file_size = 0;

    /* 条目末尾标记 */
    entries[2].name[0] = 0x00;

    /* 将簇写入磁盘 */
    ret = fat32_write_cluster(sb_data, dir_cluster, cluster_buf);
    free_page(cluster_buf);
    if (ret != 0) {
        fat32_set_fat_entry(sb_data, dir_cluster, 0);
        return ret;
    }

    /* 在父目录中创建目录项 */
    ret = fat32_create_direntry(sb_data, parent_data->start_cluster,
                                 name, dir_cluster, 0, FAT32_ATTR_DIRECTORY);
    if (ret != 0) {
        fat32_set_fat_entry(sb_data, dir_cluster, 0);
        return ret;
    }

    /* 更新 inode 数据 */
    fat32_name_to_83(name, inode_data->name83);
    inode_data->parent_cluster = parent_data->start_cluster;
    inode_data->start_cluster = dir_cluster;
    inode_data->file_size = 0;

    return 0;
}

static int fat32_unlink_cb(vfs_dentry_t *dentry)
{
    if (!dentry || !dentry->inode || !dentry->inode->fs_data || !dentry->inode->sb)
        return -1;

    fat32_inode_data_t *inode_data = (fat32_inode_data_t *)dentry->inode->fs_data;
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)dentry->inode->sb->fs_data;

    /* 将目录项标记为已删除（首字节 = 0xE5） */
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -2;

    uint32_t cluster = inode_data->parent_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -3;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                free_page(cluster_buf);
                return -4;  /* 条目未找到 */
            }
            if (entries[i].name[0] == 0xE5)
                continue;
            if (entries[i].attr == FAT32_ATTR_LFN)
                continue;
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                continue;

            if (memcmp(entries[i].name, inode_data->name83, 11) == 0) {
                /* 找到 - 标记为已删除 */
                entries[i].name[0] = 0xE5;
                ret = fat32_write_cluster(sb_data, cluster, cluster_buf);
                free_page(cluster_buf);

                /* 释放文件链中的所有簇 */
                if (inode_data->start_cluster >= 2) {
                    uint32_t cl = inode_data->start_cluster;
                    while (cl >= 2 && cl < FAT32_EOC_MIN) {
                        uint32_t next = fat32_get_fat_entry(sb_data, cl);
                        fat32_set_fat_entry(sb_data, cl, 0);
                        cl = next;
                    }
                }

                return ret;
            }
        }

        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    return -4;  /* 条目未找到 */
}

/* ========================================================================
 * 目录填充辅助函数
 *
 * 从 FAT32 目录簇链中读取所有目录项，
 * 并在给定父目录项下创建 VFS 目录项/inode 对。
 * ======================================================================== */

/* ========================================================================
 * LFN（长文件名）支持
 *
 * LFN 条目位于 8.3 条目之前，以 UCS-2 存储长文件名。
 * 每个 LFN 条目最多容纳 13 个字符。它们以逆序存储
 * （序列号最高的在前）。
 * ======================================================================== */

#define LFN_MAX_ENTRIES  20   /* 最大 LFN 条目数 = 20 * 13 = 260 字符 */
#define LFN_MAX_CHARS   255

/* 从 LFN 目录项中提取 UCS-2 字符。
 * 每个 LFN 条目在固定偏移处存储 13 个 UCS-2 字符：
 *   字符 0-4:  字节 1-10   (5 个字符)
 *   字符 5-10: 字节 14-25  (6 个字符)
 *   字符 11-12: 字节 28-31 (2 个字符)
 * 0x0000 或 0xFFFF 字符表示名称结束。 */
static int lfn_extract_chars(const fat32_dir_entry_t *entry, uint16_t *chars)
{
    int count = 0;
    const uint8_t *raw = (const uint8_t *)entry;

    /* 字符 0-4：字节 1-10 */
    for (int i = 0; i < 5; i++) {
        uint16_t ch = raw[1 + i * 2] | ((uint16_t)raw[2 + i * 2] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) return count;
        chars[count++] = ch;
    }
    /* 字符 5-10：字节 14-25 */
    for (int i = 0; i < 6; i++) {
        uint16_t ch = raw[14 + i * 2] | ((uint16_t)raw[15 + i * 2] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) return count;
        chars[count++] = ch;
    }
    /* 字符 11-12：字节 28-31 */
    for (int i = 0; i < 2; i++) {
        uint16_t ch = raw[28 + i * 2] | ((uint16_t)raw[29 + i * 2] << 8);
        if (ch == 0x0000 || ch == 0xFFFF) return count;
        chars[count++] = ch;
    }
    return count;
}

static int fat32_populate_dir(vfs_superblock_t *sb, vfs_dentry_t *parent,
                              uint32_t dir_cluster)
{
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)sb->fs_data;
    uint32_t cluster_size = (uint32_t)sb_data->sectors_per_cluster * sb_data->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat32_dir_entry_t);

    void *cluster_buf = alloc_page();
    if (!cluster_buf)
        return -1;

    /* LFN 累积状态 */
    uint16_t lfn_chars[LFN_MAX_CHARS];
    int      lfn_char_count = 0;
    int      lfn_seq_expected = 0;  /* 期望的下一个序列号（递减） */

    /* 先收集所有簇中的所有条目，然后再处理。
     * 这简化了跨簇边界的 LFN 处理。 */
    /* 为简化起见，我们逐簇处理，但跨簇携带 LFN 状态。 */

    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC_MIN) {
        int ret = fat32_read_cluster(sb_data, cluster, cluster_buf);
        if (ret != 0) {
            free_page(cluster_buf);
            return -2;
        }

        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first_byte = entries[i].name[0];

            /* 目录结束 */
            if (first_byte == 0x00) {
                free_page(cluster_buf);
                return 0;
            }

            /* 已删除条目 - 跳过并重置 LFN 状态 */
            if (first_byte == 0xE5) {
                lfn_char_count = 0;
                lfn_seq_expected = 0;
                continue;
            }

            /* 处理 LFN 条目 */
            if (entries[i].attr == FAT32_ATTR_LFN) {
                uint8_t seq = entries[i].name[0];
                int is_last = (seq & 0x40) != 0;
                seq &= 0x3F;  /* 屏蔽末尾条目位 */

                if (is_last) {
                    /* 这是第一个 LFN 条目（最高序列号） */
                    lfn_char_count = 0;
                    lfn_seq_expected = seq;

                    /* 提取字符并放置到正确位置 */
                    uint16_t chars[13];
                    int nchars = lfn_extract_chars(&entries[i], chars);

                    /* 计算此段的起始位置 */
                    int start_pos = (seq - 1) * 13;
                    if (start_pos + nchars <= LFN_MAX_CHARS) {
                        for (int j = 0; j < nchars; j++) {
                            lfn_chars[start_pos + j] = chars[j];
                        }
                        lfn_char_count = start_pos + nchars;
                    }
                    lfn_seq_expected = seq - 1;
                } else if (seq == lfn_seq_expected && lfn_seq_expected > 0) {
                    /* 继续的 LFN 序列 */
                    uint16_t chars[13];
                    int nchars = lfn_extract_chars(&entries[i], chars);

                    int start_pos = (seq - 1) * 13;
                    if (start_pos + nchars <= LFN_MAX_CHARS) {
                        for (int j = 0; j < nchars; j++) {
                            lfn_chars[start_pos + j] = chars[j];
                        }
                        /* 不要为中间条目更新 lfn_char_count；
                         * 它已由第一个（最后序列号）条目设置 */
                    }
                    lfn_seq_expected = seq - 1;
                } else {
                    /* 序列外的 LFN，重置 */
                    lfn_char_count = 0;
                    lfn_seq_expected = 0;
                }
                continue;
            }

            /* 跳过卷标识 */
            if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                lfn_char_count = 0;
                lfn_seq_expected = 0;
                continue;
            }

            /* 这是一个普通 8.3 条目 - 确定名称 */
            char name[VFS_MAX_NAME];

            if (lfn_char_count > 0 && lfn_seq_expected == 0) {
                /* 有完整的 LFN - 将 UCS-2 转换为 ASCII/UTF-8 */
                int pos = 0;
                for (int j = 0; j < lfn_char_count && pos < VFS_MAX_NAME - 1; j++) {
                    if (lfn_chars[j] < 0x80) {
                        name[pos++] = (char)lfn_chars[j];
                    } else {
                        /* 非 ASCII：使用 '?' 占位符 */
                        name[pos++] = '?';
                    }
                }
                name[pos] = '\0';
            } else {
                /* 无有效 LFN，使用 8.3 文件名 */
                fat32_format_83_name(entries[i].name, name, VFS_MAX_NAME);
            }

            /* 重置 LFN 状态 */
            lfn_char_count = 0;
            lfn_seq_expected = 0;

            /* 跳过 "." 和 ".." */
            if (name[0] == '.' && (name[1] == '\0' ||
                (name[1] == '.' && name[2] == '\0')))
                continue;

            /* 获取起始簇 */
            uint32_t entry_cluster = (uint32_t)entries[i].cluster_hi << 16 | entries[i].cluster_lo;
            int is_dir = (entries[i].attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;

            /* 检查目录项是否已存在 */
            vfs_dentry_t *existing = NULL;
            vfs_dentry_t *child = parent->child;
            while (child) {
                if (strcmp(child->name, name) == 0) {
                    existing = child;
                    break;
                }
                child = child->next;
            }
            if (existing)
                continue;

            /* 创建 inode */
            vfs_inode_t *new_inode = (vfs_inode_t *)alloc_page();
            if (!new_inode) continue;
            memset(new_inode, 0, PAGE_SIZE);

            new_inode->ino = fat32_next_ino++;
            new_inode->type = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            new_inode->mode = VFS_S_IRUSR | VFS_S_IRGRP | VFS_S_IROTH;
            if (is_dir)
                new_inode->mode |= VFS_S_IXUSR | VFS_S_IXGRP | VFS_S_IXOTH;
            new_inode->size = is_dir ? 0 : entries[i].file_size;
            new_inode->nlinks = 1;
            new_inode->sb = sb;

            /* 分配 inode 私有数据 */
            fat32_inode_data_t *inode_data = (fat32_inode_data_t *)alloc_page();
            if (!inode_data) {
                free_page(new_inode);
                continue;
            }
            memset(inode_data, 0, PAGE_SIZE);
            inode_data->start_cluster = entry_cluster;
            inode_data->file_size = entries[i].file_size;
            inode_data->parent_cluster = dir_cluster;
            memcpy(inode_data->name83, entries[i].name, 11);
            new_inode->fs_data = inode_data;

            /* 为文件设置读/写操作 */
            if (!is_dir) {
                new_inode->read = fat32_inode_read;
                new_inode->write = fat32_inode_write;
            }

            /* 创建目录项 */
            vfs_dentry_t *new_dentry = (vfs_dentry_t *)alloc_page();
            if (!new_dentry) {
                free_page(inode_data);
                free_page(new_inode);
                continue;
            }
            memset(new_dentry, 0, PAGE_SIZE);
            strncpy(new_dentry->name, name, VFS_MAX_NAME - 1);
            new_dentry->inode = new_inode;
            new_dentry->parent = parent;

            /* 添加到父目录的子目录列表 */
            new_dentry->next = parent->child;
            parent->child = new_dentry;
        }

        /* 沿簇链继续 */
        cluster = fat32_get_fat_entry(sb_data, cluster);
    }

    free_page(cluster_buf);
    return 0;
}

/* ========================================================================
 * 递归目录树填充
 *
 * 遍历 FAT32 目录树，创建对应的 VFS 目录项。
 * ======================================================================== */

static void fat32_populate_recursive(vfs_superblock_t *sb, vfs_dentry_t *parent,
                                     uint32_t dir_cluster)
{
    /* 首先填充此目录 */
    fat32_populate_dir(sb, parent, dir_cluster);

    /* 然后递归进入子目录 */
    vfs_dentry_t *child = parent->child;
    while (child) {
        if (child->inode && child->inode->type == VFS_TYPE_DIR && child->inode->fs_data) {
            fat32_inode_data_t *id = (fat32_inode_data_t *)child->inode->fs_data;
            fat32_populate_recursive(sb, child, id->start_cluster);
        }
        child = child->next;
    }
}

/* ========================================================================
 * FAT32 文件系统操作
 * ======================================================================== */

static int fat32_mount(vfs_superblock_t *sb, uint8_t blkdev_id, const char *options)
{
    /* 如果提供了选项字符串，从中解析块设备 ID。
     * 格式："blkdev=N" */
    uint8_t dev_id = blkdev_id;

    if (options) {
        const char *p = options;
        while (*p) {
            if (strncmp(p, "blkdev=", 7) == 0) {
                p += 7;
                int val = 0;
                while (*p >= '0' && *p <= '9') {
                    val = val * 10 + (*p - '0');
                    p++;
                }
                dev_id = (uint8_t)val;
                break;
            }
            p++;
        }
    }

    if (dev_id == 0xFF) {
        printf("[FAT32] No block device specified\n");
        return -1;
    }

    /* 从扇区 0 读取 BPB */
    void *sector_buf = alloc_page();
    if (!sector_buf)
        return -2;

    int ret = blkdev_read(dev_id, 0, 1, sector_buf);
    if (ret != 0) {
        printf("[FAT32] Failed to read BPB (err=%d)\n", ret);
        free_page(sector_buf);
        return -3;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;

    /* 验证 FAT32 签名 */
    if (bpb->signature != 0xAA55) {
        printf("[FAT32] Invalid boot signature: 0x%04X\n", bpb->signature);
        free_page(sector_buf);
        return -4;
    }

    /* 验证这确实是 FAT32 */
    if (bpb->bytes_per_sector == 0 ||
        (bpb->bytes_per_sector & (bpb->bytes_per_sector - 1)) != 0) {
        printf("[FAT32] Invalid bytes_per_sector: %u\n", bpb->bytes_per_sector);
        free_page(sector_buf);
        return -5;
    }

    if (bpb->sectors_per_fat_32 == 0) {
        printf("[FAT32] Not a FAT32 filesystem (sectors_per_fat_32 = 0)\n");
        free_page(sector_buf);
        return -6;
    }

    /* 检查 fs_type 字段中的 "FAT32" */
    if (memcmp(bpb->fs_type, "FAT32   ", 8) != 0) {
        printf("[FAT32] fs_type mismatch: %.8s\n", bpb->fs_type);
        /* 某些 FAT32 镜像可能未正确设置此项，继续执行 */
    }

    /* 分配超级块私有数据 */
    fat32_sb_data_t *sb_data = (fat32_sb_data_t *)alloc_page();
    if (!sb_data) {
        free_page(sector_buf);
        return -7;
    }
    memset(sb_data, 0, PAGE_SIZE);

    sb_data->blkdev_id = dev_id;
    sb_data->bytes_per_sector = bpb->bytes_per_sector;
    sb_data->sectors_per_cluster = bpb->sectors_per_cluster;
    sb_data->reserved_sectors = bpb->reserved_sectors;
    sb_data->sectors_per_fat = bpb->sectors_per_fat_32;
    sb_data->root_dir_cluster = bpb->root_dir_cluster;

    /* 计算 FAT 起始扇区 */
    sb_data->fat_start_sector = bpb->reserved_sectors;

    /* 计算数据起始扇区 */
    sb_data->data_start_sector = bpb->reserved_sectors +
                                  (uint32_t)bpb->num_fats * bpb->sectors_per_fat_32;

    /* 计算总簇数 */
    uint32_t total_sectors = bpb->total_sectors_32;
    if (total_sectors == 0)
        total_sectors = bpb->total_sectors_16;

    uint32_t data_sectors = total_sectors - sb_data->data_start_sector;
    sb_data->total_clusters = data_sectors / sb_data->sectors_per_cluster;
    sb_data->total_data_clusters = sb_data->total_clusters;
    sb_data->next_free_cluster = 2;

    sb->fs_data = sb_data;
    sb->blkdev_id = dev_id;

    printf("[FAT32] Mounted on blkdev %u, bytes/sector=%u, sectors/cluster=%u\n",
           dev_id, sb_data->bytes_per_sector, sb_data->sectors_per_cluster);
    printf("[FAT32] FAT start=%u, data start=%u, root cluster=%u, total clusters=%u\n",
           sb_data->fat_start_sector, sb_data->data_start_sector,
           sb_data->root_dir_cluster, sb_data->total_clusters);

    /* 创建根 inode */
    vfs_inode_t *root = (vfs_inode_t *)alloc_page();
    if (!root) {
        free_page(sb_data);
        free_page(sector_buf);
        return -8;
    }
    memset(root, 0, PAGE_SIZE);
    root->ino = fat32_next_ino++;
    root->type = VFS_TYPE_DIR;
    root->mode = VFS_S_IRUSR | VFS_S_IXUSR | VFS_S_IRGRP | VFS_S_IXGRP |
                 VFS_S_IROTH | VFS_S_IXOTH;
    root->size = 0;
    root->nlinks = 2;
    root->sb = sb;

    /* 分配根 inode 私有数据 */
    fat32_inode_data_t *root_data = (fat32_inode_data_t *)alloc_page();
    if (!root_data) {
        free_page(root);
        free_page(sb_data);
        free_page(sector_buf);
        return -9;
    }
    memset(root_data, 0, PAGE_SIZE);
    root_data->start_cluster = sb_data->root_dir_cluster;
    root_data->file_size = 0;
    root->fs_data = root_data;

    /* 创建根目录项 */
    vfs_dentry_t *root_dentry = (vfs_dentry_t *)alloc_page();
    if (!root_dentry) {
        free_page(root_data);
        free_page(root);
        free_page(sb_data);
        free_page(sector_buf);
        return -10;
    }
    memset(root_dentry, 0, PAGE_SIZE);
    strcpy(root_dentry->name, "/");
    root_dentry->inode = root;
    root_dentry->parent = root_dentry;

    sb->root = root_dentry;
    sb->root_inode = root;

    free_page(sector_buf);

    /* 填充目录树 */
    fat32_populate_recursive(sb, root_dentry, sb_data->root_dir_cluster);

    return 0;
}

static int fat32_unmount(vfs_superblock_t *sb)
{
    if (sb->fs_data) {
        free_page(sb->fs_data);
        sb->fs_data = NULL;
    }
    return 0;
}

static vfs_inode_t *fat32_alloc_inode(vfs_superblock_t *sb)
{
    vfs_inode_t *inode = (vfs_inode_t *)alloc_page();
    if (!inode) return NULL;
    memset(inode, 0, PAGE_SIZE);

    inode->ino = fat32_next_ino++;
    inode->sb = sb;

    /* 分配 inode 私有数据 */
    fat32_inode_data_t *data = (fat32_inode_data_t *)alloc_page();
    if (!data) {
        free_page(inode);
        return NULL;
    }
    memset(data, 0, PAGE_SIZE);
    inode->fs_data = data;

    return inode;
}

static void fat32_destroy_inode(vfs_inode_t *inode)
{
    if (inode->fs_data) {
        free_page(inode->fs_data);
        inode->fs_data = NULL;
    }
    free_page(inode);
}

/* ========================================================================
 * 文件系统类型定义
 * ======================================================================== */

vfs_filesystem_t fat32_fs = {
    .name = "fat32",
    .mount = fat32_mount,
    .unmount = fat32_unmount,
    .alloc_inode = fat32_alloc_inode,
    .destroy_inode = fat32_destroy_inode,
    .create = fat32_create,
    .mkdir = fat32_mkdir_cb,
    .unlink = fat32_unlink_cb,
};

/* ========================================================================
 * 初始化
 * ======================================================================== */

void fat32_init(void)
{
    vfs_register_fs(&fat32_fs);
}
