# Tasks：类 niri 可滚动平铺图形环境

对应规格：[spec.md](file:///home/fwqfzqaz/work/SpiritFoxOS/docs/specs/graphics-niri/spec.md)
执行顺序即编号顺序；每项完成后用真实命令验证，不跳步。

## T1 布局核心（纯 C，可宿主测试）

- [x] 新建 `include/tiling.h`：按规格第 4 节定义常量、`TileWindow`/`TileColumn`/`TileWorkspace`/`TileLayout`/`TileRect` 与全部 API 原型。
- [x] 新建 `services/tiling.c`：实现 `tile_init/add/close_focused/consume/expel/move_column/focus_window/focus_column/focus_workspace/set_width/set_height/scroll_by/scroll_to_focus/layout/tile_focused_id/tile_scroll`。
- [x] 约束：无堆、无 libc、仅 `<stdint.h>`；满容量失败必须无副作用；所有焦点索引夹紧。
- 验收：`python3 tools/build.py` 前先能单独编译（见 T3 测试）。

## T2 宿主测试

- [x] 新建 `tests/host/test_tiling.c`：覆盖规格 8.1 列出的全部用例，含“满容量不改变状态”与“列内矩形不重叠/间距正确”。
- [x] `tools/test.py` 宿主测试区追加 `host_test('test_tiling','tests/host/test_tiling.c','services/tiling.c')`。
- 验收：`python3 tools/test.py --host-only` 中 `test_tiling` 通过（保留输出）。

## T3 ABI 追加

- [x] `include/sf.h`：追加 `SF_FB`、`SF_FB_BYTES`、`SfFbInfo`（含 `_Static_assert(sizeof==32)`）、`SYS_FB_GRANT 32`、`SYS_FB_MAP 33`、`OP_WM_INPUT/STOP/STATUS`。
- [x] `abi/x86_64-current.json`：仅在 ABI 快照测试提示不一致后，用脚本产出的 `build/abi-current.json` 做**追加式**更新。
- 验收：`python3 tests/integration/test_abi_snapshot.py` 通过。

## T4 内核 framebuffer 授权与映射

- [x] `kernel/core.c`：新增 FB 静态状态；`kernel_main` 校验并保存 `SfBootInfo` FB 字段（非法清零）；系统调用分发新增 `SYS_FB_GRANT`（仅 PID1，记录目标）与 `SYS_FB_MAP`（校验授权/指针/可用性，调用 `space_map_fb`，`copy_out` 返回 `SfFbInfo`）。
- [x] `kernel/memory.c`：新增 `space_map_fb(Task*, virtual, physical, bytes)`，逐页 `space_map(...,3)` + 置 `0x18` 缓存禁用位，不置 `PTE_MMIO`，上限 `SF_FB_BYTES`。
- 验收：`python3 tools/build.py` 零警告零错误；宿主 `test_memory` / `test_memory_quota` 继续通过。

## T5 ring-3 WM 模块

- [x] 新建 `services/sf_wm.c`：`sf_main` 映射 FB、初始化 `TileLayout`、绘制栏/列/窗口/焦点环、处理 `OP_WM_INPUT/STOP/STATUS`、按规格输出 `WM: fb=... checksum=...` 与 `WM: stopped`。
- [x] 输入映射严格按规格第 5 节（含 `ESC [ A/B/C/D`、`x` expel、`1..4` 工作区、`Ctrl-]` 由 init 处理）。
- [x] 仅 32bpp；无 framebuffer / 非 32bpp 时打印确定性错误并退出 1。
- 验收：编译通过；QEMU 中日志行与 checksum 变化符合预期。

## T6 `sf-init` 的 `gui` 命令

- [x] `services/init.c`：新增 `gui` 分支（root/PID1）；复用模块启动序列拉起 `org.spiritfox.wm`；循环读 `SYS_CONSOLE_READ` 并 `OP_WM_INPUT` 转发；`0x1d` 发送 `OP_WM_STOP` 并回收；打印 `GUI: started pid=` / `GUI: stopped` / 失败信息。
- [x] 不修改既有命令、提示符、启动自检行为。
- 验收：QEMU 中 `gui` 可进可出，退出后 `help` 仍响应。

## T7 构建打包

- [x] `tools/build.py`：追加 `sf-wm` 链接（`services/sf_wm.c` + `services/tiling.c`）与 `sf-wm.sfmod` 清单（`module_id=org.spiritfox.wm`、`category=service`、无 caps、memory 262144）。
- 验收：`python3 tools/build.py` 成功且 `build/sf-wm.sfmod` 存在。

## T8 QEMU 图形测试

- [x] 新建 `tests/qemu/qemu_gui_test.py`：启动无显示 QEMU（必要时固定 `-vga std`），按规格 8.2 的 7 步断言（含 checksum 至少两个不同值、退出后 shell 可用）。
- [x] `tools/test.py` QEMU 区注册该测试（`qemu_net_test.py` 之后）。
- 验收：`python3 tests/qemu/qemu_gui_test.py` 通过并保留输出。

## T8b QEMU 真实像素验证（跑通图形化）

- [x] 新建 `tests/qemu/qemu_gui_screenshot_test.py`：QMP `screendump` 导出 PPM，解析 P6 并断言顶栏/页签/窗口边框/正文/空工作区背景的真实像素。
- [x] `tools/test.py` 中在该测试之后注册截图测试。
- [x] 新建 `tools/gui_run.py`：VNC 交互运行入口（自动执行 `gui`、终端按键转发、`Ctrl-]` 退出、`--seconds` 冒烟）。
- [x] 新建 `docs/architecture/GRAPHICS.md` 的“开发者可视化运行”章节，说明 VNC/截图/PNG。
- 验收：`python3 tests/qemu/qemu_gui_screenshot_test.py` 输出 `GUI SCREENSHOT PASSED`；`python3 tools/gui_run.py --seconds 12` 可见 `GUI: started` 与 `WM:` 状态行。

## T9 文档

- [x] 新建 `docs/architecture/GRAPHICS.md`（中文，内容见规格第 9 节）。
- [x] `README.md` 能力列表追加一行；`docs/architecture/IMPLEMENTATION.md` 追加模块与系统调用条目；若存在 `ROADMAP.md`/`STATUS.md` 各加一行，不存在则不创建。
- 验收：文档与实现常量/命令一致（人工核对）。

## T11 视觉与交互完善（niri 参考）

- [x] `include/tiling.h` / `services/tiling.c`：新增 `tile_swap_window`（列内 move-window-down/up），越界拒绝且状态不变。
- [x] `tests/host/test_tiling.c`：覆盖交换、边界、空工作区与单窗口。
- [x] `services/sf_wm.c`：圆角窗口/焦点环、`W<id>` 徽标、内置 5x7 位图字体顶栏数字与 `WS/COL/WIN` 状态文本、按窗口 id 奇偶交替配色。
- [x] 新键位：`J/K` 交换、`r` 列宽预设、`,`/`.` 按 1/4 视口滚动、`g/G` 首末列。
- [x] `tests/qemu/qemu_gui_test.py`：断言滚动夹紧 32、667‰ 预设后 245、`J` 交换改变校验和。
- [x] `tests/qemu/qemu_gui_screenshot_test.py`：圆角/焦点环/徽标/文本/列间距/交替色，15 探针、3 张截图。
- [x] 更新 `docs/architecture/GRAPHICS.md` 的绘制、键位与测试章节。
- 验收：`TILING PASS: 391 checks`、`GUI TEST PASSED: 13 status lines, checksums=11`、`GUI SCREENSHOT PASSED: 1280x800, 15 pixel probes, 3 captures`。

## T12 应用注册与搜索启动器

规格：[spec.md](file:///home/fwqfzqaz/work/SpiritFoxOS/docs/specs/graphics-niri/spec.md) 第 12 节。

- [x] 新建 `include/launcher.h` + `services/launcher.c`：固定容量 16 的应用目录、大小写不敏感子串搜索（id 与 title）、查询编辑与选择夹紧；无堆、无 libc、非法/满容量无副作用。
- [x] 新建 `tests/host/test_launcher.c`，并在 `tools/test.py` 注册 `host_test('test_launcher','tests/host/test_launcher.c','services/launcher.c')`。
- [x] `include/tiling.h` / `services/tiling.c`：新增 `TILE_NOT_FOUND=-5` 与 `tile_focus_id`（跨工作区/列/窗口聚焦并滚动）；`tests/host/test_tiling.c` 覆盖命中与未找到不改状态。
- [x] `include/sf.h`：追加 `OP_WM_REGISTER=113`（`data` 为 `id\0title\0`）。
- [x] `services/sf_wm.c`：启动时 `launcher_init` 并登记 `org.spiritfox.term`/`TERMINAL`/window=1；处理注册（打印 `WM: app <title>`）；`/` 打开搜索、Ctrl-C 关闭、可打印字符/退格/Enter/↑↓；Enter 走 `tile_focus_id` 或 `tile_add` 并回写窗口；居中叠加层面板与扩展 5x7 字体；既有日志行不变。
- [x] 新建 `services/sf_gui_probe.c`：向 `a->service` 注册 `FILES`/`NOTES`/`SETTINGS`，失败退出 1，成功常驻让出 CPU。
- [x] `tools/build.py`：追加 `sf-gui-probe` 映像与 `sf-gui-probe.sfmod` 清单（`module_id=org.spiritfox.gui-probe`、`category=service`、无 caps、memory 131072）。
- [x] `services/init.c`：`gui_start` 在 WM 就绪后拉起 probe（`RIGHT_SEND`）并保存 pid；`gui_cleanup` 先杀 probe；失败路径有界回收。
- [x] `tests/qemu/qemu_gui_test.py`：末尾追加 `/`→`FIL`→Enter 建窗与校验和变化、`/`→Ctrl-C 不改计数、`WM: app FILES` 注册日志。
- [x] `tests/qemu/qemu_gui_screenshot_test.py`：新增 `gui-search` 截图与面板/边框/选中行/文本像素探针，Ctrl-C 后再 Ctrl-] 退出。
- [x] `docs/architecture/GRAPHICS.md` 增加“应用注册与搜索”章节；`README.md` 补一句 `gui` 内按 `/` 搜索。
- 验收：`python3 tools/build.py`；`python3 tools/test.py --host-only`（含 `test_launcher` 与扩展后的 `test_tiling`）；两个 QEMU GUI 测试；`python3 tools/gui_run.py --seconds 12`。

## T13 SFK 图形 shell

- [x] 新建 `include/sfk.h` + `services/sfk.c`：输入缓冲 48 字节（最多 47 字符）、12 行 × 64 字节滚动历史；`sfk_init/push/backspace/clear_input/print/clear/submit` 与只读历史访问器；大小写不敏感命令解析（HELP/APPS/OPEN/CLOSE/WS/LAYOUT/SEARCH/CLEAR/ECHO/ABOUT/EXIT/NEW/UNKNOWN/NONE），非法输入与满容量无副作用。
- [x] 新建 `tests/host/test_sfk.c`，并在 `tools/test.py` 注册 `host_test('test_sfk','tests/host/test_sfk.c','services/sfk.c')`。
- [x] `services/sf_wm.c`：`` ` ``（`0x60`）打开/切换叠加层；打开时独占输入（字符/退格/Enter/Ctrl-C），执行全部命令；`open` 复用启动器目录与 `tile_focus_id`/`tile_add`；约 1216×720 面板绘制标题、历史、`SFK>` 输入与光标、底部提示。
- [x] 状态行改为单次 `SYS_LOG` 写入，避免与 `sf-init` 的分片日志交错拆行；格式与语义不变。
- [x] `tools/build.py`：`sf-wm` 源追加 `services/sfk.c`。
- [x] `tests/qemu/qemu_gui_test.py`：搜索流程后追加 `` ` ``→`apps`→`open notes`（win +1）→`ws 2`→`exit` 的状态断言。
- [x] `tests/qemu/qemu_gui_screenshot_test.py`：新增 `gui-sfk` 截图与面板/边框/输入框/标题/历史/提示探针，Ctrl-C 关闭后再 `Ctrl-]` 退出。
- [x] 更新 `docs/architecture/GRAPHICS.md` 的 SFK 章节、键位表与测试说明；`README.md` 补一句 `` ` `` 打开 SFK。
- 验收：`SFK PASS: 325 checks`；`GUI TEST PASSED: 47 status lines, checksums=43`；`GUI SCREENSHOT PASSED: 1280x800, 21 pixel probes, 5 captures`；`python3 tools/gui_run.py --seconds 12` 可见 `GUI: started` 与 `WM:` 状态行。

## T14 取消图形化快捷键、niri 上栏与指针输入

- [x] 新增 `kernel/ps2.c`：i8042 中断驱动（`0xA8` 启用 AUX、`0xD4`+`0xF4` 启用上报、打开 AUX 中断并放开级联 IRQ2/IRQ12）、3 字节包解析、9 位符号扩展与屏幕钳制；中断侧写入 16 项环形缓冲，读取侧 `SYS_POINTER_READ` 弹快照，保留轮询兜底。
- [x] `include/sf.h` 追加 `SYS_POINTER_READ=34`（仅 PID 1；`a=1` 复位居中并清积压，`a=0` 返回打包状态，无事件 `SF_WOULD_BLOCK`）与 `OP_WM_POINTER=114`。
- [x] `kernel/core.c` 启动时 `ps2_mouse_init`，系统调用分发处理 `SYS_POINTER_READ`；`kernel/kernel.h` 增加原型；`tools/build.py` 加入 `kernel/ps2.c`。
- [x] `services/init.c`：`gui` 循环复位指针，合并同周期移动后转发 `OP_WM_POINTER`；`Ctrl-]` 仍为退出 `gui`。
- [x] `services/sf_wm.c`：删除全部布局快捷键与 `/`、`` ` ``、Esc 序列、搜索键盘处理；上栏高 32px（工作区 pill + `WS/COL/WIN` + 可点击 `SHELL`/`SEARCH`，悬停/激活态）；搜索改为点击行打开、点击外部关闭；SFK 仅经 `SHELL` 打开并新增右上角 `X`；12×12 箭头光标绘制在叠加层之上。
- [x] `tests/host/core_fixture.h`：为包含 `kernel/core.c` 的宿主测试提供 PS/2 桩函数。
- [x] `tests/qemu/qemu_gui_test.py`：改用 QMP `input-send-event` 注入 PS/2 相对位移与左键，闭环校正坐标；覆盖 SHELL/SEARCH/应用行/外部关闭/工作区 pill/窗口聚焦，并断言旧快捷键不产生任何状态行。
- [x] `tests/qemu/qemu_gui_screenshot_test.py`：改为指针流程，新增按钮/悬停/光标/搜索行/同列平铺/SFK `X` 探针。
- [x] 更新 `docs/architecture/GRAPHICS.md`（上栏与指针、搜索、SFK、`gui`、测试章节）与 `README.md`。
- [x] 指针移动改为 14×14 备份区增量重画（只恢复旧光标区域并绘制新箭头），仅按下或悬停目标变化时整屏重绘；`gui` 轮询降到 1ms 并合并同周期移动，消除快速滑动时的掉包与闪屏。
- [x] 指针加速对齐 niri/libinput 的 adaptive 曲线（低速 1:1、快速放大至 1.75 倍，1/16 像素累加），快速甩动不再欠调。
- [x] PS/2 改为 IRQ12 中断驱动 + 16 项内核环形缓冲：中断侧排空 i8042 入环，`kernel/core.c` 在 0x2c/0x22/0x50 三条可能路径统一处理并 EOI，`kernel/device.c` 把 GSI12 配成 ISA 边缘/高电平有效；突发 40 包（i8042 仅约 5 包容量）全部保留，位移终值与注入一致。
- 验收：`python3 tools/build.py`；`python3 tools/test.py --host-only --skip-build`；`GUI TEST PASSED: 30 status lines, checksums=29`；`GUI SCREENSHOT PASSED: 1280x800, 23 pixel probes, 5 captures`；`python3 tools/gui_run.py --seconds 12`。

## T10 全量验证

- [x] `python3 tools/build.py`
- [x] `python3 tools/test.py --host-only`
- [x] `python3 tests/qemu/qemu_gui_test.py`
- [x] `python3 tests/qemu/qemu_gui_screenshot_test.py`（1280x800，15 个像素探针、3 张截图）
- [x] `python3 tools/gui_run.py --seconds 12`（VNC 冒烟，`GUI: started pid=18`）
- [ ] 时间允许时 `python3 tools/test.py`：执行两次，第一次因运行中修改内核源码导致信任测试基线哈希不一致（重建后单测通过）；第二次在既有的 `qemu_net_test.py` IRQ 唤醒断言处偶发失败（该测试与本次改动无关，单独复跑两次通过）。其余全部 QEMU 套件已逐个复跑通过。
- 验收：记录每条命令的真实结果；如有失败，报告原始错误与未完成项，不宣称通过。

## 依赖关系

T1 → T2；T3 → T4 → T5 → T6 → T7 → T8；T9 可与 T5 并行；T11 依赖 T8b；T12 依赖 T5/T8b；T13 依赖 T12；T14 依赖 T13；T10 最后。
