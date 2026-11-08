# 当前能力与剩余工作

更新日期：2026-11-07。当前是可启动的 x86_64 UEFI 微内核原型；各阶段仍有未完成项，阶段编号不代表整体完成率。用户说明 M7 已由作者完成，本聊天保留作者成果；本轮按用户要求补齐控制台账户与多用户会话（见 [SESSIONS.md](../architecture/SESSIONS.md)），其余非 M7 事项继续。

## 现在能做什么

UEFI 引导进入 Core 0 与隔离用户态，提供能力 IPC、抢占、进程/内存和浮点上下文隔离。用户态管理器可装卸模块、固定依赖代次并监督 echo/native；提供 RAM 文件/目录、受控只读目录委派和 virtio 磁盘访问。签名应用支持 argv、显式环境与受限位置无关装载；持久化原型提供笔记、一个应用的 A/B 更新、健康确认/失败回退、软件版本下限和密钥撤销。控制台支持构建期账户、四个有界多用户会话与 uid/gid 所有者权限，非 root 会话不能执行管理命令。

## 最近交付与验证

- 作者 d6a71f2 的 FP 环境、echo 监督和 Windows DLL 构建修复已随 PR #10 合入主分支；该提交 Windows/Linux CI 均成功。
- 文件客户端 SDK 已在 002ea46 / PR #11 交付第一轮：416510 项宿主断言，两次生产传输专项，完整 151 VM / 1086.09 秒通过；Windows/Linux 托管 CI 均成功。
- 后续旧事务银行兼容修复：对真实旧引擎生成的样本，修复“有效 generation 2 被误判而回退到空 generation 1”。旧四记录与当前五记录均严格校验；稳定只读挂载不改盘，普通提交原子迁移，并保留双槽/笔记/版本和第五撤销键。
- 包含上述两项的最终本地回归：1112.0 秒、157 次 VM，2046559 项兼容检查与 783 个首次迁移故障场景通过；七项交付摘要一致。Windows/Linux 托管检查必须按每次最新提交查看，不能沿用旧提交结果。

2026-10-31 的后续交付：M0 独立清单 schema、有界源文件/离线命令、共享头文件 ABI 采集和比较；M3/M4 的同所有者句柄复制、共享游标与最后引用回收。be5bda1 的四项 Windows/Linux 托管检查均成功，Windows PR 完整回归 1306.09 秒；本地完整含构建回归 1107.92 秒、158 VM 通过。

2026-11-07 的通用文件系统拆分：磁盘格式探测、SFRFS1/SFRFS2 解析与文件服务迁入独立 `sf-fs.sfmod`，sf-init 通过块代理服务其半块请求并保留 `/disk` 命名空间投影；新增 SFRFS2（≤48 条目、单文件 ≤1 MiB、镜像 ≤32 MiB、允许空文件与嵌套目录），SFRFS1 旧上限不变。宿主新增自动探测/SFRFS2 测试，`qemu_test.py --storage v2` 通过；装载暂存区扩为 1280 KiB。

最新已推送 031e360 补齐发布 ZIP 的离线清单工具依赖；实际解压副本在空 PATH、无 build/密钥/认证共享库条件下完成 12 项检查，运行时代码及七项产物保持 be5bda1。该提交的四项 Windows/Linux 检查也已全部成功。随后定位读取 OP_PREAD 已通过 198935 项宿主、9 组 ABI、四次专项 VM，以及包含初始构建的完整 1134.6 秒/158 VM；发布门和 12 项实际 ZIP 离线验证也已完成，定位读取已提交 5d7fc5a / PR #14，新 HEAD 托管检查仍独立核对。

- 2026-11-07：构建期账户表、四个有界控制台会话与 uid/gid 所有者权限完成。启动自动建立 root 会话保持旧提示兼容；`login/logout/whoami/users/sessions`、隐藏口令输入、`/home` 0700 隔离、服务端文件 RPC 身份强制及非 root 管理命令门均已实现。宿主 100 项新断言与 `qemu_user_test.py`（legacy/modern 各一次真实启动）通过，说明见 [SESSIONS.md](../architecture/SESSIONS.md)。

