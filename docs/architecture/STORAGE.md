# 存储服务与只读区域

2026-11-07。真正的磁盘请求交给独立 CPL3 `sf-virtio-blk.sfmod`；格式探测、SFRFS1/SFRFS2 解析与文件服务已迁入独立 `sf-fs.sfmod`。sf-init 只保留 `/disk` 命名空间投影、块 RPC 代理与装载暂存。没有把磁盘文件预装进引导包来模拟磁盘加载。

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

## 文件系统服务（sf-fs.sfmod）

磁盘格式层与文件服务运行在独立 ring-3 地址空间 `org.spiritfox.fs.disk`。它不直接持有 virtio-blk 端点：`sf-init` 仍是驱动回复的唯一接收方，`sf-fs` 通过 `OP_FS_BLOCK_READ` 请求 256 字节半块，由 init 的 `serve_once` 与 `fs_wait_reply` 内的块代理执行。这样避免两个进程并发接收同一驱动端点，持久化拆分后的回复路由保持不变。

| 操作 | 请求 | 成功回复 |
| --- | --- | --- |
| `OP_FS_ATTACH_SEND (95)` | 附带 SEND 能力；data 无 | status=0；模块记录块代理请求端点与容量 |
| `OP_FS_ATTACH_REPLY (96)` | 附带 RECV 能力 | status=0；模块记录代理回复端点 |
| `OP_FS_MOUNT (97)` | hint:0 自动、1 SFRFS1、2 SFRFS2 | status=0，offset=探测到的格式号；模块逐个校验文件摘要 |
| `OP_FS_ENTRY (98)` | offset=索引；length=输出容量 | 路径、大小与数据偏移 |
| `OP_FS_FIND (99)` | data=绝对路径 | offset=索引，length=文件大小；未找到 `SF_NOT_FOUND` |
| `OP_FS_READ (100)` | offset=索引、handle=文件内偏移、length≤256 | 有界数据块 |
| `OP_FS_STOP (101)` | 无 | 模块退出，由管理器回收 |
| `OP_FS_BLOCK_READ (102)` | offset=512 对齐，mode=0/256，length=256 | 半个扇区；只接受已附着的代理回复端点 |

能力仍由 `SYS_CAP_DERIVE` 派生并随 `cap_count=1` 转移；服务端回复前清零 `cap_count` 与 `deadline_ns`，避免把附带能力反向转移或让长校验被请求截止时间拒绝。模块映像常住 1 MiB 文件校验缓冲，因此 sf-init 装载暂存区为 1280 KiB（仍在 2 MiB 可写段预算内）。命名空间目录、RAMFS、`/disk` 节点投影和持久化客户端仍留在 sf-init。

## SFRFS1 与 SFRFS2 格式

两者都是本项目的有界只读目录，不宣称兼容 FAT、ext4 或通用 POSIX 文件系统。所有整数小端；镜像为 512 字节倍数，共享 512 字节头部、128 字节目录记录与目录表 SHA-256。启动时按 magic 自动探测；未知 magic 返回 `SF_BAD_FORMAT`。

| 格式 | magic | 条目 | 单文件 | 镜像 | 空文件 |
| --- | --- | --- | --- | --- | --- |
| SFRFS1 | `SFRFS1\0\0` | 1–16 | ≤128 KiB | 1 KiB–8 MiB | 拒绝 |
| SFRFS2 | `SFRFS2\0\0` | 0–48 | ≤1 MiB | 1 KiB–32 MiB | 允许 |

SFRFS2 增加嵌套目录路径、空文件和更大的单文件/总容量；记录仍按路径字节序严格递增，文件偏移 512 对齐且互不重叠，任何文件不能同时作为父目录。SFRFS1 保持原始上限，旧镜像行为不变。

头部固定 512 字节：

| 偏移 | 字段 |
| --- | --- |
| 0 | magic[8] = `SFRFS1\0\0` 或 `SFRFS2\0\0` |
| 8 | version:u32 = 1 |
| 12 | header_bytes:u32 = 512 |
| 16 | image_bytes:u64，与设备容量完全相等 |
| 24 | entry_count:u32 |
| 28 | entry_bytes:u32 = 128 |
| 32 | table_offset:u64 = 512 |
| 40 | table_bytes:u64 = entry_count × 128 |
| 48 | table_sha256[32] |
| 80 | reserved[432]，全零 |

每条目录记录固定 128 字节：path[64]、offset:u64、bytes:u64、sha256[32]、flags:u32=1、reserved[12]=0。path 为 NUL 终止的可打印 ASCII 绝对路径，尾部填零；拒绝空分量、`.`、`..`、反斜线、尾随 `/`、重复路径和文件作为父目录。记录按路径字节序严格递增；文件偏移 512 对齐，必须位于目录之后，非空文件互不重叠。SFRFS1 要求每个文件非空；SFRFS2 允许长度为 0，但仍要求偏移合法且对齐。

挂载先校验容量、结构、目录摘要和每个文件摘要，再发布只读节点。摘要用于检测损坏，不能认证磁盘来源；可执行包仍须独立通过 Ed25519 信任策略。文件摘要通过但包签名错误的场景有专门 VM 测试。

创建自定义镜像（默认 SFRFS1；`--format 2` 生成 SFRFS2）：

```powershell
python3 tools/package/mkreadfs.py --out build/custom.img /bin/demo.sfk=build/hello.sfk
python3 tools/package/mkreadfs.py --format 2 --out build/custom2.img /bin/demo.sfk=build/hello.sfk
```

## 装载与引用

`/disk` 的文件、目录 open-description 都持有文件系统租约。独立 open 分别计数，dup 共享租约，最后一次 close 才释放。文件系统模块依赖块驱动；已挂载时停止驱动返回依赖阻塞。卸载先关准入，忙时取消并恢复访问；没有引用后才移除节点、释放依赖并停止驱动。

从盘装载模块时，进程服务固定最大 128 KiB 私有包快照，校验签名后创建地址空间。从盘装载应用时，另以顺序、有界 `OP_NATIVE_STAGE` 复制到 native 运行时；拒绝缺块、乱序和长度变化，再由运行时验证签名与装载计划。进程服务仍核对计划并由内核复查映射权限。源盘不会在验证与装载间被再次读取；镜像完全复制后运行不依赖打开的磁盘句柄。

## 验证与来源

M5 的 151 条宿主 C 断言和九组 QEMU 回归继续保留，所有只读场景的镜像运行前后 SHA-256 一致。可写介质拒绝用例针对没有事务格式的旧盘。M6 额外验证可写事务盘、重启与故障恢复，详见 `PERSISTENCE.md`、`../quality/TESTING.md` 和本机 `build/test-results.json`。

`tests/integration/test_readfs.py` 通过宿主 `sf_fs_check_auto` 覆盖自动探测、SFRFS2 空文件/嵌套目录、构建器上限、目录表损坏、越界/重叠/未排序记录和文件作为父目录的拒绝。`tests/qemu/qemu_test.py --storage v2` 使用 `build/storage-v2.img` 启动产品镜像，断言 `[STORAGE] format 2` 并重复挂载、只读拒绝、卸载/再挂载和从盘运行签名包等命令；默认 `normal` 继续覆盖 SFRFS1。

接口实现依据 [OASIS virtio 1.1 legacy 规范](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)、[QEMU PCI ID 定义](https://www.qemu.org/docs/master/specs/pci-ids.html)和 [QEMU blkdebug 文档](https://www.qemu.org/docs/master/devel/testing/blkdebug.html)。SFRFS1、SFRFS2、RPC 和授权策略由本项目定义。
