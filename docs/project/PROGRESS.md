# SpiritFoxOS 实施进度

更新日期：2026-10-31（北京时间）。

- 工作区：`E:\SpiritFoxOS`；分支 `codex/requirements-followup-20261031`；origin `https://github.com/fwqfzqaz/SpiritFoxOS.git`。
- 本轮目标：按用户恢复指示连续接续非 M7 工作；M0 两轮已推送并通过托管检查，同所有者文件句柄复制已验证推送，当前补齐发布副本的离线清单命令依赖。
- status: offline_release_tools_verified_pending_publish
- round_complete: false
- continuous_work: true
- quota_paused: false
- user_paused: false
- pause_reason: none
- 自动任务：`spiritfoxos-2` ACTIVE，每五分钟接续；任一剩余 <=10% 保存暂停，暂停后两项均 >20% 才恢复；活跃阶段只要两项 >10% 继续。用户再次明确暂停时同时暂停自动任务。
- 基线：main `9f3346b`。PR #12 已合并，原修复 head 四项 Windows/Linux CI 均成功；作者的 M7 保留，未重复实现。
- 新需求核对：已检查远端各分支、库内文档、全部独立 Issue（无）、近期 PR/评论、发布和讨论入口，未定位独立的新需求说明；继续已有授权且不依赖该说明的非 M7 工作。以实际 Git/API 提交为准，避免使用旧网页缓存。证据 `.cache/github-requirements-evidence-20261031.json`。
- 已推送 M0 首轮：`ab085f9` / PR #13，独立 native-source-v1 schema、无密钥离线命令及可选严格源约定，兼容作者 APP 能力/内存约束；12 组专项、含构建 host-only 76.11 秒通过。该 head 四项 Windows/Linux 已成功，证明 `.cache/manifest-round-hosted-proof-20261031.json`。
- 已推送 M0 后续：`789581f` / PR #13，真实编译共享头文件探针，采集首轮 549 项编号/大小/对齐/成员偏移/宽度/标量种类；9 组专项含三类真实临时头文件漂移。该 head 四项 Windows/Linux 已成功，证明 `.cache/abi-round-hosted-proof-20261031.json`。
- M0 后续验证：含初始构建 host-only 75.08 秒通过，原 25 个 C 测试及 51 组 Python 用例保留；日志 `build/abi-inventory-host-full-20261031.log`、`build/abi-snapshot-final-20261031.log`，结果 `build/abi-inventory-host-20261031-results.json`。当前 DUP 轮次的完整验证见下节，不能沿用此旧结果。
- 已交付 M0 摘要：七项产品摘要当时与作者基线相同，549 项 ABI 参考/采集为 `2036cab366d18b535cd005430eec23cd7c4acc3ee1a11b8663c0e1fda417bf24`。DUP 已提交 be5bda1，ABI 为 550 项，完整本地验证和发布门已通过。
- 进程：本地完整测试 PID 32004 已以 0 退出，无本聊天存活 VM；`.cache/active-work.json` 保存结束状态。
- 当前完整验证：`python tools/test.py` 包含初始构建，1107.92 秒、158 次 VM 全部通过。23068 项复制宿主检查、两种生产传输与原 416510 项 SDK 检查均保留；ABI 550 项只新增 `constant.OP_DUP=25`，原 549 项无删改。结果 `build/file-dup-final-test-results.json`，证明 `.cache/file-dup-full-proof-20261031.json`。
- 下一步：额度已在 21:27 实际刷新（短期 99%、长期 46%），清除额度暂停并恢复五分钟接续。be5bda1 的两项 Linux 已成功，两项 Windows 完整套件仍运行；继续核对。当前将现有清单离线命令依赖纳入发布 ZIP，对实际解压副本验证，无重复本地 VM。

## 同所有者文件句柄复制（2026-10-31）

OP_DUP=25 / sf_file_dup 通过现有描述引用实现同所有者复制，保留权限、共享游标和最后引用回收；关闭默认根后显式目录副本仍可使用，但 OP_ROOT 不会重新授予根。挂载、管道 EOF、控制台、回收与槽位耗尽语义由真实 namespace/file_rpc 宿主组合验证。畸形成功回复会停止客户端；不确定超时可能保留引用至进程回收，不提供跨进程转移或取消。

完整含初始构建回归 1107.92 秒、158 VM 通过；复制复用现有 legacy/modern 两次客户端 VM，没有为复制额外增加启动。全部既有 CPU/能力/服务/签名/设备/更新/旧事务兼容测试通过，七项交付摘要与本轮验证产物逐项相同。ABI 参考及采集均为 408e6cbdcf7ccf50eb900b7e06401280812c8bf823eca2ce7510dc0d575fd7ab。当前本地通过，托管 CI 按新提交另行核对。


## 已完成