作者已合并 [PR #13](https://github.com/fwqfzqaz/SpiritFoxOS/pull/13) 为 main 9acc42e。当前工作 PR：[PR #14](https://github.com/fwqfzqaz/SpiritFoxOS/pull/14)。如果后续产生新 PR，以 [实施进度](PROGRESS.md) 和仓库实际记录为准。

## 还差什么

| 阶段 | 当前基础 | 主要缺项 |
| --- | --- | --- |
| M0 格式与 ABI | 容器、CBOR、签名、IPC/BootArgs；独立源 schema、离线源校验、真实共享头文件 ABI 目录和漂移比较 | ABI 语义冻结、更完整行为/格式兼容与清单扩展 |
| M1 内核 | 隔离、抢占、SMP（MADT/LAPIC/IPI，最多 8 核，本地 `-smp 2` 验证）、同进程多线程（共享 CR3）、根配置基础优先级/等待老化、句柄/页面/线程/CPU 时间预算配额、能力端点 | 优先级继承/上限、其他内存对象与 NUMA 配额、线程 join/返回值、x2APIC/热插拔/亲和性 |
| M2 服务管理 | 依赖、租约、受控重启及 echo/native 有界监督 | 完整生命周期 RPC、通用健康/挂起监督、取消、并发事务与状态迁移 |
| M3 文件与命名空间 | RAMFS、目录句柄、同所有者复制/共享偏移、元数据、只读委派和受控挂载、只读 `/proc` `/sys` 系统投影；定位读取本地完整通过 | 通用授权/撤销、进程挂载视图、符号链接、事件队列、可写 proc/sys 策略 |
| M4 应用与 SDK | native、argv/环境、受限 PI 与文件客户端 | 受限导入、完整 SDK、独立 sfexec/sf-process、异步/取消协议 |
| M5 存储设备 | 受限 legacy/modern virtio-blk、SFRFS1/SFRFS2 自动探测、独立 `sf-fs.sfmod` 文件服务 | 更多设备/传输、可写通用磁盘格式 |
| M6 持久化与安全 | 双银行、A/B、版本/签名/撤销及旧盘兼容 | 通用持久文件系统、IRQ/通用设备隔离、ACS/多单元、生产根、可信防回滚与真实断电验证 |
| M7 扩展系统 | 网络纵向切片：ring-3 virtio-net、IOAPIC INTx、DHCP、连接对象与 `/net` 投影、磁盘与网络并行授权；构建期账户、四个有界控制台会话、uid/gid 所有者权限与管理员命令门 | 图形、选定服务状态迁移 |

## 验证边界与下一步

当前事务卷仍是最多五个文件、每个上限 16 KiB 的原型，面向笔记和一个应用双槽；文件 SDK 不等于通用文件系统。native 支持单一认证运行时库的有界按名/ABI 导入和 15 项 POSIX 子集；没有通用动态库、多库依赖、TLS、惰性绑定或完整 POSIX/libc。PI 基址选择不是安全随机化。设备隔离仅覆盖已测试 Q35/双设备范围（磁盘与网络并行授权），QEMU 结果不替代真实硬件/IRQ/断电验收。

下一阶段按 [路线图](ROADMAP.md) 的 `v0.3` 方向执行：P0 完成 `sf-init` 剩余拆分（命名空间/RAMFS/`/disk` 投影下沉到 `sf-fs.sfmod`）；P1 在单一运行时库约束内扩展有界应用接口与 ABI 稳定性，并补齐命名空间、持久化、文件服务的生命周期 RPC 与故障注入；P2 再启动 virtio-input/帧缓冲纵向切片。每项均以宿主 + QEMU 回归、ABI 快照审阅和文档同步为完成门禁。实现边界见 [IMPLEMENTATION.md](../architecture/IMPLEMENTATION.md)，验证记录见 [TESTING.md](../quality/TESTING.md)。

M1 基础优先级后续：本地含初始构建完整 1187.54 秒/160 VM、1605642 项选择/控制和 10000 项 IPC 唤醒检查已通过；协议与限制见 [调度说明](../architecture/SCHEDULING.md)。正在完成发布门与正常推送，新 HEAD 托管检查单独核对；此前定位读取 97ae85a 已四项托管全绿。

内核句柄上限后续：本地含构建完整 1167.02 秒/162 VM、4014 项控制与 55043 项 IPC 准入检查已通过；见 [配额说明](../architecture/IPC.md)。发布及新 HEAD 托管检查独立核对，前一优先级 960b62c 已四项全绿。

M2 追加未投递 IPC 撤回：SYS_SEND_CANCEL=22 按实际发送者和完整编号稳定移除自己的排队条目，既有交付/副作用不撤销；见 [协议边界](../architecture/IPC.md)。本地含构建完整 1210.79 秒/164 VM 与 25776 项宿主检查通过，新 HEAD 托管检查独立核对。

根任务只读能力占用查询与 cap-limit used 显示已本地完整通过（1230.53 秒/164 VM）；旧 cc1db73 四项 Windows/Linux 已全绿，本轮新 HEAD 独立核对。查询快照不保证随后降低成功。

M1私有物理池页面预算本地完整通过：1192.6秒/166VM、298045项分配器与3033项core检查，默认4096页、不预留，借用物理页不重复计费，新页表计费。原清单映像内存声明保持。详见[页面配额](../architecture/SCHEDULING.md)；新HEAD托管结果独立核对。

原设计13.3的IPC复制字节计量已本地完整通过（1246.1秒/168VM）：根只读全局固定宽度饱和快照，独立复制事件/128位参照验证。它不表示延迟/吞吐或payload计数，见[计量协议](../architecture/IPC.md)。新HEAD托管结果另核对。

本轮任务槽复用内核栈清理已本地完整通过（1268.57秒/168VM、10022项创建/上下文检查），583项ABI保持，相关结论已并入 [调度与资源配额](../architecture/SCHEDULING.md)。

SMP、线程与配额本轮已实现：MADT 收集启用 LAPIC，AP 经 0x8000 trampoline 启动并建立每 CPU GDT/TSS/故障栈/空闲栈，IPI 抢占与 TLB 刷新接入全局 ticket 锁；`SYS_THREAD_CREATE` 创建共享 CR3 的同进程线程，`SYS_THREAD_LIMIT` 与 `SYS_TASK_CPU_BUDGET` 提供线程上限和 100 tick 窗口预算。`tests/qemu/qemu_smp_test.py` 已用 `-smp 1` 与 `-smp 2` 启动产品镜像验证启动、线程/预算 CLI 与关机；ABI 只追加 14 项至 597。x2APIC、CPU 热插拔、线程 join 未实现，真实硬件多核仍待独立验证。

有界图形切片已本地验证：固件 GOP framebuffer 经 PID 1 授权后由 ring-3 `sf-wm` 映射（RW+NX+缓存禁用），类 niri 布局核心（工作区/列/纵向堆叠/滚动）宿主 368 项检查通过，QEMU `gui` 测试断言初始布局、增删/拆列/焦点/工作区切换状态行、至少两个不同 framebuffer 校验和，并确认退出后串口 shell 仍可用。无真实输入驱动、GUI 客户端协议、GPU/DRM 与多显示器；串口/VNC 回退路径不变，详见 [GRAPHICS](../architecture/GRAPHICS.md)。
