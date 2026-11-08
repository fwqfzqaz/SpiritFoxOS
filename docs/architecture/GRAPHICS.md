# 有界类 niri 图形切片

SpiritFoxOS 的图形路径是一条**有界纵向切片**：固件 framebuffer + 一个 ring-3 合成器 + 固定容量布局核心。它不是 niri/Wayland 移植，不引入协议栈、GPU、DRM/KMS 或堆分配。

## 目标与非目标

目标：

- 内核只新增“framebuffer 授权/映射”机制，不解析用户格式。
- 合成器 `sf-wm` 在 CPL3 运行，直接写显示内存，级联使用纯 C 的可滚动平铺模型。
- 布局核心可在宿主测试中完整验证；QEMU 无显示环境下用串口状态行与 framebuffer 校验和断言真实绘制。
- 保留无图形/串口回退：不运行 `gui` 时系统行为不变。

非目标：USB HID 完整驱动、GUI 客户端协议、动画、字体渲染、多显示器、GPU 加速、完整 niri 配置/手势语义。

## framebuffer 授权与映射

| 项目 | 值 |
| --- | --- |
| 用户映射基址 | `SF_FB = 0x70000000` |
| 映射上限 | `SF_FB_BYTES = 32 MiB` |
| 授权系统调用 | `SYS_FB_GRANT(32)`：仅 PID 1 可调用，记录单一目标 PID |
| 映射系统调用 | `SYS_FB_MAP(33)`：仅被授权 PID，返回 `SfFbInfo` 并映射显示内存 |

内核在 `kernel_main` 保存 `SfBootInfo` 的 framebuffer 字段，并要求：物理基址非零且 4 KiB 对齐、`framebuffer_size <= 32 MiB`、`pitch > 0`、宽高 `<= 8192`、`pitch*height <= framebuffer_size`；任一不满足即视为无图形，串口路径照常。

`space_map_fb()` 逐页建立 `RW + NX` 映射并置缓存禁用位 `0x18`，**不置** `PTE_MMIO`（framebuffer 是普通可写内存，不是设备寄存器）。映射只消耗任务的页表页，不占用私有页池数据页。

授权是单 PID 的：新的 `SYS_FB_GRANT` 会覆盖旧目标；目标进程死亡后不会残留可映射权限。

## 布局模型

布局核心位于 `include/tiling.h` 与 `services/tiling.c`，固定容量：

- 4 个工作区；每工作区 16 列；每列最多 8 个窗口。
- 列从左到右排列，列内窗口纵向堆叠（niri 的核心形态）。
- 每列有独立宽度千分比（`250..3000`，默认 `500`），每列有窗口高度千分比（同范围）。
- 每工作区有非负滚动偏移；`tile_scroll_to_focus` 以最小移动让焦点列完整可见（列宽超过视口时左对齐）。
- 操作：新增（插入焦点窗口之后）、关闭、`consume`（并入左侧列末尾）、`expel`（拆到右侧新列）、列移动、窗口/列/工作区焦点、宽度/高度调整、滚动。
- 所有焦点索引在变更后夹紧；容量耗尽返回 `TILE_NO_SPACE` 且状态不变。
- `tile_layout` 返回活动工作区的未裁剪矩形与 `visible` 标记；空工作区返回 `TILE_EMPTY`。

## 合成器 `sf-wm`

- 模块 ID `org.spiritfox.wm`，`category=service`，无额外能力；由 `sf-init` 通过模块管理器启动并授予服务端点。
- `sf_main` 调用 `SYS_FB_MAP`（对 PID 1 授权时序做有界重试），仅接受 32bpp；否则打印确定性错误并退出 1。
- 绘制：圆角窗口（半径 8、1px 边框；焦点窗口外加 2px 金色圆角焦点环）、按窗口 id 奇偶交替的正文色、左上角 `W<id>` 圆角徽标；上栏高 32px：左侧 4 个工作区 pill，中间 `WS/COL/WIN` 状态文本，右侧可点击的 `SHELL` 与 `SEARCH` 按钮（悬停变亮、打开态高亮），全部使用内置 5x7 位图字体（不引入字体库、不分配堆）。
- 输入：阻塞接收服务端点消息；`sf-init` 把串口字节逐字节打包为 `OP_WM_INPUT`（仅在 SFK 打开时作为文本输入），并把指针状态打包为 `OP_WM_POINTER`。
- 每次状态变化重绘并输出一行：

