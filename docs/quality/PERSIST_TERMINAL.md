# Persist 内部终止契约与拥有者验证

2026-11-17：服务端 parser、生产接收循环和 Root 拥有者已完成初次组合验证：
只有认证接受、准确旧 PID 的正常零退出与原端点成功退役才报告关闭终态。
这不是通用停止/重启、候选就绪或激活；没有普通 stop CLI、裸 kill、104 停止复用或
bootstrap 例外。管理员诊断只用于隔离测试启动，不能把已停止实例重新开放。

## 105 请求与回复

终止原语本身仅追加 `OP_PERSIST_STOP=105`，对应675项基线；作者 S4 及原674项
逐值保留。后续空闲候选协议追加106/107，当前677项，见下节。请求必须来自 RECV 的实际 sender 1，并有完整 40 字节头、major 1/minor 0、
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

## 原服务端分离阶段的验证边界

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


## Root 拥有者整合（2026-11-17）

Root 保留围栏令牌、旧 Persist PID/端点、存储实例、Fs/驱动身份及信任快照，只发送一次终止请求。仅完整且来自旧 PID 的 accepted ACK、同一有界窗口内精确 64 位 task status 的正常零退出、未变更的拥有者绑定和两份原始端点的成功撤销，才能报告 STOPPED_CLOSED。坏 ACK、丢失回复、异常退出、未知宽值、身份变化或撤销失败均保持准入关闭；不会裸 kill、重发或清除未知 DIRTY。认证的明确拒绝只有在原健康上下文仍匹配时才允许取消围栏。

`sf_persist_fence_stop_status` 只读快照，不重复观察进程。`persist-terminal-test` 是管理员内部诊断，仅在独立测试启动中使用；它验证终态仍关闭、旧能力失效与 RAM/echo 可用，不提供重启或普通 stop 功能。旧工作树及其六个未提交文件已备份，当前整合沿用已修复的 Linux 额度夹具。

旧分离 E1 记录不代替本组合证据。实际生产构建/全宿主201.62秒通过，包含服务端550项、Root拥有者642533项/187场景与控制台689项；发布入口及新证据门接入后另一次全宿主135.34秒通过，包括实际发布门AST的20项过期/矛盾/假成功拒绝检查。

实际八启动E2 169.24秒通过：两种传输各验证正常接受、丢ACK后的保守关闭、真实延迟退出和明确注入的缓存BUSY拒绝；每次均验证普通用户拒绝、真实旧PID/端点、RAM/echo与盘字节，七份生产产物和初始盘在夹具退出后保持。BUSY只注入缓存返回值，不冒充真实卷忙或物理I/O故障。正常/丢ACK/延迟路径仍使用真实缓存与生产接收循环。第一次运行在两次正常启动后因故障夹具头文件路径错误中断，不计整套成功；修正后三个模式编译及完整八启动均通过，原失败保留。

证据在`.cache/thread-pressure-20261117/terminal-integrated-host.json`、`terminal-gated-host.json`、`terminal-retry-e2.json`及`build/persist-terminal-results.json`。新的发布门核对十八份生产/测试来源、七产物、八场景组合和准确终态字段，拒绝丢ACK误报成功、死亡未确认、端点未退役、旧来源或产物等情况。完整本地回归、发布ZIP和当前提交托管结果仍须独立确认，不能把专项当作完整成功。


后续当前组合的非破坏围栏四启动89.26秒、命名空间两启动/六次真实Fs死亡96.77秒及原WRITE坏ACK/恢复八启动140.37秒通过。包含终止原语共22次实测启动；这些专项不等同完整套件。全部夹具已还原，终止证据十八源/七产物在追加回归后仍匹配，原六个未提交文件未改动。


## 已停止拥有者的固定候选只读检查（2026-11-17）

`sf_persist_fence_inspect` 是 Root 私有接口。只有当前令牌对应的 STOPPED_CLOSED、
已认证 accepted ACK、旧 PID 的正常零退出、两份旧端点撤销及原存储/Fs/驱动/信任
绑定仍成立时，才调用可信的只读探针；调用后再次核对这些条件。它不解除围栏，
不改变终止快照，不授予能力，也不把返回的描述当成之后执行或激活的许可。

生产探针只从既有引导 bundle 选取固定 `sf-persist.sfmod`，使用已拥有的暂存区。
纯函数 `sf_persist_candidate_prepare` 执行实际信任/撤销/签名验证，只接受固定服务
标识及 service 类别、静态 process 配置、无导入/导出和合法装载计划。源、信任头、
间接公钥表与可写暂存/输出的重叠或溢出在写入前拒绝；通过范围验证后的失败将
结果清零。成功只返回包摘要、签名者标识和无授权/参数的映像计划。普通装载入口
仍被围栏阻挡；不调用可能回滚 TRYING 的 MOUNT，不启动候选进程或进行磁盘 I/O。

