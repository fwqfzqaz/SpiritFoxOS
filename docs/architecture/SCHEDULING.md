# 任务、线程、调度与资源配额

本文件合并原调度说明、CPU 上下文边界与私有池页面配额三份说明，覆盖任务/线程选择、浮点上下文隔离和每任务内存准入。

## 任务、线程与 SMP 调度

内核同时支持多 CPU（MADT/LAPIC，`SF_MAX_CPUS=8`）与同进程多线程（`SF_MAX_THREADS=32`，进程上限仍为 `SF_MAX_TASKS=12`）。调度仍为 100 Hz 抢占、0–3 基础优先级与每四 tick 等待老化，但就绪集合、选中任务和 CPU 归属都带核索引：每个 CPU 在全局内核锁下独立扫描任务表并切换上下文。

### 线程模型

任务槽同时承载线程。创建任务时首线程 `tid=1` 且标记 `main=1`，承载进程级状态（CR3、PID、能力表、页/能力/线程/CPU 配额）；`SF_THREAD_CREATE` 创建的同进程线程共享主线程的 CR3 与 PID，各自拥有 16 KiB 内核栈和独立的 FXSAVE64 状态，用户栈固定映射在 `SF_THREAD_STACK_BASE(0x40300000)` 起每线程 64 KiB 的槽位。线程数上限为每进程 `SF_THREAD_LIMIT_MAX=8`（含主线程），槽位位图防止复用未回收的栈空间。线程独立参与优先级老化与 CPU 预算，但配额、能力和页计数始终记在主线程上。

`smp_preempt_all()` 只在在线 CPU 数大于 1 时发送 LAPIC 广播 IPI（vector 33）；BSP 的 PIT 仍只在自己的核上推进 `ticks` 并调用预算窗口重置，AP 由 IPI 进入调度。所有 CPU 的调度、任务表修改、能力的授予/回收都在全局 ticket 自旋锁 `kernel_lock` 内完成；获取锁先 `cli`，因此临界区不可被同核中断重入。

### 选择与唤醒

调度器从当前任务之后环形扫描 READY 集合，选择有效优先级最高的任务；有效优先级相同按环形顺序轮转。每等待 4 个 tick 提升一个有效级别，上限为 3，基础级别不变。刚运行的任务先清除等待时间；成功选择后开始新的等待区间。新任务在全部映射/授权完成、实际发布 READY 时记录时间，创建耗时不兑换为等待信用。IPC 直接交付、接收期限到期及端点撤销唤醒都把等待起点设为当前 tick，阻塞时间不兑换为 READY 老化。

任务只有在 `state==T_READY`、`cpu==-1`（未被其他核占用）且主线程 CPU 预算未耗尽时才可被选中；选中后把该任务标记为当前核运行（`cpu=ci`）。一个任务同一时刻只能在一个 CPU 上运行，这是线程安全与内核栈独享的前提。

### CPU 时间预算

`SYS_TASK_CPU_BUDGET=28` 仅允许 PID 1 调用，参数 `(pid, limit, operation)`：

- `SF_LIMIT_QUERY=0`：返回当前预算上限，0 表示不限。
- `SF_LIMIT_SET=1`：`limit` 为 0–`SF_CPU_WINDOW_TICKS(100)`；低于已用返回 `SF_BUSY`，但 `0`（取消限制）等价于抬高上限，不因已用大于 0 而拒绝。
- `SF_LIMIT_USED=2`：返回当前窗口内已用 tick 数。

已用计数在每次本核调度/中断记账时递增（`cpu_account`），只统计主线程预算非零的任务；每 `SF_CPU_WINDOW_TICKS` 个 tick 由 BSP 统一清零所有主线程的已用计数，预算耗尽的进程在本窗口内暂不可调度，其余任务继续运行。

### 空闲与上下文切换

被换出的任务不再使用自己的内核栈等待：`schedule()` 在无就绪任务时调用 `enter_idle`，先切到每 CPU 独立的 16 KiB 空闲栈，再切换到内核地址空间（`space_ap_cr3`，含 LAPIC MMIO 映射），清除被换出任务的 `cpu` 归属，释放全局锁，最后在 `sti; hlt` 循环中等待；每次唤醒重新加锁并调度。这样另一 CPU 可以立即复用原任务的内核栈，也不会在已回收的进程页表上执行。

