# 下一步开发交接

更新时间：2026-10-19（Asia/Shanghai）

## 当前落点

codex/devfs-services 在 e15af9b 后追加 PCI 只读快照、BAR 描述和 Bochs 设备选择。
作者 main 仍为 414fd39，PR #2 未合并，沿用同一 PR；下轮先 fetch 复核。
本轮接口与实测见 [PCI 对接](../integration/20261019-pci-snapshot.md)，此前 null/zero
对接记录保留。ABI 升至 1.3，PCI 协议 version=1，文件 route 保持 1.2 的布局。

仍属阶段 3，AHCI 数据面、统一设备注册与阶段 4 Ring 3 隔离未完成。
BAR size=0 且 SIZE_KNOWN 未置位表示未知，不可当作可映射资源范围。
工作区初版两套构建/启动通过；新增边界测试在干净导出源码上通过宿主 UBSan。
最终提交仍由 pre-push 完整复测；远端结果以 PR 的 Actions 记录为准。

3c5b51f 的远端 Ubuntu 暴露旧 PID 提前 READY 竞态，已改为 PROC_ALLOCATING，
上下文完成才发布。新增启动预留槽抢占回归；最终修复必须通过两套完整检查。
后续创建进程不得在 alloc_pid 与 process_enqueue 之间重置为 UNUSED 或 READY。

## 下一步：统一设备注册与目录视图

先按 [设备注册提案](../DEVICE-REGISTRY.md) 评审布局、逻辑 owner、节点代数、
名字链接/打开引用、旧 devfs 适配器。提案尚未实现，可以按作者新文档调整并记录依据。
建议先交启动期固定登记和只读 LOOKUP/READDIR，公共 /dev 与消息枚举读取同一记录；
至少覆盖 null/zero、input、dri。旧 inode 操作继续作为后端适配器，不同时迁移驱动
数据面。动态注销可以后续实现，首版明确返回不支持，不伪造成功；保留热卸载禁令。

## 验收与提交

- 两种目录中各设备只出现一次；重复、坏路径、未知 owner、错误版本/长度与容量有测试。
- open/dup/fork/close 生命周期不回归，不发布没有可用后端的节点。
- 保留 PCI 快照对照、Bochs 走服务、AHCI FAT32、input/DRM、null/zero、tmpfs、fork/wait 与 Wayland 检查。
- 改 ABI 时同步 SDK、打包器、模块、调用方、文档和旧包拒绝测试。
- Ubuntu/Arch 编译及 UEFI 启动通过后推个人 Fork，作者决定是否合并 PR。
- 每次提交前同时更新 docs/integration/*.md 和本文件，记录基线、实测与未完成项。

后续再推进 AHCI 控制接口、IRQ/DMA 边界和阻塞/取消 IPC。当前同步 RPC 不能睡眠，
Ring 0 端点/设备编号不是安全能力，不能仅包装旧函数就宣称用户态驱动完成。