1. Core 0：隔离页表、异常、单核抢占、能力端点、队列与超时、任务回收。
2. 用户态模块管理、依赖与资源门控、忙停止取消、新旧实例并存。
3. 用户态内存命名空间、文件、管道、控制台、串口 shell。
4. 确定性 CBOR、SHA-256/Ed25519 包工具；独立 native 运行时验证装载计划；hello 与故障测试应用。
5. `python3 tools/test.py` 已完整通过：116 条 C 服务断言、8 组包测试与 1500 次固定种子变异、128/256/512 MiB QEMU 启动及 shell 回归。
6. README、实际实现边界、测试记录、后续路线均在工作区内；Windows CI 定义已加入，本地通过不代表托管 CI 已运行。
7. M5：独立用户态 virtio-blk、PCI/端口与固定 DMA 页授权、SFRFS1 只读磁盘、`/disk` 挂载与引用、从盘运行签名模块和应用、磁盘快照上传至 native 运行时。
8. M5 完整测试通过：151 条 C 断言、13 组 Python 测试（2000 次固定种子变异）、128/256/512 MiB 正常 VM 与六种存储失败 VM，共九组。盘文件前后摘要不变；测试结果含发布镜像摘要。
9. 驱动进程死亡会执行设备复位；旧进程释放后仍存活时禁止新 DMA 拥有者，地址空间销毁后才允许接管。M5 阶段没有 IOMMU；现在增加下述受限 Q35 支持，无 DMAR 时仍为可信模式。
10. M6 有界持久化：显式写授权和范围、实际 virtio OUT/FLUSH、双银行提交/恢复、`/persist/notes`、签名应用 A/B 安装与每次运行重验。写/flush 失败停止该卷读写，重挂载或重启恢复。
11. 最终 `python3 tools/test.py` 完整通过（70.11 秒）：151 条基础/存储 C 断言、事务 C 测试、14 组 Python 测试与 2000 次固定种子变异、261 个操作边界 × 三种故障模式、十八次 VM 启动。验证笔记/包重启、坏签名拒绝、损坏回退、实际写与 flush EIO、银行写入中段终止 VM 后恢复。原始构建盘及只读底区保持不变。
12. 上一轮持久化交付为 `820208d`，发布包是 `SpiritFoxOS-M6.zip`；以下为当前最新结果。
13. 本轮：UEFI 交付 ACPI，Core 0 有界验证 DMAR、只为一个 BDF 映射固定 DMA arena、同步 context/IOTLB 失效、内核寄存器 MMIO 隔离。modern virtio PCI 协商 ACCESS_PLATFORM，拒绝隔离模式中的 legacy 旁路；实际越界 DMA 读写触发硬件记录，保护页不变，驱动终止且新拥有者被内核拒绝。
14. 最终 `python3 tools/test.py --skip-build` 完整通过（125.49 秒；此前已用最终源码重新构建）：151 条基础/存储 C 断言、229 条 IOMMU C 检查、事务 C 故障测试、14 组 Python 测试、33 次 VM 启动。包含 legacy/modern 两种正常存储、真实 DMA 读写负例、重复 DMAR 与 legacy 拒绝、两轮九次持久化恢复。结果 `build/test-results.json` 含精确 EFI/盘/公钥摘要。
15. 上一轮发布包为 `build/SpiritFoxOS-M6-IOMMU.zip`，通过打包门检查精确镜像摘要；格式与限制见 `docs/architecture/IOMMU.md`。
16. 本轮：160 字节规范更新状态绑定应用身份与包摘要；备用包和 PENDING 原子提交，先持久化 TRYING 再执行，按时退出 0 后确认；页故障、非零退出、超时、装载失败与中断后回退已确认槽。待更新时拒绝覆盖，首装失败回到未安装，旧单字节记录经签名认证迁移。回退只影响应用选择器，不撤销外部副作用。
17. 完整测试通过：151 条基础/存储 C 断言、229 条 IOMMU 检查、783 个原事务故障场景、3132 个更新阶段故障场景及规范状态检查、14 组 Python 测试与 2000 次变异、61 次 VM 启动。legacy 与 VT-d/modern 都覆盖真实故障/超时、试运行终止、开始/恢复/确认写 EIO、身份/摘要/签名拒绝及旧格式兼容。结果 `build/test-results.json` 记录精确镜像摘要与完整更新标记。
18. 更新阶段发布包为 `build/SpiritFoxOS-M6-Updates.zip`；当前同名包已更新为下述版本策略成果。协议与限制见 `docs/architecture/UPDATES.md`。
19. 本轮：已确认包的签名 `major.minor.patch` 形成软件版本下限；拒绝降级及同版本换包，精确重装不写盘并返回 UNCHANGED。候选失败/中断不提高下限，健康确认事务成功后才与已确认包一起更新。既有 160 字节格式保留，旧待更新记录在执行时也重新校验版本；`persist` 显示经认证的版本策略。
20. 最终完整验证通过：151 条基础/存储 C 断言、229 条 IOMMU 检查、4069 条版本策略检查、783 个事务故障及 3132 个更新故障场景、14 组 Python 测试、75 次 VM 启动。legacy 与 VT-d/modern 均覆盖降级、同版本换包、幂等安装字节不变、签名正确的非法版本、失败后版本下限保留、确认后重启、旧 PENDING 策略拒绝及全部已有恢复测试。
21. 当前发布包仍为 `build/SpiritFoxOS-M6-Updates.zip`；打包门新增要求完整版本宿主/VM 标记，并校验精确镜像摘要。软件下限不能抵御旧磁盘回放；损坏后恢复旧银行快照时以下层完整快照为准，不宣称 TPM/可信单调计数。
22. 本轮审查：修复驱动 MMIO 被当作系统调用 RAM 缓冲、CAP_CLOSE 泄漏全局端点、死亡驱动的持久化缓存准入、退出 -7 与活任务状态混淆，以及死亡任务槽复用后错误拒绝驱动停止。另补写 LBA 回绕边界、挂载失败发布顺序、退休能力槽和返回用户态 RSP 检查。具体影响与已声明限制见 `docs/quality/SECURITY.md`。
23. 当前完整测试新增 950 条内核宿主检查与两个可写盘 VM，77 次启动全部通过；既有 151 条基础/存储断言、229 条 IOMMU、4069 条版本检查、783/3132 个事务/更新故障场景及 14 组 Python 测试继续通过。原始盘保持不变，安全发布标记和精确镜像摘要纳入打包门，发布包继续为 `build/SpiritFoxOS-M6-Updates.zip`。
24. M3：目录句柄 openat、OPEN_DIRECTORY 类型限制、父权限衰减、规范相对路径及显式 runat 只读委派。应用必须经签名清单声明 namespace.read，并由 shell 选择目录；默认 run 没有文件根。通过真实 IPC 获得目录、列举和读取，父关闭不撤销子文件，退出/故障/超时回收全部引用。协议与限制见 `docs/architecture/NAMESPACE.md`。
25. 最终完整测试通过（320.67 秒）：108 条新增 openat 检查、签名请求能力第十组包测试、legacy/modern 两个新增 VM，实际总数 79。连续 40 次进程遗留文件仍可卸载，故障/超时后恢复；持久化只读授权不改盘，设备失效后也不能读取缓存。全部原有回归通过，发布门增加 openat 标记要求，发布包仍为 `build/SpiritFoxOS-M6-Updates.zip`。
26. M3/M6：统一文件句柄 truncate 与 RPC，RAMFS 和持久笔记支持缩短/清空/补零扩展，保持打开偏移；旧偏移写入会补零空洞。只读目录派生文件不能截断。笔记使用原双银行格式，同长度不写盘、不提高 generation；写/flush 错误关闭卷但仍释放 dirty 引用，应用槽与更新选择器不变。
27. 最终完整验证通过（386.03 秒）：83 条截断检查、3132 个新增故障场景、legacy/modern 各八次新增 VM，实际总数 95；原有全部内核、目录、包、版本、IOMMU、事务/更新回归继续通过。语义与边界在 `docs/architecture/TRUNCATE.md`，发布门要求本轮标记和精确镜像摘要；发布包仍为 `build/SpiritFoxOS-M6-Updates.zip`。
28. M6：构建时最多八个公钥，签名尾部 key_id 从外部策略精确选取公钥，区分 module/app 用途并拒绝撤销密钥，无开发根回退。启动器、管理器、持久化安装/已安装包读取和独立 native 运行时采用同一策略；guest `trust` 与离线工具可查询。开发默认策略保持单公钥，外部应用密钥可先重叠信任、确认新包，再撤销旧密钥并重建启动。
29. 最终完整验证通过（461.53 秒）：21 组 Python 测试、3000 次变异、105 次 VM，新增十次验证启动用途/撤销、跨密钥更新、未知/撤销/用途错误拒绝、已确认旧密钥版本下限失败关闭、待更新旧密钥撤销后回退；全部旧回归通过。临时策略结束后重新构建原有效策略，EFI/两种盘/公钥/策略摘要一致。发布门新增信任标记及策略摘要，语义和恢复限制见 `docs/architecture/TRUST.md`。
30. 默认开发签名角色分离：保留已有模块种子，首次创建独立应用种子，两个公钥必须不同；默认模块仅 module、应用仅 app。所有构建应用与已签名版本负例改用应用种子，模块继续使用原种子。自定义策略必须显式准入两个实际签名者的对应用途；不自动补齐或放宽用户策略。公开 application.pub 纳入交付摘要和发布包，私钥不提交。
31. 最终完整验证通过（486.45 秒、109 VM）：新增默认角色宿主检查，应用密钥签署 bootstrap/模块和模块密钥签署应用均拒绝；legacy/modern 各新增两次旧单密钥显式迁移与严格策略重启，确认新应用密钥包后移除旧应用用途仍可执行已确认新包，拒绝旧签名不改盘。十四次信任 VM 后恢复原策略，EFI、两种盘、两个公钥、策略摘要一致；所有已有回归通过，测试结果增加 separate_signing_keys=true。
32. M4：native argv 有界传递，最多八项、512 字节规范载荷，96 字节 BootArgs 头保持不变。显式参数复制到私有只读页，内核与 SDK 独立检查规范偏移、数量、长度和终止符；无额外参数保持旧 96 字节/argc=0 布局。run/runat 引号、空串和转义在启动及试运行事务前解析，模块拒绝额外参数，调用参数不写入安装状态。
33. 完整验证通过（547.21 秒、111 VM）：8155 项 C 检查含 1000 次往返，真实 syscall 拒绝头部偏移/数量溢出，两个新增 legacy/modern VM 验证 argv、只读页写故障、最大数量、进程重用、runat、installed 健康确认及非法参数不改盘；全部既有回归通过。SDK 及兼容边界见 docs/architecture/ARGUMENTS.md，发布门新增 arguments_host_checks=8155 和 arguments_qemu=true。

