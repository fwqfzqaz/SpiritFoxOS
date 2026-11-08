# Spec：类 niri 的可滚动平铺图形环境（有界垂直切片）

状态：待评审 / 待批准
适用项目：SpiritFoxOS（微内核原型，P2 路线：ring-3 驱动 + framebuffer + 单一有界合成器）

## 1. 目标与非目标

### 1.1 目标

在不破坏现有无图形/串口/VNC 回退路径的前提下，提供一条**有界、可测试**的类 niri 图形路径：

1. 纯 freestanding 的“可滚动平铺”布局核心（工作区 / 列 / 窗口纵向堆叠 / 滚动 / 焦点）。
2. 内核仅新增最小 framebuffer 授权与映射机制（不解析 ELF/CBOR/包格式）。
3. 新增 ring-3 图形模块 `sf-wm`：映射 framebuffer、绘制工作区栏/列/窗口/焦点环，并通过 IPC 接收输入操作。
4. `sf-init` 新增 `gui` 命令：拉起 `sf-wm`，把串口按键转发为 WM 输入，`Ctrl-]` 退出，退出后串口 shell 保持可用。
5. 宿主测试覆盖布局核心；QEMU 无显示环境下通过串口断言布局状态与 framebuffer 校验和。
6. 中文文档说明边界与后续演进。

### 1.2 非目标（明确不做）

- 不移植完整 niri / Wayland / libwayland / Smithay，不实现协议栈、GPU、DRM/KMS。
- 不实现真实物理键盘/鼠标驱动；输入仅来自串口字节（`gui` 命令转发）。真实输入设备留给后续 P2 驱动任务。
- 不实现窗口客户端（无 GUI 应用协议）；窗口为合成器内部有界模型。
- 不引入堆分配；布局核心与 WM 使用固定数组。
- 不改变现有 `SYS_MAP_TEST`、设备 MMIO 模型与调度语义。
- 不新增桌面 shell、动画、字体渲染引擎。

## 2. 总体架构

```
sf-init (PID 1, 串口 shell)
  └─ gui 命令：创建 service 端点 → 启动 sf-wm 模块 → 读取 SYS_CONSOLE_READ
       └─ IPC(OP_WM_INPUT / OP_WM_STOP) → sf-wm
sf-wm (ring-3, 无特权)
  ├─ SYS_FB_MAP 取得 SfFbInfo + 映射 SF_FB
  ├─ include/tiling.h + services/tiling.c（布局）
  └─ 直接写 framebuffer；串口日志输出确定性状态行
kernel
  └─ SYS_FB_GRANT(SF_FB_GRANT=32) / SYS_FB_MAP(SF_FB_MAP=33)
       └─ space_map_fb()：RW+NX+缓存禁用，最多 32 MiB
```

约束遵循：内核不解析用户格式；模块为 `.sfmod`；W^X（FB 映射 RW+NX）；ABI 只追加；`-Wall -Wextra -Werror`；先宿主测试后 QEMU。

## 3. ABI 变更（仅追加）

### 3.1 `include/sf.h`

```c
#define SF_FB 0x70000000ull       /* 用户侧 framebuffer 映射基址 */
#define SF_FB_BYTES 0x2000000ull  /* 32 MiB 上限 */

typedef struct {
    uint32_t size, width, height, pitch, bpp, reserved;
    uint64_t bytes;
} SfFbInfo;
_Static_assert(sizeof(SfFbInfo)==32,"framebuffer info ABI");

#define SYS_FB_GRANT 32
#define SYS_FB_MAP   33

/* WM 输入操作（追加到 OP_* 序列尾部，不复用既有值） */
#define OP_WM_INPUT  110
#define OP_WM_STOP   111
#define OP_WM_STATUS 112
```

系统调用语义：

