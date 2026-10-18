# 2026-10-19 对接：特殊字符设备服务

## 基线与范围

开发前已 fetch upstream/origin，从作者 main `414fd39` 建立 `codex/devfs-services`。
作者已合并上一轮 PR #1。此前未找到专门的对接/交接文件，按 ARCHITECTURE.md
阶段 3 的 devfs 顺序继续；本目录和 `../handoff/NEXT.md` 从本次作为交付入口。

本次将 `/dev/null`、`/dev/zero` 的实际策略迁入新模块 `dev.tmpfs`，经 fs.vfs
消息供普通 Linux ABI 程序使用。阶段 3 尚未整体完成；PCI/AHCI/DRM/evdev/tty
仍在原内核，Ring 3 驱动隔离尚未实现。

## 接口与接入顺序

1. SDK、打包器、全部模块一起重编译。ABI 提升到 **1.2 / 0x00010002**，拒绝旧包。
   消息头仍为 64 字节，载荷至多 4096 字节；偏移 56 的 reserved 改成 uint64_t route。
   0=tmpfs，1=devfs，其他值由 fs.vfs 拒绝。
2. 文件身份为 `(route, handle)`，handle 只在后端内有效；route 是固定后端编号，
   不是端点、指针或安全能力。响应返回实际 route，兼容桥在 inode.fs_data 保存编号。
3. SFM_DEPENDS 支持多个名称。fs.vfs 依赖 fs.tmpfs、dev.tmpfs，按依赖顺序启动；
   清单次序不是启动次序。设备模块仍受依赖卸载检查约束。
4. 固定启动挂载表保留三个 tmpfs 根，增加 `/dev` 到设备后端。绝对 OPEN/MKDIR
   按路径组件匹配；相对及其他句柄请求按 route 转发。FS_ROOT 仅供后端初始化，
   fs.vfs 不对外转发。没有动态挂载协议。
5. sfmodfs 在启动时绑定旧 devfs 的 null/zero 两个节点，保留其他设备。
   绑定发生在 chardev_init 前，使用 inode 回调，避免驱动注册表清零影响服务。
   UINT32_MAX chardev_id 表示没有旧分发槽，不能传给原 ioctl/mmap 分发器。

## 行为与边界

| 项目 | 当前实现 |
| --- | --- |
| 类型/设备号 | S_IFCHR，null=1:3、zero=1:5，size=0 |
| null read/write | 立即 EOF / 丢弃数据并返回接收长度 |
| zero read/write | 返回零数据 / 丢弃数据 |
| 单次传输 | 最多 4096 字节，允许短读写，调用方循环 |
| SEEK | whence=0..2 返回位置 0，其他值 EINVAL；有符号偏移原样传输 |
| 生命周期 | 独立 open RETAIN，dup/fork 共用描述，最后 close RELEASE |
| 名字空间 | 固定 null/zero；CREATE/REMOVE/MKDIR/SYMLINK 被拒绝 |
| 公共 unlink | 旧 devfs 回调拒绝删除模块设备节点 |
| ioctl/mmap | ENOTTY / ENODEV；mmap 在保留虚拟地址前失败 |

服务直接枚举 `/dev` 只有 null/zero；普通程序通过旧 VFS 枚举还看见 input、dri、
块设备等。这是迁移期两种视图，尚无统一设备注册协议或运行期热卸载/替换。

启动器在父进程缺少标准描述时，为 stdin 补 null，为 stdout/stderr 补 console。
write/writev 对启动控制台保留串口输出，主动 dup2 到其他节点时走 VFS，
显式继承的管道和重定向仍然有效。这样应用重定向到 null 不会泄漏到串口。

语义参考 Linux [drivers/char/mem.c](https://github.com/torvalds/linux/blob/master/drivers/char/mem.c)。
这里只实现上述子集；Linux 的 zero mmap 尚未实现，本版明确返回 ENODEV，
不以匿名页的成功返回掩盖缺少设备映射。poll、完整权限、IRQ/DMA 隔离也未实现。

## 验证

Linux：`bash tools/check-linux.sh`；Windows：`tools/dev.ps1 verify` 验证工作区。
pre-push 用 `tools/dev.ps1 verify-ref <commit>` 对最终提交重建 Ubuntu/Arch
产物并分别启动。日志在被忽略的 build/linux、build/arch，不随源码提交。

测试覆盖宿主 UBSan、旧 ABI 拒绝、依赖/OOM 回滚、设备枚举/引用/错误路由、
4 KiB 零数据和只读名字空间。启动的用户程序验证 stat、短读写、lseek、dup/close、
访问模式、unlink 保护、ioctl/mmap 错误和标准输出/错误 write/writev 重定向。
串口出现 `[sfmod-user] redirect leaked` 必须失败；原 tmpfs、16 次 fork/wait、
Wayland 10 帧及观察期检查继续执行。

2026-10-19 工作区实测通过：Ubuntu GCC 13.3/NASM 2.16 与独立 Arch 用户空间
GCC 16.2/NASM 3.02 分别完成全量构建、宿主测试和 Windows QEMU TCG UEFI
启动。两套串口均出现设备自测/用户态设备通过、原文件与 fork/wait、Wayland 帧标记，
未出现重定向泄漏或异常。verify 收据 build_exit_code=0、exit_code=0。
Arch 在 Ubuntu VM 内 chroot 构建，并非原生 Arch 内核；真机和长期压力未验证。
最终提交的 pre-push 会重新验证，远端 CI 以对应 PR/Actions 记录为准。

## 后续协作

先同步远端，读本目录和 NEXT.md。每次提交前同时更新对接文档和 NEXT.md，
写明基线、接口、接入、实际验证、限制与下一步验收；pre-commit 检查两个路径
均有暂存改动。`bash tools/enable-hooks.sh` 启用；代码推个人 Fork，PR 交作者审阅，
不直接推送作者分支或自行合并。
