# 2026-10-19 对接：统一设备注册与目录视图

## 基线与范围

开始前 fetch upstream/origin。作者 main 仍为 414fd39，PR #2 未合并，继续从
d098b55 在 codex/devfs-services 追加。按 NEXT.md 和 DEVICE-REGISTRY 提案推进。
本轮实现启动期登记、封存、LOOKUP/ENUM 与公共目录枚举；不实现动态注销、
硬件 I/O 消息化、能力隔离或用户态驱动。仍属阶段 3。

## 接入顺序

1. 内核、打包器、全部模块一起编译，ABI 升到 **1.4 / 0x00010004**，旧包拒绝。
   新头 sfmod_device.h 定义设备协议 version=1；消息头仍 64 字节，文件 route 不变。
2. dev.registry 依赖 dev.tmpfs；fs.vfs 依赖 fs.tmpfs 和 dev.registry。
   route=1 的 `/dev` 现在到 registry，null/zero 的数据请求再转发 dev.tmpfs。
3. sfm_boot 仍先启动模块并验证文件/PCI；设备自测推迟到真实节点创建完成之后。
   init_input、init_drm 后调用 sfm_devices_boot：准备 null/zero，遍历实际旧 devfs，
   验证后端，登记真实节点，封存，再验证公共视图及设备 I/O。
4. 适配器使用有界队列，不递归。最多 64 个节点（含根）、8 层目录；路径至多
   255 字节，组件至多 63 字节。超限启动失败，不截断或悄悄漏掉设备。
5. 公共 inode 保留原驱动操作，只有目录 readdir 从 registry 取名字、类型和句柄。
   inode 编号绑定为 registry 句柄，权限保留并补齐文件类型位；原 dentry 树作为
   冻结路径缓存。启动时逐节点及逐目录核对名字、inode、数量，防止两份视图漂移。

## 协议

sfm_device_record_t 为 **328 字节**：版本/大小，handle、parent、backend_handle、
rdev、bytes，owner/type/mode/backend/flags/reserved，最后 256 字节路径。
字段固定宽度，path 偏移 72，有静态断言；保留字段与路径尾部归零。消息不传指针。

| 操作 | 输入 | 结果 |
| --- | --- | --- |
| DEV_REGISTER=0x4000 | flags=1，完整 record；handle/parent 输入为 0 | 仅封存前，返回节点句柄；父目录必须已登记 |
| DEV_SEAL=0x4001 | flags=1，无载荷 | 要求 null/zero 的正确后端已登记；成功后不可再改，重复返回 EBUSY |
| DEV_LOOKUP=0x4002 | flags=1，NUL 结尾的 `/dev/...` | 完整 record，result=1；缺失 ENOENT |
| DEV_ENUM=0x4003 | flags=1，offset=全表索引，无载荷 | record/result=1；等于节点数返回 0，越界 EINVAL |
| DEV_UNREGISTER=0x4004 | flags=1 | ENOSYS，尚未实现，不伪造成功 |

管理请求 route/handle 为 0，ENUM 以外 offset 为 0；错误版本 EPROTONOSUPPORT，
错误长度/字段/路径 EINVAL，未知 owner EPERM，重复 EEXIST，配额 ENOSPC。
拒绝空组件、点/双点、控制字符、空格及 DEL。LOOKUP/ENUM 在封存前返回 EAGAIN。
FS_ROOT 可供依赖初始化取得根；封存后的 FILE_OPEN/STAT/READDIR 使用同一目录表。
节点句柄包含完整端点代数、端点槽和节点索引，失效句柄不能作为新节点使用；
route 与句柄共同确定资源，编号不是安全能力。

owner=1 是可信启动适配器，owner=2 是 null/zero。后端为目录、dev.tmpfs、旧字符、
旧块和旧 console。null/zero 名称保留给正确特殊后端，注册时经 STAT 校验句柄及
设备号；适配器验证旧字符操作表和块设备存在，再导入。逻辑 owner 不是认证：
当前 Ring 0 注册调用方受信任，模块不能独立证明任意自报的旧硬件 ID 真实存在。
旧设备未知 rdev 保留 0，RDEV_KNOWN 不置位，不虚构 major/minor。

## 冻结、I/O 与生命周期

封存后公共 /dev 禁止 create/mkdir/symlink/unlink 和动态节点增删，卸载返回 EBUSY。
旧 void 块设备增删入口封存后记录不支持并不执行；chardev_create_node 返回 EROFS。
直接操作原字符驱动表不是注册服务的热插拔接口，本轮未提供运行期替换协议。

null/zero 的 read/write/seek/retain/release 经 fs.vfs -> registry -> dev.tmpfs。
端点禁止重入，服务复用转发缓冲，使最后 close/release 不依赖分配；dup/fork 仍共享
文件描述。硬件 read/write/ioctl/poll/mmap 保留旧 VFS 驱动，registry 的消息 I/O 对
这些节点返回 EOPNOTSUPP；目录可枚举不等于硬件驱动已迁移。旧块设备 devfs 读写
原有的不支持行为也未在本轮补全。

挂载期间没有卸载服务/动态注销入口，不据此宣称支持驱动重启或热拔插。

## 验证

Windows tools/dev.ps1 verify；最终提交 pre-push 使用 verify-ref。
Linux bash tools/check-linux.sh。宿主 UBSan 覆盖版本/长度/owner/坏路径/重复、
长名字、容量、注册 OOM 后重试、依赖卸载和失效句柄；内存分配失败时 release 仍成功。
内核导入后核对每个真实节点和公共枚举，用户程序核对 /dev、input、dri 的唯一名称、
inode/类型，拒绝创建空节点或删除硬件节点，并检查 zero 在 dup/fork 后继续有效。
保留 PCI/Bochs、16 次 fork/wait、原 tmpfs、Wayland 10 帧及观察期。

2026-10-19 工作区主体已通过 Ubuntu GCC 13.3/NASM 2.16 和独立 Arch 用户空间
GCC 16.2/NASM 3.02 的全量构建及各自产物的 Windows QEMU TCG UEFI 启动。
两套均出现目录一致、用户态统一设备目录、特殊设备及 Wayland 10 帧标记，
verify 收据 build_exit_code=0、exit_code=0。随后补充的路径边界、注册分配点故障和
无分配 release 测试在干净源码导出的宿主 UBSan 程序通过，进程退出码为 0。
最终提交由 pre-push 再完整构建/启动；收据与远端 CI 为最终依据。
Arch 为 Ubuntu VM 中的独立 chroot，未验证原生 Arch 内核、真机或长期负载。