- `SYS_FB_GRANT(a0=pid)`：仅 `pid==1` 可调用；目标必须是存活进程；记录单一授权 PID（可覆盖）。返回 `SF_OK`；非 PID1 → `SF_DENIED`；目标不存在 → `SF_NOT_FOUND`。
- `SYS_FB_MAP(a0=用户指针)`：调用者必须等于授权 PID，否则 `SF_DENIED`；无 framebuffer → `SF_NOT_FOUND`；`a0` 必须通过 `user_range(task,a0,sizeof(SfFbInfo),1)`（可写）否则 `SF_INVALID`；把 FB 物理页映射到 `SF_FB`（已映射则直接返回信息，不重复映射）；成功后 `copy_out` 写入 `SfFbInfo` 并返回 `SF_OK`；页表/页池不足 → `SF_NO_SPACE`。
- 不修改任何既有系统调用编号或结构体布局；`abi/x86_64-current.json` 仅追加。

### 3.2 内核数据与函数

`kernel/core.c`：

- 新增文件级静态变量：`fb_physical`、`fb_bytes`、`fb_width`、`fb_height`、`fb_pitch`、`fb_bpp`、`fb_grant_pid`。
- `kernel_main(SfBootInfo *b)` 在现有校验后保存并夹紧 FB 字段（必须：物理基址非 0 且 4 KiB 对齐；`framebuffer_size` 非 0 且 ≤ `SF_FB_BYTES`；`pitch>0`；`width/height` ≤ 8192），非法则全部清零（图形不可用）。
- 系统调用分发追加两个 `case`；`SYS_FB_MAP` 复用现有 `copy_out`/`user_range` 机制。
- `SYS_FB_GRANT` 校验 `current` 任务的 `pid`（主线程 `task_main`）。

`kernel/memory.c`：

```c
int space_map_fb(Task *t, uint64_t virtual, uint64_t physical, uint64_t bytes);
```

- 要求 `virtual==SF_FB`、`physical` 4 KiB 对齐、`bytes` 非 0 且 ≤ `SF_FB_BYTES`，否则 `SF_INVALID`。
- 按 4 KiB 逐页 `space_map(t,v,p,3)`（RW、NX），随后对每个 PTE 置 cache-disable 位 `0x18`；**不置** `PTE_MMIO`（framebuffer 不是设备寄存器，允许普通写）。
- 任一步失败返回错误；不新增全局页表，复用任务私有页表。

## 4. 布局核心：`include/tiling.h` + `services/tiling.c`

纯 C、无堆、无 libc，仅 `<stdint.h>`；返回码使用负值（`TILE_INVALID=-1`、`TILE_DENIED=-2`、`TILE_NO_SPACE=-3`、`TILE_EMPTY=-4`）。

```c
#define TILE_WORKSPACES 4u
#define TILE_COLUMNS 16u
#define TILE_WINDOWS 8u

typedef struct { uint64_t id; } TileWindow;
typedef struct {
    uint32_t count, active;          /* 窗口数 / 焦点窗口 */
    uint32_t width_permille;         /* 列宽，默认 500，范围 250..3000 */
    uint32_t height_permille;        /* 窗口高度，默认 1000，范围 250..3000 */
    TileWindow windows[TILE_WINDOWS];
} TileColumn;

typedef struct {
    uint32_t count, active;          /* 列数 / 焦点列 */
    int32_t scroll;                  /* 内容滚动像素，>=0 */
    TileColumn columns[TILE_COLUMNS];
} TileWorkspace;

typedef struct {
    uint32_t viewport_width, viewport_height;
    uint32_t outer_gap, gap, bar_height;
    uint32_t active;                 /* 活动工作区 */
    uint64_t next_id;
    TileWorkspace workspaces[TILE_WORKSPACES];
} TileLayout;

typedef struct { int32_t x, y; uint32_t width, height, visible; } TileRect;

void tile_init(TileLayout *, uint32_t viewport_width, uint32_t viewport_height,
               uint32_t bar_height, uint32_t outer_gap, uint32_t gap);
int  tile_add(TileLayout *, uint64_t *out_id);      /* 在焦点列焦点窗口之后插入 */
int  tile_close_focused(TileLayout *);
int  tile_consume(TileLayout *);                    /* 焦点窗口并入左侧列末尾 */
int  tile_expel(TileLayout *);                      /* 焦点窗口拆到右侧新列 */
int  tile_move_column(TileLayout *, int delta);     /* 焦点列左右移动 */
int  tile_focus_window(TileLayout *, int delta);    /* 列内上下移动，夹紧 */
int  tile_focus_column(TileLayout *, int delta);    /* 左右移动，夹紧 */
int  tile_focus_workspace(TileLayout *, int delta); /* 1..4 循环 */
int  tile_set_width(TileLayout *, int delta_permille);
int  tile_set_height(TileLayout *, int delta_permille);
int  tile_scroll_by(TileLayout *, int delta_pixels);
int  tile_scroll_to_focus(TileLayout *);
int  tile_layout(TileLayout *, TileRect *out, uint32_t capacity, uint32_t *count);
uint64_t tile_focused_id(const TileLayout *);
uint32_t tile_scroll(const TileLayout *);
```