进程回收（`SYS_KILL`/退出）先标记全部线程 `T_DEAD`，向其他 CPU 发送 IPI 抢占，并等待其 `cpu` 归属清零后再释放共享页表与线程栈；当前核若正在运行该进程则先切回内核页表。

### 配置

受信任串口 shell 提供：

```text
threads 1            # 查询进程线程上限/已用
threads 1 4          # 设置上限
cpu-budget 1         # 查询 CPU 预算
cpu-budget 1 60      # 设置窗口内 tick 上限
threads-test         # 创建/回收线程并检查配额与预算
```

`priority` 命令保持原有语义。没有新增 wire 结构：线程请求复用既有系统调用编号 26–28，`SfThreadRequest` 为 24 字节（size/flags/entry/argument），ABI 快照只追加。

### 验证与限制

`test_scheduler`、`test_sched_ipc` 覆盖原有选择/老化/唤醒路径；`test_threads` 用实际 `task_new`/`SYS_THREAD_CREATE` 路径覆盖线程创建、共享地址空间、配额拒绝与预算过滤。`qemu_priority_test.py` 在真实 VM 验证优先级；`qemu_smp_test.py` 分别以 `-smp 1` 与 `-smp 2` 启动产品镜像，检查 `SMP: cpus=2`、线程/预算命令输出与关机路径。

尚未实现：线程 join/等待返回值、x2APIC、CPU 热插拔与亲和性绑定；每 CPU 预算窗口只在 BSP 推进，严格硬实时与优先级继承不在当前范围。原有固定期限 RPC 的等待会受新调度影响，协议没有自动延长期限。

## CPU 上下文边界

Core 0 的每个执行流（任务主线程或同进程线程）拥有私有、16 字节对齐的 512 字节 FXSAVE64 状态。该状态是内核内存，不接受用户提供的保存/恢复指针；同进程线程共享地址空间但各自保存浮点上下文。

### 已复现的问题

在 c565c40 的原镜像中，两个正确签名的普通应用先后运行：第一个向 XMM0 写入哨兵并 yield/退出，第二个读出相同值。legacy 和 VT-d/modern 两个实际 VM 均复现，见 build/cpu-state-baseline.json 及对应串口日志。六项原发布摘要和磁盘字节保持，测试没有修改生产镜像或增加特权接口。此为进程间寄存器数据泄漏，不是仅根据缺少代码作出的推断。

### 实现

初始化要求 CPUID 的 x87、FXSR、SSE 和 SSE2；CR0 保留 WP，设置 MP/NE，清除 EM/TS。CR4 设置 OSFXSR/OSXMMEXCPT，CPU 宣告时同时启用 SMEP 与 SMAP，明确清除 OSXSAVE；EFER 清除 FFXSR，确保 CPL0 不省略 XMM 的保存/恢复。内核 RAM 访问走恒等映射，SMAP 不需要在用户窗口临时置 AC。只支持 x87、MMX、XMM0–15 和 MXCSR；AVX/AVX-512、XSAVE 扩展、AMX 和完整 TLS 不在当前配置内。CPU 宣告 AVX 硬件不代表本操作系统启用了 AVX。

每次从用户态进入中断/系统调用后，在可能结束或切换任务前以 FXSAVE64 保存当前任务。调度选中下一任务、更新 CR3 后，先以无等待 FNINIT 和固定零指针 FLDENV 环境清理旧状态，再以 FXRSTOR64 恢复其私有状态；初次进入同样恢复。内核 C 继续禁用编译器 SSE/MMX，内核 IRQ 路径不使用浮点，保存操作不等待未屏蔽的 x87 异常。用户的待处理异常由其后续用户指令触发，不能在其他任务中继承。

创建或复用任务槽时清空整个状态，显式写入 x87 控制字 0x037f 和 MXCSR 0x1f80。物理 x87 数据、XMM、状态/标签、指令/数据指针均从零开始。仅执行 FNINIT 不会清除 x87 数据寄存器，也不会初始化 XMM/MXCSR，因此不能单独用来隔离新进程。上下文从用户硬件状态保存，恢复时不存在用户可写的内核状态映像。

