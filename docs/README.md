# 文档索引

## 项目定位

**SpiritFoxOS 只有一个核心：极简的 Core 0 内核；文件系统、驱动、网络、图形、运行时与管理器全部是 `.sfmod` 模块；应用以签名 `.sfk` 交付，并依托模块提供的运行时与服务运行。**

先读 [系统分层与运行模型](architecture/OVERVIEW.md)：它用一张分层图、职责边界表和当前硬上限表说明“内核—模块—SFK”的关系。当前开发方向是**提高系统上限**：抬升容量、硬件、性能与可靠性上限，而不是继续增加互不相连的演示切片，见[路线图](project/ROADMAP.md)。

文档按用途分层：设计说明描述目标规范，架构文档描述当前代码的真实行为，质量文档记录验证结果，项目文档记录状态、方向与历史进度。实现与文档冲突时，以代码和测试为事实来源，并同步修正架构文档。

## 设计规范

- [设计说明 v0.2](design/DESIGN-v0.2.md)：目标架构、模块、ABI 与安全模型的原始规范。
- [Linux 应用兼容模块规划](specs/linux-compat/spec.md)：兼容等级、装载与系统调用架构、S0–S5 已交付；属于可选用户态轨道，不进入 v1.0 验收矩阵。

## 架构与协议

### 内核与运行时

- [系统分层与运行模型](architecture/OVERVIEW.md)：极简内核、模块职责、SFK 运行路径与当前硬上限。
- [实现边界与 ABI](architecture/IMPLEMENTATION.md)：当前实现、限制与未完成项。
- [与 Linux 的关系与差异](architecture/LINUX-RELATIONSHIP.md)：架构、ABI、文件系统与兼容层的对比，说明本项目不是 Linux 分支。
- [任务、线程、调度与资源配额](architecture/SCHEDULING.md)：任务/线程选择、SMP、CPU 时间预算、x87/MMX/SSE 上下文隔离、私有池页面配额。
- [IPC 容量、撤回与计量](architecture/IPC.md)：每任务句柄上限、排队发送撤回、复制字节统计。
- [生命周期](architecture/LIFECYCLE.md)：依赖代次、租约与有界监督。
- [ABI 快照](architecture/ABI_SNAPSHOT.md)：真实头文件编号/布局采集、离线差异与参考审阅。
- [语言与工具链](architecture/LANGUAGE.md)：受限 C 运行时约定与构建工具链边界。

### 文件、会话与存储

- [命名空间、目录能力与文件语义](architecture/NAMESPACE.md)：openat、只读目录委派、所有者/模式权限、文件截断与回收。
- [账户与多用户会话](architecture/SESSIONS.md)：构建期账户、有界会话、身份传播与管理员命令门。
- [管理器身份与装载策略](architecture/MANAGER_POLICY.md)：私有控制台授权、启动身份快照与独立包装载准入。
- [有界图形切片](architecture/GRAPHICS.md)：framebuffer 授权、类 niri 平铺布局、`sf-wm` 合成器与 `gui` 命令。
- [存储协议与磁盘格式](architecture/STORAGE.md)：SFRFS1/SFRFS2、SFRWFS1、sf-fs 文件服务、virtio-blk/AHCI/NVMe 与只读区域。
- [事务持久化与应用更新](architecture/PERSISTENCE.md)：双银行事务、A/B 槽、健康确认、失败回退与密钥撤销。
- [通用可写文件系统 SFRWFS1](architecture/WRITABLE_FS.md)：格式布局、COW 写与日志提交、权限/配额/rename、fsck 与宿主故障注入验证；Phase 1 已接入 `sf-fs` 的 `/disk`。

### 应用、包与信任

- [应用调用接口与文件客户端 SDK](architecture/APP_SDK.md)：argv/环境/BootArgs 与有界文件 RPC 客户端。
- [native 配置](architecture/NATIVE.md)：静态/relative-v1 装载、导入表与限制。
- [清单源文件](architecture/MANIFESTS.md)：独立 schema、有界 JSON、可选严格作者约定与离线检查。
- [包信任策略](architecture/TRUST.md)：用途受限签名、重叠轮换与运行期撤销。

### 设备与隔离

- [DMA 隔离与 VT-d](architecture/IOMMU.md)：QEMU q35 的 VT-d 支持与验证边界。

## 质量与验证

- [安全审查与修复](quality/SECURITY.md)：权限边界、已修复缺陷与回归范围。
- [测试记录](quality/TESTING.md)：各阶段的宿主与 QEMU 验证结果。
- [Persist 准入围栏](quality/PERSIST_FENCE.md)：非破坏性围栏、令牌与取消；宿主和四启动 E2 专项通过，完整套件及发布独立验证。
- [Persist terminal 服务端契约](quality/PERSIST_TERMINAL.md)：105 的认证、串行缓存检查与有界 ACK 后退出；目前仅服务端原型和宿主 E1。
- [GitHub Actions 配置与排错](quality/CI.md)：Windows/Linux 两个工作流的完整 YAML、Rust 目标、手动命令和失败定位。

## 项目

- [目标纲要](project/GOALS.md)：最终目标、实用级验收矩阵、证据等级与阶段阶梯。
- [路线图：提高系统上限](project/ROADMAP.md)：容量、硬件、性能与可靠性上限的抬升计划与优先级。
- [当前能力与剩余工作](project/STATUS.md)：现在能做什么、当前上限与下一档目标。
- [实施进度](project/PROGRESS.md)：按轮次记录的历史进度与逐提交证据。

## 归档与合并说明

- `recovered/` 是早期代码与文档的冻结快照，只作历史参考，不属于当前文档体系。
- 2026-11-07 文档整理：`IPC_LIMITS` / `IPC_CANCEL` / `IPC_STATS` 合并为 `IPC.md`；`CPU_CONTEXT` / `PAGE_LIMITS` 合并入 `SCHEDULING.md`；`TRUNCATE` 合并入 `NAMESPACE.md`；`UPDATES` 合并入 `PERSISTENCE.md`；`ARGUMENTS` / `FILE_SDK` 合并为 `APP_SDK.md`。
- 同日删除已完成的临时规格 `docs/specs/`、失效的 `docs/PROGRESS.md` 指针文件与仓库根目录的调试缓存日志。

## 阅读顺序

新读者建议按“项目定位与分层模型 → 目标纲要 → 路线图 → 实现边界 → 调度/IPC → 安全审查 → 测试记录”阅读。构建命令与工作区结构见根目录 [README](../README.md)，提交与推送规则见 [CONTRIBUTING](../CONTRIBUTING.md)。