布局规则（deterministic）：

- 内容区：`y = bar_height`，高 `viewport_height - bar_height`，宽 `viewport_width`。
- 列宽：`max(1, viewport_width * width_permille / 1000)`，默认 500‰（半屏，贴近 niri 双列观感）；列 x 从左到右累加，列间 `gap`，首列左侧与末列右侧各 `outer_gap`。
- 窗口：列内等高，`base=(content_h-(n-1)*gap)/n`，实际高 `max(1, base*height_permille/1000)`，从列顶向下堆叠、窗口间 `gap`；超出列底部的窗口仍返回几何，但 `visible=0`。
- `visible=1` 当且仅当矩形与内容区相交（允许部分相交；`x/y/width/height` 返回未裁剪几何，由 WM 裁剪）。
- 滚动：`scroll` 夹紧到 `[0, max(0, total_width + outer_gap - viewport_width)]`；`tile_scroll_to_focus` 最小化移动使焦点列完整可见（含 `outer_gap` 边距）。
- 空工作区：`count==0` 时布局返回 0 个矩形且 `TILE_EMPTY`。
- 容量错误：列/窗口/工作区已满时返回 `TILE_NO_SPACE` 且状态不变（严格无副作用）。

## 5. ring-3 WM：`services/sf_wm.c`

- 入口 `sf_main(const SfBootArgs*, size_t)`；`SYS_FB_MAP(&info)` 失败 → 打印 `WM: no framebuffer`，退出码 1。
- 仅支持 `bpp==32`；否则 `WM: unsupported bpp=%u` 并退出 1。
- 内存写：`volatile uint32_t *fb = (void*)SF_FB`，行步长 `pitch/4`；颜色常量集中定义（背景、栏、列、窗口、边框、焦点环）。
- 绘制内容：顶部工作区栏（4 个页签，活动页签高亮）、当前工作区各列/窗口矩形（间距、1px 边框、焦点窗口 2px 焦点环）、无文本渲染（仅几何与色块）。
- 输入：阻塞 `sf_recv` 服务端点；`OP_WM_INPUT` 的 `data[]` 为按键字节，支持 ASCII 与 ESC 序列：
  - `n` 新建窗口；`c` 关闭焦点窗口；`j/k` 列内上下焦点；`h/l` 列焦点；`H/L` 移动列；`w/W` 列宽增减；`e/E` 窗口高度增减；`C` consume；`x` expel。
  - `1..4` 切换工作区；`s` 滚动到焦点；方向键 `ESC [ A/B/C/D` 映射为 `k/j/h/l`。
  - `OP_WM_STOP`：退出主循环，打印最终状态后 `SYS_EXIT`。
  - `OP_WM_STATUS`：只打印状态行，不重绘。
- 每次状态变化后重绘并打印一行确定性日志：

```
WM: fb=%ux%u bpp=%u ws=%u col=%u win=%u scroll=%u checksum=%08x
```

`checksum` 为整个 framebuffer 的 FNV-1a 32 位（按 `uint32_t` 遍历，含 pitch 填充）。该值证明实际写入了 framebuffer。
- 退出日志：`WM: stopped`。