### 多核上下文（SMP）

BSP 在 `smp_init` 中先建立 `cpus[0]` 记录、启用 LAPIC 并把 `GS_BASE`（MSR `0xC0000101`）指向本核 `Cpu`，`cpu_index()` 由此读取核索引，不依赖共享全局。AP 通过 0x8000 的 16→32→64 位 trampoline 进入 `ap_entry_c`，建立自己的 GDT/TSS/故障栈后置 `online`；每 CPU 的 GDT 槽位固定（0x08 代码、0x10 数据、0x28 TSS），TSS 的 `rsp0` 在 `arch_task` 切换时指向该任务的内核栈。IDT 全核共享，定时器中断仍只走 BSP 的 8259/PIT，AP 通过 LAPIC IPI（vector 33/34）参与抢占与 TLB 刷新。

切换上下文时 `arch_task` 更新 `rsp0`、CR3、FNINIT/FLDENV/FXRSTOR，再 `iretq` 进入用户帧。空闲等待不继续使用刚换出任务的内核栈：每 CPU 有独立 16 KiB 空闲栈，进入前先切到内核地址空间，因此 LAPIC MMIO 页始终可写，进程页表回收不会影响空闲核。LAPIC 基址固定为 `0xFEE00000`，在启动页表、AP 页表和每个任务空间的 3–4 GiB MMIO 窗口中都建立映射；AP 的 LINT0 屏蔽，BSP 保持 ExtINT。

SMP 机制的当前边界：x2APIC、CPU 热插拔、每核独立定时器、中断亲和性与跨核实时保证未实现；验证范围是 `-smp 1` 与 `-smp 2` 的 QEMU q35 TCG。

### 验证范围

最终产品完整回归通过（1074.85秒、143VM），日志build/kernel-cpu-isolation-final-test.txt，全部既有回归与六项摘要一致。tests/host/test_task_pid.c 在实际 task_new 路径增加一千次带脏状态的 DEAD 槽复用，检查完整初始映像及对齐，新增 3000 项断言。services/fpprobe.c 检查初始物理寄存器、指针与控制值；按 PID 生成不同的 x87/MMX/XMM0–15 和舍入控制，经过 yield、系统调用与不主动让出的 TSC 循环后检查保持。

cpu-state-test 诊断会启动两个实际同时存活的应用。专项 VM 使用宣告 AVX/XSAVE 的 max CPU，同时要求 OSXSAVE 未启用，验证 AVX、x87 和跨 yield 的待处理 x87 异常仅终止原应用，以及超时终止后槽复用的清零。该工具用于当前 QEMU 平台回归，不代表所有物理 CPU、微架构侧信道已经验证；多核路径另由 tests/qemu/qemu_smp_test.py 在 -smp 2 下验证。

部分 CPU/模拟器对 ES=0 的异常指针采用省略行为，单独 FXRSTOR 不能作为新任务指针清理手段。零 FLDENV 不执行会记录内核操作数地址的浮点运算；探针以 FNSTENV 再检查新任务的实际指令/数据指针。当前 TCG 专项不能证明非零 FOP/FIP/FDP 的完整恢复，不能据此宣称覆盖所有物理 CPU 的异常指针语义。

