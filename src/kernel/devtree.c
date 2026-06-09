/* SpiritFoxOS - 设备树管理
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
#include "devtree.h"
#include "log.h"
#include "vga.h"
#include "../include/string.h"

static devtree_entry_t devices[DEVTREE_MAX_ENTRIES];
static int device_count = 0;

void devtree_init(void) {
    for (int i = 0; i < DEVTREE_MAX_ENTRIES; i++) {
        devices[i].name[0] = '\0';
        devices[i].type = DEV_TYPE_OTHER;
        devices[i].status = DEV_STATUS_UNKNOWN;
        devices[i].critical = DEV_CRITICAL_NO;
        devices[i].info[0] = '\0';
        devices[i].vendor_id = 0;
        devices[i].device_id = 0;
    }
    device_count = 0;
    LOG_I("devtree", "Device tree initialized");
}

int devtree_register(const char *name, dev_type_t type, dev_status_t status,
                     dev_critical_t critical, const char *info,
                     uint32_t vendor_id, uint32_t device_id) {
    if (device_count >= DEVTREE_MAX_ENTRIES) return -1;

    devtree_entry_t *dev = &devices[device_count];
    strncpy(dev->name, name, DEV_NAME_MAX - 1);
    dev->name[DEV_NAME_MAX - 1] = '\0';
    dev->type = type;
    dev->status = status;
    dev->critical = critical;
    if (info) {
        strncpy(dev->info, info, DEV_INFO_MAX - 1);
        dev->info[DEV_INFO_MAX - 1] = '\0';
    } else {
        dev->info[0] = '\0';
    }
    dev->vendor_id = vendor_id;
    dev->device_id = device_id;

    LOG_I("devtree", "Registered device: %s [%s] status=%s critical=%s",
          name,
          type == DEV_TYPE_CPU ? "CPU" :
          type == DEV_TYPE_MEMORY ? "MEM" :
          type == DEV_TYPE_STORAGE ? "STOR" :
          type == DEV_TYPE_INPUT ? "INPUT" :
          type == DEV_TYPE_DISPLAY ? "DISP" :
          type == DEV_TYPE_NETWORK ? "NET" :
          type == DEV_TYPE_USB ? "USB" :
          type == DEV_TYPE_PCI ? "PCI" : "OTHER",
          status == DEV_STATUS_OK ? "OK" :
          status == DEV_STATUS_FAILED ? "FAIL" :
          status == DEV_STATUS_MISSING ? "MISSING" :
          status == DEV_STATUS_DISABLED ? "DISABLED" : "UNKNOWN",
          critical ? "YES" : "NO");

    device_count++;
    return 0;
}

int devtree_update_status(const char *name, dev_status_t status) {
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i].name, name) == 0) {
            devices[i].status = status;
            return 0;
        }
    }
    return -1;
}

int devtree_check_critical(void) {
    int all_ok = 1;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].critical == DEV_CRITICAL_YES &&
            devices[i].status != DEV_STATUS_OK) {
            all_ok = 0;
            LOG_E("devtree", "Critical device FAILED: %s (status=%d)",
                  devices[i].name, devices[i].status);
        }
    }
    return all_ok;
}

int devtree_get_critical_fail_count(void) {
    int count = 0;
    for (int i = 0; i < device_count; i++) {
        if (devices[i].critical == DEV_CRITICAL_YES &&
            devices[i].status != DEV_STATUS_OK) {
            count++;
        }
    }
    return count;
}

static const char *type_name(dev_type_t type) {
    switch (type) {
        case DEV_TYPE_CPU:     return "CPU";
        case DEV_TYPE_MEMORY:  return "Memory";
        case DEV_TYPE_STORAGE: return "Storage";
        case DEV_TYPE_INPUT:   return "Input";
        case DEV_TYPE_DISPLAY: return "Display";
        case DEV_TYPE_NETWORK: return "Network";
        case DEV_TYPE_USB:     return "USB";
        case DEV_TYPE_PCI:     return "PCI";
        default:               return "Other";
    }
}

static const char *status_name(dev_status_t status) {
    switch (status) {
        case DEV_STATUS_OK:       return "OK";
        case DEV_STATUS_FAILED:   return "FAILED";
        case DEV_STATUS_MISSING:  return "MISSING";
        case DEV_STATUS_DISABLED: return "DISABLED";
        default:                  return "UNKNOWN";
    }
}

void devtree_print_errors(void) {
    int has_errors = 0;
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    for (int i = 0; i < device_count; i++) {
        if (devices[i].status != DEV_STATUS_OK) {
            if (!has_errors) {
                vga_puts("\n  !! Device Self-Check Errors !!\n");
                has_errors = 1;
            }
            vga_printf("  [FAIL] %-12s %-10s %s\n",
                       devices[i].name,
                       type_name(devices[i].type),
                       status_name(devices[i].status));
            if (devices[i].info[0]) {
                vga_printf("         %s\n", devices[i].info);
            }
        }
    }
    if (!has_errors) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("  All critical devices passed self-check.\n");
    }
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

void devtree_print_all(void) {
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_puts("\n  System Device Tree\n");
    vga_puts("  ------------------\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (int i = 0; i < device_count; i++) {
        vga_printf("  %-12s %-10s %-8s %s%s\n",
                   devices[i].name,
                   type_name(devices[i].type),
                   status_name(devices[i].status),
                   devices[i].critical ? "[CRITICAL] " : "",
                   devices[i].info[0] ? devices[i].info : "");
    }
    vga_puts("\n");
}

int devtree_get_count(void) {
    return device_count;
}

devtree_entry_t *devtree_get_entry(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}