## 6. `sf-init` 集成：`gui` 命令

- 新增 `gui` 内建命令（仅 PID1 串口 shell 上下文可用；非 root 会话拒绝并打印 `gui: permission denied`）。
- 流程：
  1. 若内核未提供 framebuffer（通过 `SYS_FB_GRANT` 前无法直接查询；以模块日志为准）仍尝试启动，失败由模块日志体现。
  2. 复用 `net_start()` 的模块启动模式：创建 service 端点 → 在 bundle 中定位 `org.spiritfox.wm` → `sfmod_start` 并授予 service 句柄。
  3. 打印 `GUI: started pid=<pid>`；循环 `SYS_CONSOLE_READ` 读取字节。
  4. 字节 `0x1d`（Ctrl-]）→ 发送 `OP_WM_STOP`，等待/回收模块，打印 `GUI: stopped`，返回 shell。
  5. 其它字节原样打包为 `OP_WM_INPUT` 发送；发送失败（模块已退出）→ 打印 `GUI: wm exited` 并返回。
- 不改变现有 `command()` 中其它分支、串口提示符与启动自检。

## 7. 构建集成（`tools/build.py`）

在 `sf-fs` 打包之后追加：

```python
image = link_elf('sf-wm', shared + objects('wm', ['services/sf_wm.c', 'services/tiling.c']), 'services/user.ld')
packages.append(('sf-wm.sfmod', pack({
    'module_id': 'org.spiritfox.wm', 'version': '0.1.0', 'category': 'service',
    'requested_caps': [], 'resource_limits': {'memory': 262144}
}, image, 1, seed)))
```

不新增签名角色；使用现有 `seed`（module 角色）。

## 8. 测试与验证

### 8.1 宿主测试 `tests/host/test_tiling.c`

在 `tools/test.py` 的宿主测试区追加：

```python
host_test('test_tiling','tests/host/test_tiling.c','services/tiling.c')
```

覆盖：初始化默认值；新增窗口/列；焦点上下左右夹紧；关闭窗口与空列回收；consume/expel；列移动；工作区切换与隔离；宽度/高度夹紧到 250..3000；滚动夹紧与 `tile_scroll_to_focus`；布局矩形数量、列内不重叠、列间距正确、`visible` 判定、满容量返回 `TILE_NO_SPACE` 且状态不变。

### 8.2 QEMU 测试 `tests/qemu/qemu_gui_test.py`

- 参考 `tests/qemu/qemu_test.py` 的 `command()`；启动参数保持 `-display none -serial stdio -monitor none`；若固件不提供 GOP framebuffer，则测试命令追加 `-vga std`（以实际验证结果为准，测试内固定最终可用组合）。
- 断言序列：
  1. 启动完成出现串口提示符。
  2. 发送 `gui\n`，出现 `GUI: started pid=` 与首行 `WM: fb=`。
  3. 发送 `n`、`n`，出现 `win=3`（初始窗口 + 2）且 `col=` 递增。
  4. 发送 `l`、`j`、`c`，断言 `col`/`win`/`scroll` 按规格变化。
  5. 发送 `1`、`2`，断言 `ws=` 变化。
  6. 断言每次重绘的 `checksum` 至少出现两个不同值（证明 framebuffer 被实际写入）。
  7. 发送 `\x1d`，出现 `GUI: stopped`，随后串口 shell 可继续执行（发送 `help\n` 有响应）。
- 在 `tools/test.py` 的 QEMU 区、`qemu_net_test.py` 之后注册：`subprocess.run([sys.executable, 'tests/qemu/qemu_gui_test.py'], check=True)`。

### 8.3 ABI 快照

`tests/integration/test_abi_snapshot.py` 失败时，按脚本产出的 `build/abi-current.json` 更新 `abi/x86_64-current.json`（仅追加 `SYS_FB_GRANT`/`SYS_FB_MAP`/`SfFbInfo`/`OP_WM_*`），再复跑。

### 8.4 实际执行顺序