```
WM: fb=<宽>x<高> bpp=<bpp> ws=<1..4> col=<列数> win=<窗口数> scroll=<像素> checksum=<FNV-1a 32>
```

`checksum` 覆盖 `pitch/4 * height` 个 32 位字，证明显示内存确实被写入。`OP_WM_STATUS` 仅打印状态，`OP_WM_STOP` 打印最终状态与 `WM: stopped` 后退出。

## 上栏与指针输入

图形化不再接受任何布局快捷键；桌面操作全部由指针命中完成。

- 内核 `kernel/ps2.c` 以 IRQ12 中断驱动 PS/2 指针（i8042 端口 `0x60/0x64`）：`0xA8` 启用 AUX，`0xD4`+`0xF4` 启用数据上报，最后打开 i8042 的 AUX 中断并解除 8259 的级联 IRQ2/IRQ12 屏蔽；中断里排空 i8042、解析 3 字节包，把绝对坐标快照压进 16 项内核环形缓冲。PID1 经 `SYS_POINTER_READ` 只弹快照，突发移动不会因 i8042 队列（约 5 包）满而丢位移；中断未接通时同一读取路径仍以轮询兜底。
- 指针加速对齐 niri 的默认输入行为（niri 把 `accel-profile=adaptive` 交给 libinput）：以包内最大轴位移近似速度，≤3px 保持 1:1 精细瞄准，随速度线性放大至 1.75 倍；增益按 1/16 像素累加，小位移不丢精度，闭环瞄准仍可收敛。
- `sf-init` 在 `gui` 循环中调用 `SYS_POINTER_READ(34)`：`a=1` 复位到屏幕中心并清空积压，`a=0` 取事件。返回值打包为 `bit31 事件标记 | buttons[26..28] | y[13..25] | x[0..12]`，无事件返回 `SF_WOULD_BLOCK`。仅 PID 1 可调用。
- 同一轮询周期内环形缓冲中的多个快照合并为最新位置，再以 `OP_WM_POINTER=114` 发给合成器，避免逐包全屏重绘；`OP_WM_POINTER` 与 `OP_WM_INPUT` 一样只接受 PID 1。
- 上栏高 32px：左侧 4 个工作区 pill（点击 `tile_focus_workspace`），中间 `WS/COL/WIN` 状态，右侧 `SHELL` 与 `SEARCH` 按钮；悬停用选中色、打开态保持激活色。
- 光标为 12×12 箭头位图（四向黑色描边 + 白色填充），使用 14×14 静态备份区做增量重画：指针移动只恢复旧区域并绘制新箭头，不整屏重绘、不重算校验和；只有按下或悬停目标变化时才重绘场景，因此快速滑动不会闪烁。
- `sf-init` 的 `gui` 循环每轮弹出环形缓冲中的全部快照并合并为最新位置；10ms 定时器节拍只影响转发时刻，不再决定是否丢包。
- 命中顺序：SFK `X` 关闭 → 搜索面板行/面板外关闭 → 上栏工作区 pill → `SHELL` / `SEARCH` → 内容区窗口聚焦。
- `Ctrl-]`（`0x1d`）仍由 `sf-init` 拦截为退出 `gui` 回到串口 shell，不属于图形化快捷键。

## 应用注册与搜索

应用可在启动后把自身登记到合成器目录，无需重新编译 WM：

- 新增 ABI `OP_WM_REGISTER=113`，消息 `data` 为 `id\0title\0`；WM 只把条目写入固定容量 16 的目录，不立即创建窗口，因此不改变既有 `win=` 计数。
- 授权模型：持有 WM service 端点发送权即视为可注册；`OP_WM_INPUT/STOP/STATUS` 仍只接受 PID 1，避免伪造输入。
- 示例模块 `org.spiritfox.gui-probe`（`services/sf_gui_probe.c`）向端点注册 `FILES`/`NOTES`/`SETTINGS` 后常驻；`gui` 在 WM 就绪后拉起它，退出时先回收 probe。
- 目录与搜索核心为 `include/launcher.h` + `services/launcher.c`：固定数组、无堆、无 libc；匹配 id 与标题、ASCII 大小写不敏感、结果顺序稳定。

搜索启动器改为纯指针操作：

- 点击上栏 `SEARCH` 打开居中的 620×420 圆角应用列表；点击任意行打开应用（已有窗口则跨工作区聚焦，否则新建窗口并回写目录）。
- 点击面板外关闭；行悬停高亮，右侧显示该应用已绑定的窗口 `W<id>`。
- 面板标题 `SEARCH APPS`、副标题 `CLICK AN APP TO OPEN`、底部 `CLICK OUTSIDE TO CLOSE`；不再接受键盘查询。
- 限制：窗口仍是合成器占位模型，打开动作不启动真实应用进程，也没有图标或中文输入。

