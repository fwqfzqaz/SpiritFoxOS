# 规划：Linux 应用兼容模块（有界兼容层）

状态：规划草案 / 待评审
日期：2026-11-13
适用项目：SpiritFoxOS（x86_64 UEFI 微内核原型）

本文件回答四个问题：兼容到什么程度、由哪个模块承担、需要哪些内核机制增量、按什么顺序实施与验收。
它是**规划**，不是实现承诺；在评审通过前不改变 [GOALS](../../project/GOALS.md) 的 v1.0 范围，也不进入 ROADMAP 的当前优先级。

## 1 结论摘要

1. 采用「**用户态兼容运行时 + 内核最小机制增量**」：新增 ring-3 模块 `sf-linux.sfmod`（`native_role: runtime`）负责 ELF 解析、库认证、映像合并与装载计划；兼容进程内的 syscall 翻译器负责语义；Core 0 仍然不解析 ELF、CBOR、路径或包签名。
2. 兼容目标按等级递进：先 **静态 PIE（musl）命令行程序**，再 **动态 musl 程序**，再线程/信号；首期明确**不支持** glibc 动态二进制、`fork`、`ptrace`、`io_uring`、user namespace、X11/Wayland。
3. 内核只追加**有界机制**：每进程兼容描述符、`int 0x80` 兼容上行调用（upcall）与返回、兼容地址窗口内的 `mprotect/mmap`、每线程 FS 基址、futex 等待队列、有界熵源。
4. 分九步 S0–S8。**S0–S1 为纯用户态工作**，不动内核，可先行原型；真正可运行 Linux ELF 需要 S2 的入口机制。
5. 整条轨道定位为 **v1.0 之后的 v1.x 可选能力**；S6 依赖 GOALS G1 的可写文件系统，S7 依赖 G2/G3 的会话与可靠性基础。

## 2 现状事实与硬约束

以下均为当前代码与文档的真实行为，兼容方案必须在其内设计。

| 约束 | 现状 | 对 Linux 兼容的影响 |
| --- | --- | --- |
| 内核边界 | Core 0 不解析 ELF/重定位，只校验并复制装载计划（[NATIVE.md](../../architecture/NATIVE.md)、[IMPLEMENTATION.md](../../architecture/IMPLEMENTATION.md)） | ELF 解析、动态链接、syscall 语义必须全部在 ring-3 |
| 包与装载 | `.sfk/.sfmod` 容器；relative-v1；最多 8 个映射段、8 MiB 映像、256 条相对重定位 | 需新增 `linux-elf-v1` profile，并把多库合并进 8 段/8 MiB 预算 |
| 运行时 | 单一认证库 `sf-libsf.sfmod`，应用最多 32 项导入，库最多 64 项导出，19 项 POSIX 子集 | 不能靠导入表承载完整 libc；Linux libc 必须随包交付并由兼容模块自解析 |
| 地址空间 | 静态基址 `0x40000000`；PI 区 `0x44000000–0x4f800000`；栈 `0x41010000`（64 KiB）；参数 `0x42000000`；DMA `0x50000000`；bundle `0x60000000`；FB `0x70000000`；堆 `0x74000000`（64 MiB 窗口）；共享内存 `0x78000000`（8×8 MiB） | Linux 程序习惯在任意地址 `mmap`；需新增受控兼容窗口与 `mprotect`，或限定只运行 PIE |
| 内存 | 无按需分页；固定 4096 页（16 MiB）内核页池；每进程默认页配额 | 映像必须整体常驻；大体积程序需先提升配额或后续加入按需分页 |
| 进程/线程 | 任务 32、线程 64；每进程线程上限 32；只有 PID 1 可 `SYS_SPAWN/SYS_KILL` | `clone/fork` 需进程代理与配额调整，首期只做 `posix_spawn` 式派生 |
| 系统调用 | `int $0x80`；`rax` 调用号，`rdi/rsi/rdx` 三个参数；`Frame` 保存全部 GPR（[kernel.h](../../../kernel/kernel.h)、[core.c](../../../kernel/core.c)） | 三参数不足以承载 Linux 六参数 ABI，需 upcall 把完整寄存器帧交给用户态翻译器 |
| CPU 上下文 | 每线程 x87/MMX/SSE 私有上下文；未设置/保存 `IA32_FS_BASE` | musl 的 TLS、`errno`、`pthread` 依赖 FS 基址，必须新增 |
| 文件 | 普通应用只读；持久卷为 5×16 KiB 事务原型；文件 RPC 单次读写最多 256 B，`pread` 最多 4096 B | 真实 Linux 程序需要可写文件系统与更高吞吐，属 S6 前置条件 |
| 网络 | 8 个连接对象，IPv4 TCP/UDP 子集 | socket 兼容只能在既有网络服务上做有限映射 |
| 信号/等待 | 无信号投递；无线程级等待原语（futex） | 首期把信号降级为「默认动作 = 终止」；futex 在 S4 补齐 |
| 身份 | 构建期账户 + 四个控制台会话，uid/gid 由会话决定 | 不实现 `setuid/setgid`；兼容进程继承会话身份 |

