# SpiritFoxOS 多核 SMP 实施计划

## Context

SpiritFoxOS 当前运行在单核模式下，但已具备部分 SMP 基础设施（smp.c/smp.h 中有 CPU 枚举、AP 启动、per-CPU 数据、自旋锁、IPI）。然而调度器、GDT/TSS、内存管理器等核心子系统仍基于单核假设（全局 current、全局 need_reschedule、单一 TSS、无锁 PMM/kmalloc）。本计划按五阶段增量改造，每阶段可独立测试，逐步将系统从单核升级到多核。

**已有 SMP 基础（可直接复用）：**
- CPU 枚举：smp_enumerate_cpus() 解析 MADT LAPIC 条目
- AP 启动：smp_start_aps() 执行 INIT-SIPI-SIPI，trampoline 在 0x8000
- AP 入口：ap_entry_c() 设置 GS base、初始化 LAPIC、标记在线
- Per-CPU 数据：cpu_local_t 通过 GS base 访问
- 自旋锁：票据锁实现（需修复中断标志保存）
- IPI：smp_send_ipi() / smp_broadcast_ipi()

**必须改造的单核假设：**
- 全局 process_t *current（process.c:38）
- 全局 int need_reschedule（process.c:39）
- 全局 proc_table[256] 无锁扫描（scheduler.c）
- 单一 tss_t tss（gdt.c:20），GDT 只有 7 项
- ISR 存根 mov gs, ax 破坏 per-CPU 语义（isr_stub.S:149,217）
- smp_init() 在 process_init() 之前调用（init.c:115 vs 224）
- PMM/kmalloc/slab/mmu 无锁

---

## 阶段一：Per-CPU 基础设施加固

### 1.1 修复 ISR 存根中 GS 段寄存器处理

文件：kernel/src/arch/x86_64/isr_stub.S

问题：isr_common(L149) 和 irq_common(L217) 执行 mov gs, ax（ax=0x10），返回路径 (L164, L230) 恢复 GS。与 syscall_entry(L394-400) 做法矛盾（那里注释"不设置 GS"）。

改动：在 isr_common、irq_common、spurious_common 的入口和返回路径中，删除所有 mov gs, ax 指令，仅设置/恢复 DS/ES/FS。

### 1.2 扩展 cpu_local_t 结构

文件：kernel/include/smp.h

新增字段：need_reschedule、*tss、tss_selector、*syscall_cpu_area、lapic_timer_count、runqueue_head/tail/count、runqueue_lock

### 1.3 添加 this_cpu() 内联函数

文件：kernel/include/smp.h

通过 hal_read_msr(MSR_IA32_GS_BASE) 返回 cpu_local_t 指针。

### 1.4 修复自旋锁中断标志保存

文件：kernel/include/smp.h, kernel/src/arch/x86_64/smp.c

spinlock_t 添加 saved_flags；acquire 保存，release 用 hal_restore_interrupts 恢复。

### 1.5 Per-CPU syscall 暂存区

文件：kernel/src/syscall/syscall.c, kernel/src/arch/x86_64/smp.c

BSP syscall_init() 和 AP ap_entry_c() 各自分配 syscall_cpu_area，设置 KERNEL_GS_BASE。

---

## 阶段二：Per-CPU TSS 与 GDT

### 2.1 设计决策：Per-CPU GDT

每个 CPU 独立 GDT（含自己的 TSS 条目），选择子值全局一致（0x28）。AP 启动阶段仍用 BSP GDT，在 ap_entry_c() 中切换。

### 2.2 数据结构改造

文件：kernel/include/gdt.h, kernel/src/arch/x86_64/gdt.c

新增 cpu_gdt_t（entries[7] + tss），全局数组 cpu_gdts[SMP_MAX_CPUS]。

### 2.3 实现 Per-CPU GDT API

文件：kernel/src/arch/x86_64/gdt.c

新增：gdt_init_cpu()、gdt_load_cpu()、gdt_set_tss_rsp0()。gdt_init() 改为调用 gdt_init_cpu(0) + gdt_load_cpu(0)。

### 2.4 修改所有 tss.rsp0 引用

通过 this_cpu()->tss 访问，替代全局 tss。关键位置：scheduler.c、process.c、process_user.c。

### 2.5 AP 加载 GDT 和 TSS

文件：kernel/src/arch/x86_64/smp.c — ap_entry_c() 中添加 gdt_init_cpu + gdt_load_cpu。

---

## 阶段三：Per-CPU 调度器改造

### 3.1 消除全局 current 指针

文件：kernel/src/proc/process.c, kernel/include/process.h

删除全局 current 变量，process_current() 返回 this_cpu()->current_process。全局搜索替换所有直接使用 current 的位置。

### 3.2 消除全局 need_reschedule

删除全局变量，need_reschedule_check() 读取 this_cpu()->need_reschedule。

### 3.3 在 process_t 中添加调度字段

cpu_id（-1=未运行）、cpu_affinity（0=任意）、next_in_queue（运行队列链接）。

### 3.4 重写调度器核心

文件：kernel/src/proc/scheduler.c

