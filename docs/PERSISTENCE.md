# M6 事务持久化原型

日期：2026-10-23。实现有界事务盘、持久化笔记和签名应用双槽；另有受限 Q35 IOMMU 与应用试运行确认。真实设备断电与通用文件系统仍是后续工作。`storage.img` 保持 M5 只读格式，`persistent.img` 必须以可写 virtio 盘连接。

## 提交与恢复

`services/txn.c` 同时用于宿主故障测试和用户态 sf-init。它只依赖三个回调：读 512 字节、写 512 字节、flush。成功 flush 必须使此前写入持久化；未 flush 的扇区允许丢失、乱序或撕裂。摘要检测损坏，不提供存储认证或防回滚。

每次事务完整复制最多四个文件，写入当前银行之外的另一个银行：

1. 清零备用提交记录，完成 flush。
2. 写入备用银行的全部 256 个扇区，完成 flush。
3. 写入含 generation 和银行 SHA-256 的提交记录，完成 flush。
4. 最后才更新内存中的当前文件并返回成功。

当前银行和它的提交记录始终保持不变。任何写入或 flush 失败都返回 `SF_COMMIT_UNCERTAIN (-20)`，关闭该卷的后续读写，要求重新挂载或重启后恢复。即使返回错误，新记录也可能已经落盘，所以不能承诺错误等于回滚。没有后台脏缓存；每个成功的持久化 write 都跨过完整提交边界。

恢复检查两个记录的结构、保留字节、卷 ID、记录摘要、银行摘要和文件结构，选择 generation 最大的有效快照。最新快照无效时退回另一份完整快照；两份均无效或两份有效记录声称相同 generation 时拒绝挂载。读 I/O 错误同样失败关闭。generation 达到 `UINT64_MAX` 后禁止继续提交，避免回绕。

## 固定磁盘布局

全部整数小端，容量必须恰为 4096 个 512 字节扇区（2 MiB）：

| LBA | 内容 |
| --- | --- |
| 0–2047 | SFRFS1 头、目录、只读签名包；文件必须完全位于这个区间 |
| 2048 | 不可变事务几何头 |
| 2049 / 2050 | 银行 0 / 1 的提交记录 |
| 2051–2306 | 银行 0，128 KiB |
| 2307–2562 | 银行 1，128 KiB |
| 2563–4095 | 保留，不写入 |

几何头：magic[8]=`SFTXN1\0\0`，version:u32=1，sectors:u32=4096，volume_id[16]，reserved[448]=0，sha256[32]。最后摘要覆盖前 480 字节。格式化工具从只读区域内容派生卷 ID；现阶段不把卷 ID 当作认证。

提交记录：magic[8]=`SFCMT1\0\0`，generation:u64，bank:u32，reserved:u32=0，volume_id[16]，bank_sha256[32]，reserved[408]=0，sha256[32]。记录所在槽和 bank 必须一致。

银行头位于前 512 字节：magic[8]=`SFBANK1\0`，generation:u64，count:u32，reserved[492]=0。紧接四条 128 字节记录：path[64]、bytes:u32、reserved[28]=0、content_sha256[32]。每个记录对应从银行偏移 `1024+i*16384` 开始的固定 16 KiB 数据槽。未使用记录、数据尾部与银行剩余空间全零。路径是小于 64 字节的 NUL 结尾 ASCII 绝对名，拒绝空分量、点路径、反斜线和重复名。这是固定键集合的快照，不是完整目录树文件系统。

`tools/mkpersist.py` 只格式化新构建镜像，写入空 generation 1；系统不会自动格式化无法识别的盘。重新构建会重建 `build/persistent.img`，实际使用时先复制为自己的工作镜像。

`python tools/qemu_test.py --interactive --persistent` 会在首次运行时创建 `build/user-persistent.img` 并以可写盘启动，以后复用这份副本。重新构建不会删除该副本；它仍受创建时的包签名公钥约束。

## 设备与命名空间

