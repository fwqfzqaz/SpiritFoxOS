//! 应用启动参数打包与命令行拆分，从 `services/argparse.c` 等价迁移。
//!
//! 两个核心能力：
//! - `sf_invocation_pack` / `sf_arguments_pack`：把 argv/env 打包进定长的
//!   [`SfArguments`]（偏移表 + 字符串区，全部写入受 512 字节上界约束）；
//! - `sf_arguments_split`：就地拆分带引号/转义的命令行，写回原缓冲。
//!
//! 全部边界判定与错误码与 C 实现逐位一致；环境变量名合法性检查在本模块
//! 内自带等价实现（见 [`environment_name_valid`]），保证归档自包含。

use crate::ffi::{
    SfArguments, SF_ARGUMENT_BYTES, SF_ARGUMENT_MAX, SF_BOOT_ARGS_SIZE, SF_ENVIRONMENT_MAX,
    SF_INVALID, SF_NO_SPACE,
};
use core::ffi::{c_char, c_int};

/// 环境变量名合法性检查，与保留 C 实现的 `sf_environment_name_valid`
/// （services/arguments.c）逐位一致：`[A-Za-z_][A-Za-z0-9_]*`，1–63 字节。
///
/// 静态库内自带实现使 sfcore 归档完全自包含——任何成员被任意镜像
/// 拉取都不会引入对 C 侧符号的依赖；C 侧函数仍供 arguments.c 自用，
/// 两侧行为由同一批宿主测试（test_environment）共同覆盖。
fn environment_name_valid(s: *const c_char, n: usize) -> bool {
    if n == 0 || n > 63 {
        return false;
    }
    unsafe {
        for i in 0..n {
            let c = *s.add(i) as u8;
            let letter = c == b'_' || c.is_ascii_uppercase() || c.is_ascii_lowercase();
            let digit = i > 0 && c.is_ascii_digit();
            if !letter && !digit {
                return false;
            }
        }
    }
    true
}

/// 小端写入 u32（与 C `put32` 一致）。
unsafe fn put32(p: *mut u8, v: u32) {
    for i in 0..4 {
        *p.add(i) = (v >> (i * 8)) as u8;
    }
}

/// 拆分参数/环境打包的公共入口。
///
/// 偏移表占 `data[0..total*4]`，偏移值以 [`SF_BOOT_ARGS_SIZE`]（96）为基准，
/// 保证打包结果可直接嵌入 `SfBootArgs` 的字符串区。
///
/// # Safety（C 调用方契约）
/// - `values`/`environment` 非空时必须指向 `count`/`env_count` 个合法
///   NUL 终止字符串指针；
/// - `out` 必须指向一个完整的 `SfArguments`。
#[no_mangle]
pub unsafe extern "C" fn sf_invocation_pack(
    out: *mut SfArguments,
    values: *const *const c_char,
    count: u32,
    environment: *const *const c_char,
    env_count: u32,
) -> c_int {
    if out.is_null() || (values.is_null() && count != 0) || (environment.is_null() && env_count != 0) {
        return SF_INVALID;
    }
    if count > SF_ARGUMENT_MAX as u32 || env_count > SF_ENVIRONMENT_MAX as u32 {
        return SF_NO_SPACE;
    }
    let total = count as usize + env_count as usize;
    let mut lengths = [0usize; SF_ARGUMENT_MAX + SF_ENVIRONMENT_MAX];
    let mut used = total * 4;
    // 第一遍：度量每个字符串长度并校验环境变量名与唯一性。
    for i in 0..total {
        let s = if i < count as usize {
            *values.add(i)
        } else {
            *environment.add(i - count as usize)
        };
        if s.is_null() {
            return SF_INVALID;
        }
        let room = (SF_ARGUMENT_BYTES - used) as usize;
        // 有界测长：n==room 即视为溢出（还需容纳 NUL 终止符）。
        let mut n = 0usize;
        while n < room && *s.add(n) != 0 {
            n += 1;
        }
        if n == room {
            return SF_NO_SPACE;
        }
        lengths[i] = n + 1;
        used += n + 1;
        if i >= count as usize {
            // 环境变量必须形如 NAME=VALUE，NAME 需合法且互不重复。
            let mut key = 0usize;
            while key < n && *s.add(key) as u8 != b'=' {
                key += 1;
            }
            if key == n || !environment_name_valid(s, key) {
                return SF_INVALID;
            }
            for j in 0..i - count as usize {
                let other = *environment.add(j);
                let mut k = 0usize;
                while *other.add(k) != 0 && *other.add(k) as u8 != b'=' {
                    k += 1;
                }
                // 长度相同且逐字节相等即重复。
                if k == key && key > 0 {
                    let mut equal = true;
                    for b in 0..key {
                        if *s.add(b) != *other.add(b) {
                            equal = false;
                            break;
                        }
                    }
                    if equal {
                        return SF_INVALID;
                    }
                }
            }
        }
    }
    // 第二遍：清零结构体，写偏移表与字符串。
    core::ptr::write_bytes(out as *mut u8, 0, core::mem::size_of::<SfArguments>());
    (*out).count = count;
    (*out).env_count = env_count;
    (*out).bytes = used as u32;
    let mut cursor = total * 4;
    for i in 0..total {
        let s = if i < count as usize {
            *values.add(i)
        } else {
            *environment.add(i - count as usize)
        };
        put32((*out).data.as_mut_ptr().add(i * 4), SF_BOOT_ARGS_SIZE + cursor as u32);
        core::ptr::copy_nonoverlapping(s as *const u8, (*out).data.as_mut_ptr().add(cursor), lengths[i]);
        cursor += lengths[i];
    }
    0
}