34. M4：显式 runenv/runenvat 环境，最多八个 NAME=VALUE 与八个 argv 共用 512 字节规范载荷；环境不继承，标识符/重复名/数量和词法在启动及持久化试运行前拒绝。BootArgs 固定头保持 96 字节，环境和参数同属子进程私有只读页，提供完整字符串与按名称查值 SDK。
35. 完整验证通过（602.82 秒、113 VM）：12034 项环境检查含 1000 次往返，两个新增 VM 验证 legacy/modern、空值/缺失/字面量、目录委派、8+8 上限、只读环境写故障、进程复用与已安装更新确认。全部原有测试通过，发布门要求 environment_host_checks=12034、environment_qemu=true；六个交付摘要一致。

36. M4：relative-v1 APP 的位置无关代码、签名清单整段 RELOC 子表、最多 256 个相对项，只写 RW 完整八字节槽、非负映像内加数。用户态认证同一快照后准备，Core 0 校验隔离基址并复制私有页，不增加 ELF/重定位解析器。确定性交替两个基址，不宣称安全随机化；导入/TLS/动态库未实现。
37. 完整验证通过（550.52 秒、117 VM）：新增六组宿主/500 次已签名记录变异及四次 legacy/modern 装载、安装确认和重启 VM。每次正常启动同时运行两个 PI 应用，验证函数/数据/BSS 指针和独立数据。另复现并修复 native 回复与已有应用请求串扰，使用独立回复端点且重启撤销重建；全部原有回归通过。协议与内部装载 ABI 变化见 NATIVE.md。

38. 文件 RPC 分发逻辑提取为共享 file_rpc.c，READ/WRITE/ENUMERATE 在访问后端前独立拒绝超过 256 字节的数据长度；有界路径、权限、目录根和回收语义保持。内核原 SYS_SEND 已有相同上限，此项是分层防御，不宣称之前能经正常 IPC 越界。
39. 完整验证通过（562.15 秒、119 VM）：353 项宿主检查验证保护字节、256 字节边界、大目录、无副作用和原权限；两个真实 VM 验证内核拒绝超长发送后仍可正常处理大文件、控制台和持久笔记，连续回收不泄漏挂载引用。全部原有回归和六项摘要一致，打包门新增 file_rpc 标记并拒绝旧结果。

40. 修复真实复现的块设备回复与应用 namespace 请求串扰：存储层为每个驱动实例创建私有回复端点，root 显式授予 SEND，块 RPC 只在此接收并继续核对发送者/请求 ID。失败启动或正常停止均撤销旧端点，设备停止不确定时仍保持隔离策略。
41. 最终完整验证通过（580.69 秒、121 VM）：新增两个 legacy/modern VM，各八次冷缓存并发读与驱动停止/启动，验证引用、任务和端点回收及持久笔记/磁盘字节不变；所有既有宿主、签名、设备、事务/更新/恢复回归通过。发布门新增 block_reply_channel 标记，并对旧结果拒绝打包；六项摘要一致。

