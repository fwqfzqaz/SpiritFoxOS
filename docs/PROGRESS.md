# SpiritFoxOS 实施进度

更新日期：2026-10-23（北京时间）。

- 工作区：`E:\SpiritFoxOS`。指定源目录的三项顶层内容已移动到这里。
- 本轮目标：用户再次要求继续。在 M5 上推进 M6 事务持久化：显式写权限、设备 flush、双银行快照与提交记录、签名包双槽激活、故障恢复验证。
- status: m6_persistence_tested
- round_complete: true
- quota_paused: false
- 分支：`codex/boot-only-restart-20261022`；远端：`origin`。
- 当前没有遗留的构建、QEMU 或 DeepSeek 辅助进程。本轮两次只读审查因权限预设初始化失败而超时，没有采用结果。

## 已完成

1. Core 0：隔离页表、异常、单核抢占、能力端点、队列与超时、任务回收。
2. 用户态模块管理、依赖与资源门控、忙停止取消、新旧实例并存。
3. 用户态内存命名空间、文件、管道、控制台、串口 shell。
4. 确定性 CBOR、SHA-256/Ed25519 包工具；独立 native 运行时验证装载计划；hello 与故障测试应用。
5. `python tools/test.py` 已完整通过：116 条 C 服务断言、8 组包测试与 1500 次固定种子变异、128/256/512 MiB QEMU 启动及 shell 回归。
6. README、实际实现边界、测试记录、后续路线均在工作区内；Windows CI 定义已加入，本地通过不代表托管 CI 已运行。
7. M5：独立用户态 virtio-blk、PCI/端口与固定 DMA 页授权、SFRFS1 只读磁盘、`/disk` 挂载与引用、从盘运行签名模块和应用、磁盘快照上传至 native 运行时。
8. M5 完整测试通过：151 条 C 断言、13 组 Python 测试（2000 次固定种子变异）、128/256/512 MiB 正常 VM 与六种存储失败 VM，共九组。盘文件前后摘要不变；测试结果含发布镜像摘要。
9. 驱动进程死亡会执行设备复位；旧进程释放后仍存活时禁止新 DMA 拥有者，地址空间销毁后才允许接管。未实现 IOMMU，不宣称隔离恶意 DMA。
10. M6 有界持久化：显式写授权和范围、实际 virtio OUT/FLUSH、双银行提交/恢复、`/persist/notes`、签名应用 A/B 安装与每次运行重验。写/flush 失败停止该卷读写，重挂载或重启恢复。
11. 最终 `python tools/test.py` 完整通过（70.11 秒）：151 条基础/存储 C 断言、事务 C 测试、14 组 Python 测试与 2000 次固定种子变异、261 个操作边界 × 三种故障模式、十八次 VM 启动。验证笔记/包重启、坏签名拒绝、损坏回退、实际写与 flush EIO、银行写入中段终止 VM 后恢复。原始构建盘及只读底区保持不变。
12. 结果为 `build/test-results.json`，日志为 `build/m6-final-test.txt` 与 `build/qemu-persist-*.log`。打包入口生成 `build/SpiritFoxOS-M6.zip`，包含精确测试的 EFI、两种盘、公钥及文档；打包前复核产物摘要。

## 交付与续做

本轮基于 M5 提交 `315f737` 完成持久化原型。交付提交由本分支 Git HEAD 标识，推送是否完成以 `origin/codex/boot-only-restart-20261022` 与 HEAD 实际对照为准；若不一致，仅续做已授权提交/推送，不重新实现或重复测试。IOMMU 与真实硬件断电保证仍需单独实现和验证，不把本轮事务原型称为完整 M6，也不自行开始 ROADMAP 后续阶段。

交互：`python tools/qemu_test.py --interactive --persistent`。首次创建 `build/user-persistent.img`，后续继续使用该副本；构建只覆盖原始 `persistent.img`。操作与格式见 `docs/PERSISTENCE.md`。

## 额度保护

本轮使用自动任务 `spiritfoxos-2` 每五分钟检查。任一窗口剩余严格低于 10% 时保存并暂停，只有两项均严格高于 20% 且 ordinaryUsageAllowed=true 才恢复；以账户 `resetsAt` 为准，缺失额度不能视为充足。本轮未触发暂停。交付完成后保留该监测，状态无变化时静默，供本聊天后续工作使用。自动化状态以应用实际记录为准，不依赖本文件的历史数值。

恢复时先检查 Git 状态和此文件，再续做已授权且未完成的事项。已经测试推送的本轮成果不重复开发、测试或推送；没有新工作时保持安静。