## 3 与现有目标的冲突及处理

[GOALS.md](../../project/GOALS.md) §2.3 明确把「Linux/Windows 二进制兼容、完整 POSIX 系统调用兼容或通用 ELF 动态链接」列为 **v1.0 前的非目标**；§8 要求这类目标变更必须显式审阅。

因此本规划的处理方式是：

1. **不修改 v1.0 验收矩阵**，不把 Linux 兼容计入 G0–G4 的退出条件；
2. 在 GOALS 中新增一条 **v1.x 可选轨道**（建议编号 G5），本文评审通过后再修订该文件；
3. 补充一条架构不变量解释：兼容层是 ring-3 模块，`int 0x80` 上行调用是**有界机制**（固定帧页、固定入口、固定嵌套上限），不把 ELF 或路径解析放入 Core 0；
4. 兼容包的信任链复用现有包签名与「认证库」模型（类似 `native_library` 的认证方式），不新增加载器特权。

在上述修订完成前，本文只能作为技术预研与 S0/S1 原型依据。

## 4 兼容等级与支持矩阵

| 等级 | 输入 | 依赖步骤 | 明确不支持 |
| --- | --- | --- | --- |
| L0 源码兼容 | 用 sfos 工具链 + 扩展 SDK 重新编译的 POSIX 程序 | 现有 + S3 子集 | 未修改的 Linux 二进制 |
| L1 静态 PIE | x86_64 ET_DYN、静态链接 musl 的命令行程序 | S1–S3 | ET_EXEC 非 PIE、动态库、线程 |
| L2 动态 musl | 依赖 `libc.so`/`libm.so` 的 musl 程序 | S5 | glibc 符号版本、`LD_PRELOAD` 插件、运行时 `dlopen` 任意库 |
| L3 线程与信号 | pthread 程序、常用信号 | S4、S7 | `fork`、`ptrace`、`io_uring`、实时信号全语义 |
| L4 桌面/服务 | 图形或网络服务型 Linux 程序 | S6、S9（可选） | X11/Wayland 原生客户端、systemd、容器 |

首期验收目标是 **L1**；L2 作为下一里程碑。任何等级都要求程序来自受信任包（签名 + 摘要），不接受任意磁盘路径直接执行。

## 5 架构设计

### 5.1 组件与职责

```text
shell / installed 入口
  └─ sf-init（身份、授权、装载编排，保持现有策略）
       └─ sf-linux.sfmod（ring-3，native_role=runtime）
            ├─ ELF 解析与校验（有界，宿主同源代码）
            ├─ 库解析与认证（摘要 + 签名，绑定信任存储）
            ├─ 映像合并（≤8 段、≤8 MiB）与重定位
            ├─ Linux 初始栈构造（argv/env/auxv）
            └─ 返回 SfLoadPlan + 兼容元数据
Core 0
  ├─ 校验并复制装载计划（既有行为，不解析 ELF）
  ├─ 兼容描述符：入口、帧页、嵌套计数
  ├─ 兼容窗口内 mmap/mprotect/munmap（W^X 强制）
  ├─ 每线程 FS 基址、futex 等待、有界熵
  └─ int 0x80 上行调用 / SYS_COMPAT_RET 返回
兼容进程
  ├─ 启动 stub（随映像交付）：注册兼容入口、构造 Linux 栈、跳 e_entry
  └─ syscall 翻译器 + musl 适配层（同地址空间，CPL3，无额外权限）
```

