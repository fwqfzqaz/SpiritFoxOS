/* SpiritFoxOS - UEFI渐进式测试
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
/* 渐进式UEFI测试 - 步骤1：直接返回 */
#include <stdint.h>

typedef unsigned long long UINTN;
typedef long long EFI_STATUS;
typedef void *EFI_HANDLE;

/* 与我们的efi.h布局匹配的最小化EFI_SYSTEM_TABLE */
typedef struct {
    uint64_t sig;
    uint32_t rev;
    uint32_t hdr_sz;
    uint32_t crc32;
    uint32_t reserved;
    uint16_t *fw_vendor;
    uint32_t fw_rev;
    void *con_in;
    void *con_out;
    void *err_out;
    void *runtime;
    void *boot;
    uint64_t num_tables;
    void *config_table;
} EFI_SYSTEM_TABLE_T;

#define EFI_SUCCESS 0

/* 向串口COM1（0x3F8）写入字符 */
static void serial_out(char c) {
    volatile unsigned char *port = (volatile unsigned char *)0x3F8;
    /* 等待发送缓冲区为空 */
    while ((*port & 0x20) == 0);
    *(port + 0) = c;
}

static void serial_str(const char *s) {
    while (*s) serial_out(*s++);
}

/* 步骤1：入口，向串口打印，返回 */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE_T *SystemTable) {
    (void)ImageHandle;
    serial_str("[TEST] efi_main entered\r\n");

    /* 测试：能否访问SystemTable？ */
    if (SystemTable != 0) {
        serial_str("[TEST] SystemTable non-null\r\n");
        /* 尝试访问偏移0x30处的con_out */
        void **st = (void **)SystemTable;
        void *con_out = st[0x30 / 8];  /* 偏移0x30 = 第6个指针 */
        if (con_out != 0) {
            serial_str("[TEST] con_out non-null\r\n");
        } else {
            serial_str("[TEST] con_out is NULL\r\n");
        }
        /* 尝试访问偏移0x48处的boot */
        void *boot_srv = st[0x48 / 8];
        if (boot_srv != 0) {
            serial_str("[TEST] boot services non-null\r\n");
        } else {
            serial_str("[TEST] boot services is NULL\r\n");
        }
    } else {
        serial_str("[TEST] SystemTable is NULL!\r\n");
    }

    serial_str("[TEST] returning EFI_SUCCESS\r\n");
    return EFI_SUCCESS;
}