42. Echo 每实例独立回复端点绑定 pid/service，涵盖初始、新旧代、控制台和磁盘模块；停止与失败启动撤销回收。真实能力耗尽分别验证服务/回复端点分配失败时生命周期退回 STOPPED，随后重新启动成功。
43. 最终源码重新构建后完整验证通过（628.35 秒、123 VM），日志 build/echo-final-resumed-test.txt。legacy/modern 各四十次冷并发读取与 echo 停启、实际能力耗尽恢复、新旧代并存和持久字节保持通过；所有原有回归及六项交付摘要一致，发布门新增 echo 标记。之前因额度停止的 98 次启动日志不作为通过证据。

44. 管理器在认证后立即匹配 MODULE/APP 启动意图，错误类型 SF_DENIED；所有模块/设备/echo 与应用包装显式选择预期类型，native 特殊授权也要求 MODULE。类型来自认证容器，后缀互换不改变类型；已安装策略保持。
45. 完整验证通过（687.44 秒、125 VM），日志 build/launch-kind-final-test.txt。新增两个 legacy/modern VM，正确密钥签署的 APP 不能作为启动模块，MODULE 无参数也不能作为应用；反向后缀 APP 可正常执行。每种传输重复 84 次拒绝并继续普通/PI 应用、echo 和卸载停止，原始及副本磁盘保持；全部旧回归和六项摘要一致。发布门新增 launch_kind 标记。

46. M2 登记时固定依赖提供者 generation，拒绝空/卸载槽和高位依赖。提供者槽位即使以相同 ID 复用，旧消费者也不能自动绑定；同一登记停止/启动仍可恢复，需迁移时显式重新登记消费者。代次耗尽返回 SF_NO_SPACE，不回绕。
47. 启动和停止先检查全部依赖，再统一增加/扣除租约，容量/代次/不平衡失败不留下部分状态。25080 项产品 C 宿主检查含一千次替换回收；正常 VM 启动增加真实管理器图的代次替换检查。最终源码构建后完整回归通过（678.98 秒、125 VM），日志 build/manager-dependencies-final-test.txt，全部旧回归及六项交付摘要通过，发布门要求新增 manager_dependency 标记。语义和当前内部 API 边界见 LIFECYCLE.md。

48. M2 echo 进程死亡监督：按任务状态识别实际死亡，回收服务/私有回复端点，25 tick 退避、最多三次连续自动启动，运行满 1000 tick 后下一次故障重置预算。手动停止禁用恢复，显式成功启动重置预算；清理不确定时阻塞，启动失败消耗预算。仅 echo 死亡，不覆盖挂起、native/设备或持久监督状态。
49. 97101 项策略 C 检查含一千条固定种子轨迹、稳定边界、时钟回退与饱和期限；两个 legacy/modern VM 实际终止 echo，三次恢复后第四次阻塞，应用/磁盘/shell 保持、显式重试、保持手停、十秒稳定后预算恢复及端点/任务回收通过。最终完整回归通过（736.3 秒、127 VM），日志 build/echo-supervision-final-test.txt，全部既有回归、六项摘要和发布门验证通过。

50. M2 echo 周期健康检查：100 tick 周期、100 ms 回复期限，实际发送者/请求 ID/操作码/成功状态/精确 ping 校验；普通调用保留一秒期限。实际仍存活的自旋或认证回复协议错误都会回收原实例，进入已有退避和三次尝试预算；显式停止不检查/复活，设备/native 策略保持。
51. 新增 legacy/modern 两个 VM，真实可抢占自旋、正确实例返回错误状态和错误操作码分别触发三次恢复，第四次故障进入 BLOCKED；普通/PI 应用、笔记、显式重试和六轮冷并发读仍通过。最终完整回归通过（761.42 秒、129 VM），日志 build/echo-health-final-test.txt，全部既有回归和六项摘要一致，发布门新增 echo_health 标记。

52. M2 native 事务启动：回复/服务端点及进程创建失败均撤销回滚，保留旧句柄时拒绝覆盖。死亡或健康失败先 DRAINING 并回收旧实例，已有私有映像应用继续 namespace RPC、释放租约；排空后才按共享三次/退避/稳定策略重启。手动停启与健康诊断不改变设备隔离或持久化格式。
53. 新增 legacy/modern 两个 VM，实际根能力耗尽的两条分配失败、实际进程容量耗尽时撤销两端点、两个真实应用持租约排空和拒绝新启动通过；死亡/自旋/错误状态三次恢复后错误操作码阻塞，显式停启、静态/PI/环境/目录应用与十二次端点重建通过。完整回归通过（778.66 秒、131 VM），日志 build/native-lifecycle-final-test.txt；全部原有宿主/VM 回归及六项摘要一致，发布门新增 native_supervision 标记。

54. M1 进程标识仅从 1..INT32_MAX 单调分配。原实际 task_new 在宿主注入 0x80000000 时创建 READY 任务却返回负值；现在在空槽查找后、任何任务/地址空间修改前拒绝耗尽/零值，计数不变。后续分配失败已取标识仍消耗，不回绕、不复用。
55. 5022 项宿主检查直接包含实际 core.c 的创建路径，页表/页帧/硬件机制替换为模型，含一千个计数状态、末两个合法 ID、BootArgs 标识、耗尽不发布/分配、失败后不重用、无空槽/非法计划和 root-only 权限。没有在 VM 创建二十亿个进程，也没有新增 guest 调计数接口。最终完整回归通过（781.16 秒、131 VM），日志 build/task-pid-final-test.txt，全部原有回归和六项摘要一致，发布门新增 task_pid_host_checks=5022。