关键分工：**装载期语义在 `sf-linux`，运行期语义在兼容进程自身**。这样翻译器不持有跨进程文件能力，也不形成「兼容服务代理所有应用 I/O」的环境权限（ambient authority）。

### 5.2 装载流程

1. shell/`installed` 把只读包能力、argv 与环境交给 sf-init（沿用现有流程）。
2. sf-init 认证包签名与类型，读取新增清单字段 `native_profile: linux-elf-v1`（只追加）。
3. `sf-linux` 解析 ELF：`PT_LOAD`、`PT_DYNAMIC`、`PT_INTERP`；逐项检查边界、对齐、重叠、W^X、段数、映像大小。
4. 解析依赖库（L2）：按 `DT_NEEDED` 查找已认证库包，校验摘要/签名/ABI，最多 8 个对象。
5. 在 `sf-linux` 私有缓冲中合并映像：代码页合并为 RX，只读数据合并为 R，可写数据合并为 RW；应用 `R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT/TPOFF/DTPMOD`；追加启动 stub 与翻译器。
6. 返回 `SfLoadPlan`（既有 8 段上限）与兼容元数据（入口、帧页要求、syscall 白名单摘要）。
7. Core 0 对照同一认证快照校验并复制到子进程私有页——**仍然不解析 ELF**。
8. 子进程 stub 注册兼容入口、把 Linux 栈切到兼容窗口、跳转 `e_entry`；此后每次 `int 0x80` 进入翻译器。

装载失败必须在创建子进程前返回明确错误（`SF_BAD_FORMAT`/`SF_DEPENDENCY`/`SF_NO_SPACE`），且不改变磁盘状态。

### 5.3 syscall 入口机制（关键决策）

现状：内核只接受 `int 0x80` 三参数调用，Linux x86_64 使用 `syscall` 六参数 ABI，二者不兼容。候选方案：

| 方案 | 做法 | 评价 |
| --- | --- | --- |
| A. 兼容 upcall（推荐） | 装载期在**私有缓冲**中把代码页的 `0F 05` 改写为 `CD 80`；这些 `int 0x80` 由带兼容描述符的进程触发内核上行调用 | 复用现有 IDT 与 `Frame`，无需 SYSCALL/SWAPGS/MSR；改写发生在映射前，不破坏 W^X |
| B. SYSCALL 门 | 内核实现 `SYSCALL/SYSRET`、STAR/LSTAR/FMASK、`swapgs` | 改动最大、安全敏感；仅在方案 A 的指令改写被证伪时采用 |
| C. 外部跟踪 | 类似 ptrace 的逐指令拦截 | 性能与实现复杂度不可接受，否决 |

方案 A 的机制细节：

- `SYS_COMPAT_SET(handler, frame_page, flags)` 由兼容进程在启动 stub 中调用一次：`handler` 必须是本进程 RX 段内地址，`frame_page` 必须是本进程 RW 页且页对齐，`flags` 必须为零。
- 兼容进程执行 `int 0x80` 时，内核保存完整 `Frame`（已含 `r8/r9/r10`），把帧写入该线程的帧页，令 `RIP=handler`、`RSP` 保持合法值、`RDI=帧页地址`，返回 CPL3。
- 翻译器使用新的 `int 0x81` 向量执行 SpiritFoxOS 原生系统调用；该向量对非兼容任务返回拒绝，避免两条 ABI 混淆。
- 翻译完成调用 `SYS_COMPAT_RET(value, flags)`：内核把 `value` 写入保存帧的 `rax`，恢复全部寄存器并回到 `int 0x80` 之后。
- 有界性：单入口、单帧页、嵌套计数 1（翻译器内再次触发兼容调用即终止进程）、帧页不可写代码、handler 越界即拒绝注册。
- 性能预算：每次兼容系统调用 = 2 次特权切换 + 翻译器开销；S2 起记录基准，目标 < 5 µs/次（QEMU 参考值）。

`0F 05` 改写只作用于签名映像的 RX 页副本，且只在装载期私有缓冲进行；误改写数据页的风险由「只扫描可执行段」+ 固定种子变异测试约束。

