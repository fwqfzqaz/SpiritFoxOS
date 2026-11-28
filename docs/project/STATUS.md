# 当前状态与上限差距

更新日期：2026-11-28

## 0. 一句话状态

SpiritFoxOS 已是可启动、可运行模块与签名应用的 x86_64 UEFI 微内核原型；下一阶段的目标不是增加演示切片，而是**抬高容量、存储、硬件、性能与可靠性上限**。

当前阶段：GOALS 的 **G0 / 路线图 U0（收敛与度量）**。系统定位与分层见 [系统分层与运行模型](../architecture/OVERVIEW.md)。

## 1. 现在能做什么

| 层 | 已具备 |
| --- | --- |
| Core 0 | UEFI 启动、ring 3 隔离、抢占、SMP（最多 8 核，QEMU 2 核验证）、线程、能力 IPC、32 句柄/任务、64 端点、4096 B 消息、设备/VT-d 授权、W^X/NX、SMEP/SMAP |
| 引导与管理 | `BOOTX64.EFI` 校验并装载内嵌内核与启动包；`sf-init` 负责引导、模块生命周期、依赖代次、账户会话与外壳 |
| 文件与存储 | `sf-fs` 独立模块；SFRFS1/SFRFS2 只读镜像；SFRWFS1 已接入 `/disk`，支持 `mkfs`、目录创建/删除/改名、文件读写、链接/权限/配额与 fsck；virtio-blk、AHCI、NVMe（512–4096 B 逻辑扇区） |
| 持久化 | `sf-persist` 双银行事务、A/B 更新、健康确认、失败回退、版本下限、密钥撤销 |
| 应用 | 签名 `.sfk`、native 静态/relative-v1、argv/显式环境、单一认证运行时库 `sf-libsf.sfmod`、19 项 POSIX 子集、文件 RPC SDK |
| 网络 | ring 3 legacy virtio-net、DHCP、ARP/IPv4/ICMP、一次性 UDP/TCP、8 个连接对象、`/net` 投影 |
| 图形与输入 | `sf-wm` 合成器直写 framebuffer、滚动平铺、PS/2 与 xHCI USB HID、串口/VNC 回退 |
| Linux 兼容（可选） | ring 3 翻译器；动态 musl hello + libc + libm 可运行；不进入内核 ABI，不属于 v1.0 验收矩阵 |
| 构建与验证 | 宿主 + QEMU 回归、ABI 快照、Linux/Windows CI；QEMU 默认对齐当代 x86_64（q35/UEFI、`-cpu max`、`-smp 2`） |

## 2. 当前上限与下一档目标

| 上限 | 当前 | 下一档目标 |
| --- | --- | --- |
| CPU | 最多 8 核，QEMU 2 核 | ≥ 4 核真机稳定，x2APIC 与中断亲和 |
| 任务/线程 | 32 任务 / 64 线程固定槽 | ≥ 256 进程 / ≥ 1024 线程，按预算配置 |
| 物理内存 | 内核页池 16 MiB；已验证 128–512 MiB | 整机 RAM ≥ 4 GiB，按需分配与回收 |
| 能力/IPC | 32 句柄/任务、64 端点、4096 B 正文 | 按任务预算配置；大消息走共享内存 |
| 存储容量 | SFRWFS1 最大 256 MiB | ≥ 100 GiB 卷、≥ 100k 文件 |
| 存储可靠性 | 随机掉电采样、fsck、只读降级 | 1000 次掉电注入、E4 证据 |
| 网络 | 8 连接、248 B 载荷、legacy virtio-net | ≥ 64 连接、IPv6/DNS、≥ 500 Mbps、现代网卡 |
| 图形 | 单一合成器直写 framebuffer | 客户端 surface 协议、多窗口、文本输入 |
| 应用生态 | 8 MiB 映像、单一运行时库 | ABI 1.0、SDK、包仓库、第三方分发 |
| 性能 | `tools/limits.py` 可重复基线（宿主 IPC/存储 + QEMU 启动） | 启动 ≤ 10 s / 图形 ≤ 20 s；回退 > 10% 阻断 |
| 可靠性 | QEMU 故障注入 | 72h soak、真机升级回滚、内核转储 |

### 2.1 本机实测基线（2026-11-28）

`python3 tools/limits.py`（或 `make limits`）一次生成声明上限、宿主基准与 QEMU 启动耗时，
原始结果写入 `build/limits-baseline.json`，串口输出写入 `build/qemu-baseline.log`。
数字随宿主与 QEMU 配置变化，仅用于同机回归对比，不作跨机承诺。

| 指标 | 实测 | 证据等级 |
| --- | --- | --- |
| IPC 往返延迟（4096 B 正文） | 392.0 ns/round | E1（宿主 CPU） |
| guest 内 IPC 往返（含系统调用、调度与跨 vCPU 唤醒） | 373.3 µs/round | E2（256 MiB / q35 / 2 核 TCG） |
| SFRWFS1 顺序写（4 KiB，128 块写回窗口） | 72.2 MiB/s（54 µs/块） | E1（内存设备） |
| SFRWFS1 写后 fsync | 0.2 ms | E1（内存设备） |
| SFRWFS1 顺序读 | 6198 MiB/s | E1（内存设备） |
| SFRWFS1 create / rename | 4352 / 4608 ops/s | E1 |
| SFRWFS1 mkdir / dir-rename / rmdir | 3637 / 4143 / 4208 ops/s | E1 |
| SFRWFS1 readdir（899 项，128 目录 × 4 文件） | 1.70 M entry/s | E1 |
| SFRWFS1 fsck（32 MiB 卷） | 0.5 ms | E1 |
| QEMU 加电到系统就绪（产品构建） | 5.49 s | E2（256 MiB / q35 / 2 核 TCG） |
| QEMU 启动到 shell 提示符 | 5.49 s | E2 |
| QEMU 启动到 `SELFTEST OK`（shell 命令） | 5.55 s | E2 |

