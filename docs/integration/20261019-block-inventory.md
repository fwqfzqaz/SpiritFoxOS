# 只读块设备清单对接

日期：2026-10-19，Asia/Shanghai。基于 b5e4d4d，继续 codex/devfs-services / PR #2。
开发前已 fetch，作者 main=414fd39，未合并；上一步 b5e4d4d 已经个人 Fork 推送，
最终快照 Ubuntu/Arch 本机 verify-ref 通过。本文不改作者分支。

## 实现与接口

新 dev.block 模块在早期建立空服务，不调用平台 API 或磁盘。AHCI/ramdisk 探测结束后、
VFS 前，核心将实际 blkdev_get(0..15) 的有效槽通过 REGISTER 登记，再 SEAL 冻结。
随后按 ENUM 与 GET 分别逐字节核对真实清单；成功后打印
`[sfmod] sealed block inventory matches platform devices`。

没有新增核心 API 表字段，ABI 仍为 1.4，包格式不变；新增协议头 sfmod_block.h。
模块清单增加第七个 dev.block，构建依赖包含新头，内核与全部模块应同树构建。

| 字段 | 类型/语义 |
|---|---|
| version、size | uint32，各为 1、64 |
| snapshot | uint64，登记输入为 0，读出为带代数端点句柄 |
| sector_count | uint64，驱动报告扇区数 |
| sector_size、flags | uint32；未确认几何标志为 bit0 |
| index、type | uint32；旧块槽号 0..15；AHCI=1、RAMDISK=2、IDE=3 |
| name、reserved | 16 字节含 NUL 名称（最多 15 字符），两个 uint32 保留项为 0 |

消息 flags=1、route=0；REGISTER=0x5000（size=64，handle/offset=0）、
SEAL=0x5001（空载荷、handle/offset=0）、ENUM=0x5002（offset=清单序号、handle=0）、
GET=0x5003（offset=旧槽号、handle=snapshot）。读响应 result=1、size=64、
handle=snapshot、offset=清单序号；ENUM 按旧槽号升序，空洞不占清单项。

未冻结查询 EAGAIN；ENUM 在 count 返回 result=0/size=0，超过 count EINVAL。
GET 不存在槽或失效快照 ENOENT，槽号越界 EINVAL。重复槽/名称 EEXIST，
重复冻结 EBUSY，冻结后登记 EROFS，未知操作 ENOSYS，版本错误 EPROTONOSUPPORT。
拒绝坏长度、路径式名称、未终止/非规范填充名称、保留位、未知类型/标志、
非 2 的幂或零扇区大小及乘积超 uint64 的容量。零扇区数允许表示空软件设备。

## 几何与范围

原 AHCI IDENTIFY 失败会默认填 2048 扇区；现有 blkdev_t 没有“是否探测成功”字段。
因此 AHCI/IDE 记录一律带 GEOMETRY_UNVERIFIED，保留驱动报告值供诊断，
不把它转成已验证容量或赋予读写权限。RAMDISK 大小来自软件缓冲区，无此标记。
不新增指针、配置写入、块读写、DMA/IRQ、热插拔或运行期更新接口。
原 /dev 块节点、blkdev_read/write、AHCI 和 FAT32 继续旧路径。

## 失败与生命周期

登记、查询使用模块固定 16 项数组，端点创建后不临时分配。核心先分配两个消息缓冲，
失败不发送登记；登记中途出现坏平台数据或协议错误，启动即 panic，不重试部分清单。
没有事务式登记回滚；卸载的标准 exit 清空状态，端点代数令旧快照失效。
这是可信 Ring 0 启动约定，不是调用者认证；未封存时不允许不可信代码运行。
后续驱动若新增块设备，须重新设计代数/引用/冻结策略，不能继续沿用启动快照假设。

## 验证与结果

运行 tools/dev.ps1 verify，提交后 pre-push 再执行 verify-ref；Linux 入口
bash tools/check-linux.sh。宿主 UBSan 验证真实模块的未冻结/冻结状态、坏版本/长度/
名称/索引/类型/保留位、重复、16 项满表、容量溢出、15 字符名、0/1 分配点 OOM，
登记/读取不依赖分配，以及空清单、稀疏槽、多盘、逐项核对和卸载旧句柄失效。
保留 PCI/AHCI、FAT32、统一设备、tmpfs、null/zero、PID、fork/wait、Wayland 检查。

2026-10-19 工作区主体已通过 Ubuntu GCC 13.3/NASM 2.16 与独立 Arch chroot
GCC 16.2/NASM 3.02 全量构建和各自产物 Windows QEMU TCG UEFI 启动。
verify 回执 build_exit_code=0、exit_code=0；两套均有块清单核对、两 SATA 盘/FAT32、
统一设备目录及 Wayland 10 帧。随后补充的不同槽同名拒绝和零扇区输入断言，由
最终提交 pre-push 再完整验证；最终 SHA/远端结果见 PR 和交付记录。
RAMDISK 的记录是软件声明的逻辑容量，不表示逐扇区内存后端已验证。
没有真机、长期负载或用户态隔离验收。下一步见 NEXT.md。