### 5.4 地址空间布局

在现有窗口之外新增**兼容窗口**（建议 `0x10000000–0x3F000000`），只对带兼容描述符的进程开放：

- `mmap` 只允许 `MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED` 的有限组合；地址必须页对齐、落在窗口内、不与既有区域重叠。
- `mprotect` 只允许 R/RX/RW 三种组合，且禁止 W+X；代码页一旦变为 RX 不得再变回 W（防止运行期改写）。
- 映射总量受进程页配额约束（默认 4096 页，建议 G1 后在参考硬件上评估提升）。
- 无按需分页意味着映像与堆必须整体常驻；因此 L1 首期上限沿用 8 MiB，超出返回 `SF_NO_SPACE`，不做静默截断。
- 非 PIE `ET_EXEC` 需固定低地址（如 `0x400000`），与现有静态基址和内核低 1 GiB 恒等映射冲突，**首期拒绝**。

### 5.5 文件、身份与网络

- 文件访问全部经兼容进程自己的文件 RPC 客户端（与现有 SDK 同协议），不新增特权通道；可写语义依赖 S6 的通用可写文件系统。
- 文件 RPC 单次 256 B 的吞吐不足以支撑动态链接装载，S1/S5 需使用只读共享内存缓存或块缓存服务，并在文档中标注实际带宽。
- 身份继承会话 uid/gid；拒绝 `setuid/setgid/setresuid` 等提权调用，返回 `EPERM`。
- socket 兼容映射到现有网络对象的 8 连接上限；超出返回 `EMFILE`，不静默排队。
- 熵：当前无内核 RNG。`getrandom` 在 S3 只允许从启动期采集的有界种子池返回；S8 前补齐 `SYS_GETRANDOM` 机制，否则直接拒绝而不是返回可预测字节。

## 6 内核 ABI 追加候选

全部为**追加**，需 ABI 快照审阅与宿主/ QEMU 负例；编号从 41 起，最终以审阅结论为准。

| 编号 | 名称 | 语义与边界 |
| --- | --- | --- |
| 41 | `SYS_COMPAT_SET` | 注册兼容入口/帧页/标志；仅本进程、入口在 RX、帧页在 RW、每进程一次 |
| 42 | `SYS_COMPAT_RET` | 恢复保存帧并写入返回值；只允许在兼容上行调用内使用 |
| 43 | `SYS_MPROTECT` | 兼容窗口内改权限；R/RX/RW，禁止 W+X，不可跨窗口 |
| 44 | `SYS_COMPAT_MMAP` | 兼容窗口内匿名映射；地址提示/固定、页对齐、受页配额约束 |
| 45 | `SYS_COMPAT_MUNMAP` | 解除兼容窗口映射；整页、区间校验 |
| 46 | `SYS_SET_FSBASE` | 设置当前线程 FS 基址；保存/切换随线程上下文，地址须在本进程映射内 |
| 47 | `SYS_FUTEX` | `WAIT/WAKE` 两种操作，键为（地址空间，用户地址，值），带截止时间 |
| 48 | `SYS_GETRANDOM` | 有界熵读取；长度上限、失败关闭 |

另需两处非编号变更：IDT 新增 `0x81` 向量（原生调用，仅兼容任务），`SfThreadRequest.flags` 追加 TLS 位（创建线程时携带 FS 基址）。`Frame`、`SfMessage`、BootArgs 的既有布局不变。

## 7 实施步骤

每步都必须满足统一门禁：`-Wall -Wextra -Werror` 零告警；宿主测试通过；涉及内核/ABI 时完整 `python3 tools/test.py` 通过；ABI 快照只追加并有审阅记录；文档同步；Linux/Windows CI 均为最新提交。

### S0 立项、边界与兼容契约（纯文档）

- 交付：GOALS 非目标修订提案、本文件评审结论、支持矩阵与「明确不做」清单。
- 验收：审阅记录；不修改 v1.0 验收矩阵；明确 L1 为第一交付目标。
- 回退：仅文档，可直接撤回。

### S1 纯用户态 ELF 装载核心（无内核改动）

