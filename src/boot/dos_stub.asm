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
; DOS Header Stub for UEFI PE/COFF executable
; Minimal MZ header required by UEFI firmware PE loader
; ============================================================

bits 64

section .dos_header progbits align=1

; DOS Header (e_lfanew at offset 0x3C points to PE signature)
dos_start:
    db "MZ"                    ; e_magic: 0x5A4D ("MZ")
    dw 0x90                    ; e_cblp: bytes on last page of file
    dw 0x03                    ; e_cp: pages in file
    dw 0x00                    ; e_crlc: relocations
    dw 0x04                    ; e_cparhdr: size of header in paragraphs (0x04 * 16 = 64 = 0x40)
    dw 0x00                    ; e_minalloc: minimum extra paragraphs needed
    dw 0xFFFF                  ; e_maxalloc: maximum extra paragraphs needed
    dw 0x00                    ; e_ss: initial (relative) SS value
    dw 0x00B8                  ; e_sp: initial SP value
    dw 0x00                    ; e_csum: checksum
    dw 0x00                    ; e_ip: initial IP value
    dw 0x00                    ; e_cs: initial (relative) CS value
    dw 0x00                    ; e_lfarlc: file address of relocation table
    dw 0x00                    ; e_ovno: overlay number
    times 8 dw 0x00            ; e_res[4]: reserved
    dw 0x00                    ; e_oem_id: OEM identifier
    dw 0x00                    ; e_oem_info: OEM information
    times 10 dw 0x00           ; e_res2[10]: reserved
    dd 0x78                    ; e_lfanew: file address of new exe header (PE sig at offset 0x78)
dos_end:

; Pad to 64 bytes (0x40) if needed - the header should be exactly this size
; Actually the dos_header section in the linker script starts at 0 and we need
; the PE signature at 0x78, so we need padding from end of our data to 0x78.
; But since we're in a section that gets placed at offset 0, and the next section
; is forced to 0x78 via ". = 0x78", the assembler/linker handles alignment.

global _dos_header_end
_dos_header_end:
