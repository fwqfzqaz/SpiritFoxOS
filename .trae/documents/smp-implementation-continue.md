# SpiritFoxOS SMP 多核实施计划（续）

## 当前状态

阶段一已完成：
- ✅ 1.1 ISR 存根 GS 段寄存器修复（isr_stub.S）
- ✅ 1.2 cpu_local_t 结构扩展（smp.h）
- ✅ 1.3 this_cpu() 内联函数（smp.h）
- ✅ 1.4 自旋锁中断标志保存修复（smp.h, smp.c）

阶段一剩余：
- 🔲 1.5 Per-CPU syscall 暂存区

阶段二~五均未开始。

---

## 阶段一：Per-CPU syscall 暂存区（Task 1.5）

### 问题分析

当前 `syscall_init()`（syscall.c:362-395）使用 `static void *syscall_cpu_area` 分配一个全局暂存区，写入 `MSR_IA32_KERNEL_GS_BASE`。多核环境下每个核心需要独立的暂存区。

syscall 入口点（isr_stub.S:336-338）通过 `swapgs` 交换 GS_BASE 和 KERNEL_GS_BASE，然后使用 `gs:0`（用户 RSP）、`gs:8`（内核 RSP）、`gs:16`（进程 PML4）、`gs:24`（内核 CR3）访问暂存区。

当前 cpu_local_t 中已有 `syscall_cpu_area` 字段，但未被使用。

### 实施方案

**文件：kernel/src/syscall/syscall.c**

1. 修改 `syscall_init()`：
   - 不再使用 `static void *syscall_cpu_area` 局部变量
   - 改为通过 `this_cpu()->syscall_cpu_area` 分配和访问
   - 分配暂存区后，写入 `cpu_locals[bsp_index].syscall_cpu_area`
   - 设置 `MSR_IA32_KERNEL_GS_BASE` 指向该暂存区
   - 需要 `#include "smp.h"` 和 `#include "memory.h"`

**文件：kernel/src/arch/x86_64/smp.c — ap_entry_c()**

2. 在 `ap_entry_c()` 中为 AP 分配 syscall 暂存区：
   - `alloc_page()` 分配一页
   - 写入 `local->syscall_cpu_area`
   - 初始化暂存区（清零、写入内核 CR3 到偏移 24）
   - 设置 `MSR_IA32_KERNEL_GS_BASE` 指向暂存区

**文件：kernel/src/proc/process_user.c — process_enter_user()**

3. 修改 `process_enter_user()` 中的 `gs_base_kernel` 获取方式：
   - 当前通过 `hal_read_msr(MSR_IA32_KERNEL_GS_BASE)` 获取
   - 改为通过 `this_cpu()->syscall_cpu_area` 获取（语义更清晰）
   - 需要 `#include "smp.h"`

### 具体改动

#### syscall.c — syscall_init()

```c
void syscall_init(void)
{
    /* 为 BSP 分配 Per-CPU syscall 暂存区。
     * 每个核心需要独立暂存区，因为 syscall 入口通过 swapgs + gs: 访问。
     * 布局：
     *   gs:0  = 保存的用户态 RSP
     *   gs:8  = 内核态 RSP
     *   gs:16 = 进程 PML4
     *   gs:24 = 内核 CR3 */
    cpu_local_t *cpu = this_cpu();
    if (!cpu->syscall_cpu_area) {
        cpu->syscall_cpu_area = alloc_page();
        if (cpu->syscall_cpu_area) {
            memset(cpu->syscall_cpu_area, 0, PAGE_SIZE);
            printf("[SYSCALL] BSP per-CPU area at %p\n", cpu->syscall_cpu_area);
        } else {
            printf("[SYSCALL] ERROR: failed to allocate per-CPU area!\n");
            return;
        }
    }
    hal_write_msr(MSR_IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)cpu->syscall_cpu_area);

    /* 保存内核 CR3 到 per-CPU 区域偏移 24 */
    uint64_t *cpu_area = (uint64_t *)cpu->syscall_cpu_area;
    cpu_area[3] = hal_read_cr3();   /* offset 24 = 内核 CR3 */

    /* 验证 */
    uint64_t verify = hal_read_msr(MSR_IA32_KERNEL_GS_BASE);
    printf("[SYSCALL] KERNEL_GS_BASE=%llx kernel_cr3=0x%llx\n",
           (unsigned long long)verify, (unsigned long long)hal_read_cr3());
}
```