- 交付：`services/linux_image.c` + `include/linux_image.h`（有界 ELF 解析、段合并、重定位应用、初始栈构造）；`tools` 侧打包/检查工具复用同一解析代码；`tests/host/test_linux_elf.c`。
- 内容：`PT_LOAD` 校验、`PT_DYNAMIC` 解析、重定位类型与目标校验、8 段/8 MiB 预算、W^X、固定种子变异测试。
- 验收（E1）：宿主夹具覆盖合法/非法 ELF、越界、重叠、段超限、重定位越界；对同一输入产物字节确定；装载计划可被现有 `sf-native` 校验器接受。
- 回退：独立源文件与测试，不接入构建产物即可停用。

### S2 兼容入口机制（首次内核增量）

- 交付：`SYS_COMPAT_SET/RET`、`0x81` 向量、帧页与嵌套计数；启动 stub 与最小翻译器（仅 `write/exit`）；装载期 `0F 05→CD 80` 改写。
- 验收（E2）：QEMU 运行静态 PIE 的 hello，输出与期望一致、退出码正确；负例覆盖非 RX 入口、错页帧、重复注册、嵌套调用、跨进程帧页均被拒绝。
- 回退：兼容描述符为每任务可选字段，关闭后内核行为与当前完全一致。

### S3 syscall 子集 v1 与 errno 映射

- 交付：约 35 个调用的翻译与测试：`read/write/openat/close/lseek/fstat/newfstatat/brk/mmap/munmap/mprotect/exit/exit_group/clock_gettime/gettimeofday/nanosleep/getpid/gettid/getuid/uname/getrandom/arch_prctl/set_tid_address/ioctl(TIOCGWINSZ)/poll/ppoll/pipe2/dup/dup2/dup3/fcntl/getcwd/readlinkat/prlimit64/sysinfo/sched_yield`。
- 验收（E1+E2）：宿主 errno 表全覆盖；QEMU 运行 `busybox` 子集（cat/ls/echo/true/false/seq/wc）并比对输出；未知调用返回 `ENOSYS`，危险调用（`ptrace/mount/io_uring/bpf`）按白名单拒绝。
- 回退：翻译器按调用号分发表，未实现的号显式拒绝，不影响已交付调用。

### S4 TLS、线程与 futex

- 交付：`SYS_SET_FSBASE`、`SfThreadRequest.flags` 的 TLS 位、`SYS_FUTEX`；`clone` 到 `SYS_THREAD_CREATE` 的映射；`pthread_create/join` 冒烟。
- 验收（E2）：多线程计数器与互斥锁；TLS 变量逐线程独立；线程上限拒绝返回 `EAGAIN`；futex 超时精度与唤醒丢失负例。
- 回退：TLS 位与 futex 为独立增量，不影响单线程路径。

### S5 动态链接与 musl 认证库

- 交付：`sf-linux` 的 `PT_INTERP` 与 `DT_NEEDED` 解析、符号解析（全局/局部范围、无版本）、`RELATIVE/GLOB_DAT/JUMP_SLOT/TPOFF/DTPMOD`、最多 8 个对象；musl 适配层（替换 `syscall_arch.h`，保留 malloc/TLS/stdio 实现）。
- 验收（E1+E2）：动态 musl hello + `libc.so` + `libm.so`；缺失库返回 `SF_DEPENDENCY`；摘要不匹配的库拒绝；符号缺失在装载期失败而非运行期崩溃。
- 回退：动态路径独立于静态路径；静态 L1 不受影响。

### S6 文件系统与设备（依赖 GOALS G1）

- 交付：可写文件语义（创建/截断/重命名/`fsync`）、`/dev/{null,zero,urandom,console,tty}`、`MAP_SHARED` 的明确边界（首期只支持私有映射，`MAP_SHARED` 返回 `ENOTSUP`）、文件 RPC 吞吐优化。
- 验收（E2+E3）：文件操作工具集；掉电注入后已提交数据可读；只读拒绝语义与现状一致。
- 回退：兼容层保持只读可用，写入按 `EROFS` 拒绝。

### S7 进程与信号

