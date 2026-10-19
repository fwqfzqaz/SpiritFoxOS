# 下一步开发交接

更新时间：2026-10-19（Asia/Shanghai）

## 本轮落点

从 d098b55 继续 codex/devfs-services；作者 main 414fd39、PR #2 开放未合并，
开发前已 fetch。新 dev.registry 实现启动登记/封存及统一目录，公共 /dev 枚举已走
同一服务，原硬件数据路径保留。最新接口与实测见
[统一设备对接](../integration/20261019-device-registry.md)。ABI 已升 1.4，设备协议
version=1（328 字节记录）；PCI version=1 不变，全部模块须重编译。

启动登记后禁止动态增删，不支持热拔插或用户态隔离；逻辑 owner 不是认证。
本轮测试覆盖实际 /dev、input、dri、zero 经 dup/fork 的生命周期，保留既有回归。
工作区主体两套构建/启动已通过，补充路径/OOM 边界在干净导出的宿主 UBSan 程序
通过。最终提交由 pre-push 再完整验证，远端结果见对应 PR/CI。

## 下一步：拆分启动阶段，为 AHCI 设备选择走 PCI 服务建立顺序

先 fetch 并查作者是否合并或新增文档，再按最新基线继续。目前 AHCI 初始化早于
sfm_boot，而 PCI 模块在 sfm_boot 才启动，不能直接把 AHCI 查找替换成消息调用。

建议先审计 init_core/hardware/storage/filesystem/services 的依赖，将启动包加载、
消息/PCI 自测与挂载/文件自测分成明确阶段。使内存分配和原 PCI 枚举就绪后，
不依赖磁盘的模块能在存储初始化前启动，再让 AHCI 从 dev.pci 选择控制器。
文件挂载、自测、真实设备登记仍在所需驱动就绪后进行。保留原算法，不同时改动
DMA、IRQ、AHCI 命令提交、FAT32 或调度器。

DEVICE-REGISTRY.md 记录已实现与后续接口建议。动态注销须另行评审引用/代数、
供应者失效和可睡眠锁，不是本轮已实现功能；当前热卸载禁令继续有效。

## 验收

- 写出启动依赖顺序和失败回滚，模块加载不反向依赖待初始化的磁盘/VFS。
- AHCI 实际通过 PCI 服务选控制器，已知 BDF/BAR 与原枚举一致；不虚构未知 BAR 大小。
- 启动包只加载一次，文件/设备各阶段自测仍执行；保留 PID 预留槽抢占回归。
- Ubuntu/Arch 各自完整编译及 UEFI 启动；AHCI FAT32 程序加载、统一设备目录、PCI/Bochs、
  null/zero、tmpfs、fork/wait 和 Wayland 检查全部通过。
- 每次提交同时更新对接文档与本文件；推个人 Fork，以 PR 交作者决定，不直接改作者分支。