#### smp.c — ap_entry_c() 新增段

在 `ap_entry_c()` 中，`hal_enable_interrupts()` 之前添加：

```c
    /* 为本 AP 分配 Per-CPU syscall 暂存区 */
    local->syscall_cpu_area = alloc_page();
    if (local->syscall_cpu_area) {
        memset(local->syscall_cpu_area, 0, PAGE_SIZE);
        uint64_t *cpu_area = (uint64_t *)local->syscall_cpu_area;
        cpu_area[3] = hal_read_cr3();   /* 内核 CR3 */
        hal_write_msr(MSR_IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)local->syscall_cpu_area);
        printf("[SMP] AP %u syscall_cpu_area at %p\n", my_apic_id, local->syscall_cpu_area);
    } else {
        printf("[SMP] WARNING: AP %u failed to allocate syscall_cpu_area\n", my_apic_id);
    }
```

#### process_user.c — process_enter_user()

将 `hal_read_msr(MSR_IA32_KERNEL_GS_BASE)` 替换为 `this_cpu()->syscall_cpu_area`：

```c
    /* Set up GS:8 (kernel RSP) and GS:16 (process PML4) for syscall entry point */
    {
        void *cpu_area_ptr = this_cpu()->syscall_cpu_area;
        if (cpu_area_ptr) {
            uint64_t kstack_top = (current && current->kernel_stack)
                ? (uint64_t)current->kernel_stack + (KERNEL_STACK_PAGES * PAGE_SIZE)
                : tss.rsp0;
            uint64_t *cpu_area = (uint64_t *)cpu_area_ptr;
            cpu_area[1] = kstack_top;          /* gs:8 = 内核栈顶 */
            cpu_area[2] = current->pml4;       /* gs:16 = 进程 PML4 */
        }
    }
```

> 注意：此处 `tss.rsp0` 的替换依赖阶段二（Per-CPU TSS）。在阶段一过渡期，保留对全局 `tss` 的引用。

---

## 阶段二：Per-CPU GDT/TSS

### 2.1 设计决策

每个 CPU 拥有独立 GDT（含自己的 TSS 描述符），选择子值全局一致（0x28）。AP 启动阶段仍用 BSP GDT（由 trampoline 加载），在 `ap_entry_c()` 中切换到自己的 GDT。

### 2.2 数据结构改造

**文件：kernel/include/gdt.h**

新增 `cpu_gdt_t` 结构和 Per-CPU GDT API：

```c
/* Per-CPU GDT 结构 */
typedef struct {
    uint8_t  entries[GDT_ENTRIES * 8];  /* GDT 条目（原始字节） */
    tss_t    tss;                       /* 本 CPU 的 TSS */
    uint16_t limit;                     /* GDTR 界限 */
    uint64_t base;                      /* GDTR 基地址 */
} cpu_gdt_t;

/* 全局 GDT 数组（索引 = CPU index） */
extern cpu_gdt_t cpu_gdts[];

/* Per-CPU GDT API */
void gdt_init(void);                          /* BSP 初始化（内部调用 gdt_init_cpu(0)） */
void gdt_init_cpu(int cpu_index);             /* 初始化指定 CPU 的 GDT/TSS */
void gdt_load_cpu(int cpu_index);             /* 加载指定 CPU 的 GDT 并装载 TR */
void gdt_set_tss_rsp0(int cpu_index, uint64_t rsp0);  /* 设置指定 CPU TSS 的 rsp0 */

/* 向后兼容宏 */
#define gdt_set_tss(rsp0) gdt_set_tss_rsp0(0, rsp0)
```

### 2.3 Per-CPU GDT API 实现

**文件：kernel/src/arch/x86_64/gdt.c**

重构为 Per-CPU 模式：

1. `cpu_gdt_t cpu_gdts[SMP_MAX_CPUS]` — 全局数组（需 `#include "smp.h"`）
2. 保留 `tss_t tss` 定义，使其地址等于 `&cpu_gdts[0].tss`（或直接 `#define tss cpu_gdts[0].tss`）
3. `gdt_init_cpu(int cpu_index)` — 初始化指定 CPU 的 GDT 条目和 TSS
4. `gdt_load_cpu(int cpu_index)` — lgdt + ltr + 重载段寄存器
5. `gdt_set_tss_rsp0(int cpu_index, uint64_t rsp0)` — 设置指定 CPU 的 TSS rsp0 并更新 GDT 描述符
6. `gdt_init()` — 调用 `gdt_init_cpu(0)` + `gdt_load_cpu(0)` + 设置 BSP cpu_local 的 tss/tss_selector

