#ifndef SF_SCHED_POLICY_H
#define SF_SCHED_POLICY_H
#include "sf.h"
/* 单任务就绪状态（Rust 侧 rust/sfcore/src/ffi.rs 以 #[repr(C)] 镜像此布局，
 * 两侧均为 16 字节；改动必须同步并通过 ABI 快照测试）。 */
typedef struct {uint64_t ready_since;uint32_t priority,ready;} SfSchedEntry;
/* 调度策略（Rust 实现，见 docs/architecture/LANGUAGE.md）：
 * 从 current 之后环形遍历，选有效优先级（基础优先级 + 每 4 滴答 +1 的等待
 * 老化，封顶 3）最高的就绪任务；同级因遍历起点固定而轮转。
 * 无就绪任务或入参非法（空表/超界数量/越界 current）返回 -1。 */
int sf_sched_pick(const SfSchedEntry*,unsigned,int,uint64_t);
#endif
