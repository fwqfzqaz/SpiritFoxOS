# 只读块消息与 AHCI 几何对接

日期：2026-10-19，Asia/Shanghai。基于 77db849，开始前已 fetch，作者 main=414fd39，
PR #2 开放未合并，继续 codex/devfs-services，个人 Fork + PR 交付。

## 范围与版本

完成 NEXT 的容量前提和受控 READ。ABI 由 1.4 升 1.5（0x00010005），核心表追加
block_read(index,lba,count,data,capacity)，偏移 88、表大小 96；包/SDK/所有模块和
坏版本测试一起更新，旧 1.4 包/模块拒绝。PCI/设备协议仍 v1，块协议升级 v2。

64 字节块记录不变，偏移 56 原 reserved[0] 改 geometry_source，偏移 60 保留为零。
UNKNOWN=0、SOFTWARE=1（仅 RAMDISK）、ATA=2（仅 AHCI）；UNKNOWN 必须带
GEOMETRY_UNVERIFIED，已知来源必须无此标志。不能按设备类型直接认定容量可信。

REGISTER/SEAL/ENUM/GET 的消息编号、索引/序号及快照语义不变，flags 必须为 2。
存储结束先登记，再冻结旧 blkdev 注册表与消息清单；blkdev_init/register/unregister
不得重置或复用冻结槽。冻结不是能力认证，可信 Ring 0 代码仍可任意改内存。

## AHCI 容量与传输

IDENTIFY 成功后检查 ATA、DMA/LBA、有效 LBA48 支持字、非零且可用 48 位寻址的容量。
当前只支持 512 字节逻辑扇区；512e 可用，4Kn/LBA28-only 或坏字段拒绝，不假装已经支持。
失败不再填 2048 扇区，不登记该盘。软件 RAMDISK 保留显式来源，借用 initrd 仍由调用者保活。

修正旧 DMA EXT FIS 高位布局：bytes 8..10 放 LBA 24..47，device 字节不塞旧 LBA28 位。
非零 count<=65535、LBA+count 在 48 位范围、缓冲区对齐/地址范围先检查；范围受已有
8 项 PRDT 资源约束。命令完成须 PRDBC 等于预期字节数，不把短 DMA 当完整成功。
每端口命令槽以保存中断状态和 try-lock 保护，避免时钟抢占/其他 CPU 覆盖 FIS/PRDT。
忙立即 EBUSY；超时标坏端口，禁止后续发命令。不是完整 HBA 恢复或 IRQ/DMA 代理重构。

核对字段使用 [Intel AHCI 1.3.1 规范](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf)
及 [Linux ATA 字段定义与有效性检查](https://github.com/torvalds/linux/blob/master/include/linux/ata.h)。
本仓库自行实现纯解析/编码函数与测试向量，没有引入 Linux 驱动源码。

## READ 消息

READ=0x5004，flags=2、route=0、handle=端点 snapshot、offset=LBA、size=16，
载荷 `{uint32 index,count; uint64 reserved=0}`。index 是旧槽号，不是 ENUM 序号；
count 非零，乘扇区大小不得超过 4096 字节，LBA/count 用减法检查容量边界。
未封存 EAGAIN；失效快照/无设备 ENOENT；坏字段/长度/越界 EINVAL；未知容量 ENODEV。
响应 result=size=字节数、handle=snapshot、offset=LBA。错误 size=0、数据全清零。
WRITE=0x5005 始终 EROFS（版本/route 须合法），没有新写 API；其余未知操作 ENOSYS。
该只读保证只针对新消息路径，原文件系统/块驱动仍保留兼容写功能。

模块先做策略检查；核心 API 再核对冻结状态、真实记录和容量/输出容量，禁止超过单页。
每设备首次读惰性分配一页物理连续缓冲，归核心终身持有，不作为模块内存交给 DMA。
成功后只复制实际请求字节；页先清零，失败不复制，不泄露前次读取或部分 DMA 数据。
分配失败 ENOMEM 且可以重试；底层忙 EBUSY 且不隔离；其他底层错误统一 EIO，隔离该
设备页并禁止本服务重试。超时控制器可能仍写旧页，因此该页不能释放/复用。
最多 16 页（64 KiB），不是可卸载/热重启方案。底层 polled I/O 期间会关闭本 CPU 中断，
没有提供有界实时延迟保证；后续 IRQ/等待/取消机制需要单独设计。
上述 DMA 生命周期保护覆盖新消息缓冲和失败的 IDENTIFY 缓冲；原 FAT32 等直接块调用者
在控制器超时时的缓冲回收尚未统一收口，不据此声称整个旧驱动的超时安全已经解决。
下一步读取桥将减少这类直接调用，旧写路径和控制器复位仍须另行处理。

## 实测与验收

make test-modules 执行实际模块及核心适配器的 UBSan：旧 ABI 拒绝、几何来源/标志一致、
空/稀疏/满清单、未封存、快照、0/最大长度、首/末/越界/溢出、512/4096 软件扇区、
空已知设备、未知来源、OOM、无分配重读、重入忙、硬件忙可重试、部分失败不泄露、
错误页禁止复用。make test-ata 用 ASan/UBSan 验证纯 IDENTIFY 与固定 FIS 字节向量；
原 RAMDISK 的 ASan/UBSan 继续验证真实驱动/分派代码及冻结槽不能复用。

启动时通过 READ 与实际 blkdev_read 对照各可用盘的首、末和最大 4096 字节范围，
检查越界/失效快照/写拒绝。普通启动不强制存在 AHCI 或 FAT；CI 则必须看到两 SATA 盘、
默认 RAMDISK 和 FAT 首扇区签名，且全部既有用户文件/设备/PID/fork/Wayland 回归通过。
Windows tools/dev.ps1 verify；提交后 pre-push 完整 verify-ref，Linux tools/check-linux.sh。

2026-10-19 最终工作区已通过 Ubuntu GCC 13.3/NASM 2.16 与独立 Arch chroot
GCC 16.2/NASM 3.02 全量编译、各自产物 Windows QEMU TCG UEFI 启动。
宿主模块/核心 UBSan、ATA 和 RAMDISK ASan+UBSan 全部通过，verify 回执
build_exit_code=0、exit_code=0。两套均出现已确认 LBA48/512 几何、真实块 READ 对照、
FAT 首扇区签名及原 Wayland 10 帧标记；底层忙/部分失败/来源不匹配等补充用例已纳入。
最终提交由 pre-push 再完整验证，远端结果以对应 PR Actions 和最终交付记录为准。
未验证真机、4Kn、长期压力、超时硬件故障恢复或 Ring 3 隔离。
下一步见 NEXT.md，FAT32 读取和普通 /dev 块节点字节读取尚未迁到消息路径。
