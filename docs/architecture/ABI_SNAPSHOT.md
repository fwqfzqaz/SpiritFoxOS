# 共享头文件 ABI 快照

`tools/abi.py` 采集和比较当前 x86_64 共享头文件的接口编号及结构布局，参考为 `abi/x86_64-current.json`。首个目录有 549 项数值，来源是实际编译的 `tools/abi_snapshot.c`，不是从 C 文本猜测布局。

句柄复制新增 `OP_DUP=25` 后当前目录为 550 项。该次已审阅差异只有这一项新增，原 549 项无删改；记录见对应代码提交和 TESTING。它展示了增加操作码时显式维护目录及参考的流程。

后续定位读取新增 `OP_PREAD=26`，当前为 551 项；该轮差异仅新增此常量，前 550 项保持。共享消息/BootArgs 大小和原有编号不变。

2026-11-12 工作树目录为 646 项：在已交付的 602 项上只明确追加 44 个 Root/Fs 私有
控制操作/回调及掩码常量。原布局、操作号与应用文件 RPC 保持。审阅逐项列出 200–227、
240–247 和七位掩码，记录在 `.cache/namespace-rpc-abi-review-20261112.json`；数值核对不
代替语义冻结、完整回归或最新提交托管检查。阴性编译漂移测试仍必须拒绝旧号、字段顺序
和符号类型改变；采集器先直接包含 sf.h，再包含间接共享头，确保替换头夹具确实生效。

目录遍历游标追加 `OP_ENUMERATE_AT=27` 与 `OP_NS_ENUMERATE_AT=228`，当前 662 项；旧
658 项无删改。两项均追加在既有操作码区间的尾部或空位，消息结构与原编号不变。

Linux 兼容上行调用追加 `SYS_COMPAT_SET=41`、`SYS_COMPAT_RET=42` 两个系统调用号，
当前 662 项，旧 660 项无删改；两者只使用既有 rdi/rsi/rdx 参数与 rax 返回值，
没有新增消息或结构体布局。

TLS/线程/futex 追加 `SYS_SET_FSBASE=43`、`SYS_FUTEX=44`、`SF_FUTEX_WAIT/SF_FUTEX_WAKE`
与 `SF_THREAD_FLAG_TLS/SF_THREAD_FLAG_COMPAT/SF_COMPAT_FRAME_BASE`，以及
`SfThreadRequest` 的 `tls/clear_tid` 偏移与宽度，共 11 项；当前 673 项，旧 662 项
无删改。`SfThreadRequest` 由 24 增至 40 字节，前 24 字节与 `entry/argument` 偏移不变，
`size==24` 的旧调用方继续按旧布局解释（`tls/clear_tid` 视为 0）。

## 采集与离线比较

在源码仓库根目录执行：

```bash
python3 tools/abi.py capture
python3 tools/abi.py check build/abi-current.json
python3 tools/abi.py diff abi/x86_64-current.json build/abi-current.json
```

`capture` 使用仓库内固定 Zig，经 `tools/build.py` 设置的工作区缓存/临时目录编译宿主探针，只执行打印元数据的程序。输出默认为 `build/abi-current.json`，可用 `--out` 指定已有父目录中的文件。它不会更新审阅参考，也不运行内核或服务。

`check` 默认与仓库参考比较；`--reference` 可选择另一份参考。`diff` 比较任意两份合法报告，输出按名称排序的 `added`、`removed`、`changed`；`changed` 含 before/after。相同布局返回 0，任何差异（包括新增项目）返回非零，留待审阅；它不自动判定新增项目向后兼容。

离线 `check` / `diff` 只需要 Python 和两份报告，不需要编译器、构建目录或签名密钥。发布 ZIP 同时提供脚本、审阅参考 `abi/x86_64-current.json` 与本次真实采集 `abi/x86_64-captured.json`。发布包中的离线脚本用于比较；重新采集需要源码和工具链。

已经审阅的常量新增可以单独核对。例如从新增撤回前的 562 项报告到当前报告：

```bash
python3 tools/abi.py check-additions before.json after.json --allow-add constant.SYS_SEND_CANCEL=22
```

`check-additions` 要求所有基线项目原值保留，实际新增项目与每个显式 `--allow-add constant.NAME=DECIMAL` 名称和值恰好一致。漏审、多审、旧项目修改/删除、重复审阅和数值不符均失败；不能用此选项批准布局修改或删除。无新增时不提供该选项即可核对相同报告。名称和整数使用有界规范十进制，不接受十六进制、前导零或 `-0`。

输出 `baseline_preserved` 和 `reviewed_additions_match`；两者均真才返回 0。它核对已明确审阅的数值增量，不自动判断操作码冲突、行为兼容、调用约定或安全性，也不修改文件。原 `check` / `diff` 继续拒绝所有差异，包括新增。

