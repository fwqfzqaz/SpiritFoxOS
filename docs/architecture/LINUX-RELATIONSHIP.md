# SpiritFoxOS 与 Linux 的关系与差异

本文用于说明 SpiritFoxOS 的架构来源与 Linux 的边界，并回应“这个项目是不是 Linux 分支”的疑问。结论先说：

- SpiritFoxOS 是独立设计、独立实现的操作系统，**不是 Linux 的分支、fork 或衍生发行版**。
- 仓库中的 Linux 兼容模块是一个**运行在 ring 3 的可选翻译器**，用于运行现成的 Linux 用户态二进制；它不修改内核 ABI，也不包含 Linux 内核代码。
- 内核、ABI、文件系统、包格式、驱动模型与进程模型均为本项目自定义。

## 一句话区分

Linux 是“内核 + 约 400 个 POSIX 系统调用 + 内核内 VFS/网络/驱动”的单体内核；SpiritFoxOS 是“极小的 Core 0 微内核 + 46 个自定义能力系统调用 + 用户态服务与驱动”，Linux 兼容只是其中一个用户态兼容层。

## 架构对比

| 维度 | SpiritFoxOS | Linux |
| --- | --- | --- |
| 内核结构 | 微内核式 Core 0（约 2.6k 行 C/汇编），只负责地址空间、任务、IPC、设备授权与中断 | 单体内核，VFS、网络栈、调度器、驱动大量位于内核态 |
| 启动方式 | UEFI `BOOTX64.EFI` 校验并加载内置内核；内核再以 ring 3 启动 `sf-init` | 多种引导协议与引导器，进入内核后由内核自行初始化并切换根文件系统 |
| 原生 ABI | SfAbi 1，系统调用号 `0..45`，消息 4096 字节，句柄/能力模型 | 约 400 个系统调用，POSIX 语义，`clone/fork/execve`、信号、`uid/gid` |
| 进程与权限 | 能力句柄（每任务上限 32）、派生/撤销、按端点授权 | 文件描述符 + DAC/MAC（LSM）、命名空间、seccomp |
| IPC | 端点消息、能力随消息转移、撤回级联；无内核内信号机制 | 管道、Unix socket、信号、共享内存等 POSIX 机制 |
| 文件系统 | SFRFS1/SFRFS2 只读镜像；SFRWFS1 可写（COW + 日志）；命名空间由 `sf-init`/`sf-fs` 服务实现 | VFS 抽象，ext4/btrfs/xfs 等具体文件系统 |
| 应用格式 | 签名的 `.sfk` 包、`.sfmod` 模块、native 静态/relative-v1 镜像 | ELF + 动态链接器 `ld.so` |
| 驱动模型 | 存储/USB/网络驱动为 ring 3 可加载模块，经 MMIO/DMA 授权访问硬件 | 驱动通常为内核模块，运行在内核态 |
| 调度 | 0–3 固定优先级 + 老化，100 Hz 滴答，最多 8 个 CPU | 多种调度类（CFS/EEVDF、实时、deadline 等） |
| 隔离 | W^X/NX、SMEP/SMAP、VT-d DMA 隔离、每任务私有池 | 页表隔离、LSM、命名空间、cgroup 等 |
| 许可与来源 | 原始代码，GPL-3.0；第三方仅 Monocypher（密码学）与 musl（兼容测试夹具） | Linux 内核及其衍生代码 |

## Linux 兼容模块到底是什么

`services/linux_compat.c` 与 `include/linux_abi.h` 实现的是一个**用户态翻译器**：

1. 应用通过 `SYS_COMPAT_SET` 注册一次性的兼容描述符。
2. 应用执行 Linux 的 `int 0x80` 时，内核只是在向量 128 上把这次陷入重定向到该进程的 ring 3 翻译器，并把完整寄存器帧保存到内核私有副本。
3. 翻译器在用户态解析 Linux 系统调用号与参数，再通过本项目原生的 SfAbi（以及 `SYS_COMPAT_RET` 的帧恢复）完成实际工作。
4. 兼容范围是有界子集，不包含完整的信号、线程、`fork/exec`、TLS 等 POSIX 语义。

这与 Wine、WSL1 的定位类似：**兼容层是普通进程，内核并不因此获得一套 Linux ABI**。删除该模块不会影响 SpiritFoxOS 自身的启动、ABI 或任何原生应用。

可以把它理解为“给 Linux 程序准备的一台用户态翻译机”，而不是“把 Linux 内核塞进了 SpiritFoxOS”。

## 可验证的证据

在仓库根目录执行以下命令，可以自行核对边界：

```sh
# 内核、引导与服务代码中没有引用 Linux 内核头文件
grep -rn "include <linux" kernel include boot services

# 原生系统调用表是项目自定义的 0..45
grep -n "SYS_" include/sf.h

# Linux 相关代码只存在于用户态翻译器与兼容测试
ls services/linux_* include/linux_abi.h

# 内核入口只处理通用中断向量，不解析 Linux 语义
sed -n '/Frame \*interrupt_dispatch/,/^}/p' kernel/core.c
```

补充事实：

- 内核源码规模约 2.6k 行，`boot/` 约 0.5k 行；真正的文件系统、包解析、ELF/manifest 处理均在用户态服务中。
- `abi/x86_64-current.json` 是项目自己的 ABI 快照，与 Linux syscall 表无对应关系。
- `vendor/musl` 仅在构建 Linux 兼容测试镜像时用作**测试夹具**，不会链接进内核或原生运行时。
- `vendor/monocypher` 用于 Ed25519 校验，是独立的密码学库，与 Linux 无关。

## 常见误解

**“有 `int 0x80` 和 Linux 系统调用号，所以是 Linux 分支。”**
`int 0x80` 只是 x86 的历史陷阱号，原生应用也用它发起 SfAbi 调用。本项目在向量 128 上做的是“已注册兼容描述符的进程重定向到用户态翻译器”，是否进入兼容路径由 `SYS_COMPAT_SET` 显式开启；未注册的进程执行兼容上行调用 `int 0x81` 会收到 `SF_DENIED`，而不是执行 Linux 语义。

**“用了 musl，所以是 Linux。”**
musl 只是兼容测试中的被测 Linux 用户态运行时，用来验证翻译器能否加载真实动态 ELF。它不在内核中，也不是系统 libc。

**“看起来像 Unix，所以是 Unix 衍生。”**
SpiritFoxOS 借鉴了能力系统与消息 IPC 的通用思想，但 ABI、句柄语义、撤销模型、文件系统格式、包格式和启动流程都是独立设计；它与 Linux 没有代码血缘关系。

**“有 POSIX 名字的调用，所以内核是 POSIX。”**
`SYS_*` 的命名和语义以项目 ABI 为准（例如 `SYS_SPAWN`、`SYS_CAP_REVOKE`），并不实现 `fork/execve`、信号、`uid/gid` 等 Linux 进程模型。

## 相关文档

- [设计说明 v0.2](../design/DESIGN-v0.2.md)
- [实现边界与 ABI](IMPLEMENTATION.md)
- [Linux 应用兼容模块规划](../specs/linux-compat/spec.md)
- [存储协议与磁盘格式](STORAGE.md)
- [包信任策略](TRUST.md)
