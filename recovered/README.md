# 重构前代码回收（recovered）

本目录是从 Git 历史中抽取的**重构前实现冻结快照**，用于给当前微内核版本提供可复用的协议、算法和实现参考。它不参与构建，也不代表当前支持的接口。

## 背景与分界点

- 2026-07-04 `ad0f9f6` 首次上传完整项目。
- 2026-10-23 `29fc53e`（*Build isolated UEFI microkernel and sfmod application prototype*）是重构分界点：`587 files changed, 6288 insertions(+), 116299 deletions(-)`。旧单体内核、驱动、网络栈、图形、内置 libc、Wayland 等在此提交中被整体移除，替换为当前的 Core 0 + ring-3 sfmod 结构。
- `29fc53e` 的直接父提交是 `f52bf11`（2026-10-21），保留了重构前的完整旧实现。**本目录全部 148 个文件均取自 `f52bf11` 的对应原路径，内容未做任何改写**，并已逐文件与 Git blob 校验一致。
- 2026-10-24 `741b8db` 只是把文档、测试、工具目录重排，不是代码丢失点。
- 旧 [REFACTOR-LOG.md](docs/REFACTOR-LOG.md) 当时明确选择“不复制一份易失去同步的 legacy 目录，依靠 Git 历史保存”。本目录是按当前路线需要重新抽取的一次性快照，与当时的决定不冲突。

## 筛选标准

1. 当前树中没有等价实现，且 [ROADMAP.md](../docs/project/ROADMAP.md) 仍将其列为后续工作；
2. 包含可复用的协议、算法或数据结构，而不只是旧 ABI 的薄封装；
3. 可以迁移到 ring-3 sfmod + 能力 IPC，或可作为当前内核机制的参考实现。

## 目录内容

| 目录 | 文件/行数 | 内容 | 原路径 | 当前缺口 |
| --- | --- | --- | --- | --- |
| `net/` | 18 / 2726 | ARP、IPv4、ICMP、UDP、TCP、BSD socket、AF_UNIX、校验和 | `kernel/include/net*.h`、`kernel/src/net/*.c` | M7 的 DHCP/UDP/TCP；当前 [net_proto.c](../services/net_proto.c) 只有有界 ARP/IPv4/ICMP 辅助函数 |
| `fs/` | 10 / 4907 | VFS、FAT32 读写、memfs、procfs、devfs | `kernel/include/{vfs,fat32,memfs,procfs,devfs}.h`、`kernel/src/fs/*.c` | M3 的 proc/sys 适配服务；M6/M7 的通用持久文件系统 |
| `storage/` | 10 / 1279 | AHCI、ATA 几何探测、块/字符设备抽象、ramdisk | `kernel/include/{ahci,blkdev,chardev,ramdisk,ata_geometry}.h`、`kernel/src/drivers/*.c`、`kernel/src/fs/ramdisk.c` | M5/M7 的更多磁盘格式与真实硬件支持 |
| `graphics/` | 24 / 9551 | Bochs VBE 帧缓冲与双缓冲、终端、窗口管理器、VGA 文本、键盘/鼠标/evdev、DRM/KMS/GEM、logo 资源 | `kernel/src/video/*`、`kernel/src/drivers/{keyboard,mouse,input_core,evdev}.c`、`kernel/src/drivers/drm/*` | M7 的图形与用户会话 |
| `arch/` | 14 / 2979 | ACPI/MADT、APIC、GDT/IDT/ISR、SMP AP trampoline、PIT 定时器 | `kernel/src/arch/x86_64/*`、`kernel/src/drivers/timer.c` | M7 的多核与 IRQ 隔离 |
| `proc/` | 8 / 2785 | 进程、用户内存拷贝、调度器、信号、futex | `kernel/include/{process,futex,clone}.h`、`kernel/src/proc/*` | M1/M3 的调度与信号语义参考 |
| `runtime/` | 5 / 1339 | ELF64 解析/装载、ET_REL 模块装载 | `kernel/src/mm/elf_loader.c`、`kernel/core/module/{elf,loader}.c` | M4 的更完整 PI 配置与受限导入 |
| `session/` | 7 / 5538 | 旧 shell（40 余个命令）、Wayland 合成器与测试客户端 | `kernel/src/services/shell.c`、`wayland/compositor/*`、`wayland/client/*` | M7 的用户会话 |
| `libc/` | 36 / 2781 | 内置 libc：stdio、malloc、string、signal、time、syscall 封装 | `libc/` | M4 的 SDK 基线 |
| `docs/` | 15 / 1009 | 迁移设计、重构日志、设备注册表、存储/FAT32 等对接记录 | `docs/ARCHITECTURE.md`、`docs/REFACTOR-LOG.md`、`docs/integration/*` | 历史约束与设计背景 |
| `tools/` | 1 / 74 | `png2c.py`：PNG 转 XRGB8888 C 头 | `tools/png2c.py` | 图形资源生成 |

