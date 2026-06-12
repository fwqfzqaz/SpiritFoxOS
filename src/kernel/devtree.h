/* SpiritFoxOS - 设备树接口
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
#ifndef DEVTREE_H
#define DEVTREE_H

#include <stdint.h>

/* 设备类型 */
typedef enum {
    DEV_TYPE_CPU = 0,
    DEV_TYPE_MEMORY,
    DEV_TYPE_STORAGE,
    DEV_TYPE_INPUT,
    DEV_TYPE_DISPLAY,
    DEV_TYPE_NETWORK,
    DEV_TYPE_USB,
    DEV_TYPE_PCI,
    DEV_TYPE_OTHER
} dev_type_t;

/* 设备状态 */
typedef enum {
    DEV_STATUS_UNKNOWN = 0,
    DEV_STATUS_OK,
    DEV_STATUS_FAILED,
    DEV_STATUS_MISSING,
    DEV_STATUS_DISABLED
} dev_status_t;

/* 设备关键性 */
typedef enum {
    DEV_CRITICAL_NO = 0,
    DEV_CRITICAL_YES
} dev_critical_t;

/* 设备树条目 */
#define DEV_NAME_MAX 32
#define DEV_INFO_MAX 64

typedef struct {
    char name[DEV_NAME_MAX];
    dev_type_t type;
    dev_status_t status;
    dev_critical_t critical;
    char info[DEV_INFO_MAX];
    uint32_t vendor_id;
    uint32_t device_id;
} devtree_entry_t;

#define DEVTREE_MAX_ENTRIES 64

/* 初始化设备树 */
void devtree_init(void);

/* 在设备树中注册一个设备 */
int devtree_register(const char *name, dev_type_t type, dev_status_t status,
                     dev_critical_t critical, const char *info,
                     uint32_t vendor_id, uint32_t device_id);

/* 更新设备状态 */
int devtree_update_status(const char *name, dev_status_t status);

/* 检查所有关键设备是否正常（返回1表示通过，0表示失败） */
int devtree_check_critical(void);

/* 获取失败的关键设备数量 */
int devtree_get_critical_fail_count(void);

/* 将所有设备错误打印到控制台 */
void devtree_print_errors(void);

/* 打印完整的设备树 */
void devtree_print_all(void);

/* 获取设备数量 */
int devtree_get_count(void);

/* 根据索引获取设备条目 */
devtree_entry_t *devtree_get_entry(int index);

#endif /* DEVTREE_H */