发布 ZIP 解压后可直接执行 `python3 tools/abi.py check abi/x86_64-captured.json`。

## 采集内容

- 已登记的系统调用、操作码、状态、能力权限、地址/容量约束、模块/应用信任用途，以及作者现有网络操作码。
- 15 种固定宽度布局，包括 IPC header/message、文件元数据、DMA 状态、BootArgs/BootInfo、加载计划、参数环境、native 计划、网络消息和公钥策略记录。
- 每个结构的 size/align，每个已登记成员的 offset/width，以及标量种类。

标量种类为 1:uint8、2:int8、3:uint16、4:int16、5:uint32、6:int32、7:uint64、8:int64；0 表示数组或复合成员，需结合其总字节宽度、子结构及目录审查。它不包含密钥内容或实例内存。C11 `_Generic` 按固定宽度 typedef 分类，不把宿主 `long` 的 Windows/Linux 差异当作共享结构变化。

报告格式是 `spiritfoxos-x86_64-abi-snapshot-v1`，根只有 `format` 和 `values`。输入最多 256 KiB，1–2048 个有界名称/整数项目；重复字段、布尔/小数、非法名称、未知格式和越界整数都拒绝。工具比较时不修改输入或审阅参考。

## 维护与边界

目录是显式登记的；新增枚举或结构需扩展探针。修改字段顺序、固定宽度类型或编号时先查看差异，再审查协议版本和调用者，最后通过单独可审阅的改动更新参考。不能为让检查通过而无条件覆盖参考。

这份快照包含当前仍在演进的内部装载结构，提供漂移检测，不宣告整个 ABI 冻结。数组成员的完整元素语义、系统调用寄存器/调用约定、权限与错误语义、超时取消、包/磁盘格式和行为兼容仍按各自协议验证。布局匹配只回答所登记的数值是否相同。

## 回归与发布门

`tests/integration/test_abi_snapshot.py` 先实际采集并对照参考，再用独立 little-endian struct 格式检查关键 wire 大小。它在隔离副本中验证无编译器的离线操作，并覆盖报告边界、重复键及增删改。

三项负例在工作区临时目录内编译经过修改的真实共享头文件：系统调用中间插入新编号、BootArgs 在保持总大小下交换 pid/manager、pid 在保持大小和偏移下改为有符号类型。三者都产生真实探针输出，并被离线检查定位；正式头文件保持原状。

统一测试入口记录参考和本次采集的摘要；发布门核对测试标记与两份摘要后才把报告和离线脚本纳入 ZIP。Windows/Linux 的固定宽度布局分别由相应 CI 核对，完整托管结果以当前提交为准。

基础优先级后续新增 SYS_TASK_PRIORITY=20 及六项策略常量，当前 558 项；前 551 项均无删改。系统调用参数为固定宽度寄存器值，没有新 wire 结构。

内核句柄上限新增 SYS_TASK_CAP_LIMIT=21 和三个常量，当前 562 项；前 558 项无删改，没有新增 wire 结构。

未投递 IPC 撤回仅追加 SYS_SEND_CANCEL=22，当前 563 项；前 562 项无删改，没有新增 wire 结构。

能力占用查询只新增 SF_LIMIT_USED=2，当前 564 项，前 563 项无删改；新增审阅命令已用于核对该差异，不更改 wire 布局。

私有池页面预算追加SYS_SPAWN_LIMITED=23、SYS_TASK_PAGE_LIMIT=24、SF_PAGE_LIMIT_MAX=4096，当前567项，旧564项无删改，SfLoadPlan等wire布局保持；三项新增已由显式审阅命令核对。

IPC复制计数追加SYS_IPC_STATS=25、SF_IPC_STATS_VERSION=1与SfIpcStats的size/align及四成员布局，共16项；当前583项，旧567项无删改。新结构固定24字节，<IIQQ，偏移0/4/8/16，不改变原消息结构。

SMP/线程/配额追加SYS_THREAD_CREATE=26、SYS_THREAD_LIMIT=27、SYS_TASK_CPU_BUDGET=28 与 SF_MAX_CPUS/SF_MAX_THREADS/SF_THREAD_LIMIT_MAX/SF_THREAD_STACK_BASE/SF_THREAD_STACK_BYTES/SF_CPU_WINDOW_TICKS 六项常量，以及 SfThreadRequest 的 size 与 entry/argument 各偏移/宽度，共14项；当前597项，旧583项无删改。新结构固定24字节，<IIQQ，偏移0/8/16，flags 位于偏移4，不改变既有消息与装载计划布局。

设备中断投递追加 SYS_IRQ_ACQUIRE=29、SYS_IRQ_RELEASE=30、SYS_IRQ_WAIT=31 三个常量，当前600项，旧597项无删改；系统调用参数沿用固定宽度寄存器值，没有新增 wire 结构。