BootArgs 保持 96 字节：尾部由 `device_bdf:u32` 细分为 `device_bdf:u16,device_rights:u16`；装载计划原保留字段成为 `device_rights`。ABI 1 仍是开发中接口，旧驱动包不构成二进制兼容承诺。只有根进程能创建带设备授权的任务；未知权利以及无设备的写授权被拒绝。

默认驱动只接受只读介质。根服务可以给驱动显式 `SF_DEVICE_WRITE`，此模式要求设备可写并提供 `VIRTIO_BLK_F_FLUSH`，协商 FLUSH 后才能提交。只有 SFRFS1 文件不越过 1 MiB、事务几何与快照均验证成功，才启用 LBA 2049–2562 的写范围。这是可信块协议中的写边界；即使使用可选 VT-d 限制 RAM DMA，也不限制恶意驱动重新编程磁盘扇区写入。DMA 模式见 `IOMMU.md`。

OUT 请求通过两个严格有序的 256 字节消息组装。FLUSH 使用类型 4、sector=0、没有数据描述符；状态与队列完成仍经过校验。普通应用没有驱动端点、端口或 DMA 映射。

`/persist/notes` 映射到事务键 `/notes/user`，支持普通 open/read/write/seek 和独立偏移，最大 16 KiB。每次 write 保留原文件未覆盖的尾部，与 RAMFS 一致，不隐式截断。打开文件持有 `/disk` 文件系统引用，事务持有实际 `RES_DIRTY` 资源；卸载失败会恢复准入。其余三个键为 `/slots/a.sfk`、`/slots/b.sfk`、`/boot/current`，没有授予普通文件写接口。当前应用策略仍只允许控制台输出。

`/persist` 目录禁止创建其他文件，避免把 RAMFS 文件误认为持久化数据。卸载同步移除这两个投影节点；重新挂载从盘恢复。

## 签名应用双槽

```text
write /persist/notes durable-note
cat /persist/notes
install /disk/bin/diskhello.sfk
run installed
persist
```

install 固定源包快照，验证现有根公钥的 Ed25519 签名、应用类型、native 运行时和 ABI 1。包上限 16 KiB。备用包槽及 PENDING 选择记录在同一事务内提交，最后 flush 成功才输出 `install: COMMITTED`。每次启动 installed 都重新验证签名，再上传相同快照给隔离 native 运行时独立复查。文件摘要重新计算也不能让坏签名获得执行权限。

现在支持持久化试运行标记、按时退出 0 后确认、失败及中断后选择器回退，并保护 app_id 和包摘要。160 字节选择记录、旧格式迁移与精确健康边界见 [应用更新](UPDATES.md)。没有版本单调计数、TPM 防回滚、密钥轮换或多应用包管理器。银行是完整卷快照；A/B 文件是应用更新槽。四文件、每文件 16 KiB 是明确的原型配额。

## 验证依据

宿主在 261 个写/flush 操作边界分别注入丢失、部分持久化/撕裂、已持久化但返回错误，共 783 个失败场景。每次模拟崩溃再挂载，两个关联键只能同时是旧值或同时是新值。另测成功提交、损坏银行回退、两份损坏拒绝、相同 generation 拒绝、非法路径、超大文件和 generation 溢出。

QEMU 测试使用镜像副本，覆盖文件和 A/B 安装重启、卸载/重启驱动后重新挂载、存储摘要正确但签名错误的槽、银行损坏回退、实际 OUT/FLUSH EIO，以及写入银行中段时由 blkdebug 暂停后强制终止 QEMU，再启动恢复。只读底区和原始构建镜像摘要必须不变。进程终止测试不等于宿主机或物理盘断电测试。

协议参考 [OASIS virtio 1.1](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html) 的块设备 FLUSH 语义；错误注入依据 [QEMU blkdebug](https://www.qemu.org/docs/master/devel/testing/blkdebug.html)，暂停状态通过 [QMP query-status](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-query-status) 确认。
