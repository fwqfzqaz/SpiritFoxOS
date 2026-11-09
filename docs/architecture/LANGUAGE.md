# 语言分工：Rust 与 C

SpiritFoxOS 采用 **C + Rust 双语言**实现。本页说明语言分工的原则、当前边界、
构建集成方式与跨语言契约（FFI），以及如何新增 Rust 组件。

## 1. 分工原则

| 维度 | 语言 | 理由 |
| --- | --- | --- |
| 内核机制（页表、IPC、设备、ACPI/VT-d） | C | 与 entry.S 中断/上下文切换、链接脚本、特权级指令紧耦合，规模小且已被 ABI 快照锁定 |
| 驱动与服务的 IPC/装载/存储边界 | C | 大量结构化 I/O 与能力交互，保持微内核既有实现稳定 |
| **纯逻辑、无副作用、有严格边界检查**的组件 | Rust | 无堆、无系统调用、无设备状态的纯函数；用 Rust 的所有权与切片模型消除越界/截断类缺陷 |

判断标准：**一个组件适合迁入 Rust，当且仅当它的全部输入输出都是有界缓冲、
错误码与纯计算**，且其 C ABI 符号可以被静态归档成员粒度地替换。

## 2. 当前已迁入 Rust 的组件

Rust 实现位于 [`rust/sfcore/`](../../rust/sfcore/)（crate `sfcore`，`#![no_std]`，
`crate-type = ["staticlib"]`，零第三方依赖）。导出符号与原 C 实现完全同名同 ABI：

| Rust 模块 | 导出符号 | 原 C 文件（已删除） | 说明 |
| --- | --- | --- | --- |
| `src/sha256.rs` | `sf_sha256` | `services/sha256.c` | SHA-256（FIPS 180-4），供 SFK 包哈希、事务盘封条、密钥指纹 |
| `src/argparse.rs` | `sf_invocation_pack`、`sf_arguments_pack`、`sf_arguments_split` | `services/argparse.c` | argv/env 打包与命令行就地拆分（引号/转义） |
| `src/net.rs` | `sf_net_checksum`、`sf_net_udp_checksum`、`sf_net_tcp_checksum`、`sf_net_parse_ipv4`、`sf_net_format_ipv4`、`sf_net_build_arp_request`、`sf_net_parse_arp_reply`、`sf_net_build_icmp_echo`、`sf_net_parse_icmp_reply`、`sf_net_build_udp`、`sf_net_parse_udp`、`sf_net_build_tcp`、`sf_net_parse_tcp` | `services/net_proto.c` | ARP/IPv4/ICMP/UDP/TCP 有界帧构造与解析、伪头校验和 |
| `src/sched.rs` | `sf_sched_pick` | `kernel/sched_policy.c` | 0–3 基础优先级 + 等待老化 + 同级轮转的调度策略 |

其余组件（arguments.c 的启动参数校验、trust/package 的 CBOR 与 Ed25519 验签、
事务存储、装载器等）**保持 C 实现**，不迁入 Rust。

## 3. 构建集成

`tools/build.py` 通过 cargo 把 `sfcore` 编译为**三个目标变体**的静态归档
`libsfcore.a`，由 zig cc 在链接期按需拉取成员：

| 变体 | Rust 目标 | 消费方 |
| --- | --- | --- |
| `none` | `x86_64-unknown-none` | 内核镜像、全部 ring-3 服务镜像（freestanding ELF） |
| `host` | `x86_64-unknown-linux-gnu`（Linux）/ `x86_64-pc-windows-gnu`（Windows） | `sfcrypto` 宿主动态库（sfpack 打包工具经 ctypes 调用）、宿主测试 |
| `efi` | `x86_64-pc-windows-gnu` | `BOOTX64.EFI`（PE/COFF，Linux/Windows CI 产物一致） |

要点：

- `rust_lib(kind)` **始终调用 cargo**（未变更时增量构建近乎零开销），避免
  源码更新被陈旧产物掩盖；产物位于 `build/rust/<target>/release/`，随 `make clean` 清理。
- Cargo profile 仅设 `opt-level = "z"`；**禁止 `lto = true` 与 `codegen-units = 1`**——
  它们会把整个 crate 合并为单个目标文件，破坏归档成员粒度（拉取任一符号
  即拖入全部代码，并在宿主动态库中引入对 C 侧符号的依赖）。