- 交付：进程代理（受权兼容运行时或 sf-init 按策略创建子进程）、`posix_spawn`/`execve`、`wait4`、信号投递的有界实现（同步故障→默认终止；异步信号仅支持终止/忽略）；`fork` 明确拒绝并返回 `ENOSYS`。
- 验收（E2）：子进程执行与回收、僵尸回收、信号终止、`SIGPIPE` 默认行为；无泄漏与 PID 复用错误。
- 回退：`exec/wait` 与信号为独立开关，关闭后只支持单进程程序。

### S8 打包、签名、分发与回归

- 交付：`sfpack --profile linux-elf-v1`；清单新增 `linux_loader`、`needed_libraries`（名称+摘要）、`syscall_allowlist`、`resource_limits`；`sf-linux.sfmod` 版本与 ABI `liblinux-1`；兼容性语料库与 CI 任务；文档（安装、限制、故障排查）。
- 验收（E2）：语料库全绿；篡改包/库/白名单均被拒绝；旧 `.sfk/.sfmod` 不受影响；完整回归通过。真机阶段另需 E3。
- 回退：profile 由清单显式声明，未知 profile 仍按现有规则拒绝。

## 8 测试计划

| 层次 | 内容 | 证据 |
| --- | --- | --- |
| 宿主 | ELF 解析/重定位/合并/预算、errno 表、白名单、固定种子变异（≥1000 次） | E1 |
| 宿主负例 | 越界段、重叠页、W+X、超 8 段/8 MiB、坏重定位、坏 TLS、重复入口 | E1 |
| QEMU 正例 | hello、busybox 子集、动态 musl、多线程、文件操作、子进程 | E2 |
| QEMU 负例 | 未注册即触发、嵌套 upcall、帧页越界、危险 syscall、缺失库、超配额 | E2 |
| ABI | 每次追加前后快照差异、导入/导出上限、结构体大小断言 | 审阅记录 |
| 性能 | 兼容系统调用延迟、装载时间、映像大小、文件 RPC 带宽；回退 > 10% 阻断 | E2 |
| 真机 | 参考硬件上的装载、运行、退出与资源回收 | E3（后续） |

## 9 风险与未决决策

| 编号 | 风险/决策 | 处理建议 |
| --- | --- | --- |
| R1 | 与 GOALS 非目标冲突 | 先完成 S0 显式修订，再写内核代码 |
| R2 | 无按需分页限制程序体积 | L1 限定 8 MiB；G1 后评估配额与按需分页 |
| R3 | 8 段/8 MiB/32 线程上限偏紧 | 装载期合并段；把上限做成清单可声明并在构建期检查 |
| R4 | 文件 RPC 吞吐不足 | S1/S5 引入只读共享内存缓存；基准不达标不进入 L2 |
| R5 | `fork`/完整信号缺失 | 文档明确不支持；用 `posix_spawn` 与默认终止语义覆盖首期场景 |
| R6 | 兼容层扩大攻击面 | syscall 白名单、W^X、无 ambient authority、包与库双重认证、模糊测试 |
| R7 | 证据等级滑移 | QEMU 绿不替代真机；对外表述必须标注 E1/E2/E3 |
| D1 | 入口方案 A 还是 B | 默认 A；若改写误伤率不可接受再评审 B |
| D2 | 静态优先还是动态优先 | 静态优先（L1），动态作为 L2 |
| D3 | musl 还是 glibc | musl（MIT、无符号版本、体积可控）；glibc 不作为目标 |
| D4 | `MAP_SHARED` 写回 | 首期不支持；需要时先设计文件缓存与一致性协议 |
| D5 | 图形/网络边界 | X11/Wayland 明确不做；socket 仅做既有网络服务的映射 |

## 10 与路线图的挂接

| 本文步骤 | 建议时点 | 前置条件 |
| --- | --- | --- |
| S0 | G0 收尾后 | GOALS 修订审阅 |
| S1 | 与 G1 并行 | 无内核依赖，可纯宿主验证 |
| S2–S5 | G2 工作台之后 | 可写文件系统之外的装载与运行基础 |
| S6 | G1 完成后 | 通用可写文件系统、fsck、配额 |
| S7 | G3 工作负载阶段 | 服务监督、会话、故障注入 |
| S8 | v1.0 发布工程之后 | 包仓库、签名与发布流程稳定 |

在 S0 评审完成前，本文件只登记为规划草案；`docs/README.md` 的索引条目仅说明其存在，不代表已排期。