## SFK 图形 shell

`sf-wm` 内置一个有界图形 shell「SFK」：点击上栏 `SHELL` 打开；点击面板右上角 `X` 或输入 `exit` 关闭。打开时其它按键都不是快捷键，只有输入行接受文本编辑。

- 核心为 `include/sfk.h` + `services/sfk.c`：输入缓冲 48 字节（最多 47 字符）、12 行 × 64 字节滚动历史；固定数组、无堆、无 libc。
- 叠加层为居中约 1216×720 的圆角面板（随 framebuffer 夹紧）：标题 `SFK // SPIRITFOX GRAPHICAL SHELL`、右上角 `X` 关闭按钮、历史行、带块状光标的 `SFK> <INPUT>` 输入行、底部命令提示。
- 提交时先把原始输入写入历史，再执行动作；输入与历史在两次打开之间保留。
- 每次文本输入仍输出一行既有格式的 `WM:` 状态行；执行布局命令（`open`/`close`/`ws`/`layout`/`new`）会即时重绘桌面。

SFK 打开时的文本输入（不是图形化快捷键）：

| 键 | 操作 |
| --- | --- |
| 可打印字符 | 追加输入（最多 47 字符） |
| `Backspace`（`0x08`/`0x7f`） | 删除末尾字符 |
| `Enter`（`0x0d`/`0x0a`） | 提交并执行 |

命令大小写不敏感：

| 命令 | 动作 |
| --- | --- |
| `help` | 列出命令 |
| `apps` | 列出目录条目与已绑定窗口（`W<id>` 或 `-`） |
| `open <app>` | 按标题或 id 精确匹配（大小写不敏感）：已有窗口则跨工作区聚焦，否则新建窗口并回写目录 |
| `close` | 关闭焦点窗口 |
| `ws <1-4>` | 切换到指定工作区 |
| `layout [333\|500\|667\|1000]` | 设置列宽千分比；无参数或非法值时按 333→500→667→1000 循环 |
| `new` | 新建窗口 |
| `search` | 关闭 shell 并打开应用搜索面板 |
| `clear` | 清空历史 |
| `echo <text>` | 回显文本 |
| `about` | 版本与实现边界 |
| `exit` | 关闭 shell |

未知命令打印 `UNKNOWN: <cmd>`；参数缺失或非法只打印用法提示，不改变布局。SFK 不引入新 ABI，也不启动真实应用进程，与搜索面板一样仍是有界占位模型。

## `gui` 命令

仅 root 会话可用的内置命令：

1. 通过模块管理器登记并启动 `org.spiritfox.wm`。
2. 创建服务端点、启动 `sf-wm.sfmod`、调用 `SYS_FB_GRANT` 授权该进程。
3. 启动 `sf-gui-probe.sfmod`（授予服务端点发送权）演示应用注册；失败则回收并报 `gui: unavailable code=`。
4. 打印 `GUI: started pid=<pid>`，把指针复位到屏幕中心；随后循环拉取 `SYS_POINTER_READ` 并以 `OP_WM_POINTER` 转发（合并同周期移动），同时读取 `SYS_CONSOLE_READ` 作为 SFK 文本输入转发。
5. `Ctrl-]` 发送 `OP_WM_STOP`，先回收 probe、再有界等待模块结束，回收端点与模块状态，打印 `GUI: stopped`。
6. 指针或按键转发失败时打印 `GUI: wm exited` 并返回 shell。

`gui` 阻塞期间仍周期性运行 `echo_supervise()`、`native_supervise()`、`manager_serve()`，避免既有服务监督停顿。

## 测试