1. `python3 tools/build.py`
2. `python3 tools/test.py --host-only`（含新 `test_tiling`；ABI 快照检查）
3. `python3 tests/qemu/qemu_gui_test.py`
4. 全量 `python3 tools/test.py`（若时间允许；至少完成 1–3）

只有命令真实输出通过才可声明验证成功；失败需报告原始错误。

## 9. 文档

- 新增 `docs/architecture/GRAPHICS.md`（中文）：目标/非目标、FB 授权与映射、布局模型、WM 输入操作、串口测试方法、后续演进（真实输入、客户端协议、多显示器）。
- 最小更新：`README.md`（能力列表加“有界类 niri 平铺合成器”一行）、`docs/architecture/IMPLEMENTATION.md`（新增模块与 syscall 条目）；若存在 `ROADMAP.md`/`STATUS.md` 则各加一行状态，不存在则不创建。

## 10. 风险与回退

- **固件无 GOP/无显示**：`-display none` 下如果无 framebuffer，`gui` 打印 `WM: no framebuffer`，串口 shell 不受影响；测试固定使用可工作的 QEMU 显示组合。
- **页池压力**：32 MiB FB 最多额外 16 个页表页；若 `SF_NO_SPACE`，WM 报错退出，不影响系统其它功能。
- **ABI 兼容**：仅追加；旧程序与新内核、新程序与旧内核（旧内核返回 `SF_INVALID` 系统调用）均不破坏既有测试。
- **回退**：删除 `sf-wm` 包与 `gui` 命令即可回到纯串口路径；内核新增字段/系统调用保留但无调用者。

## 11. 需用户确认

请确认以下取舍后再进入实现：

1. 接受“有界垂直切片”（串口转发输入、无 GUI 客户端协议），而非完整 niri/Wayland 移植。
2. 接受新增 `SF_FB=0x70000000`、两个系统调用与 `SfFbInfo` ABI（仅追加）。
3. 接受 WM 无文本渲染、仅几何与色块 + 串口状态行的可视化范围。
4. 接受 Ctrl-] 作为 `gui` 退出键（与现有串口约定一致处沿用）。

## 12. 应用注册与搜索启动器（T12）

### 12.1 目标与非目标

目标：

1. GUI 应用在启动后可通过 IPC 把自身登记到合成器目录（应用 id + 显示标题），WM 不需要重新编译即可显示新条目。
2. `gui` 内按 `/` 打开搜索启动器；大小写不敏感的子串过滤（同时匹配 id 与标题）；`Enter` 打开选中项：已有窗口则跨工作区聚焦，没有窗口则新建占位窗口并回写窗口 id；`Ctrl-C` 关闭。
3. 保留既有 `WM: fb=...` 日志行格式与 QEMU 计数语义；搜索只改变像素与内部状态，不改变布局操作的计数含义。

非目标：真实 GUI 客户端协议与 surface 提交、应用进程启动（打开动作只产生占位窗口）、图标、鼠标、中文输入、持久化目录。

### 12.2 ABI（仅追加）

`include/sf.h` 的 `OP_WM_*` 序列追加：

```c
enum { OP_WM_INPUT=110, OP_WM_STOP, OP_WM_STATUS, OP_WM_REGISTER }; /* REGISTER=113 */
```

- `OP_WM_REGISTER` 的 `data[]` 为 `id\0title\0`（总长 ≤ `SF_MSG_BYTES`）。
- 合成器只把注册写入目录，不立即创建窗口，避免改变既有 QEMU 测试的 `win=` 计数。
- 授权模型：持有 WM service 端点发送权即视为可注册；`OP_WM_INPUT/OP_WM_STOP/OP_WM_STATUS` 仍只接受 PID 1，避免非 init 进程伪造输入。

### 12.3 启动器核心：`include/launcher.h` + `services/launcher.c`

纯 C、无堆、无 libc，仅 `<stdint.h>`；状态全部位于 `Launcher` 内：

