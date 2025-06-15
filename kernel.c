#include "types.h"

// 清屏函数
void clear_screen() {
    char* video_memory = (char*)0xb8000;
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        video_memory[i] = ' ';
        video_memory[i + 1] = 0x07; // 白底黑字
    }
}

// 显示 Logo 函数
void draw_logo() {
    char* video_memory = (char*)0xb8000;

    const char* logo[] = {
        "  _______ _       _____   ___  ",
        " |__   __| |     |  __ \\ / _ \\ ",
        "    | |  | |___  | |__) | (_) |",
        "    | |  | '_  \\ |  ___/ > _ < ",
        "    | |  | |_) | | |    | (_) |",
        "    |_|  |_.__/  |_|     \\___/ ",
        "",
        "        SpiritFox OS v0.1",
        NULL
    };

    int row = 5, col = 10; // 起始位置（行、列）

    for (int i = 0; logo[i] != NULL; i++) {
        const char* line = logo[i];
        int offset = ((row + i) * 160) + (col * 2); // 每行 80 字符 x 2 字节
        int j = 0;
        while (line[j]) {
            video_memory[offset + j * 2] = line[j];
            video_memory[offset + j * 2 + 1] = 0x0A; // 绿色前景
            j++;
        }
    }
}

void main() {
    clear_screen();
    draw_logo();

    while (1); // 内核主循环
}
void main() {
    char* video_memory = (char*)0xb8000;
    const char* msg = "Welcome to SpiritFox OS!";

    int i = 0;
    while (*msg) {
        video_memory[i++] = *msg++;
        video_memory[i++] = 0x07; // 白底黑字
    }

    while (1); // 内核主循环
}