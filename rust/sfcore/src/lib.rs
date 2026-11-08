//! SpiritFoxOS 核心纯逻辑组件（Rust 实现，C ABI 导出）。
//!
//! 本 crate 承载四个从 C 迁移而来的纯逻辑组件：
//! - [`sha256`]：SHA-256 摘要（FIPS 180-4）
//! - [`argparse`]：应用启动参数打包与命令行拆分
//! - [`net`]：ARP/IPv4/ICMP/UDP/TCP 有界帧构造与解析
//! - [`sched`]：0–3 级优先级调度策略（含等待老化）
//!
//! 设计约定：
//! - 全部函数与原 C 实现保持**逐位一致**的行为（含错误码与边界判定），
//!   既有 C 宿主测试直接回归 Rust 实现，保证零行为漂移；
//! - `#![no_std]`：不使用堆、不发起系统调用、不触碰任何设备状态；
//! - 恐慌策略为 abort，`panic_handler` 与内核停机语义一致（无限循环）。
#![no_std]
// ffi.rs 的常量完整镜像 C 头文件的 ABI 契约，部分成员当前无调用点，
// 保留它们以维持契约文档与编译期断言的完整性。
#![allow(dead_code)]

mod argparse;
mod ffi;
mod net;
mod sched;
mod sha256;

use core::panic::PanicInfo;

/// 恐慌处理函数：与 `sf_assert` 失败路径一致，直接停机自旋，
/// 不输出、不复位——由监督任务负责重启语义。
#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {
        core::hint::spin_loop();
    }
}

/// 预编译 core 的 panic 路径携带 unwind 表引用；本 crate 使用 panic=abort，
/// 人格局函数永远不会被调用（panic_handler 直接停机，不展开栈）。
/// 提供空实现以满足共享库的严格符号解析与 freestanding 链接。
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}

/// core 的切片相等性比较通过 `bcmp` 符号发出（POSIX 语义：相等返回 0）。
/// Windows CRT 与 freestanding 镜像均不提供该符号，由本 crate 补齐；
/// Linux 宿主场景会以强符号覆盖 libc 的弱引用版本，行为一致。
#[no_mangle]
pub extern "C" fn bcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    unsafe {
        for i in 0..n {
            let x = *a.add(i);
            let y = *b.add(i);
            if x != y {
                return if x < y { -1 } else { 1 };
            }
        }
    }
    0
}