```c
#define LAUNCHER_APPS 16u
#define LAUNCHER_ID_BYTES 40u
#define LAUNCHER_TITLE_BYTES 24u
#define LAUNCHER_QUERY_BYTES 24u
enum { LAUNCHER_OK=0, LAUNCHER_INVALID=-1, LAUNCHER_NO_SPACE=-2, LAUNCHER_NOT_FOUND=-3 };

typedef struct {
    char id[LAUNCHER_ID_BYTES];
    char title[LAUNCHER_TITLE_BYTES];
    uint64_t window;                 /* 0 表示尚未打开 */
} LauncherEntry;

typedef struct {
    uint32_t count, selected;
    uint32_t query_len;
    char query[LAUNCHER_QUERY_BYTES];
    LauncherEntry entries[LAUNCHER_APPS];
} Launcher;

void launcher_init(Launcher *);
int launcher_add(Launcher *, const char *id, const char *title);   /* 重复 id 只更新标题，保留 window */
uint32_t launcher_count(const Launcher *);
const LauncherEntry *launcher_at(const Launcher *, uint32_t index);
uint32_t launcher_search(const Launcher *, uint32_t *out, uint32_t capacity);
int launcher_query_push(Launcher *, char c);   /* 仅 0x20..0x7e，最多 23 字符 */
int launcher_query_pop(Launcher *);            /* 空查询返回 NOT_FOUND */
void launcher_query_clear(Launcher *);
int launcher_select(Launcher *, int delta);    /* 按当前匹配数夹紧 */
int launcher_set_window(Launcher *, const char *id, uint64_t window);
```

搜索语义：空查询匹配全部；非空查询对 id 与 title 做 ASCII 大小写不敏感子串匹配；结果保持目录顺序（稳定）；`out` 容量不足时只写前 `capacity` 个索引，但返回完整匹配数；查询变化把 `selected` 归零；`launcher_select` 在 `[0, matches-1]` 内夹紧，无匹配时归零。非法输入（空 id、超长 id/title、目录满）返回错误且状态不变。

### 12.4 布局核心追加

`include/tiling.h` / `services/tiling.c`：

```c
enum { /* ... */ TILE_NOT_FOUND=-5 };
int tile_focus_id(TileLayout *l, uint64_t id);
```

跨全部工作区、列、窗口查找 id；命中后设置活动工作区、焦点列、焦点窗口并调用 `tile_scroll_to_focus`；未命中返回 `TILE_NOT_FOUND` 且不修改任何状态。

### 12.5 合成器 `services/sf_wm.c`

- 启动：`launcher_init`；首个演示窗口登记为 `org.spiritfox.term` / `TERMINAL` / `window=1`。
- `OP_WM_REGISTER`：解析 `id\0title\0`，调用 `launcher_add`；接受后打印 `WM: app <title>`（独立于 `WM: fb=` 行，QEMU 正则可忽略）。
- 搜索状态：`/` 打开并清空查询；搜索打开时屏蔽全部布局键。
  - 可打印字符 `0x20..0x7e`：追加查询；
  - `0x08` / `0x7f`：退格；
  - `0x0d` / `0x0a`：激活选中项；
  - `0x03`（Ctrl-C）：关闭搜索；
  - `ESC [ A/B`：上/下选择（复用现有 ESC 解析状态）。
- 激活：条目 `window!=0` → `tile_focus_id`；否则 `tile_add` 并把新窗口 id 写回条目；随后关闭搜索并重绘。
- 叠加层：居中圆角面板 560×360；新增颜色 `WM_PANEL=0x00182028`（背景）与 `WM_SELECT=0x00406080`（选中行），边框用 `WM_ACCENT`。
  - 标题行 `SEARCH: <QUERY>`（查询显示转大写）；
  - 结果行标题用 scale 2，选中行铺 `WM_SELECT`；
  - 空结果绘制 `NO MATCH`；
  - 底部提示 `ENTER OPEN  ESC CLOSE`。
- 字体：5×7 位图扩到完整 `A-Z`、`0-9`、`-`、`_`、`.`、`/`、`:`、`<`、`>`；小写按大写绘制，未知字符按空格推进。
- `WM: fb=...` 状态行格式与输出时机保持不变。