本切片的真实签名 E1 使用八个独立签名/损坏夹具，共 688 项检查，覆盖错误身份、
类别、应用包、公钥用途、撤销、relative 库配置和别名。Root 拥有者 E1 为
216690 项/67 场景。构建、27 项实际发布门验证和全宿主结果分别核对；不能用
宿主模拟替代访客。当前生产构建与八次真实启动专项 211.81 秒通过，正常/延迟退出
在两种传输上各重复只读检查并核对能力计数和终态不变，未知 ACK/BUSY 路径保持
拒绝；20 份输入及七份生产产物保持。BUSY 仍是缓存返回值注入，非物理故障证明。
完整本地/托管/发布包状态见 PROGRESS，不宣称通用 restart、候选激活或 E3/E4 完成。


追加完整宿主143.23秒通过，含688项签名/映像检查和27项实际发布门检查；八签名夹具必需，缺失库配置负例时拒绝跳过。实际E2的20来源及七产物摘要在宿主套件结束后仍匹配。


## 私有空闲候选进程与正常丢弃

`OP_PERSIST_CANDIDATE_PROBE=106` 与 `OP_PERSIST_CANDIDATE_DISCARD=107` 使用同样
完整的空请求、实际 Root sender/flags、递增非零 nonce 和最多三秒 deadline；不使用
105 的“已挂载卷静默”语义。服务只有在未绑定任何驱动、没有卷且从未分派过普通
Root 操作时，才可接受一次 PROBE 进入空闲模式。进入后所有普通 RPC 都返回
DRAINING，包括只读元数据；只有新的 DISCARD 可被接受，尝试发送一次 ACK 后正常
零退出。消息处理不挂载、不读写盘、不授予新能力；既有普通服务路径保持。

Root 的私有周期只在旧实例已正常终止且旧端点退役后创建两个新私有端点，立即
重新验证固定签名包，再给新进程服务 RECV 和回复 SEND。没有驱动、bundle、普通
装载作用域或候选激活权限。Root 依次验证 PROBE、实际元数据请求的认证 DRAINING、
DISCARD、准确新 PID 的正常零退出，以及新端点撤销；前后均保持旧停止快照、
存储/Fs/驱动/信任绑定和围栏。成功后也不重新开放服务；未知发送/回复、异常退出或
撤销失败保存候选状态并拒绝重复创建，不采用 kill、重试或重置逃逸。

内核 PID 取号单调且不回收，但死任务的槽位及退出记录会被新任务覆盖。首轮实际
访客发现新候选替换旧任务槽后，继续查询旧 PID 会返回 NOT_FOUND。修正保持 STOP、
只读检查、周期入口和创建前的旧 status==0 严格条件；创建了唯一新 PID 之后，仅在
完整已认证接受/正常死亡/原端点退役快照仍成立时，允许旧记录被遗忘。运行中、异常
或其他未知值仍拒绝，绝不把 NOT_FOUND 单独当作死亡证明；不修改内核或放宽 STOP。

纯解析器4173项及实际 Root 拥有者437812项 E1通过，覆盖任务槽记录遗忘、未知宽值、
超时/冻结时钟、错误回复、私有能力回收和不可重试。前几次夹具期望错误和两次真实
访客失败已保留，修正后生产构建、新空闲候选八启动及旧终止八启动共350.17秒通过，
包含坏 PROBE ACK、丢 DISCARD ACK、真实延迟退出、普通用户拒绝与盘/产物保持。
这些是 E1/E2；完整宿主、完整托管与发布包仍按 PROGRESS 独立核对，不代表一般
持久化重启、挂载、候选激活或真机/掉电目标完成。

## 只读缓存快照（2026-11-17）

`OP_PERSIST_SNAPSHOT=108` 只读取已挂载的内存缓存，使用私有104字节版本化
payload（`SfPersistSnapshot`），不增加公开结构布局 ABI。payload 包含大小与版本、
缓存 generation、`/notes/user` 字节数、16字节卷标识，以及规范化文件目录和运行时
信任策略的 SHA-256 摘要。文件摘要使用域标记 `SFFILES-SNAP-V1\0`、小端文件数，
再按路径排序的记录（64字节 NUL 填充路径、小端64位内容长度、该文件内容的
SHA-256）。信任摘要使用 `SFTRUST-SNAP-V1\0`、小端策略键数和按公钥排序的记录
（32字节公钥、小端 usage/revoked），之后是小端运行时撤销项数及排序后的32字节
撤销 ID。编码不依赖指针、结构填充或缓存槽位顺序。

