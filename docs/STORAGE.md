# 存储服务与只读区域

2026-10-23。该实现将真正的磁盘请求交给独立 CPL3 `sf-virtio-blk.sfmod`，SFRFS1 解析器和命名空间适配器暂组合于 sf-init。没有把磁盘文件预装进引导包来模拟磁盘加载。

## 硬件与授权

仅支持 PCI legacy/transitional virtio-blk：vendor/device `1af4:1001`、BAR0 I/O、单队列、最多 256 个描述符、单请求在途、512 字节扇区。默认模式只协商只读特性；M6 新增显式写授权、OUT 和 FLUSH，具体见 [事务持久化](PERSISTENCE.md)。QEMU 参数为 `disable-modern=on,disable-legacy=off,vectors=0`；未知可写盘不会获得事务写范围。

sf-init 枚举 PCI 总线 0，Core 0 再核对设备身份和 BAR。legacy 驱动只能通过系统调用访问 BAR 的 64 字节端口范围；modern 驱动仅映射已核实 BAR4 的 16 KiB uncached MMIO。均不能读写 PCI 配置、串口或其他端口。只有驱动获授固定 64 KiB DMA 区；此区不进入通用页池。隔离模式交付 IOVA 而非 CPU 物理地址。

内核只管理授权、映射和撤销；描述符链、队列、特性协商、容量、超时和请求状态均由用户态驱动处理。驱动使用轮询和内存屏障，不开放设备 IRQ。

停止和崩溃都先写设备复位，清除 Bus Master，并读回确认。确认失败时保留页并禁止新授权。成功释放 I/O 后，旧进程仍可能映射 DMA 页，因此拥有者身份保留至旧地址空间销毁；随后才允许新实例映射。测试包含旧进程已停止设备但仍存活时，新实例授权返回忙的情况。

没有 DMAR 的默认配置是可信驱动模式，固定缓冲区和端口权限不能阻止任意 DMA。可选 Q35 VT-d 模式使用 modern ACCESS_PLATFORM 和仅覆盖 arena 的设备页表；真实越界 DMA 读写触发故障、保持 canary 不变并终止隔离驱动。完整范围与失败策略见 [IOMMU.md](IOMMU.md)。真实复位失败、带在途 DMA 的物理拔出和断电仍未验证。

## 块协议

通过 ABI 1 的端点消息传输，仍使用 256 字节数据区：

| opcode | 请求 | 响应 |
| --- | --- | --- |
| BLOCK_INFO (50) | 无正文 | sectors:u64、sector_bytes:u32=512、readonly:u32=0 或 1 |
| BLOCK_READ (51) | offset 为 512 对齐的字节偏移；mode=0 或 256；length=256 | 同一扇区对应的半块，恰好 256 字节 |
| BLOCK_WRITE (52) | 两个有序半块；offset/mode/length 同读请求 | 只读模式拒绝；显式可写模式只写授权范围 |
| BLOCK_STOP (53) | 无正文 | 设备复位/撤销结果，进程等待管理器销毁 |
| BLOCK_FLUSH (54) | 无正文 | 等待实际设备 FLUSH 完成 |
| BLOCK_WRITE_RANGE (55) | offset=起始 LBA，handle=结束 LBA（不含） | 根服务配置一次；相同范围可重入，不能改为其他范围 |
| BLOCK_DMA_PROBE (56) | 根测试命令指定未映射 canary 地址及读/写方向 | 仅在 ISOLATED 模式刻意触发硬件故障，不用于正常 I/O |

文件系统通过两个半块请求取得一扇区。驱动缓存一个已成功完成的扇区。使用单调截止时间；超时先撤销设备再报错。校验完成索引变化、描述符 ID、成功状态和已写长度；失败不将旧缓存当作新数据。

## SFRFS1 格式

该格式是本项目的有界只读目录，不宣称兼容 FAT、ext4 或通用 POSIX 文件系统。所有整数小端；镜像为 512 字节倍数，范围 1 KiB–8 MiB，最多 16 个文件，每文件最多 128 KiB。构建器默认生成 1 MiB 镜像。

头部固定 512 字节：

| 偏移 | 字段 |
| --- | --- |
| 0 | magic[8] = `SFRFS1\0\0` |
| 8 | version:u32 = 1 |
| 12 | header_bytes:u32 = 512 |
| 16 | image_bytes:u64，与设备容量完全相等 |
| 24 | entry_count:u32 |
| 28 | entry_bytes:u32 = 128 |
| 32 | table_offset:u64 = 512 |
| 40 | table_bytes:u64 = entry_count × 128 |
| 48 | table_sha256[32] |
| 80 | reserved[432]，全零 |

每条目录记录固定 128 字节：path[64]、offset:u64、bytes:u64、sha256[32]、flags:u32=1、reserved[12]=0。path 为 NUL 终止的可打印 ASCII 绝对路径，尾部填零；拒绝空分量、`.`、`..`、反斜线、尾随 `/`、重复路径和文件作为父目录。记录按路径字节序严格递增；文件偏移 512 对齐，必须位于目录之后，非空且互不重叠。

挂载先校验容量、结构、目录摘要和每个文件摘要，再发布只读节点。摘要用于检测损坏，不能认证磁盘来源；可执行包仍须独立通过 Ed25519 信任策略。文件摘要通过但包签名错误的场景有专门 VM 测试。

创建自定义镜像：

```powershell
python tools/mkreadfs.py --out build/custom.img /bin/demo.sfk=build/hello.sfk
```

## 装载与引用

`/disk` 的文件、目录 open-description 都持有文件系统租约。独立 open 分别计数，dup 共享租约，最后一次 close 才释放。文件系统模块依赖块驱动；已挂载时停止驱动返回依赖阻塞。卸载先关准入，忙时取消并恢复访问；没有引用后才移除节点、释放依赖并停止驱动。

从盘装载模块时，进程服务固定最大 128 KiB 私有包快照，校验签名后创建地址空间。从盘装载应用时，另以顺序、有界 `OP_NATIVE_STAGE` 复制到 native 运行时；拒绝缺块、乱序和长度变化，再由运行时验证签名与装载计划。进程服务仍核对计划并由内核复查映射权限。源盘不会在验证与装载间被再次读取；镜像完全复制后运行不依赖打开的磁盘句柄。

## 验证与来源

M5 的 151 条宿主 C 断言和九组 QEMU 回归继续保留，所有只读场景的镜像运行前后 SHA-256 一致。可写介质拒绝用例针对没有事务格式的旧盘。M6 额外验证可写事务盘、重启与故障恢复，详见 `PERSISTENCE.md`、`TESTING.md` 和本机 `build/test-results.json`。

接口实现依据 [OASIS virtio 1.1 legacy 规范](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)、[QEMU PCI ID 定义](https://www.qemu.org/docs/master/specs/pci-ids.html)和 [QEMU blkdebug 文档](https://www.qemu.org/docs/master/devel/testing/blkdebug.html)。SFRFS1、RPC 和授权策略由本项目定义。