机制参考：[Intel SDM Volume 2A，FINIT/FNINIT、FXSAVE、FXRSTOR](https://cdrdv2.intel.com/v1/dl/getContent/671199)；FINIT/FNINIT 说明物理数据不变，FXSAVE64 表规定 512 字节布局和 16 字节对齐，FXRSTOR64 恢复 64 位指针与全部 XMM。更广的硬件支持须继续独立验证。

实际专项迭代中，随仓库提供的 QEMU 11.0.92 TCG 对未屏蔽的 SSE 0/0 设置 MXCSR.IE，未交付 #XM。测试区分真实 vector 19 与 status-only 两条结果，将各传输观察写入 fpu-context-results.json 和总结果；status-only 路径额外检查 0x1f01 跨 yield 保持，不将其记为已通过 SIMD 异常交付。Core 已启用 OSXMMEXCPT 并沿用一般用户异常隔离入口，真实硬件 #XM 仍待独立验证。

FFXSR 依据 [AMD64 System Programming，EFER bit 14](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf)，其启用时 CPL0 的 FXSAVE/FXRSTOR 会省略 XMM。当前明确关闭这项优化。

## 私有物理池页面配额

Core 0 为每个任务计数现有物理分配池中由该 PID 拥有的 4 KiB 页面。全池仍为 4096 页；所有新建及复用任务默认上限 4096，不继承父任务或旧任务配置。限制是准入上限，不是预留：其他任务占满全池时，即使尚未达到自身上限，分配也会失败。

计费包括私有映像、用户栈、参数页和新分配的页表页；借用的 bundle/设备映射不按其映射字节重复计费，但为映射分配的页表仍计费。静态内核存储、内嵌内核栈、固定 DMA arena 和其他内存对象不属于这个池计数。它不等于清单 `resource_limits.memory` 的映像字节约定，也不会自动将该声明转成此配额。

### 创建与管理

`SYS_SPAWN_LIMITED=23` 参数为 `(load_plan, pages, reserved)`。仅 PID 1 可调用；pages 必须为完整规范整数 0–4096，reserved 必须为 0。SDK 为 `sf_spawn_limited(plan, pages)`。它复用原 `SfLoadPlan` 与验证流程，在分配任何子任务页面前安装预算，没有新的 wire 结构。原 `SYS_SPAWN` 保持默认上限和调用约定。

合法但不足的预算返回 NO_SPACE；构造失败会释放已经分配的页表/私有页、能力和设备引用。失败仍消耗一个单调 PID，不回收编号或绕过 PID 耗尽保护。0 上限合法，但不能完成需要页面的任务创建。当前管理器的普通应用启动仍走默认策略；新接口供根进程服务显式选择预算，不扩张普通应用的创建权限。

`SYS_TASK_PAGE_LIMIT=24` 参数为 `(pid, pages, operation)`，仅 PID 1 可管理仍为 READY/BLOCKED 的规范 PID 1–INT32_MAX：

- `SF_LIMIT_QUERY=0`：pages 必须为 0，返回配置上限。
- `SF_LIMIT_SET=1`：设置并返回 0–4096 的上限；低于已拥有数量返回 BUSY，配置和映射保持。
- `SF_LIMIT_USED=2`：pages 必须为 0，返回当前拥有数量。

SDK 为 `sf_task_page_limit` 和 `sf_task_page_usage`。无效参数/高位别名返回 INVALID，死亡/未知任务返回 NOT_FOUND；普通应用无论参数是否有效都返回 DENIED。降低不驱逐或解除既有映射，提高不预留页面。`page-limit <pid> [0..4096]` 显示 limit 与 used，两次读取不是原子快照，后续降低仍以 SET 结果为准。

### 计数与回收边界

分配按准确 PID 查找任务，允许正在构造的 EMPTY 和活任务，拒绝零/未知/死亡拥有者；只有实际分配成功才增加计数。达到任务预算或全池耗尽的失败不改变页面拥有者和计数。释放按实际池拥有者清零页面，并将该 PID 的计数归零；任务死亡先标为 DEAD 的原有顺序仍可正确回收。

一次地址空间构造可能成功分配中间页表后在后续分配失败。这些页面仍计费，并由整个失败任务的回滚统一释放；这里不宣称每次失败 `space_map` 都完全不分配，也没有新增一般用户映射接口。单核串行准入不能当作多核计数/回收证明。

### 验证状态

新增宿主测试使用实际分配器：298045 项检查，含 1000 组独立预算/交错轨迹、全池和单任务耗尽隔离、页面唯一性/清零、页表计费、借用映射和死亡/编号复用。另有 3033 项实际 core 控制/预算创建检查，含 1000 组已用/预算组合、权限/参数宽度、BUSY、创建前准入、失败回滚与默认复用；原 42 项内存与 8022 项创建检查保持。

legacy/modern 专项验证有界失败创建后仍可成功创建，真实私有页/页表占用、降低到 used 与 BUSY、非合作任务抢占、死亡回收、默认复用、普通应用拒绝和文件/磁盘字节保持。当前执行状态以 PROGRESS/TESTING 为准；未完成的专项或完整回归不作为通过发布。
