# QEMU q35 VT-d 与 modern virtio

日期：2026-10-23。该实现为已有 M6 存储原型增加受限 DMA 重映射，覆盖 QEMU q35 的一个 Intel VT-d 单元、PCI segment 0、bus 0 的直接端点和一个 virtio-blk 拥有者。它不代表通用硬件驱动或完整设备隔离已经完成。

## 发现与失败关闭

UEFI stub 从配置表传递 ACPI RSDP。Core 0 使用同一份有界解析器验证 RSDP、RSDT/XSDT 和 DMAR 的长度与校验和；物理读取只允许位于已报告 ACPI reclaim/NVS 内存中的对象，且低于 1 GiB。表长上限 4 KiB，遍历有界。重复 DMAR、重复硬件单元、非零 segment、RMRR 等未实现结构拒绝。

接受一个寄存器基址为 `0xfed90000` 的 DRHD。可使用 include-all 或显式总线 0 单跳端点范围；端点不在范围内就不给设备授权。桥和其他总线没有自动展开，也没有 RMRR 恒等映射。能力必须支持 39 位三级页表，寄存器窗口在一个 4 KiB 页内，且没有固件留下的活动转换、队列失效或中断重映射配置。

没有 DMAR 时明确输出 `absent; trusted DMA mode`，保留此前 legacy 驱动模式。存在损坏或不支持的 DMAR/能力，或根表、缓存失效、转换启用不能确认时，状态变为不可用并禁止 DMA 设备授权。不会把识别失败当作隔离成功。

## 页表与授权

启用转换前清除总线 0 设备的 Bus Master。默认根表为空，所有未授予的请求者都没有映射。授权时只为选定 BDF 发布 domain 1 的 legacy root/context 和三级 second-stage 页表：

| 设备 IOVA | CPU 物理目标 | 权利 |
| --- | --- | --- |
| `0x01000000`–`0x0100ffff` | 内核保留的 64 KiB DMA arena | 读、写 |
| 其他地址 | 无 | 拒绝 |

驱动 BootArgs 的历史字段 `dma_physical` 在此模式下表示**设备总线地址/IOVA**，不是 CPU 物理地址。现有 virtqueue 描述符统一使用这个地址。用户虚拟 DMA 地址仍为 `0x50000000`。根表、context 和 I/O 页表不包含在 DMA arena 内，普通应用也没有 DMA 或设备 MMIO 映射。

CPU 端只给内核映射 uncached、supervisor、NX 的 VT-d 寄存器页。驱动获得其 BAR4 的 16 KiB uncached RW/NX 用户映射，内核另有 supervisor 别名用于复位。普通用户进程读取 VT-d 控制寄存器实际触发页故障。非一致性页表遍历时采用 WBINVD 同步；一致性支持时使用内存屏障。该保守做法没有优化缓存维护成本。

发布和撤销都进行全局 context-cache 及 IOTLB 失效，等待完成并核对实际粒度。支持硬件报告的读/写排空位。撤销顺序为设备复位、关闭 Bus Master、清 context present、同步并失效，再保留空表。DMA arena 永久保留；CPU 地址空间销毁后才能重新分配设备拥有权。失效或复位不能确认时，禁止后续授权并保留页。

## 为什么需要 modern 传输

QEMU legacy/transitional virtio 未协商 ACCESS_PLATFORM 时会使用绕过平台 IOMMU 的 DMA 路径。仅启用 VT-d 不足以保护这个驱动。因此 Core 0 在隔离模式下拒绝 legacy virtio-blk，接受已验证 BAR4 布局的 modern-only PCI `1af4:1042`。

modern 驱动协商 `VIRTIO_F_VERSION_1` 和 `VIRTIO_F_ACCESS_PLATFORM`，确认 FEATURES_OK，再配置队列并设置 DRIVER_OK。common/device/notify 分别位于 BAR4 偏移 `0`、`0x2000`、`0x3000`，notify multiplier 为 4。仅此已验证配置受到支持；不是任意 PCI capability 或 virtio MMIO 实现。数据协议、单队列轮询、半扇区消息、OUT/FLUSH 与只读格式仍位于用户态 sfmod。

```powershell
python tools/qemu_test.py --interactive --persistent --iommu
python tools/qemu_iommu_test.py
```

QEMU 使用 `intel-iommu,intremap=off,caching-mode=on,device-iotlb=off,aw-bits=39`，块设备使用 `disable-legacy=on,iommu_platform=on,vectors=0`。没有设备 ATS/IOTLB，没有 MSI/MSI-X 或中断重映射。轮询 virtio 和既有 PIC/PIT/串口是当前验证配置。

## 故障与验证

Core 0 在用户态进入内核的时钟/系统调用边界有界轮询 VT-d fault recording，记录 source ID、地址和原因。拥有者产生越界 DMA 时，先复位、撤销并失效，再终止进程，永久隔离该设备至 VM 重启。其他用户服务继续运行。

诊断命令 `dma-test write` / `dma-test read` 向一页填充 `0xa5` 的内核 canary 发出真实越界设备 DMA；前者使用 IN 数据目标，后者使用请求头来源。命令只在已启用隔离且拥有有效映射时接受，没有 IOMMU 会拒绝执行。检测成功要求硬件 fault 数增加、source/地址吻合、canary 完整、context 已撤销、驱动死亡，并通过再次创建驱动证明内核拒绝新的拥有者。该命令会使测试 VM 的磁盘驱动不可恢复，仅供隔离故障测试使用。

宿主执行 229 条 ACPI/DMAR/页表检查，覆盖范围、校验和、非法长度、segment、RSDT、重复表和设备映射边界。QEMU 另外验证 128/512 MiB 正常启动、DMA 读/写故障、legacy 旁路拒绝、重复 DMAR 拒绝，以及全部九次持久化重启/损坏/写入和 flush 错误/中断提交用例。加上原十八次回归，共三十三次 VM 启动。故障 VM 的磁盘摘要保持不变，保护页检查不是仅由模拟页表推断。

## 剩余边界与来源

尚未实现真实硬件验证、多个 remapping unit、PCI bridge/ACS 分组、RMRR、ATS、scalable mode、queued invalidation、AMD-Vi、IRQ 重映射、物理拔出和复位失败的硬件注入。这里证明的是上述 QEMU 配置中设备对未授权 RAM 的 DMA 读写被阻止；不能将它扩展为任意恶意设备、固件或中断路径的完整安全保证。真实断电一致性也继续单独验收。

位域与顺序依据 [Intel VT-d 架构规范 Rev.4](https://cdrdv2-public.intel.com/671081/vt-directed-io-spec.pdf) 第 6、8、9、11 章；配置依据 [QEMU Intel IOMMU 参数](https://www.qemu.org/docs/master/system/invocation.html#hxtool-4)。QEMU 的 [virtio PCI](https://github.com/qemu/qemu/blob/master/hw/virtio/virtio-pci.c) 和 [virtio 核心](https://github.com/qemu/qemu/blob/master/hw/virtio/virtio.c) 是 transport/bypass 行为的核对来源；实现没有复制其驱动代码。