- panic 策略为 abort（`.cargo/config.toml` 按目标设定）；宿主/efi 变体额外启用
  `relocation-model = "pic"` 以并入共享库。
- 工具链要求：rustup + `x86_64-unknown-none`、`x86_64-pc-windows-gnu`，Linux 另加
  `x86_64-unknown-linux-gnu`。`make check-toolchain` 只检查 Cargo 入口是否存在，
  不检查已安装目标；完整 CI 配置与目标命令见 [CI 配置](../quality/CI.md)。
- Windows GNU 归档在链接前复制为 `libsfcore-zig.a`，仅移除与 Zig compiler-rt
  冲突的 Rust 栈探测成员，保留 Cargo 原件及其他内建函数。按归档索引识别成员，
  不依赖 rustc 的哈希/编号；不关闭栈探测，不全局允许重复符号。

## 4. 跨语言契约（FFI）

1. **结构体镜像**：`rust/sfcore/src/ffi.rs` 以 `#[repr(C)]` 镜像 `include/sf.h` 的
   `SfArguments`、`SfSchedEntry` 与相关常量，并用编译期断言锁定大小；C 侧头文件
   本身带 `_Static_assert`，两侧任一改动不匹配都在构建期失败。结构体布局另受
   [`ABI_SNAPSHOT.md`](ABI_SNAPSHOT.md) 的快照测试约束。
2. **符号替换**：Rust 导出函数与被替换的 C 函数同名同签名（`#[no_mangle]
   pub extern "C"`），调用方头文件与调用点零改动。
3. **自包含归档**：sfcore 不得引用 C 侧符号。环境变量名合法性检查在
   argparse.rs 内自带与 `sf_environment_name_valid`（保留 C 实现）逐位一致的
   私有实现；`bcmp`、`rust_eh_personality` 两个链接器 shim 由 lib.rs 补齐
   （前者是 core 切片相等性比较所需且 Windows CRT/freestanding 均不提供，
   后者在 panic=abort 下永不调用）。`memcpy`/`memset`/`memcmp` 由
   `services/memory.c` 提供，Rust 侧不得重复定义。
4. **行为逐位一致**：迁移以"零行为漂移"为验收标准——错误码、边界判定、
   回绕语义（校验和用 `wrapping_*`）、字节序全部与 C 一致。C 宿主测试
   （`test_arguments`、`test_environment`、`test_net`、`test_scheduler`、
   `test_txn*`、`test_update`、`test_trust` 等）直接改链 Rust 宿主变体
   静态库，同一套百万级随机检查回归 Rust 实现；`tools/test.py` 中被替换的
   `.c` 编译行已同步移除。

## 5. 如何新增 Rust 组件

1. 在 `rust/sfcore/src/` 新建模块，用 `#[repr(C)]` 镜像涉及的结构体并加
   编译期断言；导出函数与 C 头文件声明逐一同名同签名。
2. 在 `src/lib.rs` 挂载模块；若函数使用切片/索引，确认 panic 路径只会命中
   `panic_handler`（停机），不引入新的链接器依赖。
3. 在 `tools/build.py` 从对应 `objects(...)` 编译清单移除原 `.c` 文件
   （链接无需改动：`link_elf` 已统一追加 `none` 变体归档）。
4. 在 `tools/test.py` 的对应 `host_test(...)` 行移除该 `.c` 并确认
   `rust_lib('host')`（`host_test` 已统一追加）。
5. 运行 `make build && make test-host && make test` 全量回归；
   更新本页的组件表格与受影响的架构文档。

## 6. 当前边界与已知取舍

- Rust 侧不使用堆（无 `alloc`）、不发起系统调用、不触碰设备状态；
  需要这些能力的组件留在 C。
- `sf_environment_name_valid` 在 C/Rust 两侧各有一份等价实现，由
  `test_environment` 共同覆盖；若修改其一必须同步另一侧。
- `panic_handler` 停机自旋与 `sf_assert` 失败路径语义一致；宿主测试二进制
  中若 Rust 触发 panic 会挂起而非退出，属预期调试信号。
- QEMU/CI 的 QEMU 集成测试与 ABI 快照不受本次迁移影响：ABI 快照只锁定
  结构体布局与系统调用常量，函数实现语言不在快照范围内。
