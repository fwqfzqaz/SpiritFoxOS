# 早期 PCI 服务与 AHCI 控制器选择对接

日期：2026-10-19，Asia/Shanghai。

## 基线与接口

从 d6fc9ba 继续 codex/devfs-services；开发前已 fetch，作者 main=414fd39，
PR #2 开放未合并，沿用个人 Fork 与同一 PR。无作者分支写入或合并操作。

模块 ABI 仍为 1.4（0x00010004），PCI/设备协议 version=1，SDK/API 表和包格式不变。
新接口仅内核内部：sfm_boot_early、sfm_boot_filesystems、init_allocators，
以及 sfm_pci_find_ahci(record*)：1 为选中，0 为无可用记录，负数为错误。
旧 sfm_boot 合并入口删除；树外调用者应按下列阶段接入，不能只替换函数名。

## 接入顺序与失败处理

1. init_core：物理内存、MMU、原始 PCI 枚举；init_hardware：CPU/GS、APIC/时钟。
2. init_allocators 初始化 kmalloc/slab，之后不再重置。中断仍关闭。
3. sfm_boot_early 加载内嵌启动包、启动全部受信任模块、执行 PCI 快照自测。
   当前模块 init 不依赖磁盘、旧 VFS、进程；未来添加模块须重新核实这一限制。
4. AHCI 通过 dev.pci FIND(class=01/subclass=06/prog_if=01) 选取第一个可用 BAR5。
   之后沿用 MMIO 映射、端口识别、DMA/命令和块设备登记；ramdisk 初始化不变。
5. 根 VFS/FAT32、进程/syscall/系统服务就绪，再执行模块消息文件自测与 VFS 桥挂载。
6. input/DRM 就绪后统一登记并封存 /dev，开中断，保留 PID 预留槽回归和用户程序检查。

启动阶段状态检查拒绝重复加载、乱序挂载或失败后的重试；包失败由原 loader 逆序
回收端点/分配，早期或文件阶段失败则 panic 停机。兼容挂载没有事务回滚，不能宣称
挂载失败后可热恢复。所有模块仍在 Ring 0，不提供故障隔离。

## AHCI 边界

验证响应长度、版本、快照/端点、索引单调增长、BDF/类型及 AHCI class 三元组。
BAR5 仅接受非零、16 字节对齐、映射区间不越 32 位地址空间的 MEM32；拒绝 I/O、
64 位头/上半部、未知/非法标志。若大小已知，须覆盖驱动使用的 0x1100 范围；
当前 PCI 缓存没有大小探测，size=0/无 SIZE_KNOWN，0x1100 是原驱动映射跨度。
不可用 BAR 继续找下一匹配项；协议错误返回失败，不使用旧 PCI 扫描兜底。
未更改 PCI 配置写入、总线主控、IRQ、DMA、FAT32、分配或调度算法。

## 验证

执行 `powershell -File tools/dev.ps1 verify` 检查工作区；提交后由 pre-push 自动执行
`verify-ref <commit>` 验证最终快照。Linux 等价入口 `bash tools/check-linux.sh`。

新增宿主 UBSan 测试调用实际 AHCI 选择器，经真实端点注入无服务/空结果、OOM、
错误响应/陈旧快照、越界/不前进索引、坏 BDF/类、各类坏 BAR、跨界/对齐/大小边界，
验证跳过第一项后选中第二项和失败不改输出。保留 loader 的逐分配点 OOM/回滚测试。
UEFI 检查新增阶段顺序、每个模块仅一次 ready、分配器不晚于模块、AHCI 实际服务选择。
保留 AHCI/FAT32 程序加载、PCI/Bochs、统一 /dev、null/zero、tmpfs、fork/wait、PID、
Wayland 窗口与至少 10 帧，以及完成后 3 秒异常观察。

2026-10-19 工作区主体已通过 Ubuntu GCC 13.3/NASM 2.16、独立 Arch chroot
GCC 16.2/NASM 3.02 的完整构建和各自产物 Windows QEMU TCG UEFI 启动。
verify 回执 build_exit_code=0、exit_code=0；两套出现早期 PCI、AHCI 选择、
后期文件、设备及 Wayland 全部标记。Ubuntu 实测 BDF=0:3:0、BAR5=0x81010000，
两块 SATA 盘可识别，FAT32 挂在 /mnt。随后将两盘识别/FAT32 挂载加入显式必需标记，
已核对两套既有日志均包含它们；最终提交由推送钩子再完整覆盖。
最初测试夹具缺少 core owner enable，端点注册断言失败；补全测试夹具后重新全量通过。
Arch 是独立用户空间构建；未验证原生 Arch 内核、真机或长期负载。

## 下一步

见 [NEXT](../handoff/NEXT.md)：建立只读块设备清单服务及启动登记核对，
暂不把 DMA/IRQ 或块读写搬进模块。
