//! 调度策略：0–3 基础优先级 + 等待老化 + 同级轮转。
//!
//! 从 `kernel/sched_policy.c` 等价迁移。策略是纯函数：给定任务就绪表
//! 与当前滴答数，选出下一个应运行的任务；不持有状态、不触碰任务表本体，
//! 便于在宿主测试中以百万级随机序列回归。

use crate::ffi::{SfSchedEntry, SF_INVALID, SF_MAX_THREADS, SF_PRIORITY_AGING_TICKS, SF_PRIORITY_MAX};
use core::ffi::c_int;

/// 从 `current` 之后开始环形遍历，挑选有效优先级最高的就绪任务。
///
/// 等待时间每满 [`SF_PRIORITY_AGING_TICKS`] 滴答提升一级有效优先级，
/// 封顶于 [`SF_PRIORITY_MAX`]；同级任务因遍历起点固定在 `current` 之后
/// 而天然形成轮转。
///
/// 返回选中的任务下标；入参非法或无就绪任务时返回 `-1`（SF_INVALID）。
///
/// # Safety（C 调用方契约）
/// `entries` 为空时必须 `count == 0`；否则可读 `count` 个 `SfSchedEntry`。
#[no_mangle]
pub unsafe extern "C" fn sf_sched_pick(
    entries: *const SfSchedEntry,
    count: u32,
    current: c_int,
    now: u64,
) -> c_int {
    // 与 C 一致：空表、超界数量、越界 current 都直接判无效。
    if entries.is_null()
        || count == 0
        || count as c_int > SF_MAX_THREADS
        || current < -1
        || current >= count as c_int
    {
        return SF_INVALID;
    }
    let mut selected: c_int = -1;
    let mut best: u32 = 0;
    for step in 1..=count {
        let i = (current + step as c_int) % count as c_int;
        let e = &*entries.add(i as usize);
        if e.ready == 0 || e.priority > SF_PRIORITY_MAX {
            continue;
        }
        // 等待老化：当前滴答早于就绪时刻按 0 处理（时钟回退安全）。
        let age = if now >= e.ready_since { now - e.ready_since } else { 0 };
        let boost = age / SF_PRIORITY_AGING_TICKS;
        let score = if boost >= (SF_PRIORITY_MAX - e.priority) as u64 {
            SF_PRIORITY_MAX
        } else {
            e.priority + boost as u32
        };
        // 严格大于保证同级按轮转顺序选中。
        if selected < 0 || score > best {
            selected = i;
            best = score;
        }
    }
    selected
}