/// 仅打包位置参数（无环境变量）的便捷入口。
///
/// # Safety（C 调用方契约）
/// 同 [`sf_invocation_pack`]。
#[no_mangle]
pub unsafe extern "C" fn sf_arguments_pack(
    out: *mut SfArguments,
    values: *const *const c_char,
    count: u32,
) -> c_int {
    sf_invocation_pack(out, values, count, core::ptr::null(), 0)
}

/// 判断是否空白分隔符（空格或水平制表）。
fn space(c: c_char) -> bool {
    c == b' ' as c_char || c == b'\t' as c_char
}

/// 就地拆分命令行为参数数组，返回词数或负错误码。
///
/// 支持 `'单引号'`、`"双引号"` 与反斜杠转义（单引号内反斜杠为字面量）；
/// 拆分结果直接写回 `line` 缓冲，`values` 指向各词的就地首地址。
///
/// # Safety（C 调用方契约）
/// - `line` 必须指向 NUL 终止可写字符串；
/// - `values` 必须可写 `capacity` 个指针。
#[no_mangle]
pub unsafe extern "C" fn sf_arguments_split(
    line: *mut c_char,
    values: *mut *mut c_char,
    capacity: u32,
) -> c_int {
    if line.is_null()
        || values.is_null()
        || capacity == 0
        || capacity > (SF_ARGUMENT_MAX + SF_ENVIRONMENT_MAX + 2) as u32
    {
        return SF_INVALID;
    }
    let mut read = line;
    let mut write = line;
    let mut count: usize = 0;
    while *read != 0 {
        // 跳过前导空白；行尾即结束。
        while space(*read) {
            read = read.add(1);
        }
        if *read == 0 {
            break;
        }
        if count == capacity as usize {
            return SF_NO_SPACE;
        }
        *values.add(count) = write;
        count += 1;
        let mut quote: c_char = 0;
        while *read != 0 {
            let c = *read;
            read = read.add(1);
            if quote == 0 && space(c) {
                break;
            }
            // 反斜杠转义：单引号内不生效（与 C 语义一致）。
            if c == b'\\' as c_char && quote != b'\'' as c_char {
                if *read == 0 {
                    return SF_INVALID;
                }
                *write = *read;
                write = write.add(1);
                read = read.add(1);
                continue;
            }
            if c == b'\'' as c_char || c == b'"' as c_char {
                if quote == 0 {
                    quote = c;
                    continue;
                }
                if quote == c {
                    quote = 0;
                    continue;
                }
            }
            *write = c;
            write = write.add(1);
        }
        if quote != 0 {
            return SF_INVALID;
        }
        *write = 0;
        write = write.add(1);
    }
    count as c_int
}