合计 **148 个文件、34968 行**。

## 未纳入的内容及原因

| 旧内容 | 原因 |
| --- | --- |
| `kernel/src/services/{pkgmgr,registry,sandbox}.c`、`sfk_registry.c` | 已被当前的 [package.c](../services/package.c)、[trust.c](../services/trust.c)、[manager.c](../services/manager.c) 取代 |
| `kernel/src/syscall/*`、Linux ABI 相关代码 | 当前架构有意用能力 IPC 替换旧的 POSIX 风格 syscall 层，参考价值低且易误用 |
| 旧 `.sfmod` ABI、`kernel/include/sfmod.h`、`modules/` | 包格式和模块 ABI 已在当前仓库重写，旧 ABI 不兼容 |
| `kernel/src/services/{module,autorun}.c`、`kernel/core/` 其余部分 | 当前 `services/manager.c`、`services/init.c` 已有等价职责；仅保留其中的 ELF 装载器 |
| `kernel/src/services/gunzip.c`（1737 行 DEFLATE 实现） | 当前设计明确首版禁用包内压缩；若以后支持压缩资源，可再按同一方式从 `f52bf11` 恢复 |
| `kernel/src/mm/{kmalloc,slab}.c`、`kernel/src/mm/{memory,mmu}.c` | 当前 [kernel/memory.c](../kernel/memory.c) 已有页分配与映射实现，暂不需要旧分配器 |
| `nuklear.h`（约 31k 行） | 第三方单头 GUI 库，应从上游版本获取；`graphics/src/nuklear_fb.c` 依赖它 |
| `wayland/protocol/{wayland,xdg-shell}.xml`、`sysroot/` | 上游协议定义或第三方工具链产物，不作为本项目代码回收 |
| 旧 `Makefile`、`tools/build.sh`、`demo.py` 等 | 已由 [tools/build.py](../tools/build.py) 和根 `Makefile` 取代 |

## 使用须知

- **冻结快照，不参与构建。** `tools/build.py` 不会扫描本目录；不要把其中文件加入当前编译目标。
- **不能直接编译。** 旧代码运行在 ring 0 单体内核中，依赖旧 `hal.h`、`kmalloc.h`、`vfs.h`、`process.h`、旧 syscall ABI 以及大量未随本目录复制的头文件。应把它们当作协议/算法参考，按当前 ABI 重写，而不是整段搬入内核。
- **必须重新做安全边界。** 这些代码早于当前的包签名、规范化句柄、DMA/VT-d 隔离模型，普遍假设“调用者是内核内部可信代码”。移植时要重新加长度/偏移/溢出检查、能力授权和失败隔离，不能沿用旧的信任假设。
- **第三方缺口。** `graphics/src/nuklear_fb.c` 需要上游 `nuklear.h`；Wayland 合成器需要 `wayland.xml`/`xdg-shell.xml` 经 `wayland-scanner` 生成协议代码。
- **建议移植顺序：**
  1. `net/` 的纯协议逻辑（校验和、IPv4 分用、UDP/TCP 状态机）→ ring-3 网络模块；
  2. `libc/` 与 `runtime/` → SDK 和 native 运行时参考；
  3. `fs/`、`storage/` → 将旧 `blkdev` 调用替换为当前 file RPC/storage 协议；
  4. `graphics/`、`session/` → 在 MMIO 能力授权和用户态合成器边界下重做；
  5. `arch/` 的 SMP/APIC → 最后做，涉及当前单核调度器和锁顺序的整体重构。
- 当前网络模块已有 `sf_net_*` 命名，旧 `net/` 中的同名函数/类型需要改名或封装后再引入，避免 ABI 和符号冲突。

## 逐文件恢复方式

需要查看或重新导出某个文件时，使用其原路径：

```bash
git show f52bf11:kernel/src/net/tcp.c
git show f52bf11:kernel/src/video/window.c
git show f52bf11:libc/src/stdio.c
```

本目录内容的来源分界可用以下命令复核：

```bash
git rev-parse 29fc53e^        # f52bf11，重构前最后状态
git show --stat 29fc53e       # 587 files, 6288 insertions, 116299 deletions
```
