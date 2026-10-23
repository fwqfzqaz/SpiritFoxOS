# 文档索引

文档按用途分层。设计说明描述目标规范；架构文档描述当前代码的真实行为；质量文档记录验证结果；项目文档记录进度与后续工作。

## 设计规范

- [设计说明 v0.2](design/DESIGN-v0.2.md)：目标架构、模块、ABI 与安全模型的原始规范。

## 实现与协议

- [实现边界与 ABI](architecture/IMPLEMENTATION.md)：当前实现、限制与未完成项。
- [存储协议与磁盘格式](architecture/STORAGE.md)：SFRFS1、virtio-blk 与只读区域。
- [目录能力与相对打开](architecture/NAMESPACE.md)：openat、只读目录委派与回收。
- [DMA 隔离与 VT-d](architecture/IOMMU.md)：QEMU q35 的 VT-d 支持与验证边界。
- [事务持久化](architecture/PERSISTENCE.md)：双银行、密钥撤销、失败恢复。
- [应用更新与回退](architecture/UPDATES.md)：双槽试运行、确认与健康回退。
- [截断与补零扩展](architecture/TRUNCATE.md)：文件长度变更语义与故障边界。

## 质量与验证

- [安全审查与修复](quality/SECURITY.md)：权限边界、已修复缺陷与回归范围。
- [测试记录](quality/TESTING.md)：各阶段的宿主与 QEMU 验证结果。

## 项目

- [路线图](project/ROADMAP.md)：后续工作与尚未验收的能力。
- [实施进度](project/PROGRESS.md)：按轮次记录的历史进度。

## 阅读顺序

新读者建议按“设计说明 → 实现边界 → 安全审查 → 测试记录 → 路线图”阅读。命令与工作区结构见根目录 [README](../README.md)。
