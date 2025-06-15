org 0x7c00      ; BIOS 加载地址

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    ; 显示引导信息
    mov si, boot_msg
    call print_string

    ; 读取 stage2 到内存地址 0x7e00
    mov ah, 0x02        ; BIOS 磁盘读取功能
    mov al, 1           ; 读取一个扇区
    mov ch, 0           ; 柱面 0
    mov cl, 2           ; 扇区号 2
    mov dh, 0           ; 磁头 0
    mov dl, 0           ; 驱动器 0 (软盘/硬盘)
    mov bx, 0x7e00      ; 加载到 0x7e00
    int 0x13
    jc disk_error       ; 如果出错跳转

    jmp word 0x0000:0x7e00

; =============================
; 子程序：打印字符串
; =============================
print_string:
    push ax
    push bx
    mov bh, 0
    mov bl, 0x07        ; 属性：黑底白字
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    int 0x10            ; BIOS 输出字符
    jmp .loop
.done:
    pop bx
    pop ax
    ret

; =============================
; 错误处理
; =============================
disk_error:
    mov si, error_msg
    call print_string
    hlt

; =============================
; 数据定义
; =============================
boot_msg db "Booting Linghu OS Bootloader...", 0
error_msg db 0x0d, 0x0a, "Disk Read Error!", 0

; =============================
; 填充至 512 字节并添加引导签名
; =============================
times 510 - ($ - $$) db 0
dw 0xaa55