/* SpiritFoxOS Wayland Compositor 入口 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "sf_compositor.h"

static sf_compositor_t comp;

static void
signal_handler(int sig)
{
    (void)sig;
    comp.running = 0;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fprintf(stderr, "[compositor] SpiritFoxOS Wayland Compositor\n");
    fprintf(stderr, "[compositor] Initializing...\n");

    /* 信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 初始化合成器 */
    memset(&comp, 0, sizeof(comp));
    if (sf_compositor_init(&comp) < 0) {
        fprintf(stderr, "[compositor] Failed to initialize\n");
        return 1;
    }

    fprintf(stderr, "[compositor] Running. Press Ctrl+C to exit.\n");

    /* 主循环 */
    sf_compositor_run(&comp);

    /* 清理 */
    sf_compositor_finish(&comp);
    fprintf(stderr, "[compositor] Exited cleanly\n");
    return 0;
}
