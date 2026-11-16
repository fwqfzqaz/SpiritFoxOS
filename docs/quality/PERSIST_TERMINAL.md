# Persist terminal 服务端契约原型

2026-11-16：本轮先交付服务端 parser、生产接收循环及宿主 E1。Root 的围栏拥有者、
死亡确认和旧端点退役由后续整合提供；本页不把 ACK 当成死亡，也不宣称通用停止、
重启、候选就绪或激活完成。没有普通 stop CLI、裸 kill、104 停止复用或 bootstrap 例外。

## 105 请求与回复

`OP_PERSIST_STOP=105` 是唯一 ABI 追加；作者 S4 及原 674 个目录项目逐值保留，当前
675 项。请求必须来自 RECV 的实际 sender 1，并有完整 40 字节头、major 1/minor 0、
opcode 105、内核 flags 1、非零 uint64 request_id、cap_count 0、payload_bytes 4128。
status、handle、offset、length、mode 及全部 4096 字节 data 必须为零，不能靠截断
高位或 length 0 绕过。意外转移的一份能力在认证和解析之前关闭，不被返回给发送者。

deadline_ns 必须非零、严格晚于本次读取的 tick 时间（每 tick 10ms），且不超过
该时间后 3 秒；上限加法饱和。过期返回 TIMEOUT，过远或其他畸形字段返回 INVALID，
非 Root 实际发送者返回 DENIED。完整有效信封的 request_id 必须严格递增；
它在调用缓存 quiescent 检查前消耗，即使 BUSY 也不能重放该 nonce；旧 nonce 返回
STALE_HANDLE。畸形或外来信封不消耗 nonce。缓存检查的正常负错误保留，越界或
正状态转为 BAD_FORMAT。拒绝后服务继续接收普通请求。

接收循环在同一串行分派中执行缓存 `persist_quiescent`，返回 0 才进入 terminal。
此路径不挂载、不恢复 trial、不访问块设备、不修改未知 DIRTY、信任或卷状态。
回复头归一化为 40/1/0/105，cap_count 0，除 decision marker 外业务字段全零，回显原 uint64 nonce；
合法请求回显原 deadline。畸形或过期 deadline 的错误回复使用本次时间后最多 3 秒，
避免零 deadline 的无界 SEND。发送之前 flags 为 0，由内核填写真实提供者身份。

accepted 回复 status 0 且 mode 1；明确拒绝是负 status 且 mode 0。mode 来自独立的
terminal 决定，任何 status/mode 交叉不一致都不能让 Root 当成安全拒绝后取消围栏。
这里的 1 是105私有回复契约，没有新增 ABI 常量。
仅尝试一次有界 ACK 发送，随后从生产主循环正常返回 0，
无论 SEND 成功或失败都不再 RECV，也不再分派排队的普通写。这只定义提供者的退出
路径；Root 必须另外认证 ACK、核对围栏拥有者并确认精确旧 PID 的正常零退出。
丢失/坏 ACK 不能自动重放或恢复准入。104 始终只是原有 advisory preflight。

`persist.c` 把本地 `sf_send` 传给实际循环，保留三份 WRITE/trust/REVOKE 故障服务的
宏包装；新增终止 E2 也可通过同一发送回调注入故障，无须复制服务循环。

## 当前验证边界

实际生产 `persist_terminal.c` 与真实 metadata dispatcher 的宿主 E1 已通过 550 个
检查。传输、tick 和只读缓存结果在测试中明确替换；覆盖完整字段/宽值/尾部 data、
实际发送者伪造、BUSY 后重放、nonce 耗尽、deadline 边界、能力清理、原104时点变化、
拒绝后继续普通写、accepted 后排队写不再分派、ACK 发送失败仍退出、发送包装保留，
以及单字段 status 故障仍保留独立 accepted marker。
这不是内核 IPC、真实进程死亡或块 I/O 证据。
生产入口、helper 和三份旧故障服务另做了 freestanding 目标的单文件 `-c` 检查；
只借私有占位公共策略头核对编译/函数签名，不链接或执行，也不代表真实策略构建。

使用既有 Zig 只读编译 ABI catalog，采集 675 项；相对提交 381dc27 的实际目录仅新增
constant.OP_PERSIST_STOP=105，旧 674 项无删改。`check-additions` 和当前参考比较通过。
缓存证据位于该工作树 `.cache/terminal-e1/`。没有运行生产构建、签名、VM、完整宿主、
九组完整 ABI 漂移测试、E2 或托管 CI；整合后的这些验证必须独立执行。