Root 在同一有效围栏令牌的 ACTIVE 阶段最多发起一次只读 capture。回复必须来自被
记录的旧 Persist PID，且完整回显 request ID、deadline 和协议字段；Root 校验长度、
未用字节、能力数、缓存版本/边界，以及对围栏中信任快照计算的信任摘要。丢失、
矛盾、格式错误或信任不匹配的回复将 capture 标为失败并保持旧拥有者隔离；不会重试、
清除 DIRTY、改变信任或关闭旧 provider。只读 getter 返回缓存的历史事实，不表示
STOPPED_CLOSED、STOP 许可或未来启动许可。旧 `OP_PERSIST_STOP=105` 接口与其判定
保持不变；capture 既不替代也不预先满足 STOP。

Init 的独立 `persist-terminal-test` 诊断记录 `PERSIST SNAPSHOT CAPTURE PASSED` 或
`PERSIST SNAPSHOT FAILED CLOSED`，并比较快照前后的旧拥有者、事务字节和状态；该诊断
结果不代表普通启动流程已启用重启或候选激活。

本专项证据为：信任指纹95项、parser 8197项、volume 87项、owner 170613项；生产构建
及24次 VM 批次527.45秒、全宿主184.03秒通过。完整本地回归以及新 head 的托管结果和
发布包尚未通过或确认，因此不宣称全绿。该专项 E1/E2 结果不能替代这些待核验项。

## 私有候选卷只读重建（OP_PERSIST_CANDIDATE_VERIFY=109，2026-11-18）

109是ABI相对678项目录的唯一新增，当前679项；它没有新增公开结构布局。该周期只能在
旧108缓存快照已经捕获、105 STOP已达到 STOPPED_CLOSED，且原 PID正常零退出、旧端点已
撤销、存储/Fs/驱动/信任元组仍密封一致后启动。候选来自固定签名的 Persist 服务包，
spawn前重新验证；候选仅获得私有服务接收端和回复发送端，不获得驱动端点或卷写权限。
108的capture和105的STOP仍各自独立，109不会放宽它们。

Root通过同一私有端点代理sector read：109开始请求携带完整缓存快照及父 request nonce、
verify deadline；每个读取帧使用mode 1、一个512字节扇区、严格递增的handle序号与
`[2048,2563)`范围内的LBA，最多515帧（3个事务头扇区与两个256扇区bank）。Root只调用
已有存储只读读取回调，不给候选驱动能力，也不提供write或flush。全部读取共享独立
30秒有界deadline；probe/普通RPC拒绝/discard控制交换仍受3秒deadline约束。每帧都核对
双方的内核认证发送者、opcode、nonce、deadline、sequence、LBA、状态、长度、能力数和未使用字节；
成功终包必须逐字节匹配捕获的104字节快照。错误、超时、矛盾回复或上下文改变均封闭
失败，不重放109，不kill、不清除围栏或DIRTY，也不重置信任状态。

候选侧对收到的Root请求执行实际 `persist_candidate_verify_volume`。它只用只读回调运行
`sf_txn_mount`解析已提交事务，不调用会回滚TRYING的`persist_mount`；任何写/flush回调
均返回READ_ONLY。随后从事务中的durable revocation列表重建运行时撤销集合，并通过实际
缓存摘要核对generation、notes长度、卷UUID、规范化文件摘要和信任指纹。TRYING状态返回
BUSY，损坏/重复撤销项、读取失败、超界或任一摘要不一致均拒绝且不改盘。这里重建的只是
隔离缓存。候选的普通RPC仍返回DRAINING；即使109成功，Root还必须完成普通元数据拒绝、
正常DISCARD、准确候选PID的零退出确认和两个私有端点撤销。成功后也不发布候选、不开放
普通Persist服务；candidate status只是历史结果，不是激活许可。

init的正常verify诊断覆盖STABLE/PENDING两种卷状态及legacy/modern两种传输；
verify-readerror、verify-badreply和verify-dropped各覆盖STABLE的两种传输，共十次新启动。
verify-readerror只验证Root
注入的sector读取错误；badreply和drop路径保持候选进程仍活着且不discard，验证旧快照与
盘字节保持。E1实际卷重建5907项、109 parser/transport 4441242项/66场景、Root owner
751802项/58场景；release evidence AST测试450项通过。两批合计34次真实VM专项批次
（193.62秒与368.72秒）通过，覆盖稳定/待处理卷、读取错误、错误终包、丢终包及两种传输。
随后完整宿主源码冻结批次171.91秒通过；四个旧Windows系统调用mock已由32位long
改为int64_t，未改变生产比较或关闭告警。五组实际选定发布门及42份联合来源、七产物
摘要通过。完整本地回归、新head托管和完整发布尚未完成，不能据此宣称全绿。