### 2.4 修改所有 tss.rsp0 引用

需修改的文件和位置：

| 文件 | 行 | 当前代码 | 改为 |
|------|------|----------|------|
| scheduler.c:205 | `tss.rsp0 = ...` | `gdt_set_tss_rsp0(this_cpu()->index, ...)` |
| scheduler.c:212 | `tss.rsp0 = 0x800000` | `gdt_set_tss_rsp0(this_cpu()->index, 0x800000)` |
| process.c:138 | `tss.rsp0 = 0` | `gdt_set_tss_rsp0(0, 0)` |
| process_user.c:474 | `tss.rsp0 = ...` | `gdt_set_tss_rsp0(this_cpu()->index, ...)` |
| process_user.c:538 | `tss.rsp0 = ...` | `gdt_set_tss_rsp0(this_cpu()->index, ...)` |

### 2.5 AP 加载 GDT 和 TSS

**文件：kernel/src/arch/x86_64/smp.c — ap_entry_c()**

在 LAPIC 初始化之后、标记在线之前添加：

```c
    /* 初始化本 AP 的 GDT 和 TSS */
    gdt_init_cpu(local->index);
    gdt_load_cpu(local->index);

    /* 设置 cpu_local 中的 TSS 指针和选择子 */
    local->tss = &cpu_gdts[local->index].tss;
    local->tss_selector = GDT_TSS_SEL;
```

BSP 的 GDT 初始化在 `gdt_init()` 中完成，同时在 `smp_start_aps()` 中设置 BSP 的 cpu_local tss 字段：

```c
    /* 设置 BSP 的 cpu_local TSS 信息 */
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_locals[i].bsp) {
            cpu_locals[i].tss = &cpu_gdts[0].tss;
            cpu_locals[i].tss_selector = GDT_TSS_SEL;
            break;
        }
    }
```

---

## 阶段三：Per-CPU 调度器改造

### 3.1 消除全局 current 指针

**文件：kernel/include/process.h**

1. 删除 `extern int need_reschedule;` 声明（process.h:192）
2. process_t 添加字段：
   ```c
   int         cpu_id;         /* 当前运行的 CPU index，-1 = 未运行 */
   int         cpu_affinity;   /* CPU 亲和性掩码，0 = 任意 */
   process_t  *next_in_queue;  /* 运行队列链接指针 */
   ```

**文件：kernel/src/proc/process.c**

1. 保留 `process_t *current` 全局变量定义，但添加注释标记为 deprecated
2. `process_current()` 改为 `this_cpu()->current_process`
3. 所有直接使用 `current` 的位置改为 `process_current()`

**文件：kernel/src/proc/scheduler.c**

1. 删除 `extern process_t *current` 和 `extern int need_reschedule`
2. 所有 `current` 引用改为 `this_cpu()->current_process`
3. 所有 `need_reschedule` 引用改为 `this_cpu()->need_reschedule`

### 3.2 消除全局 need_reschedule

- `need_reschedule_check()` 改为读取 `this_cpu()->need_reschedule`
- shell.c:815 的 `need_reschedule = 1` → `this_cpu()->need_reschedule = 1`

### 3.3 Per-CPU 运行队列

**文件：kernel/src/proc/scheduler.c**

重写调度核心：enqueue/dequeue 加锁操作本 CPU 运行队列；scheduler_schedule() 从本 CPU 队列取进程；scheduler_tick() 只操作本 CPU current。

关键改动：
- `enqueue_process(process_t *proc)` — spinlock_acquire(&cpu->runqueue_lock), 入队, spinlock_release
- `dequeue_process()` — 加锁出队
- `scheduler_schedule()` — dequeue_process() 获取新进程，旧进程 enqueue_process()，切换
- `scheduler_tick()` — 唤醒休眠进程加入本 CPU 队列，检查 need_reschedule
- 调度器切换时同步更新 TSS rsp0 和 syscall 暂存区 gs:8

### 3.4 修改进程创建和退出

