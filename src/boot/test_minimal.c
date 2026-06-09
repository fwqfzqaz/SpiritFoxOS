/* SpiritFoxOS - 最小化UEFI测试
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
/* Minimal UEFI test - returns immediately */
void efi_main(void) {
    /* Just return; entry point will do RET = return to caller */
    __asm__ volatile("movl $0x80000003, %eax");  /* EFI_SUCCESS-ish, actually just ret */
}