- 宿主：`tests/host/test_tiling.c` 覆盖初始化、增删、焦点夹紧、列内窗口交换、consume/expel、列移动、工作区隔离、宽高夹紧、滚动夹紧、按 id 跨工作区聚焦与布局矩形/容量语义；`tests/host/test_launcher.c` 覆盖目录容量、重复 id 更新、大小写不敏感搜索、查询编辑、选择夹紧与满容量无副作用；`tests/host/test_sfk.c` 覆盖输入编辑与 47 字符上限、12 行历史淘汰、非法行无副作用、空输入、大小写不敏感命令解析、参数/未知命令与输出指针为空。
- QEMU（状态层）：`tests/qemu/qemu_gui_test.py` 使用 `-display none -vga std`（固件建立 GOP framebuffer 但不打开窗口），通过串口执行 `gui`，再用 QMP `input-send-event` 注入 PS/2 相对位移与左键。断言初始布局、快速滑动不产生整屏状态行且坐标不落后、指针坐标回报、点击 `SHELL`/`SEARCH`、点击应用行 `APP NOTES`、点击面板外 `CLOSE`、点击工作区 pill 切换 `ws`、内容区点击 `FOCUS W1`、`WM: app FILES` 注册日志、至少两个不同 framebuffer 校验和；并验证旧布局键（`n`/`` ` ``/`/`/数字/hjkl`）完全不产生新状态行，最后 `Ctrl-]` 退出且串口 shell 继续响应。
- QEMU（像素层）：`tests/qemu/qemu_gui_screenshot_test.py` 在同样的无显示 QEMU 上通过 QMP `screendump` 导出 PPM，解析 P6 像素并断言真实扫描输出：上栏（工作区 pill、`SHELL`/`SEARCH` 按钮）、悬停高亮、圆角窗口边框与正文、焦点环、窗口徽标、上栏文本、光标箭头、搜索面板（背景/边框/行悬停/激活按钮/标题/行文本/提示）、同列上下平铺窗口，以及 SFK 叠加层（面板背景/边框/输入框/`X` 按钮、标题/历史/提示文本），共 23 个探针、5 张截图。这是“图形化确实跑通”的直接证据，而不是仅依赖 framebuffer 校验和。若环境装有 Pillow，会同时生成 `build/gui-ws1.png` / `gui-hover.png` / `gui-search.png` / `gui-stacked.png` / `gui-sfk.png`。

## 开发者可视化运行

本仓库使用的 QEMU 构建通常只包含 `none` 与 `vnc` 显示后端（无 gtk/sdl），因此交互式观察走 VNC，而不是 `-display gtk`。

```sh
python3 tools/build.py                       # 生成 build/esp 与 sf-wm.sfmod
python3 tools/gui_run.py                     # 启动 VNC 并自动进入 gui
```

`gui_run.py` 的行为：

- 以 `q35 + tcg + OVMF + std VGA + -serial stdio` 启动，默认 `-vnc 127.0.0.1:1`。
- 串口输出中检测到 `Type help.` 后自动发送 `gui\n`，无需手工输入。
- 在 VNC 窗口中移动鼠标即可操作上栏与搜索面板（PS/2 相对指针）；终端按键只在 SFK 打开时作为文本输入转发，`Ctrl-]` 退出 `gui` 回到 shell 并保持 QEMU 运行。
- `--vnc 127.0.0.1:2` 换端口；`--display gtk|sdl` 供本机已编译该后端时使用；`--seconds N` 到点自动发送 `Ctrl-]` 并结束，适合快速冒烟。

用 VNC 客户端连接 `127.0.0.1:1`（默认 5901 端口）即可看到布局实时变化并用鼠标点击上栏；宿主机终端同时保留串口日志与 `WM:` 状态行。

截图验证无需打开 VNC：

```sh
python3 tests/qemu/qemu_gui_screenshot_test.py
```

它在 `build/` 下生成 `gui-ws1.ppm` / `gui-hover.ppm` / `gui-search.ppm` / `gui-stacked.ppm` / `gui-sfk.ppm`（有 Pillow 时另有 PNG），可直接查看上栏、按钮悬停、搜索面板、同列平铺窗口与 SFK 图形 shell 的实际画面。

## 回退与边界

- 不运行 `gui`：系统与既有串口/VNC 路径完全一致。
- 固件无 framebuffer：`WM: no framebuffer`，`gui` 可用性由模块日志体现，shell 不中断。
- ABI：仅追加 `SF_FB`、`SfFbInfo`、`SYS_FB_GRANT/SYS_FB_MAP`、`SYS_POINTER_READ`、`OP_WM_*`，不修改既有编号与结构。
- 删除 `sf-wm` 包与 `gui` 命令即可移除图形路径，内核新增字段/系统调用保留但无调用者。

## 后续演进

1. 真实输入：把当前轮询式 PS/2 指针提升为中断驱动，并增加 USB HID 与键盘事件转发。
2. 客户端协议：最小窗口 surface 提交/缓冲交换与结束同步，仍保持有界对象。
3. 多显示器/热插拔：按输出枚举 framebuffer，布局核心增加输出维度。
4. 合成加速：仅在有真实 GPU 驱动与内存隔离后再考虑。