56. M6 已安装调用在 TRYING 前检查 native 可用性，以及有效包是否声明本次目录委派能力。拒绝无效调用时保留精确 PENDING 磁盘字节；新增只读认证预览，候选槽/摘要/版本统一选择，实际 package 入口仍拒绝 PENDING，TRYING 后继续重验。坏候选预览错误继续进入旧试运行/装载失败回退路径。
57. 真实 8061264 基线复现无 namespace.read 的有效候选被 runat 标记试运行并回退，日志 build/qemu-persist-invocation-preflight-baseline.log。新增 legacy/modern 各两次 VM，初装/升级两版本的 runat/runenvat、native 停止拒绝保留原字节，正确重试确认；损坏候选在重启后仍经 TRIAL/ROLLBACK 回到 0.2.0，笔记、根/租约回收和原始盘保持。完整验证通过（802.5 秒、135 VM），日志 build/invocation-preflight-final-test.txt，全部既有回归和六项摘要一致，发布门新增 invocation_preflight 标记。

58. M3 RAM 文件删除与目录创建：可写目录句柄和规范相对路径、现有只读/挂载父节点及目标检查；任何所有者或dup的打开描述仍引用目标时BUSY，无引用才清空并回收槽。OP_UNLINK(22)保持目录句柄，shell提供带引号单路径的rm/mkdir，普通应用仍只读，持久化格式不变。
59. 17340项产品namespace/file_rpc宿主检查含一千次节点复用、数据清零、旧句柄失效、跨所有者忙引用、只读子树和畸形消息无副作用；新增legacy/modern两个VM，各四十次复用及真实只读应用IPC拒绝，持久笔记/盘字节和卸载停止保持。完整回归通过（966.29秒、137VM），日志build/ram-unlink-final-test.txt，全部既有回归和六项摘要一致，发布门拒绝缺少新标记的135VM旧结果。

60. M3固定32字节句柄元数据OP_STAT(23)：按现有所有者/代次查询类型、描述模式、节点标志、当前大小和共享偏移，不访问后端、不增加权限、不输出节点指针或所有者；shell提供支持单个引号路径的stat，错误也关闭描述。磁盘记录是缓存元数据，不代表设备健康。
61. 修复64位消息handle窄化别名：所有消费句柄的文件操作在改变状态前拒绝非零高32位，根关闭记账保持一致。6180项真实namespace/file_rpc宿主检查含一千次大小/偏移变更、所有句柄操作/三类高位、错误无副作用与保护字节；新增legacy/modern两个VM，真实应用的高位READ/CLOSE拒绝后根/读取仍可用，目录/文件元数据和回收通过。完整回归通过（965.9秒、139VM），日志build/file-stat-final-test.txt，全部原有回归与六项摘要一致，旧137VM结果被发布门拒绝。

62. M1规范内核能力：cap保留64位参数并在查表前拒绝高位别名，CLOSE/SEND/RECV保持STALE、DERIVE/REVOKE保持DENIED。40908项实际IPC宿主检查含一千次分配/回收，三类高位、五操作无复制/无副作用和正常端点保持；两个新增VM各四十次实际probe进程，普通/PI/文件IPC和持久字节保持。基线仅能别名调用者自己的有效能力，不越过所有者/代次/权限。
63. M1修复两个真实签名应用可跨退出读到XMM0的泄漏。每任务私有FXSAVE64/FXRSTOR64、创建/复用清零、默认控制字，关闭OSXSAVE/FFXSR，并在恢复前用无等待/零FLDENV清理异常指针。实际task_new新增一千个脏FP槽复用检查，共8022项创建路径断言；两个新增max CPU VM各十二组同时存活应用验证x87/MMX/XMM0–15、舍入/状态及yield/定时抢占保持，异常/超时后清零与服务/盘保持。TCG的SIMD仅标志无#XM、非零异常指针完整恢复限制已如实记录。最终完整回归通过（1074.85秒、143VM），日志build/kernel-cpu-isolation-final-test.txt，全部既有回归和六项摘要一致；旧139VM结果被发布门拒绝。

64. M3空目录回收：OP_RMDIR(24)和带引号单路径shell rmdir，与unlink共用目录写能力、规范路径及当前只读/挂载祖先检查。仅未挂载RAM目录，任何打开引用或任何后代均BUSY，路径组件边界区分相邻前缀，检查后才清空槽；普通应用仍只读，根和递归删除不支持。
65. 11359项实际namespace/file_rpc宿主检查含一千次目录/子文件复用、跨所有者/dup忙引用、旧句柄、高位及畸形RPC无副作用；新增两个legacy/modern VM各四十次回收、嵌套非空/只读/挂载/特殊节点拒绝、引号/前缀边界，以及十二轮实际只读APP的RMDIR拒绝和回收。完整回归通过（1166.82秒、145VM），日志build/ram-rmdir-final-test.txt，全部已有宿主/VM回归和六项摘要一致；发布门实际拒绝旧143VM结果。

66. M7网络纵向切片：ring-3用户态legacy virtio-net（0x10001af4）驱动，仅协商VIRTIO_NET_F_MAC，单队列对描述符/avail/used轮询收发；新增net.h ABI与OP_NET_INFO(60)/OP_NET_PING(61)，有界ARP解析与IPv4/ICMP echo（含校验和、分片拒绝、固定超时），shell新增ifconfig与ping。设备仍为全局唯一所有者与固定64 KiB DMA arena，因此网络只在无盘启动时接入；modern网卡与VT-d隔离模式拒绝legacy设备。5043项宿主协议检查（含一千组随机往返）与其余既有宿主检查继续通过。完整python tools/test.py --skip-build通过（1007.15秒、150VM），日志build/m7-network-final-test.txt；新增无盘QEMU网络场景，slirp下MAC/静态IP/网关、ARP与ICMP往返、rx/tx计数增长、二次ping与关机退出均实际验证，其余全部旧回归与七项摘要一致。图形、用户会话、多核、服务状态迁移以及DHCP/UDP/TCP、IRQ、磁盘与网络同时授权仍未完成。

