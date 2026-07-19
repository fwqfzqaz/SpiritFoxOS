# GUI 双缓冲优化计划

## 背景
SpiritFoxOS 帧缓冲层（`video/fb.c`）已实现双缓冲基础设施，包括前台/后台缓冲区和 `fb_swap_buffer()` 提交接口。但在性能和 API 设计上仍有优化空间：`fb_fill_rect` 逐像素写入、`fb_swap_buffer` 使用通用 `memcpy`、缺乏脏区域追踪等。

## 修改内容

### 1. 重命名 `fb_swap_buffer` → `fb_flip`（fb.h + fb.c）
- 将 `fb_swap_buffer()` 重命名为 `fb_flip()`，语义更清晰
- 保留 `fb_swap_buffer` 作为兼容宏或直接全局替换
- 更新所有调用点：`window.c`、`nuklear_fb.c`、`fb_term_init()`

### 2. 优化 `fb_flip()` 整帧拷贝性能（fb.c）
- 将 `memcpy(fb_front, fb_back, fb_size)` 替换为 64 位字长优化的内联汇编拷贝
- 使用 `rep movsq` 指令，每次拷贝 8 字节，减少循环次数 50%
- 保留 VSync 等待逻辑但标注其 QEMU 兼容性限制

### 3. 优化 `fb_fill_rect` 纯色填充性能（fb.c）
- 内层循环从逐 32 位像素写入改为 64 位字长批量写入
- 对 8 字节对齐的行使用 `rep stosl` 或手动展开的 64 位写入
- 单行宽度>=8 时使用 `uint64_t` 指针写入减少循环开销

### 4. 新增脏区域追踪机制（fb.h + fb.c）
- 新增 `fb_flip_dirty()` 接口：仅拷贝脏区域列表中的矩形到前台
- 使用简单的脏矩形数组（最多 16 个），避免逐帧 3MB 全量拷贝
- `fb_mark_dirty(x, y, w, h)` 标记脏区域
- `fb_flip_dirty()` 合并重叠脏矩形后逐块拷贝
- 保留 `fb_flip()` 全帧拷贝作为回退路径

### 5. 更新 GUI 渲染主循环（window.c）
- 窗口管理器主循环改用 `fb_flip_dirty()` 替代 `fb_flip()`
- 各绘制函数自动调用 `fb_mark_dirty()` 或在主循环末尾标记全屏脏

## 涉及文件
- `kernel/include/fb.h` — API 声明更新
- `kernel/src/video/fb.c` — 核心优化实现
- `kernel/src/video/window.c` — 调用点更新
- `kernel/src/video/nuklear_fb.c` — 调用点更新

## 验证方式
- `make` 编译通过
- QEMU 启动后 `window` 命令进入 GUI，确认画面正常无撕裂
- 终端模式文字输出正常，无字符丢失或乱码
