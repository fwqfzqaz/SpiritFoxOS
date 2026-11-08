//! 与 `include/sf.h`、`include/net.h`、`kernel/sched_policy.h` 对齐的 FFI 契约。
//!
//! 这里镜像的常量与 `#[repr(C)]` 结构体是跨语言 ABI 的一部分：
//! C 侧头文件带 `_Static_assert` 锁定布局，Rust 侧用编译期断言做同等校验，
//! 任何一侧改动不匹配都会在构建期失败。`abi/x86_64-current.json` 快照
//! 测试另在 CI 中锁定字段偏移。

use core::ffi::c_int;

// ---------- 通用边界（include/sf.h） ----------

/// 启动参数区起点约定：SfBootArgs 头部 96 字节之后是字符串表。
pub const SF_BOOT_ARGS_SIZE: u32 = 96;
/// 单任务最多 8 个位置参数。
pub const SF_ARGUMENT_MAX: usize = 8;
/// 最多 8 个环境变量。
pub const SF_ENVIRONMENT_MAX: usize = 8;
/// 参数字符串区总容量 512 字节（含偏移表与 NUL 终止符）。
pub const SF_ARGUMENT_BYTES: usize = 512;
/// 进程上限（`Task::main` 为 1 的槽位数量上限）。
pub const SF_MAX_TASKS: c_int = 12;
/// 线程槽总量（调度器遍历边界）。
pub const SF_MAX_THREADS: c_int = 32;
/// 单进程线程数上限。
pub const SF_THREAD_LIMIT_MAX: u32 = 8;
/// CPU 时间配额窗口（100 Hz 下 1 秒）。
pub const SF_CPU_WINDOW_TICKS: u32 = 100;

// ---------- 错误码（include/sf.h 枚举，负值） ----------

pub const SF_OK: c_int = 0;
pub const SF_INVALID: c_int = -1;
pub const SF_BAD_FORMAT: c_int = -3;
pub const SF_NO_SPACE: c_int = -18;

// ---------- 网络常量（include/net.h） ----------

pub const SF_NET_ARP: u16 = 0x0806;
pub const SF_NET_IPV4: u16 = 0x0800;
pub const SF_NET_ICMP: u8 = 1;
pub const SF_NET_TCP: u8 = 6;
pub const SF_NET_UDP: u8 = 17;
/// ICMP 回显使用的固定标识，便于跨重启识别本机流量。
pub const SF_NET_ICMP_ID: u16 = 0x5346;
/// 单帧上限：以太网 MTU 内的有界缓冲。
pub const SF_NET_FRAME_MAX: usize = 1536;
/// UDP/TCP 载荷上限：受 IPC 消息 256 字节约束。
pub const SF_NET_PAYLOAD_MAX: usize = 248;

// ---------- 调度策略常量（kernel/sched_policy.h / sf.h） ----------

pub const SF_PRIORITY_MIN: u32 = 0;
pub const SF_PRIORITY_MAX: u32 = 3;
/// 每等待 4 个时钟滴答提升一级有效优先级（老化）。
pub const SF_PRIORITY_AGING_TICKS: u64 = 4;

/// `SfArguments`（include/sf.h）：
/// 定长偏移表 + 字符串区，`data` 前 `count+env_count` 项是 u32 小端偏移，
/// 指向 `SF_BOOT_ARGS_SIZE` 起算的字符串位置。
#[repr(C)]
pub struct SfArguments {
    pub count: u32,
    pub bytes: u32,
    pub env_count: u32,
    pub reserved: u32,
    pub data: [u8; SF_ARGUMENT_BYTES],
}

/// `SfSchedEntry`（kernel/sched_policy.h）：单个任务的就绪状态与基础优先级。
#[repr(C)]
pub struct SfSchedEntry {
    pub ready_since: u64,
    pub priority: u32,
    pub ready: u32,
}

// 编译期布局校验：与 C 侧 `_Static_assert` 对齐。
const _: () = assert!(core::mem::size_of::<SfArguments>() == 16 + SF_ARGUMENT_BYTES);
const _: () = assert!(core::mem::size_of::<SfSchedEntry>() == 16);