67. M7一次性UDP/TCP交换：在既有net服务上新增OP_NET_UDP(62)与OP_NET_TCP(63)，统一SfNetPacket（ip/port/bytes/248字节数据，正好256字节IPC正文），未把socket、连接或端口表放进内核。services/net_proto.c增加UDP/TCP伪头校验和、UDP构包与严格解包（IPv4版本/IHL/总长/分片/源地址/IP校验和/长度/端口；IPv4允许缺省UDP校验和，存在则必须通过）、TCP构包（20字节头、data offset 0x50、序号/确认/标志/窗口）与解包（强制校验和、选项偏移边界、载荷上限）。virtio_net.c的tcp_exchange按ARP→SYN→SYN-ACK→ACK→单段PSH/ACK数据→等待对端一段回复→ACK+FIN的顺序执行，SYN最多三次且数据最多重传两次，纯ACK不算回复，时长以tick为界；shell新增udp/tcp <ip> <port> <text>并以耗时毫秒回显。宿主test_net扩到14084项检查（含1000组TCP构包/解包随机往返、校验和篡改、data offset与分片/协议拒绝、超长拒绝），tools/test.py的net_host_checks同步为14084。host-only套件47.55秒通过；qemu_net_test.py新增宿主UDP(127.0.0.1:56001)与TCP(127.0.0.1:56002)回显服务，slirp下实际往返`ACK:hello`通过，日志build/qemu-net.log。限制仍为legacy轮询无IRQ、仅无盘启动、无DHCP、无连接对象与`/net`视图、磁盘与网络不能同时授权；本轮未重跑150VM完整回归。

68. M4有界用户态SDK：把recovered/libc的纯函数子集移植为include/sdk.h与services/sdk.c，全部符号加sf_前缀，只含内存/字符串/ctype/数字转换/bsearch/格式化（sf_vsnprintf），不含syscall、malloc、全局状态、流或Linux ABI；字符串长度和格式化宽度/精度上限4096，格式化始终在size内终止并正确计数，h/hh/l/ll/z按实际类型读取，无符号转换忽略+与空格标志，INT64_MIN无未定义行为。按DESIGN-v0.2，recovered/runtime的ELF/ET_REL装载器不迁入Core 0：公开交付格式仍是.sfmod/.sfk，构建期ELF中间产物由sfpack转换，依赖排序/回滚/受限导入元数据已由manager.c与package.c覆盖。tests/host/test_sdk.c以宿主glibc为独立参照，固定用例加1000组随机转换、700组随机格式化、0..40截断和100组bsearch，共171738项检查通过；sdk.c已加入build.py的user-common，host-only套件46.2秒通过，发布结果新增sdk_host_checks=171738。

69. 修复未声明的文档/代码缺口：应用清单现在强制声明并校验requested_caps（1..32项、非空UTF-8、每项不超过64字节）与resource_limits（含memory整数，必须覆盖映像映射总量且不超过32 MiB），缺失/空/低报/字符串或负值都在签名验证前按SF_BAD_FORMAT拒绝；模块清单保持原schema。CR4在CPUID宣告时启用SMEP与SMAP，内核RAM访问走恒等映射，不需要STAC窗口；QEMU默认CPU不宣告时打印诊断但不强制。Makefile纳入版本控制，PROGRESS工作区路径改为实际Linux工作区，SDK归档说明与第68条区分。host-only套件46.74秒通过（新增应用资源上限用例），256 MiB默认与max CPU各一次QEMU回归通过，max CPU日志无SMEP/SMAP不可用诊断。

70. 修复Windows原型CI在`python tools/test.py`阶段的编译失败：tests/host/test_sdk.c原先用`sf_atol("2147483648")==2147483648L`断言64位long，但Windows（LLP64）的long是32位，`-Werror -Wtautological-constant-out-of-range-compare`直接拒绝编译，导致全部宿主检查中断。现按`LONG_MAX`宽度分支断言（64位宿主比较2147483648L，32位宿主比较LONG_MAX），`%p`改为断言SDK自身形式`0x1234`（UCRT/mingw不输出0x前缀，不再依赖宿主方言），检查数171739。另以`x86_64-windows-gnu`交叉编译全部25个宿主测试（含-Wno-unused-parameter分支）确认无其他Windows编译错误；Linux host-only套件通过。

## 交付与续做

M3空目录删除完整验证通过（1166.82秒、145VM），当前全部VM关闭。正常提交推送当前origin分支、核对PR10后继续下一有界任务；不得因本轮交付停止。M4有界用户态SDK已并入并通过171738项宿主检查；下一有界事项按既有顺序从文件系统/存储子系统的有界能力开始，runtime ELF装载器按DESIGN-v0.2不迁入。保留当前只读授权与全部旧协议/探针。

内核能力与FP隔离已推送8aeb118、通过143VM。额外入口审查在两种实际VM中检查NT/DF/AC、SYSCALL/SYSENTER及FSGSBASE，均安全返回或只隔离原调用者，六项摘要与持久字节保持；记录build/entry-audit-results.json，未复现新漏洞，不把审查候选当成已确认漏洞。

用户于2026-10-24携电脑出门时要求暂停，20个文件已备份至.cache/checkpoints/user-pause-20261024-102451.zip；随后明确继续，user_paused=false，自动任务已恢复ACTIVE与五分钟接续。暂停期间未运行完整回归，也未将未验证成果推送。本次143VM结果取代暂停前的局部结果。

指定仓库为https://github.com/fwqfzqaz/SpiritFoxOS，PR10持续更新当前codex/trust-policy-20261023分支，不强制推送。此前PR9已由外部合并为main ca9e1a3。原误用xiaoniko258/SpiritFoxOS的当前代码树已按用户要求清除并保留迁移README；不再次操作该仓库。原分支头和迁移元数据在此前本地refs/archive与.cache/repo-migration中保留。

更完整持久化服务、在线信任/独立生产根、通用IRQ/设备隔离、真实硬件断电仍未完成；网络仅在无盘启动且legacy模式可用，已支持一次性UDP/TCP交换但无DHCP、连接对象与`/net`命名空间视图；IRQ、图形、用户会话、多核与选定服务状态迁移仍未完成，详见ROADMAP。当前QEMU验证不能替代物理硬件验收。

## 额度保护

使用自动任务 `spiritfoxos-2`。用户最新明确要求持续做到剩余 10%：任一窗口剩余小于等于 10%、普通使用不可用或额度缺失时保存并暂停。暂停后只有两项均严格高于 20% 且 ordinaryUsageAllowed=true 才恢复；已经在推进时只要两项均大于 10% 就继续，10%–20% 区间不能提前停工。一轮测试、推送或 round_complete=true 不是停工理由，直接选下一项有界事项；只有额度阈值、真实阻塞或整个目标完成才结束当前持续工作。

