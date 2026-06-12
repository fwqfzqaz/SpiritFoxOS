; SpiritFoxOS - DOS可执行文件桩
; Copyright (C) 2025 SpiritFoxOS Contributors
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program.  If not, see <http://www.gnu.org/licenses/>.

; ============================================================
; UEFI PE/COFF可执行文件的DOS头桩
; UEFI固件PE加载器所需的最小MZ头
; ============================================================

bits 64

section .dos_header progbits align=1

; DOS头（偏移0x3C处的e_lfanew指向PE签名）
dos_start:
    db "MZ"                    ; e_magic: 0x5A4D ("MZ")
    dw 0x90                    ; e_cblp: 文件最后一页的字节数
    dw 0x03                    ; e_cp: 文件中的页数
    dw 0x00                    ; e_crlc: 重定位项数
    dw 0x04                    ; e_cparhdr: 以节（paragraph）为单位的头大小（0x04 * 16 = 64 = 0x40）
    dw 0x00                    ; e_minalloc: 所需的最小额外节数
    dw 0xFFFF                  ; e_maxalloc: 所需的最大额外节数
    dw 0x00                    ; e_ss: 初始（相对）SS值
    dw 0x00B8                  ; e_sp: 初始SP值
    dw 0x00                    ; e_csum: 校验和
    dw 0x00                    ; e_ip: 初始IP值
    dw 0x00                    ; e_cs: 初始（相对）CS值
    dw 0x00                    ; e_lfarlc: 重定位表的文件地址
    dw 0x00                    ; e_ovno: 覆盖号
    times 8 dw 0x00            ; e_res[4]: 保留
    dw 0x00                    ; e_oem_id: OEM标识符
    dw 0x00                    ; e_oem_info: OEM信息
    times 10 dw 0x00           ; e_res2[10]: 保留
    dd 0x78                    ; e_lfanew: 新exe头的文件地址（PE签名位于偏移0x78处）
dos_end:

; 如需要则填充到64字节（0x40）- 头部应该正好是这个大小
; 实际上，链接器脚本中dos_header节从偏移0开始，我们需要
; PE签名在0x78处，所以需要从数据末尾到0x78的填充。
; 但由于我们位于一个被放置在偏移0的节中，且下一个节通过". = 0x78"强制到0x78，
; 汇编器/链接器会处理对齐。

global _dos_header_end
_dos_header_end:
