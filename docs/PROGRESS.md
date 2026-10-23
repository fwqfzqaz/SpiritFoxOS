# SpiritFoxOS 实施进度

更新日期：2026-10-23（北京时间）。

- 工作区：`E:\SpiritFoxOS`。指定源目录的三项顶层内容已移动到这里。
- 本轮目标：根据用户的新授权持续推进。基于 `820208d` 补齐受限 QEMU q35 Intel VT-d DMA 隔离：ACPI/DMAR、仅 DMA 区映射、撤销失效、真实越界 DMA 负例与持久化回归。
- status: vtd_isolation_tested
- round_complete: true
- quota_paused: false
- 分支：`codex/boot-only-restart-20261022`；远端：`origin`。
- 当前没有遗留的构建、QEMU 或 DeepSeek 进程。本轮最终测试输出为 `build/vtd-final-test.txt`。上一轮 DeepSeek 只读审查因权限预设初始化失败而超时，没有采用结果，本轮未调用它。

## 已完成

1. Core 0：隔离页表、异常、单核抢占、能力端点、队列与超时、任务回收。
2. 用户态模块管理、依赖与资源门控、忙停止取消、新旧实例并存。
3. 用户态内存命名空间、文件、管道、控制台、串口 shell。
4. 确定性 CBOR、SHA-256/Ed25519 包工具；独立 native 运行时验证装载计划；hello 与故障测试应用。
5. `python tools/test.py` 已完整通过：116 条 C 服务断言、8 组包测试与 1500 次固定种子变异、128/256/512 MiB QEMU 启动及 shell 回归。
6. README、实际实现边界、测试记录、后续路线均在工作区内；Windows CI 定义已加入，本地通过不代表托管 CI 已运行。
7. M5：独立用户态 virtio-blk、PCI/端口与固定 DMA 页授权、SFRFS1 只读磁盘、`/disk` 挂载与引用、从盘运行签名模块和应用、磁盘快照上传至 native 运行时。
8. M5 完整测试通过：151 条 C 断言、13 组 Python 测试（2000 次固定种子变异）、128/256/512 MiB 正常 VM 与六种存储失败 VM，共九组。盘文件前后摘要不变；测试结果含发布镜像摘要。
9. 驱动进程死亡会执行设备复位；旧进程释放后仍存活时禁止新 DMA 拥有者，地址空间销毁后才允许接管。M5 阶段没有 IOMMU；现在增加下述受限 Q35 支持，无 DMAR 时仍为可信模式。
10. M6 有界持久化：显式写授权和范围、实际 virtio OUT/FLUSH、双银行提交/恢复、`/persist/notes`、签名应用 A/B 安装与每次运行重验。写/flush 失败停止该卷读写，重挂载或重启恢复。
11. 最终 `python tools/test.py` 完整通过（70.11 秒）：151 条基础/存储 C 断言、事务 C 测试、14 组 Python 测试与 2000 次固定种子变异、261 个操作边界 × 三种故障模式、十八次 VM 启动。验证笔记/包重启、坏签名拒绝、损坏回退、实际写与 flush EIO、银行写入中段终止 VM 后恢复。原始构建盘及只读底区保持不变。
12. 上一轮持久化交付为 `820208d`，发布包是 `SpiritFoxOS-M6.zip`；以下为当前最新结果。
13. 本轮：UEFI 交付 ACPI，Core 0 有界验证 DMAR、只为一个 BDF 映射固定 DMA arena、同步 context/IOTLB 失效、内核寄存器 MMIO 隔离。modern virtio PCI 协商 ACCESS_PLATFORM，拒绝隔离模式中的 legacy 旁路；实际越界 DMA 读写触发硬件记录，保护页不变，驱动终止且新拥有者被内核拒绝。
14. 最终 `python tools/test.py --skip-build` 完整通过（125.49 秒；此前已用最终源码重新构建）：151 条基础/存储 C 断言、229 条 IOMMU C 检查、事务 C 故障测试、14 组 Python 测试、33 次 VM 启动。包含 legacy/modern 两种正常存储、真实 DMA 读写负例、重复 DMAR 与 legacy 拒绝、两轮九次持久化恢复。结果 `build/test-results.json` 含精确 EFI/盘/公钥摘要。
15. 当前发布包为 `build/SpiritFoxOS-M6-IOMMU.zip`，通过打包门检查精确镜像摘要，附带文档和测试结果；格式与限制见 `docs/IOMMU.md`。

## 交付与续做

本轮基于 `820208d` 完成受限 VT-d 原型。交付提交由 Git HEAD 标识，推送状态以 origin 当前分支与 HEAD 实际对照为准；不一致时续做已授权推送，不重做已通过的开发和测试。用户已明确授权：余额充足直接推进下一项，不等待再次确认；过去“不自行开始下一阶段”属于旧轮次限制。

后续优先依据要求和 ROADMAP 补齐签名/更新策略（健康度回滚等）与更完整持久化服务。IRQ 重映射、ACS/多单元分组、真实硬件断电、真实复位失败仍未验收，不能由本轮 QEMU RAM DMA 测试替代。

交互：`python tools/qemu_test.py --interactive --persistent --iommu`。首次创建 `build/user-persistent.img`，后续继续使用该副本；构建只覆盖原始 `persistent.img`。操作与格式见 `docs/PERSISTENCE.md` 和 `docs/IOMMU.md`。

## 额度保护

本轮使用自动任务 `spiritfoxos-2` 每五分钟检查。任一窗口剩余严格低于 10% 时保存并暂停，只有两项均严格高于 20% 且 ordinaryUsageAllowed=true 才恢复；以账户 `resetsAt` 为准，缺失额度不能视为充足。本轮未触发暂停。交付完成后保留该监测，状态无变化时静默，供本聊天后续工作使用。自动化状态以应用实际记录为准，不依赖本文件的历史数值。

恢复时先检查额度、Git 状态和此文件，续做未完成事项；一轮已交付后，按要求与路线图选下一项有界任务，避免重复已经完成的工作。