额度允许且还有已授权工作时保留五分钟 heartbeat，用来接续意外中断，检查已有工作区进程避免并发测试；正常执行不发送例行消息。额度不足等待时取消五分钟唤醒，按实际阻碍恢复窗口的 `resetsAt` 后约一分钟切为定点日/周复查，两个窗口都阻碍时等较晚重置；接口未更新不能假定已刷新。当前自动任务保持 ACTIVE，额度已实际刷新并恢复五分钟接续。只在额度刷新、实际恢复、真实失败或需要用户操作时通知，例行检查、仍不足和普通阶段进度静默；全目标未完成时保留有效监测。

恢复时先检查额度、Git 状态和此文件，续做未完成事项；一轮已交付后，按要求与路线图选下一项有界任务，避免重复已经完成的工作。

## 主分支合并范围（2026-10-24）

主分支将文档移动到 docs/design、docs/architecture、docs/quality、docs/project，C/Python/QEMU 测试移动到 tests/host、tests/integration、tests/qemu，格式工具移动到 tools/package。当前分支新增测试和文档一并迁移，旧文件 SDK 仍单独保存在上述 stash。

运行期撤销作为既有八密钥不可变用途策略上的附加拒绝层；它不能恢复构建时已撤销或错误用途的签名者。版本状态记账只允许忽略经过不可变策略认证后的运行期撤销，执行、安装和预览继续执行完整策略。主分支的 Linux 宿主目标、共享库后缀与 QEMU/OVMF 选择保留，本机实际验证为 Windows，不宣称在 Linux 执行过本次合并测试。

合并验证通过：1001.23 秒、149 VM；29 组 Python 测试/3500 次变异、29 条运行期信任宿主检查及全部原有回归。Windows 本地通过，Linux 构建和宿主路径保留，托管 CI 状态单独核对。主分支的备用签名包改用已有磁盘条目作为测试夹具，避免额外目录节点耗尽固定命名空间配额。

## SDK 从保存点恢复（2026-10-24）

合并修复 db9c0a5 已正常推送，149 VM / 1001.23 秒通过，GitHub 确认冲突消除；托管 CI 正在运行。SDK 六个新文件从 stash bbbbf630 恢复时逐个校验 LF SHA-256，C 宿主相对包含路径与构建/测试/打包入口按主分支分层迁移，保留七项交付摘要和两套信任验证。完整回归目标为 151 VM，旧 147 VM 中断记录不算通过。

## 用户暂停保存点（2026-10-24）

用户确认合并问题已解决后要求暂停其余开发，等待下一步指示。合并修复已正常推送 db9c0a5，PR #10 的 GitHub mergeable=true；149 VM / 1001.23 秒完整回归记录另存 build/pr10-merge-test-results.json。Linux 托管宿主 CI 已成功，Windows 托管完整 CI 最后查询仍在运行；未取消远端已交付代码的 CI。

当前 SDK 改动已按新目录恢复到工作树，尚未提交或推送。新构建和 416510 项 SDK 宿主检查、legacy/modern 两个 SDK 专项 VM 已通过，但 151 VM 完整套件在启动第 7 个 VM 时按用户要求停止，不计为完整通过。验证归档 build/file-client-user-interrupted-results.json；完整输出 build/file-client-merged-final-test.txt。

已核验并终止本聊天 PID 51564 的测试进程及子 VM，执行器已返回退出码 1；没有遗留本轮本地测试/VM。该退出是主动暂停，不能称为产品测试失败。自动任务 spiritfoxos-2 已设为 PAUSED，除非用户明确继续，不自动恢复开发或额度唤醒。

恢复时直接使用当前已迁移 SDK 工作树，不要再次运行 stash 恢复脚本；旧 stash bbbbf630 保留作为额外备份。先核对额度、工作树和进程，再完成新的完整 151 VM 回归；只有通过后才更新文档、打包、提交推送。合并提交 db9c0a5 与未完成 SDK 必须分别对待。

## Windows CI 红叉调查保存点（2026-10-24）

用户指出历史提交的检查红叉后，已读取首批提交与最近两次 Windows Actions 日志；它们均在密钥轮换重建 sfcrypto.dll 时出现 Permission denied。最新已推送 db9c0a5 的合并冲突已消除、Linux CI 成功，但 Windows CI 失败，不能称为整条交付已绿。

原因：tools/test.py 初始直接调用 build.main，ctypes 把 DLL 留在测试父进程中，后续独立构建进程不能替换。先前本地 --skip-build 绕开了此问题。已在干净 db9c0a5 快照内真实复现同一错误；本地修改改为子进程执行初始 tools/build.py。独立补丁 .cache/ci-dll-lock-fix.patch 仅包含这一入口改动，不包含暂停的 SDK。

修复后在 .cache/ci-dll-lock-db9c0a5 中执行与 CI 相同的 python tools/test.py（包含初始构建），宿主与部分 VM 通过；149 VM 全套在启动 40 个 VM 后因短期剩余 10% 停止，不能标为完整通过。进程 55780 及已核验子进程/VM 已终止；完整日志 build/ci-dll-fixed-full.txt，状态 .cache/ci-dll-repro-state.json 与 build/ci-dll-interrupted-results.json。修复尚未提交、未推送，GitHub 的 Windows 失败状态尚未解决。

用户要求的其余开发暂停继续有效；自动任务 spiritfoxos-2 保持 PAUSED，不因额度刷新擅自重启。恢复 CI 修复前须获得用户继续指示并满足额度条件。先从默认策略重建隔离快照，避免沿用被中断密钥测试的临时策略产物，再完成 149 VM/打包验证；只暂存独立 CI 补丁和对应验证文档，正常推送后核对 Windows/Linux 托管检查。暂停的 SDK 工作树与 151 VM 未完成记录仍单独保留，不混入此修复。

## 作者修复合并后继续（2026-10-24）

用户明确授权继续，额度短期 100%、长期 55%，已清除用户/额度暂停并恢复五分钟接续。远端 PR #10 已合并为 main f2e41bd，作者 d6a71f2 的 Windows 和 Linux CI 均成功。旧的本地 DLL 修复已被作者修复覆盖，不再重复验证或提交。

