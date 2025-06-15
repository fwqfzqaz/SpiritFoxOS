org 0x7e00

start:
    mov si, stage2_msg
    call print_string
    jmp 0x0000:0x100000   ; 假设内核位于 0x100000

.hang:
    cli
    hlt

; =============================
; 子程序：打印字符串
; =============================
print_string:
    push ax
    push bx
    mov bh, 0
    mov bl, 0x07
.loop:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0e
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

; =============================
; 数据定义
; =============================
stage2_msg db "Stage 2 loaded successfully.", 0