- enqueue_process/dequeue_process：加锁操作 per-CPU 运行队列
- scheduler_schedule()：本 CPU 队列取进程，空则继续当前进程（阶段五加偷取）
- scheduler_tick()：只操作本 CPU current，唤醒休眠进程加入对应 CPU 队列
- process_yield()/process_sleep()：使用 this_cpu()->need_reschedule

### 3.5 修改进程创建和退出

process_create_kthread/fork 加入当前 CPU 队列；process_exit 设 this_cpu()->need_reschedule；process_init 初始化 BSP 队列。

---

## 阶段四：LAPIC 定时器与 IPI 重调度

### 4.1 分配 IPI 向量

APIC_VECTOR_RESCHEDULE=48, APIC_VECTOR_TLB_SHOOTDOWN=49

### 4.2 在 IDT 中注册 IPI 处理程序

isr_stub.S 添加 irq16/irq17 存根；idt.c 注册向量 48/49；irq_handler 添加 case 48/49。

### 4.3 实现 LAPIC 定时器校准

lapic_timer_init(vector, freq_hz)：用 PIT 校准 LAPIC 频率，切换周期模式。

### 4.4 BSP 切换 LAPIC 定时器

timer_init() 末尾调用 lapic_timer_init()。

### 4.5 AP 启用 LAPIC 定时器

ap_entry_c() 中添加 lapic_timer_init()。

### 4.6 初始化顺序调整（关键！）

拆分 smp_init() 为 smp_enumerate_cpus()（apic_init 后）和 smp_start_aps()（process_init 后）。

### 4.7 AP 参与调度

ap_entry_c() 完整改造：GDT/TSS、LAPIC 定时器、syscall 暂存区、运行队列、idle 进程、scheduler_start()。

---

## 阶段五：内存管理加锁与负载均衡

### 5.1-5.5 全局锁

- PMM：pmm_lock 保护 alloc_page/free_page 等
- kmalloc：kmalloc_lock 保护 kmalloc/kfree
- slab：per-cache lock 保护 slab_alloc/slab_free
- MMU：mmu_lock 保护 mmu_walk_page(create=1)/mmu_map_page
- proc_table：proc_table_lock 保护 alloc_pid/fork/exit/kill/clone

### 5.6 偷取式负载均衡

try_steal_process()：遍历找最忙 CPU，从其队列头部偷取。

### 5.7 IPI 重调度触发

enqueue_process() 中目标 CPU 空闲时发送 RESCHEDULE IPI。

### 5.8 TLB 刷新 IPI

smp_flush_tlb_all()：本地刷新 + 广播 SHOOTDOWN IPI。替换所有 hal_flush_tlb()。

### 5.9 锁序约定

pmm_lock → mmu_lock → proc_table_lock → runqueue_lock → kmalloc_lock

---

## 验证方法

| 阶段 | 测试 | 验证内容 |
|------|------|---------|
| 一 | -smp 1 | 单核不变；this_cpu() 正确；syscall 正常 |
| 二 | -smp 2 | BSP/AP 独立 GDT/TSS；AP 仍空闲 |
| 三 | -smp 2 | BSP 调度正常；AP 仍空闲 |
| 四 | -smp 4 | AP 定时器中断；多线程跨核并行；用户进程正确 |
| 五 | -smp 4 | 全核调度；fork 安全；并发分配安全；TLB IPI 正常 |

---

## 关键文件清单

| 文件 | 阶段 | 改动概要 |
|------|------|---------|
| kernel/src/arch/x86_64/isr_stub.S | 一、四 | 删除 mov gs,ax；添加 IPI 存根 |
| kernel/include/smp.h | 一、三 | 扩展 cpu_local_t；this_cpu()；修复 spinlock |
| kernel/src/arch/x86_64/smp.c | 一、二、四 | 修复 spinlock；拆分 smp_init；AP 入口改造 |
| kernel/include/gdt.h | 二 | Per-CPU GDT/TSS 接口 |
| kernel/src/arch/x86_64/gdt.c | 二 | Per-CPU GDT/TSS 实现 |
| kernel/include/process.h | 三 | 添加 cpu_id/next_in_queue |
| kernel/src/proc/process.c | 三、五 | 消除全局 current；proc_table 加锁 |
| kernel/src/proc/scheduler.c | 三、五 | Per-CPU 运行队列；负载均衡 |
| kernel/src/arch/x86_64/idt.c | 四 | IPI 向量注册和处理 |
| kernel/include/apic.h | 四 | IPI 向量定义；LAPIC 定时器接口 |
| kernel/src/arch/x86_64/apic.c | 四 | LAPIC 定时器校准实现 |
| kernel/src/drivers/timer.c | 四 | BSP 切换 LAPIC 定时器 |
| kernel/src/mm/memory.c | 五 | PMM 加锁 |
| kernel/src/mm/kmalloc.c | 五 | kmalloc 加锁 |
| kernel/src/mm/slab.c | 五 | slab 加锁 |
| kernel/src/mm/mmu.c | 五 | MMU 加锁 |
| kernel/src/syscall/syscall.c | 一 | Per-CPU syscall 暂存区 |
| kernel/src/init.c | 四 | 初始化顺序调整 |