未完成工作先保存到 stash bb363147e95c1a6a02a3380986807d9e192f5cb9，再以最新 main 建立独立 SDK 分支；仅 tools/test.py 的重复 DLL 修复产生冲突，采用作者版本并保留三个 SDK 测试入口。其余五个 SDK 源文件的规范化 SHA-256 与保存点逐项一致；作者 kernel/arch.c、services/init.c、README.md 保持其基线内容。

恢复后的完整测试会包含初始构建，避免再次漏测 CI 路径。进行中进程与日志见 .cache/active-work.json；不得并发启动第二套 VM。

## 文件客户端 SDK 验证完成（2026-10-24）

以作者已合并修复为基础，完整包含初始构建的 python tools/test.py 通过（1086.09 秒、151 VM）。416510 项真实编解码/传输模型/产品分发器宿主断言、两次生产 INT80 专项通过，各传输六十次显式 RAM/磁盘/持久只读目录应用回收；无默认根、控制台、元数据/读取/定位/列举/poll/关闭、只读修改拒绝和原始高位请求探针保持。

本轮仅交付 SDK 及相应验证/文档，kernel/arch.c 与 services/init.c 保持作者基线。完整记录 build/file-client-author-test-results.json，交付摘要与发布包验证另存 .cache/sdk-author-full-proof.json。当前所有本地测试/VM 已结束。下一步正常推送新 SDK 分支并创建对应 PR，核对该提交的 Windows/Linux CI；不能沿用 PR #10 的绿色状态。一轮交付后按路线继续，额度不足时保存暂停。

## 旧事务盘兼容修复进行中（2026-10-24）

SDK 已推送 002ea46 并创建 PR #11；本地包含初始构建的 151 VM / 1086.09 秒通过，发布 ZIP 387815 字节、29 个清单条目 CRC/摘要一致。该提交 Linux CI 已成功，Windows CI 最后查询仍在运行；继续核对，不能称为已全绿。

用真实 8b704ae 事务引擎生成四记录/1024 数据起点的 v1 银行，写入 generation 2 的 legacy-notes。原新读取器回退到空 generation 1、返回 NOT_FOUND；修复后对同一原始样本只读挂载，generation 2 与完整笔记均恢复，样本字节不变。复现与修复证据分别在 .cache/txn-compat-baseline.json 和 .cache/txn-compat-fixed-reader-result.json。

产品解码器在摘要验证之后接受两个严格的既有 v1 布局，保持保留字节、路径、重复键、内容摘要、零填充和完整尾部检查；最高有效代次与同代次歧义拒绝保持。挂载不写盘；下一次普通提交仍只写当前五记录布局。

2046559 项宿主检查通过，覆盖一千个旧/混合快照、边界长度/全部槽位、畸形输入与 783 个首次升级提交故障场景。六次 legacy/modern VM 专项已通过，验证四键旧盘只读访问字节不变、笔记/双槽/版本保持、普通写升级、第五个撤销键及重启。现在运行包含初始构建的完整回归（目标 157 VM），尚未声明本轮完整通过或提交推送。唯一运行测试见 .cache/active-work.json。

## 旧事务盘兼容完整验证完成（2026-10-24）

完整含初始构建回归通过（1112.0 秒、157 VM），2046559 项兼容宿主检查与 783 个首次升级故障场景、六次迁移/重启 VM 通过；全部既有 SDK 与内核/服务/信任/更新回归保持，七项交付摘要一致。结果 build/txn-compat-test-results.json，日志 build/txn-compat-final-full.txt；当前所有本地 VM 已关闭。下一步正常提交推送工作分支并更新当前开放 PR 的最终范围，核对最新提交的 Windows/Linux CI，继续路线中的未完成工作。SDK 提交 002ea46 的已交付证明仍保留，不把旧检查状态算作本次提交通过。

SDK 首轮提交 002ea46 的托管检查已实际核对：Windows run 40096585352 与 Linux run 40096584961 均成功；该结果只属于 SDK 首轮，后续兼容修复提交须重新检查。

## 额度保存点（2026-10-31 21:17，北京时间）

quota_paused=true，user_paused=false。短期剩余 10%、长期 46%，ordinaryUsageAllowed=true；短期实际 resetsAt=1793453188，下一次检查 21:27:28。DUP 完整本地验证和发布门已通过并正常推送 be5bda1 / PR #13；四项新托管检查最后均在运行。PID 32004 已退出 0，无本地测试/VM，需要恢复时先查 CI 而非重新测试旧成果。保存点 `.cache/progress-live.json` 与 `.cache/checkpoints/file-dup-quota-paused-20261031.zip`。

可接续的下一有界事项：发布 ZIP 现已携带 schema 和 ABI 离线工具，但尚未携带 sfpack 清单校验所需的三个纯 Python 文件；恢复后可加入现有文件并对实际解压副本验证无编译器/build/密钥条件下的导出与源校验。当前仅做过依赖阅读，没有对此新事项修改实现或执行测试。

## 额度刷新后恢复（2026-10-31 21:27，北京时间）

实际接口确认短期 99%、长期 46%、ordinaryUsageAllowed=true，quota_paused=false。无本地存活测试或 VM，自动任务恢复每五分钟接续。先接续既有清单工具交付完整性：发布副本加入离线命令依赖并验证实际 ZIP，保留已完整验证的运行时代码和七项交付摘要；be5bda1 的 Windows 托管检查仍需核对。

## 发布副本工具验证完成（2026-10-31）

实际 ZIP 已补齐三个既有纯 Python 文件，CRC、39 项清单摘要和七项运行时产物核对通过。只取出 ZIP 中工具和公开输入，在 PATH 为空、没有 build/密钥/编译器/认证共享库的目录完成 12 项导出、校验、拒绝、公开策略描述和 ABI 比较检查，证明 .cache/offline-release-proof-20261031.json。此轮运行时代码和工具逻辑保持 be5bda1，不重复本地 158 VM；待正常提交推送与新 head 托管检查。quota_paused=false，user_paused=false，自动任务 ACTIVE。