产品构建（`make build`）不编译启动自检，`make test` 以测试构建打开
`SF_BOOT_SELFTEST=1` 运行同一批自检，因此启动数字不含测试开销，测试覆盖不丢失。
启动数字对当前 `build/` 镜像生效：先 `make build` 再 `make limits` 得到产品口径。
4 KiB 小块顺序写从 18.7 MiB/s 提升到 71.9 MiB/s，代价是崩溃最多丢失最近
128 块未提交写入（元数据操作与 `fsync` 仍立即提交）。

已覆盖容量、IPC（宿主与 guest 内）、存储与启动；网络 TCP 吞吐与图形帧率仍无基准。
guest 内延迟在 shell 执行 `bench` 测得（`tools/limits.py` 自动执行），与 E1 数字不可直接比较。

## 3. 距离下一档上限的主要缺口

按 [路线图](../project/ROADMAP.md) 的 U0–U2，当前最优先的缺口是：

1. **上限度量**：`tools/limits.py` 已统一测量声明上限、宿主 IPC/存储基准、guest 内 IPC 往返与 QEMU 启动耗时；网络吞吐与图形帧率仍缺脚本。
2. **容量预算化**：任务/线程/端点/能力槽仍是固定值；物理内存只覆盖内核页池与已测试范围。
3. **可写数据主存储**：SFRWFS1 已接入 `/disk` 并完成目录创建/删除/改名与目录内建文件；配额全流程、100 GiB 级卷与 1000 次掉电验证尚未完成。
4. **安装与恢复**：尚无真机安装器、分区/引导项管理与救援模式。
5. **真机矩阵**：真机启动、NVMe/AHCI、xHCI、IOMMU 与电源管理仍以 QEMU 证据为主（E2），未达到 E3/E4。

## 4. 最近交付

- **2026-11-28**：SFRWFS1 目录操作端到端（Phase 2）：`/disk` 支持 `mkdir/rmdir/mv` 与目录内建文件，namespace 钩子先落盘再改树；追加 `OP_NS_RENAME=229`，ABI 683 项；新增宿主 `test_namespace_rw`（40 项断言）并扩展 `qemu_sfrwfs_test.py` 跨四次启动验证。
- **2026-11-28**：新增 shell `bench` 与 guest 内 IPC 往返基准，`make limits` 在 `SELFTEST OK` 后自动执行并记录（QEMU/TCG 373.3 µs/round，E2）。
- **2026-11-28**：启动路径收敛：启动自检改为测试构建专属（`SF_BOOT_SELFTEST`，`make test` 打开），splash 等待 2 s→0.1 s；产品构建加电到系统就绪 17.49 s→5.49 s。
- **2026-11-28**：SFRWFS1 增加 128 块有界写回窗口，4 KiB 顺序写 18.7→71.9 MiB/s，写后 `fsync` 0.2 ms；崩溃最多丢失最近 128 块未提交写入。
- **2026-11-28**：新增 `tools/limits.py` 与 `make limits`，统一输出声明上限、宿主 IPC/SFRWFS1 基准与 QEMU 启动耗时。
- **2026-11-28**：SFRWFS1 接入 `sf-fs`（Phase 1）；`/disk` 可格式化、读写已有文件；新增 `OP_FS_BLOCK_WRITE/FLUSH/FORMAT`（117/118/119），ABI 682 项；宿主 `test_fs_rw` 与 QEMU `qemu_sfrwfs_test.py` 通过。
- **2026-11-28**：SFRWFS1 v1.1 加固，修复 fsck 目录块越界、提交目标集溢出与 size 外指针清理；新增 `sf_rw_link/chmod/chown`；宿主 15809 项检查通过。
- **2026-11-27**：Linux 兼容 S5 完成，动态 musl hello + libc + libm 在 QEMU 运行；SfLoadPlan 段预算调整为 16；宿主 LINUX ELF 检查 5667 项通过。
- **2026-11-28**：测试基础设施重构，统一 QEMU 固件探测与命令构造，默认使用 `-cpu max`、`-smp 2`；新增 [与 Linux 的关系与差异](../architecture/LINUX-RELATIONSHIP.md)。
- 更早的逐轮交付、故障注入与 ABI 变更记录统一归档在 [实施进度](PROGRESS.md)。

## 5. 验证边界

- QEMU 绿色不等于真机结论；真机单次启动不等于长期可靠。
- `SFRWFS1` 目前完成格式核心与 Phase 1/2 接入；目录变更已由宿主与 QEMU 覆盖，通用配额与大卷尚未验证。
- 事务卷仍是面向笔记与单应用 A/B 的有界原型，不能替代通用文件系统。
- native 运行时只支持单一认证库与有界导入；没有多库动态链接、TLS、惰性绑定或完整 POSIX。
- 设备隔离仅覆盖已测试的 Q35 范围；IOMMU、IRQ 与断电结论以实际证据等级为准。
- Linux 兼容是可选用户态轨道，不改变内核 ABI，也不进入 v1.0 验收矩阵。

## 6. 相关文档

- [系统分层与运行模型](../architecture/OVERVIEW.md)
- [目标纲要](GOALS.md) · [路线图](ROADMAP.md) · [实施进度](PROGRESS.md)
- [实现边界与 ABI](../architecture/IMPLEMENTATION.md)
- [测试记录](../quality/TESTING.md) · [安全审查](../quality/SECURITY.md)