匿名映射与共享内存追加 SYS_MMAP=35、SYS_MUNMAP=36、SYS_SHM_CREATE=37、SYS_SHM_MAP=38、SYS_SHM_GRANT=39、SYS_SHM_CLOSE=40 与 SF_HEAP/SF_HEAP_BYTES/SF_SHM/SF_SHM_BYTES/SF_SHM_SLOTS/SF_SHM_SLOT_BYTES 六个窗口常量，共12项；当前612项，旧600项无删改。同一批次把 SF_IMAGE_MAX 由 2 MiB 提至 8 MiB、SF_MSG_BYTES 由 256 提至 4096、SF_MAX_TASKS 12→32、SF_MAX_THREADS 32→64、SF_THREAD_LIMIT_MAX 8→32；SfMessage 布局随数据区扩至 4096 字节，其余成员偏移不变。

2026-11-13：Persist静默预检仅追加 `OP_PERSIST_QUIESCENT=104`，当前661项。
旧660项无删改；实际编译采集、check-additions只允许该常量及九组漂移阴性测试通过，
审阅 `.cache/persist-quiescence-abi-review-20261113.json`。消息大小、字段/编号和盘格式
保持；新请求不支持的旧peer应返回既有负错误，Root不能把它当成静默或停止确认。

2026-11-14整合草稿：基于作者含SYS_COMPAT_SET/RET的662项目录，只追加
OP_PERSIST_QUIESCENT=104至663，静态目录旧662项无删改。当前仅离线核对，实际
编译采集、漂移阴性、完整及新head托管尚待执行；不能沿用旧661项预检证明。


## 2026-11-15 当前作者 S4 上的 Persist 预检整合

以 ff5733c 的673项为基线，保留其全部键和值，仅新增
`constant.OP_PERSIST_QUIESCENT=104`，当前674项。作者S4自身已有
`size.SfThreadRequest:24→40`与11项新增；内核按size兼容接收24/40字节请求。
因此“仅追加”只描述本整合相对673项的差异，不声称旧662项所有值从未变化。
静态目录审阅、编译采集/九组比较及当前八个QEMU脚本已通过；新head完整CI/发布
仍须按PROGRESS独立核对，不能用专项结果代替。

2026-11-16 terminal 服务端原型：相对含104的674项目录，仅追加
`constant.OP_PERSIST_STOP=105` 至675。只读借用 Zig 实际编译 catalog，采集及
`check-additions` 核对旧674项逐值保持，并与当前参考完全匹配。作者 S4 的结构、
Linux 常量及其余编号无改动。本轮尚未跑九组完整漂移、生产构建或 E2；
契约与宿主证据边界见 [Persist terminal](../quality/PERSIST_TERMINAL.md)。


2026-11-17 空闲候选切片：对固定3e361d2的675项实际基线，仅追加
`constant.OP_PERSIST_CANDIDATE_PROBE=106` 和
`constant.OP_PERSIST_CANDIDATE_DISCARD=107` 至677项。实际 catalog 编译与
check-additions 已逐值证明旧675项不变，无结构/布局变化；完整宿主漂移与新提交CI
状态独立见 PROGRESS。106/107不表示激活或通用 restart，旧105准入检查不放宽。

2026-11-17 Persist 缓存快照：相对保留677项目录，仅追加
`constant.OP_PERSIST_SNAPSHOT=108`，当前678项；既有编号与结构布局保持。
108承载私有104字节版本化缓存描述，不登记为公开结构布局ABI。它用于核对
Persist缓存、卷标识、文件内容摘要及信任状态，不构成停止确认、执行许可或激活授权。
实现和证据边界见[Persist terminal](../quality/PERSIST_TERMINAL.md)。本专项构建、宿主
和访客结果通过，但完整本地回归及新head托管/发布尚未通过或确认；不可据此宣称全绿。

2026-11-18 Persist 候选卷只读重建：相对保留678项目录，仅追加
`constant.OP_PERSIST_CANDIDATE_VERIFY=109` 至679项。109通过私有 Root 代理重建候选侧
事务缓存，不新增公开结构布局。它依赖先前108快照及105 STOP的密封拥有者证明；不表示
候选激活或未来启动许可，详细边界见[Persist terminal](../quality/PERSIST_TERMINAL.md)。
专项E1/E2与完整宿主通过；完整本地回归及新head托管/发布仍待核对，不以专项代替全绿。


2026-11-18 独立分支撤销机制：相对679项目录，仅追加
`constant.SYS_CAP_REVOKE_BRANCH=45` 至680项。实际编译catalog与check-additions
逐值证明原679项无删改、无结构/布局变化。仅撤销调用者所持句柄及后代，不按端点
所有者身份扩大到整端点；旧SYS_CAP_REVOKE语义保持。详见[IPC](IPC.md)。
E1与实际ABI核对通过，后续E2/完整宿主/新head托管与原始产物独立记录于PROGRESS。
