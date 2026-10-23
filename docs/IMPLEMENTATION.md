# 实现边界与 ABI

日期：2026-10-23。本文说明当前代码的实际行为；原始设计说明继续作为目标规范保存。

## 启动链

固件加载 `BOOTX64.EFI`。提供的 EFI stub 已扩展为：有界加载内嵌 ELF 内核；验证启动包的 SFMOD1、CBOR、SHA-256 和 Ed25519；预装 sf-init 的段；在 BootInfo 中交付段计划和只读启动包；使用最新内存映射退出 Boot Services。

Core 0 校验 BootInfo 大小、版本、内存映射边界、引导对象的 Loader 内存归属、物理与虚拟段重叠、用户入口和段权限。Core 0 不链接 ELF、SFK、CBOR 或签名解析器。它复制已装载的初始段到自己拥有的页，建立独立 CR3，然后通过 IRET 进入 CPL3。

内核固定加载到物理 32 MiB，避免固件低内存中的分配空洞。引导映射 0–4 GiB；进入任务后只有前 1 GiB 的 supervisor 恒等映射和明确建立的用户页。当前适用范围为本次验证的 QEMU q35、单核、128–512 MiB 内存。

## Core 0

- GDT、TSS、IDT、双重故障 IST、用户异常回收。
- 单核 100 Hz PIT 抢占与轮转调度。每个任务一个线程；不是优先级调度器。
- 固定 4096 页（16 MiB）内核页池，页按 pid 归属，失败与退出时回收。没有物理内存通用分配服务或按需分页。
- 4 KiB 用户页、只读代码和常量、RW/NX 数据与栈、未映射栈保护页、只读 BootArgs。映射计划拒绝 W+X、重叠、越界和不可执行入口。
- 进程内端点能力、32 个能力槽、权利衰减、句柄 generation、端点整体撤销、8 条有界队列、复制消息和接收截止时间。
- 保留可裁剪的串口日志和恢复控制台。串口 IRQ 输入队列 256 字节，溢出显式报告。

任务槽上限为 12，端点上限为 64。启动管理器持有原型根权限；普通应用不能创建其他地址空间、杀进程、读取控制台、读取 PCI 配置或关闭系统。没有开放通用物理页/MMIO/DMA 映射接口，`SYS_MAP_TEST` 一律拒绝；另有真实恶意装载计划测试覆盖内核映射校验。M5 新增 root 专用 PCI 只读查询、由 root 指定设备的单一拥有者授权、受限端口 I/O 和设备撤销机制；virtqueue 与块协议不在内核中。

能力撤销当前以整个端点为单位，不实现完整派生树。IPC 消息上限为固定 `SfMessage`（328 字节，其中正文数据 256 字节），采用 ABI 1 的 40 字节头；拒绝伪造 cap_count。当前不提供附带能力转移、通用共享内存、调用优先级继承或大消息零拷贝。

关闭最后一个能力引用会回收端点；存在派生或跨任务引用时保持有效，拥有者退出仍整体撤销。设备 MMIO 页可由获授驱动直接访问，但不能作为系统调用复制缓冲区；普通 DMA RAM 不受该限制。返回用户态之前检查 RSP canonical 性。`SYS_TASK_STATUS` 对活任务返回 64 位 `SF_TASK_RUNNING (0x100000000)`，死亡任务返回有符号 int32 退出码；这是未冻结 ABI 的语义修订，所有当前服务已同步重建。修复及回归见 `SECURITY.md`。

## 用户态服务

sf-init 在一个用户态二进制中组合模块管理、sf-process、sfexec、namespace、RAMFS、SFRFS1 文件服务和简化 stdio 策略。回显服务、sf-native 和 sf-virtio-blk 分别运行在独立地址空间。shell、磁盘解析和普通文件逻辑不在内核中。

sf-native 接收 `OP_NATIVE_PREPARE`，验证应用签名、类型、运行时身份与段布局，返回有界的 native 装载计划。来源可以是只读 bundle，也可以是 sf-init 从磁盘固定并通过 `OP_NATIVE_STAGE` 按序复制的 128 KiB 有界快照。sf-init 将计划与同一包核对，准备映像，再由 Core 0 独立校验映射与复制数据。应用持有运行时租约，退出和失败时释放；运行时死亡返回可诊断状态并允许受控重启。

管理器有生命周期状态、依赖 DAG 检查、唯一 generation、准入门和资源账本。忙停止返回阻塞，取消后恢复 ACTIVE；替换采用新旧实例并存。M5 将挂载依赖、打开对象和 DMA 账本接入实际驱动生命周期；M6 将实际同步磁盘事务接入 dirty 资源和卸载门。早期 timer、mapping、IRQ 等资源注入测试仍仅验证回收门控。跨服务通用生命周期 RPC、外部请求并发压测与在线迁移仍需后续实现。

命名空间包含目录、1 KiB 内存文件、管道、控制台和最大 128 KiB 的只读磁盘文件。open 有独立偏移，dup 共享 open-description，并保留挂载租约至最后一个引用关闭。支持 `/disk` 的受控挂载/卸载；文件和目录的活跃句柄都会阻止卸载。namespace 路径为 UTF-8，拒绝 `.`/`..`、重复与尾随斜线；SFRFS1 磁盘名额外限制为可打印 ASCII。新增目录句柄 openat 和读写权限衰减；`runat` 按签名 namespace.read 请求与显式目录选择授予只读目录，应用经真实 IPC 相对打开、列举和读取，退出/故障/超时回收引用。普通 run 仍只授予控制台写入。没有符号链接、动态挂载视图或完整身份策略，协议和边界见 `NAMESPACE.md`。

