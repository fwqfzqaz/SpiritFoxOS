#include "types.h"
#include "interrupt.h"

void isr_handler(uint32_t int_num, uint32_t err_code) {
    char* video_memory = (char*)0xb8000;
    const char* msg = "KERNEL PANIC: Exception ";
    int i = 0;

    while (*msg) {
        video_memory[i++] = *msg++;
        video_memory[i++] = 0x04; // 红底黑字
    }

    // 显示异常号
    video_memory[i++] = '0' + (int_num / 10);
    video_memory[i++] = 0x04;
    video_memory[i++] = '0' + (int_num % 10);
    video_memory[i++] = 0x04;

    while (1); // 死循环表示崩溃
}