### 12.6 注册示例应用与生命周期

- 新建 `services/sf_gui_probe.c`：`sf_main` 向 `a->service` 发送 3 条 `OP_WM_REGISTER`：`org.spiritfox.files`/`FILES`、`org.spiritfox.notes`/`NOTES`、`org.spiritfox.settings`/`SETTINGS`；任一条失败打印错误并退出 1；全部成功后 `for(;;) sf_yield()`。
- `tools/build.py`：追加 `sf-gui-probe` 链接与清单 `module_id=org.spiritfox.gui-probe`、`category=service`、无 `requested_caps`、`memory=131072`；包名 `sf-gui-probe.sfmod`。
- `services/init.c`：`gui_start` 在 WM 启动且 `SYS_FB_GRANT` 成功后 `spawn_package("sf-gui-probe.sfmod", wm_ep, RIGHT_SEND)`，保存 `probe_pid`；失败则回收 WM 并返回错误（`gui` 需要额外一个任务槽）。`gui_cleanup` 先杀 probe，再回收 WM。
- 失败路径：probe 启动失败时 `gui` 报 `gui: unavailable code=`；不改变串口其他命令。

### 12.7 测试

- 新宿主测试 `tests/host/test_launcher.c`，并在 `tools/test.py` 注册：
  ```python
  host_test('test_launcher','tests/host/test_launcher.c','services/launcher.c')
  ```
  覆盖：初始化；新增/重复 id 更新标题且保留 window；满容量与非法输入无副作用；大小写不敏感子串（id 与 title）；空查询全量；稳定顺序；容量不足的返回计数；查询追加/退格/长度上限；选择夹紧；`launcher_set_window`。
- `tests/host/test_tiling.c` 追加 `tile_focus_id`：跨工作区命中、未找到返回 `TILE_NOT_FOUND` 且状态不变。
- `tests/qemu/qemu_gui_test.py`：在既有序列末尾追加搜索流程：按 `/` → 输入 `FIL` → `Enter`，断言 `win` 增加 1 且校验和变化；再按 `/` → `Ctrl-C`，断言布局计数不变。并断言串口出现 `WM: app FILES`（证明 probe 注册成功）。
- `tests/qemu/qemu_gui_screenshot_test.py`：新增 `gui-search` 截图（打开搜索并输入 `TE`），断言面板背景 `WM_PANEL`、边框 `WM_ACCENT`、选中行 `WM_SELECT` 与文本像素；随后 `Ctrl-C` 关闭、`Ctrl-]` 退出。既有 15 个探针与 3 张截图保持不变。
- 若 ABI 快照测试提示不一致，按脚本产出的 `build/abi-current.json` 追加式更新 `abi/x86_64-current.json`。

### 12.8 文档与验证顺序

- `docs/architecture/GRAPHICS.md` 增加“应用注册与搜索”章节：新 opcode、注册示例、键位 `/`、`Enter`、`Ctrl-C`、`Backspace`、`↑`/`↓`，限制（窗口为占位、无真实客户端内容）。
- `docs/specs/graphics-niri/tasks.md` 增加 T12；`README.md` 的 `gui` 说明补一句“按 `/` 搜索应用”。
- 验证顺序：`python3 tools/build.py` → `python3 tools/test.py --host-only` → `python3 tests/qemu/qemu_gui_test.py` → `python3 tests/qemu/qemu_gui_screenshot_test.py` → `python3 tools/gui_run.py --seconds 12`。只有真实输出通过才声明成功。

## 13. 本轮需用户确认

1. 接受应用注册为“持有 WM service 端点即可登记”，注册只进目录、不立即建窗。
2. 接受打开注册应用只产生占位窗口（尚无真实客户端协议/进程启动）。
3. 接受搜索启动器为 WM 内部叠加层（串口按键驱动、无鼠标/图标）。
4. 接受 `gui` 额外拉起一个常驻 probe 任务用于演示注册（需要额外任务槽）。