M6 新增 `/persist/notes` 的 16 KiB 有界读写适配器，每次成功 write 完成双银行事务与设备 flush。卷最多五个 16 KiB 文件，余下槽位用于签名应用 A/B 包、选择器与密钥撤销记录。安装先验证类型、ABI、运行时与签名，将备用包与 PENDING 记录一起提交；运行时再次验证固定快照。试运行前持久记录 TRYING，按时退出 0 后确认，失败或中断回退已确认槽，应用身份与包摘要绑定。已确认包的签名数字版本形成软件下限，拒绝降级与同版本换包，精确重装不写盘；候选执行时再次验证版本策略。写/flush 失败停止准入，重挂载或重启后恢复。格式、错误模型与限制见 `PERSISTENCE.md` 和 `UPDATES.md`；它不是完整 POSIX 文件系统或全局更新系统。

包验证使用构建时生成的双锚点信任集合（`services/trust.c`），boot bundle、磁盘包与持久化安装统一经过信任存储；运行期撤销把签名者 key_id 持久化到事务键 `/boot/revoked`，挂载时在任何包重新验证之前加载。被撤销签名者的有效签名按 `SF_REVOKED` 拒绝，与 `SF_BAD_SIGNATURE` 区分。`revoke-key` 要求目标包当前仍有有效锚点签名，重复撤销幂等。撤销上限 4 个 key_id；语义与限制见 `PERSISTENCE.md`。

统一句柄新增 truncate 与 RPC：RAMFS 和持久化笔记可缩短、清空或补零扩展，保持句柄偏移；只读委派不能截断。笔记长度变化走同一事务引擎，同长度不改盘，失败停止准入并允许释放 dirty 引用。实际恢复与权限边界见 `TRUNCATE.md`，没有增加磁盘格式或通用文件创建/删除能力。

## native 包子集

公开容器字段、签名消息和 CBOR 限制遵守设计说明 v0.2。包解析上限 32 MiB、64 个段、64 KiB 清单和 16 层 CBOR。入口必须在 RX 文件内容内。映射段页粒度不重叠；间隙必须全零；尾随字节拒绝。校验与装载使用同一只读 bundle 或由受信任进程服务固定的私有磁盘快照。

当前 native 代码预先链接到虚拟地址 `0x40000000`，映像上限 2 MiB；sf-init 的装载暂存区上限 1 MiB，sf-native 返回计划最多 5 段。所有进程有独立页表，因此相同虚拟基址不会共享可写物理页。该受限配置不宣称实现设计说明中完整的位置无关、相对重定位和导入协议；非空 RELOC 一律拒绝。

用户栈顶 `0x41010000`，大小 64 KiB；只读 BootArgs 在 `0x42000000`，其 argv/env 当前为空。只读启动包在 `0x60000000`，仅授予初始任务和 native 运行时。BootArgs 原 24 字节保留区现用于可选设备授权信息；驱动的 64 KiB 固定 DMA 区位于 `0x50000000`，普通应用没有该映射。普通应用通过 `_sf_start` 进入 main，并通过退出系统调用回收。

本开发版本保持 BootArgs 总长 96 字节，把尾部设备 BDF 的高 16 位定义为 device_rights；装载计划相应字段传递显式写权。只读驱动模式和带 FLUSH 的可写模式分别协商。尚未冻结 ABI，不承诺兼容此前构建的设备驱动包。

设备释放先复位并确认状态、禁用 PCI Bus Master，再撤销 I/O 权和可选 IOMMU context。DMA 页始终保留；只有旧地址空间销毁后才允许新驱动使用。复位或失效不能确认时进入隔离状态并禁止新授权。默认无 DMAR 时是 legacy 可信驱动模式；QEMU q35 可选 Intel VT-d + modern virtio ACCESS_PLATFORM，实际把未授权 RAM DMA 阻止在设备页表之外。完整限制见 `IOMMU.md`。

VT-d 实现验证 ACPI/DMAR，发布一个 BDF 的固定 64 KiB DMA 域并执行同步失效；没有通用恒等 DMA 映射。支持已核实的 modern BAR4 布局，驱动仅获授其 16 KiB uncached MMIO。BootArgs 新增只由内核报告的 ISOLATED/MODERN 标志，`dma_physical` 在隔离模式表示 IOVA，`device_io` 在 modern 模式表示用户 MMIO 基址。原 ABI 总长度仍为 96 字节，尚未冻结。

内核 VT-d 控制 MMIO 是 supervisor/NX，普通应用实际访问会页故障。越界 DMA 的硬件记录触发驱动复位、撤销、终止与永久设备隔离，测试再次创建拥有者也被内核拒绝。该范围不包括中断重映射、ACS、真实硬件和任意设备；不将 QEMU RAM DMA 负例称为完整物理设备安全证明。

## 构建与信任

构建支持 Linux 与 Windows 宿主，均使用 Zig 0.13.0 的 Clang/LLD；Linux 使用系统 qemu-system-x86_64 与 OVMF 固件，EFI stub 仍以 windows-gnu 目标产出 PE/COFF。UEFI 以 PE32+ 直接链接，EFI_STATUS/UINTN 使用明确 64 位类型，入口预留 MS ABI shadow space。内核仍以 ELF 作为固件内嵌中间产物。

内嵌文件使用按内容摘要命名的 `.incbin` 输入，防止编译缓存重复嵌入旧镜像。开发种子、备用种子、工具、缓存、共享库、镜像和日志均被 Git 忽略。

包签名使用 [Monocypher 4.0.3](https://monocypher.org/download/)，许可保留在 `vendor/monocypher/LICENCE.md`。签名验证不是 UEFI Secure Boot，也不构成完整硬件信任链。生产签名政策和密钥管理不在本原型范围内。