- `process_create_kthread()`：新进程入队到当前 CPU 的运行队列
- `process_exit()`：设 `this_cpu()->need_reschedule = 1`
- `process_init()`：初始化 BSP 的 cpu_local runqueue，设置 BSP current_process

### 3.5 所有 `current` 引用迁移

全局搜索替换所有直接使用 `current` 的位置，改为 `process_current()` 或 `this_cpu()->current_process`。

关键文件（约 40+ 处改动）：
- process.c: 约 20+ 处
- scheduler.c: 约 10+ 处
- process_user.c: 3 处
- signal.c: 10+ 处
- futex.c: 3 处
- shell.c: 1 处

---

## 阶段四：LAPIC 定时器与 IPI 重调度

### 4.1 初始化顺序调整

**文件：kernel/src/init.c**

当前：`smp_init()` (L115) → `process_init()` (L224)

改为：
1. `init_hardware()` 中 `smp_init()` → `smp_enumerate_cpus_only()`
2. `init_services()` 中 `process_init()` 之后添加 `smp_start_aps()`
3. `smp_start_aps()` 末尾调用 BSP 的 `lapic_timer_init()`

### 4.2 LAPIC 定时器实现

**文件：kernel/src/arch/x86_64/apic.c**

新增 `lapic_timer_init(uint8_t vector, uint32_t freq_hz)`：
- 使用 PIT 作为参考校准 LAPIC 定时器频率
- 设置周期模式，vector = APIC_VECTOR_TIMER (32)
- 分频系数 0x0B（1/16）
- 校准方法：设置初始计数 0xFFFFFFFF，PIT 延时 10ms，读 CCR 算差值

### 4.3 IPI 向量注册

**文件：kernel/src/arch/x86_64/idt.c**

- 向量 48（APIC_VECTOR_RESCHEDULE）：设 `this_cpu()->need_reschedule = 1`
- 向量 49（APIC_VECTOR_TLB_SHOOTDOWN）：刷新本地 TLB（cr3 重载）
- 需要在 isr_stub.S 中添加 ISR 存根或复用现有 ISR_NOERR 宏

### 4.4 AP 启用 LAPIC 定时器

`ap_entry_c()` 中添加 `lapic_timer_init(APIC_VECTOR_TIMER, TIMER_HZ)`

### 4.5 定时器中断改造

BSP 启用 LAPIC 定时器后可停用 PIT 周期中断，保留 PIT 作为校准参考。

---

## 阶段五：全局锁 + 负载均衡 + TLB IPI

### 5.1 内存管理加锁

| 文件 | 共享资源 | 锁名称 |
|------|----------|--------|
| memory.c | PMM 物理页位图 | `pmm_lock` |
| mmu.c | 页表操作 | `mmu_lock` |
| kmalloc.c | 内核堆 | `kmalloc_lock` |
| slab.c | Slab 分配器 | `slab_lock` |

锁顺序约定：`pmm_lock → mmu_lock → proc_table_lock → runqueue_lock → kmalloc_lock`

### 5.2 proc_table 加锁

添加 `proc_table_lock` 保护 proc_table[] 的扫描和修改。

### 5.3 负载均衡（Work Stealing）

空闲 CPU 从繁忙 CPU 的 runqueue 偷取进程。在 scheduler_schedule() 队列为空时调用。

### 5.4 TLB 刷新 IPI

`smp_flush_tlb_all()` 已实现，确保 IPI 处理程序注册后可用。页表修改后调用。

---

## 验证步骤

### 阶段一验证
1. `make clean && make` 编译通过
2. QEMU 单核启动正常，shell 可用
3. syscall 调用正常（通过运行用户态程序验证）

### 阶段二验证
1. 编译通过
2. QEMU `-smp 1` 单核启动正常
3. QEMU `-smp 2` 双核启动，两个 CPU 均 online
4. 用户态进程正常切换（tss.rsp0 正确）

### 阶段三验证
1. 编译通过
2. 单核模式下调度正常（进程创建、切换、退出）
3. 双核模式下调度正常（Per-CPU 队列独立工作）
4. `process_current()` 在所有 CPU 上返回正确值

### 阶段四验证
1. LAPIC 定时器周期中断正常
2. 重调度 IPI 触发目标 CPU 重新调度
3. TLB IPI 刷新生效

### 阶段五验证
1. 多核并发分配内存不崩溃
2. 负载均衡使进程分布在多个 CPU 上
3. 长时间压力测试无死锁
