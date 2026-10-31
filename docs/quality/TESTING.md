# 测试记录

日期：2026-10-24；最近验证：Windows x64（含主分支既有 Linux 验证记录）；Zig 0.13.0；QEMU TCG、q35、qemu64+NX、单核。

执行 `python3 tools/test.py`，构建时启用 `-Wall -Wextra -Werror`；自动测试完整运行于 128、256、512 MiB。输出日志与机器结果保存在忽略提交的 `build/`。

## Linux x64 重新验证（2026-10-23）

工作区迁移到 Linux x64 后，用系统 QEMU/OVMF 与工作区 Zig 0.13.0 重新构建并运行整套测试：

- 宿主：116 条基础服务、35 条存储、108 条 openat、83 条截断、908 条 IPC、42 条内存、229 条 IOMMU、4069 条版本、21 条信任撤销，以及事务/更新/截断故障场景全部通过。
- Python：11 组包测试（含 1500 次变异）与 5 组 SFRFS1 测试（含 500 次变异）通过。
- QEMU：128/256/512 MiB 正常启动、六种存储故障、持久化/更新/版本/IOMMU/安全/openat/截断/信任撤销场景全部通过。
- 结构整理后运行 `python3 tools/test.py --skip-build` 完整通过，用时 506.34 秒；`build/test-results.json` 记录精确 EFI、磁盘与公钥摘要。整理前的功能基线运行用时 488.34 秒；差异来自环境抖动，结果一致。

Linux 下 OVMF 文件名因发行版而异，QEMU 测试同时识别 `OVMF_CODE.4m.fd`、`OVMF_CODE_4M.fd` 与 `OVMF_CODE.fd`；临时文件统一放在工作区 `.cache/`，不依赖 `/var/tmp` 权限。

## 宿主验证

- 116 条基础 C 服务断言：状态转换、十类阻塞资源、依赖环链、pinned 模块、generation、句柄所有者、独立/共享偏移、路径根边界与非法 UTF-8、只读文件、部分管道 I/O、空写、EOF、poll、进程回收。
- 35 条存储 C 断言：virtqueue 布局、块边界和片段范围、磁盘文件偏移、挂载租约、dup 生命周期、排空拒绝、目录引用、只读写入与创建拒绝、卸载后句柄失效。
- 9 组包测试：所有构建包、SHA-256 边界向量与百万字符向量、RFC 8032 Ed25519 标准向量、头部/长度溢出/截断、段布局/W+X/保留字段/非零填充、签名篡改和未知公钥、非规范 CBOR/重复键/非法编码/嵌套深度，以及缺失/错误类型/不兼容的应用 ABI。
- 固定种子执行 1500 次随机单比特包变异，全部被同一份 C 加载器拒绝。此项为有界变异测试，不宣称完整模糊测试覆盖。
- 5 组 SFRFS1 测试：真实镜像、生成器约束、头部容量和整数边界、重算目录摘要后的非法区间/重叠/重复路径、文件内容摘要，以及 500 次元数据单比特变异；与用户态服务共用同一解析器。
- 事务 C 测试共用真实引擎：261 个写/flush 边界 × 三种故障模式，共 783 个失败场景；覆盖丢失、部分持久化、撕裂、持久化后报错。恢复后的关联文件必须同属完整旧版或新版。另测确认提交、银行损坏回退、双损坏拒绝、同 generation 拒绝、保留字节、非法路径、文件大小及 generation 上限。
- 229 条 IOMMU C 检查：ACPI 校验和、RSDT/XSDT、DMAR 长度/范围/重复、DRHD segment、端点覆盖；独立页表遍历验证获授 IOVA 的每个半扇区、前后边界、其他请求者和 CPU 物理地址均不能意外获得映射。
- 更新状态机与真实事务引擎组合测试：规范编码、截断与逐位变异、身份及槽约束、首装失败；stage/begin/confirm/rollback 四个提交点各 261 个边界 × 三种故障，共 3132 个失败场景。恢复时包摘要、状态和回退槽保持一致，未确认试运行不会升级为已确认。
- 4069 条版本策略检查：规范数字版本、最大 32 位分量、溢出/前导零/后缀拒绝、整数次序而非字符串次序、降级/同版本换包拒绝和精确重装识别，以及 1000 次固定种子版本字符串往返。与用户态安装器共用 `services/version.c`。
- 368490 条 SDK 宿主检查：以宿主 C 库和明确的接口期望值为独立参照，覆盖内存/字符串/ctype 固定与随机输入、1000 组 strtol/strtoul、700 组格式化、0 到 40 字节截断的返回值/终止/字节一致性、100 组随机 bsearch，以及全部 256×256 字节对的有界比较。比较函数核对符号，分词首调用与续调用分开，不完整十六进制前缀按明确值及偏移核对，二分查找验证匹配成员而非重复键下的相同下标。`long` 宽度与 `%p` 形式分别按宿主 ABI 和 SDK 约定断言；SDK 自身不调用宿主库，也不含 syscall、分配、全局状态或流。

## QEMU 端到端

三个内存配置均验证以下实际行为：

1. UEFI 签名校验、ExitBootServices、Core 0 与 CPL3 sf-init。
2. 无磁盘文件服务和 sfexec 时，回显服务先从 bundle 启动并完成 IPC。
3. 准入关闭拒绝新引用，忙卸载与依赖阻塞，取消恢复，新旧进程代并存，撤销旧端点。
4. RAMFS/独立偏移/dup/进程句柄边界、管道 seek/WOULD_BLOCK/部分读取/EOF/poll。
5. 真正的 RWX 进程装载计划拒绝；能力转移数量伪造拒绝；队列容量与背压；阻塞接收截止时间。
6. sf-native 在独立用户态地址空间处理装载 RPC；hello.sfk 通过命名空间控制台对象输出，退出释放租约。
7. 普通应用伪造句柄、内核指针、特权创建和权限放大拒绝。
8. 用户写 supervisor 页、UD2、执行 NX 栈分别触发对应任务异常；回显服务继续健康。
9. 非合作死循环被时钟抢占并由管理器停止；运行时死亡能诊断、其他服务存活、运行时可重启。
10. 串口 help/modules/echo/运行应用/停止重启模块/写读文件/列目录/selftest/shutdown。
11. 用户态 virtio-blk 只读设备、拒绝越界与不对齐请求、拒绝写入；挂载依赖阻止停止驱动，打开与复制句柄阻止卸载，取消后恢复服务。
12. 停止后的旧驱动仍存活时，禁止新实例映射同一 DMA 区；销毁旧地址空间后才能重新授权。实际空闲驱动死亡触发设备复位，旧文件访问失败，之后重启驱动与挂载成功。
13. `disk-echo.sfmod` 和 `diskhello.sfk` 未装入 boot bundle，从真实 virtio 测试盘读取并独立验证签名后运行。磁盘包通过顺序上传交给隔离的 native 运行时。
14. 额外六个 256 MiB VM 场景：缺盘、坏 SFRFS1 头、坏文件摘要、文件摘要正确但应用签名错误、可写介质拒绝、QEMU blkdebug EIO。均保留可用的串口、RAMFS、bundle 应用和回显服务；坏签名应用没有执行。

M5 回归为 151 条 C 断言、14 组 Python 测试中的包/只读格式部分、2000 次固定种子变异和九个 QEMU 启动用例。只读场景运行前后磁盘 SHA-256 不变。M6 增加事务测试和九次持久化 VM 启动，总共十八次 VM 启动：

- 显式设备写权、拒绝无设备/未知写权、写范围阻止覆盖只读区域、半块顺序校验、实际 dirty 资源阻止卸载并恢复准入。
- 持久化笔记、两次签名安装的 A/B 切换、卸载与驱动重启后重新挂载；成功状态等待实际设备 flush。
- 重新启动后 generation、笔记和 installed 应用保留；读操作不修改盘。
- 重新计算存储摘要后的坏签名包仍不能执行；bundle 应用继续可用。
- 最新银行损坏回退上一完整 generation。
- blkdebug 注入 OUT 和 FLUSH EIO：不输出写入成功，卷停止准入，重启恢复。
- 写到备用银行中段时注入错误并配置 QEMU 暂停，QMP 确认 io-error 后强制终止该 VM，再启动恢复旧版文件和包。记录已清零后才复用银行。

测试原始 `persistent.img` 与只读底区保持不变，只写副本的事务区域。机器结果附带 EFI、两种存储镜像和公钥摘要；打包工具拒绝与已验证摘要不符的镜像。`build/qemu-persist-*.log` 保留九次持久化启动的串口记录。

## VT-d 与 modern DMA 端到端

另增加十五次 VM 启动，与前述十八次合计三十三次：

- 128/512 MiB Intel VT-d + modern PCI ACCESS_PLATFORM 完整正常存储/生命周期回归。
- 两次有实际设备请求的负例：IN 数据写入未授权内核页、请求头从未授权内核页读取。VT-d 记录 source `00:05.0`、目标地址及读/写原因；内核 canary 保持 `0xa5`，context 撤销，驱动终止且新驱动授权被内核拒绝，回显与 bundle 应用仍正常。
- 普通应用访问内核 VT-d MMIO 页实际触发页故障；不能调用 root 专用 DMA 状态查询。
- 拒绝会绕过平台 DMA 的 legacy 设备，以及重复 DMAR 的固件配置；继续保留控制台与普通应用。失败场景原始盘 SHA-256 不变。
- 在 modern/VT-d 模式重跑全部九次持久化 VM，包括 OUT/FLUSH EIO、签名包、损坏回退与提交中断恢复。

结果记录 `iommu_qemu=true` 和 `iommu_host_assertions=229`，打包必须包含完整隔离回归；日志为 `qemu-*-iommu.log` 与 `qemu-persist-iommu-*.log`。驱动使用的总线地址与 CPU 物理地址刻意不同，避免以恒等映射掩盖 DMA 旁路。

测试仅覆盖当前受限原型。已有 VT-d 对指定 QEMU 配置中 RAM DMA 的读写隔离；没有 IRQ 重映射、任意设备/ACS 分组、真实硬件复位失败、在途 DMA 拔出、物理断电、多核、符号链接、在线状态迁移或完整设计验收的结论。早期计时器/IRQ 资源注入仍只是回收条件测试。

## 应用更新健康确认

在前述三十三次启动基础上，legacy 与 VT-d/modern 各增加十四次，共六十一次 VM 启动：

- 安装只提交 PENDING；重复安装拒绝，两个 installed 入口都先持久提交 TRYING，再执行，退出 0 后完成确认事务。
- 首次坏更新失败回到未安装；已有版本下，页故障、退出 42、死循环超时均回退原已确认包，笔记保留。
- 拒绝不同应用身份，拒绝正确签名但不同摘要的候选替换；重算文件/选择记录摘要的坏签名包仍拒绝。
- PENDING 可跨重启；TRYING 期间强制终止 QEMU，下一次挂载先持久回退，不自动重试坏更新。
- 开始试运行写 EIO 时不执行；恢复写 EIO 时拒绝持久化挂载；应用已退出 0 但确认写 EIO 时不输出确认或成功。解除故障后重新启动恢复。
- 旧单字节选择器经认证读取，下一次安装迁移；新记录非法保留字节即使卷摘要正确也拒绝挂载。

最终源码已构建后，`python3 tools/test.py --skip-build` 完整通过，耗时 227.14 秒，输出为 `build/update-final-test.txt`。最新结果含 `update_fault_scenarios=3132`、`update_health_qemu=true`。打包门同时要求完整更新回归和精确镜像摘要；串口输出仍在 `qemu-persist-*.log`。协议和不包含的数据副作用回滚见 `../architecture/UPDATES.md`。

## 已确认版本策略

在上一轮六十一次启动上，legacy 与 VT-d/modern 各增加七次，共七十五次。每种传输都覆盖：

- 0.2.0 已确认时，拒绝 0.1.0；精确重装 0.2.0 返回 UNCHANGED，整份磁盘字节和 generation 不变。
- 0.3.0 页故障候选失败后下限仍为 0.2.0；健康 0.3.0 确认后才提高下限。此后拒绝相同版本的不同包，重启仍保留 0.3.0。
- 四个签名正确但版本不合规范的包（缺分量、前导零、溢出、预发布后缀）均拒绝安装，磁盘不变。
- 人工构造旧格式 PENDING 状态，候选为低版本、同版本不同摘要或非法版本。installed 执行时仍拒绝，不执行入口，持久回退已确认包。
- 继续执行全部确认写 EIO、试运行中断、银行损坏和重启恢复用例；版本下限与已确认包共用同一事务结果，没有第二份计数器需要协调。

最终源码已构建后，`python3 tools/test.py --skip-build` 完整通过（271.37 秒）。结果同时具有 `version_policy_host=true`、`version_policy_qemu=true`；打包门拒绝缺失这些标记的旧结果。完整本轮日志为 `build/version-final-test.txt`。此策略不提供离线旧盘回放保护、TPM 可信计数或密钥撤销。

## 权限边界与驱动失效回归

基于 `9a25ac6` 的审查与修复见 `SECURITY.md`。新增宿主测试直接链接真实内核实现：908 条 IPC 检查覆盖端点循环创建/关闭、派生和跨任务引用、撤销唤醒、退出清理及 generation 退休；42 条内存检查覆盖跨页/只读/范围边界、MMIO 拒绝且不产生部分复制、普通 DMA RAM 正常复制。审查基线在新增两组测试分别失败，修复后全部通过。

所有启动 VM 中，非特权应用执行 128 次端点创建/关闭，退出 -7 的应用及时返回该退出码，非 canonical RSP 在返回用户态前被隔离。modern 驱动实际使用四页设备 MMIO 作为 LOG/SEND 指针，另验证拒绝的 RECV 不消费消息也不安装阻塞缓冲，随后继续正常 I/O。

在上一轮七十五次基础上增加两个可写盘 VM，共七十七次：legacy 与 VT-d/modern 均先确认应用与笔记，再以 -7 终止驱动，验证已打开笔记句柄、写入、installed、安装和状态全部失败关闭，盘字节不变，dirty 引用可以释放。先运行 bundle 应用以复用死亡任务槽，再卸载/停止/重启/重挂载驱动，恢复原数据和应用，generation 保持 5；modern 再触发真实 DMA 故障，确认隔离后的持久化入口仍拒绝缓存。

最终源码构建后，`python3 tools/test.py --skip-build` 完整通过（287.34 秒），输出 `build/security-final-test.txt`，实际日志计数为 77 个 VM。结果包含 `security_host=true`、`security_qemu=true`、`ipc_host_assertions=908`、`memory_host_assertions=42` 以及全部已有完整回归标记与镜像摘要。打包门对缺少安全标记的旧结果作拒绝测试，通过后才生成发布包。当前仅报告本地测试结果。

## 目录能力与 openat

新增 108 条宿主检查直接链接 `services/namespace.c`：目录类型、根锚定、路径逃逸/别名拒绝、所有者与 generation、读写权限衰减、UTF-8 与 95 字节边界、独立/共享偏移、父关闭后子句柄、进程回收、挂载准入和容量失败时引用回收。包测试增加第十组，覆盖签名 namespace.read 请求的十种类型/数组/精确匹配组合及错误公钥。

每次正常启动实际运行只读目录授权应用。另在上一轮 77 次 VM 基础上增加 legacy/modern 两个目录能力专项 VM，共 79 次：无授权时拒绝；runat 成功相对读文件；拒绝无请求声明、模块、缺少应用与非法目录；连续 40 次故意遗留文件的进程仍全部清理；页故障和超时后可卸载/重启/重挂载并再次读取。已有安全 VM 同时验证只读授权读取持久化笔记不改盘、已安装包不能自动获得未声明能力、驱动死亡后应用读取被拒绝。

最终产品源码构建后，`python3 tools/test.py --skip-build` 完整通过（320.67 秒），日志 `build/openat-final-test.txt`。共 108 条新增 openat 检查、950 条已有内核检查、151 条基础/存储断言、229 条 IOMMU、4069 条版本、783 个事务与 3132 个更新故障场景、15 组 Python 测试和 79 个 VM。发布门要求 `openat_host_assertions=108`、`openat_qemu=true` 及全部已有标记与精确镜像摘要；缺标记的旧结果拒绝测试通过。

## 统一截断与补零扩展

新增 83 条命名空间检查：句柄所有者/generation、写权限、只读节点与特殊对象拒绝、缩短/补零/空洞、dup 共享偏移保持、容量边界、后端失败时节点长度不变。真实 resize/事务引擎完成 474481 条检查，在缩短、扩展、清空、首次扩展四种转换的 261 个写/flush 边界分别注入三种故障，共 3132 个场景；恢复的数据只能是完整旧版或新版，三个保护文件保持原字节，同长度操作不写盘。

在上一轮 79 次 VM 上，legacy 与 VT-d/modern 各新增八次，共 95 次。验证最大容量、零扩展、清空、截断后再扩展不会暴露旧尾部、重启恢复、非法参数和同长度操作整盘不变、只读文件应用 RPC 拒绝、已安装包与更新/版本记录不变。实际 OUT/FLUSH EIO 不输出成功，卷关闭并释放 dirty 资源；银行中段暂停后终止 VM，重新启动恢复已确认文件与应用。原始构建盘和只读底区保持不变。

最终产品源码构建后，`python3 tools/test.py --skip-build` 完整通过（386.03 秒），日志 `build/truncate-final-test.txt`，机器结果包含 truncate_host_assertions=83、resize_fault_scenarios=3132、truncate_qemu=true 及全部已有标记和精确镜像摘要。打包门对缺少新增标记的旧结果作拒绝测试。详细语义及安全擦除限制见 `../architecture/TRUNCATE.md`。

## 构建时签名信任策略

新增六组宿主测试，使用与固件和服务相同的 C 验证器，覆盖多公钥、模块/应用用途、未知与撤销密钥、重复策略、8-key 上限、key_id 冒用、严格 JSON、离线查询/验证工具及 1000 次固定种子签名包变异。临时测试公钥不进入正常交付策略。

新增十次真实 VM：两次固件拒绝（应用专用密钥与有模块用途但已撤销密钥签署 bootstrap），legacy 与 modern 各四次验证重叠轮换、已确认旧密钥撤销后的失败关闭、新密钥已确认包继续执行，以及旧密钥待试运行候选撤销后回退。未知/撤销/用途错误的直接运行和安装不改盘；已确认包本身撤销时，执行和依赖旧版本认证的安装均拒绝。测试结束按原有效公开策略重建，核对 EFI、两种盘、公钥与策略摘要恢复一致。

最终源码已构建后，`python tools/test.py --skip-build` 完整通过，耗时 461.53 秒，实际 105 次启动，输出 `build/trust-final-test.txt`。包含全部原有 C 边界/故障测试和 21 组 Python 测试（3000 次变异）。结果 `build/test-results.json` 新增 trust_policy_host=true、trust_policy_host_groups=6、trust_policy_qemu=true、trust_policy_qemu_boots=10，发布门同时要求新增标记及 `trust-policy.json` 的精确摘要。规范化后的 EFI 时间戳/可选校验和亦已通过完整启动回归；生产信任链与在线策略更新仍未实现。

## 默认签名角色分离与旧应用迁移

默认模块与应用签名者改为两个不同开发公钥，实际构建的模块与应用分别按对应公钥验签。第七组信任宿主测试检查实际包、反向签名的用途拒绝及相同公钥配置拒绝；签名私钥均不进入交付文件。

信任专项现在先执行并恢复原策略，再运行全部标准 VM。legacy 与 modern 各增加两次迁移测试：显式过渡策略允许旧模块公钥的应用用途，先安装确认旧应用，再确认独立应用公钥的更高版本；严格策略重启后已确认新包继续运行，旧签名直接运行/安装被拒绝且整盘字节不变。原有应用用途/撤销 bootstrap 拒绝改用当前真实应用签名者，模块用途应用拒绝改用当前真实模块签名者，外部密钥轮换和撤销回归保留。

最终源码构建后，完整 `python tools/test.py --skip-build` 通过，耗时 486.45 秒，共 109 次实际启动、22 组 Python 测试与 3000 次变异，日志 `build/separate-keys-final-test.txt`。结果为 trust_policy_host_groups=7、trust_policy_qemu_boots=14、separate_signing_keys=true，并绑定 EFI、两种盘、两个公钥与公开策略六个摘要；发布门拒绝旧标记或文件摘要不一致。原始模块公钥不变，四轮策略构建保持两份现有种子，最终交付恢复为原有效策略，未遗留测试 VM。

## Native 参数 ABI 与命令行

新增 8155 项参数 C 检查，验证 512 字节边界、1000 次固定种子往返、规范偏移/终止、数量和长度破坏、头部别名、引用/空串/转义及非法词法。真实 SYS_SPAWN 负例检查非法数量和指向头部的参数偏移，均在创建子进程前拒绝。

legacy 与 modern 各新增一个参数 VM：带参数的普通、目录委派、installed 调用均通过；未提供额外参数保留旧布局。写参数页触发隔离页故障，随后回显及十次进程重用正常。非法引号/数量在 PENDING 更新试运行前拒绝，磁盘不变；有效参数调用完成健康确认，参数本身不落盘。所有目录及描述符可正常回收并卸载。

最终源码构建后完整 `python tools/test.py --skip-build` 通过，547.21 秒、111 次实际启动，日志 `build/arguments-final-test.txt`。全部已有签名、设备、内核、命名空间与事务/故障回归通过，交付六个摘要一致。结果和发布门增加 arguments_host_checks=8155、arguments_qemu=true；协议与尚未冻结的装载计划 ABI 见 `ARGUMENTS.md`。

## 显式环境与调用隔离

新增 12034 项 C 环境检查，覆盖 1000 次规范往返、8+8 数量/512 字节总界限、名称与重复项、空值/缺失、偏移/保留字段和失败时输出不变。旧 8155 项参数检查保留。

legacy/modern 各新增一个 VM：runenv/runenvat、引号空串与字面量、环境查值、无环境继承、私有只读页写故障后的隔离与回收、installed 健康确认、非法环境在 TRYING 之前拒绝且磁盘不变。环境字符串不写入持久化记录；内核复制规范字节，SDK 另校验环境名称和重复项。

最终源码构建后完整 `python tools/test.py --skip-build` 通过（602.82 秒、113 次启动），日志 `build/environment-final-test.txt`。原有签名、内核、设备、存储及全部故障回归通过；六项交付摘要一致。结果和发布门增加 environment_host_checks=12034、environment_qemu=true。

## 位置无关 native 与相对重定位

新增六组宿主测试，直接调用真实包解析器和用户态映像准备器，验证精确页字节、容量失败不改输出、三个合法基址、0/1/256 项、非法目标/加数/子表、ELF 导入/REL/RELR 拒绝及 500 次已签名记录变异。每个正常启动实际并发两个独立 PI 进程；复现的共享 native 回复端点串扰改为独立端点并在重启时撤销重建。

legacy/modern 各增加首次启动及重启，共四次 native VM：两种实际基址，数据/函数/BSS 修正、代码页写故障、argv/env、签名正确的非法重定位拒绝且不改盘、PI 安装健康确认、重复运行回收与重启继续运行。

最终源码构建后完整 `python tools/test.py --skip-build` 通过（550.52 秒、117 次启动），日志 `build/native-final-test.txt`，包含 28 组 Python 测试/3500 次变异和全部旧回归。六项交付摘要一致；发布门增加 native_relative_host_groups=6、native_relative_mutations=500、native_relative_qemu=true、native_relative_qemu_boots=4，并验证旧结果缺标记时拒绝打包。完整范围见 NATIVE.md。

## 文件 RPC 的独立长度边界

新增 353 项 C 检查直接链接产品 file_rpc.c 和 namespace.c，包括数据区前后保护字节、合法 256 字节读取/写入、大目录的有界枚举、257/512/2^31/UINT32_MAX 拒绝、不触及后端/控制台、不改变偏移或节点，以及只读子句柄和根关闭后子文件保持。

旧产品分发器提取基线在 257 字节枚举应拒绝处失败，日志 build/file-rpc-baseline-test.txt。宿主绕过了内核传输前置条件；现有 SYS_SEND 已拒绝 length>256，这不是曾能通过正常 IPC 触发的系统漏洞。

rpcprobe.sfk 在 legacy/modern 各一个 VM 检查四种超长请求直接在发送处返回 SF_INVALID；之后完整读取大于 256 字节的磁盘文件、正常写控制台、以只读权限访问持久笔记，连续十二次调用后仍可卸载和停止驱动。VM 并不声称超长请求进入服务后才拒绝；服务自身的边界由上述独立宿主检查覆盖。

最终源码构建后完整 `python tools/test.py --skip-build` 通过（562.15 秒、119 次启动），日志 `build/file-rpc-final-test.txt`。全部既有回归、28 组 Python 测试/3500 次变异和六个交付摘要一致。发布门增加 file_rpc_host_checks=353、file_rpc_qemu=true、file_rpc_qemu_boots=2，缺少标记的旧结果拒绝打包。

## 块设备私有回复与冷并发读

旧通道在真实 legacy VM 的 channel-test 中将两个只读应用的冷读取都误判失败，退出码 13/13，日志 `build/qemu-persist-block-channel-baseline.log`；磁盘未变且可正常关机。修复后 native、块设备与应用 namespace 回复分离，块设备每次实例均重新建立回复端点。

新增 legacy/modern 两个 VM，每种传输八次并发冷读、卸载、停止、启动、挂载，检验目录/文件/任务/端点引用回收、驱动回复代次与持久字节保持。完整 `python tools/test.py --skip-build` 通过（580.69 秒、121 次启动），日志 `build/block-channel-final-test.txt`，原有全部回归继续通过。结果和发布门新增 block_reply_channel_qemu=true、block_reply_channel_qemu_boots=2，六项镜像/公钥/策略摘要一致；旧结果缺标记时拒绝发布。

## Echo 每实例回复与实际分配失败

73108a7 基线的真实 echo-channel-test 复现 e=1/a=2/b=0，原共享 manager 队列消费应用请求；日志 build/qemu-persist-echo-channel-baseline.log。修复采用每实例私有回复端点并检查 PID、service、发送者及请求 ID，终止后撤销。

新增 legacy/modern 两个 VM，各四十次并发冷目录读取与 echo 请求、停止/启动和重新挂载。echo-capacity-test 实际耗尽能力槽，分别触发服务端点及私有回复分配失败，确认状态退回 STOPPED、已分配对象回收、释放后重新启动成功。正常启动仍验证新旧代并存，磁盘字节与笔记保持。

最终源码重建后完整 python tools/test.py --skip-build 通过（628.35 秒、123 次启动），日志 build/echo-final-resumed-test.txt；全部原有回归、28 组 Python 测试/3500 次变异和六项交付摘要通过。结果与发布门新增 echo_reply_channel_qemu=true、echo_reply_channel_qemu_boots=2。此前额度中断的批次不是完整通过结果。

## 认证包类型与启动意图

基线 run echo.sfmod 无附加参数会等待应用期限后返回 SF_TIMEOUT=9，原始日志 build/qemu-persist-launch-kind-baseline.log。专项使用正确密钥签署的两种有效包：把 APP 放入启动模块位置时拒绝 code=2 并继续启动；把 MODULE 改名 .sfk 仍拒绝应用运行；有效 APP 使用 .sfmod 后缀仍由 native 成功运行。

legacy/modern 各十二轮，每轮七种拒绝（含无参数、runat、runenv、runenvat、native 与设备模块），随后正常运行普通和 PI 应用、echo；错误安装仍由既有策略拒绝，盘字节保持并能卸载/停止。最终源码构建后完整 python tools/test.py --skip-build 通过（687.44 秒、125 次启动），日志 build/launch-kind-final-test.txt；全部原有回归和六项交付摘要一致。发布门要求 launch_kind_qemu=true、launch_kind_qemu_boots=2，旧结果缺失时拒绝发布。

## M2 依赖代次和租约事务

旧 manager.c 在独立宿主替换提供者槽后，错误允许已停止消费者绑定新登记，基线在该断言失败，日志 build/manager-dependency-baseline.txt。新测试直接链接产品 manager.c，共 25080 项检查、一千次相同/不同 ID 的提供者替换和回收，以及同一登记重启、非法位图、代次耗尽、多提供者计数溢出与停止不平衡时的全状态保持。普通应用尚未开放这些管理器内部 API。

每次正常 VM 启动执行独立管理器图的真实替换测试，输出 DEPENDENCY GENERATION TESTS PASSED，再继续系统服务与设备/应用测试。最终源码构建后完整 python tools/test.py --skip-build 通过（678.98 秒、125 次启动），日志 build/manager-dependencies-final-test.txt；全部原有回归及六项交付摘要一致。发布门要求 manager_dependency_host_checks=25080、manager_dependency_qemu=true，旧结果拒绝测试通过。协议边界见 LIFECYCLE.md。

## Echo 死亡与有界自动恢复

97101 项 C 策略检查直接链接产品 supervisor.c，覆盖一千条固定种子重启轨迹、退避前拒绝/期限到达、最多十六次配置边界、重复 begin、启动失败、显式停止/重试、稳定时间的前一 tick/精确边界、时钟回退以及 UINT64_MAX 饱和。

新增 legacy/modern 两个 VM，通过 root 诊断实际终止 echo PID。前三次自动重启且继续 ping，第四次不稳定死亡进入 BLOCKED；shell、普通和 PI 应用、持久笔记继续工作。显式启动重置预算，手停后等待也不会复活，连续运行到稳定标志后再次故障获得新预算；重复十二次手动停启验证资源回收，持久字节保持。

最终源码构建后完整 python tools/test.py --skip-build 通过（736.3 秒、127 次启动），日志 build/echo-supervision-final-test.txt；全部原有回归及六项交付摘要一致。发布门要求 supervisor_host_checks=97101、supervisor_qemu=true、supervisor_qemu_boots=2，缺标记的旧结果拒绝发布。这里检测实际死亡，不宣称已检测存活但挂起的服务。

## Echo 周期健康、挂起和认证回复协议

新增 legacy/modern 两个 VM，通过 root 专用请求使当前 echo 真正进入可抢占自旋；100 ms 接收期限后，监督回收旧进程并按已有预算重启。另让同一认证实例的下一次应答返回 SF_DENIED 或错误操作码，即使发送者/请求 ID/载荷正确也触发失败。前三次故障恢复，第四次挂起进入 BLOCKED；显式重试后普通/PI 应用、笔记、六轮冷目录并发读和卸载/停止仍通过，磁盘保持原字节。

最终源码构建后完整 python tools/test.py --skip-build 通过（761.42 秒、129 次启动），日志 build/echo-health-final-test.txt。原有死亡预算/稳定周期、97101 项策略、25080 项管理器和全部已有回归继续通过，六项交付摘要一致。发布门要求 echo_health_qemu=true、echo_health_qemu_boots=2，缺少标记的旧结果拒绝发布。覆盖内置 echo 的指定协议，不替代通用服务语义或硬件健康验证。

## Native 事务启动、租约排空与有界恢复

新增 legacy/modern 两个 VM：实际耗尽根能力槽，分别触发回复端点与服务端点分配失败并回到 STOPPED；共享空闲接收端点创建真实服务进程直至进程容量不足，确认根端点槽仍有空间，再验证 native 创建进程失败回收两个端点。释放后成功重建并继续执行应用。

native-drain-test 持有两个真实读取应用：忙显式停止保留 ACTIVE；终止实际 native 后进入 DRAINING、拒绝新启动，原应用文件 RPC 仍成功，租约释放后后台恢复。另以死亡、自旋、错误健康状态触发三次恢复，第四次错误操作码进入 BLOCKED；停止时不会复活，显式启动后静态/PI/环境/目录应用继续工作，十二次停止/启动回收通过，整盘字节不变。

最终源码构建后完整 python tools/test.py --skip-build 通过（778.66 秒、131 次启动），日志 build/native-lifecycle-final-test.txt；97101 项共享监督策略、25080 项管理器和全部原有回归继续通过，六项交付摘要一致。发布门要求 native_supervision_qemu=true、native_supervision_qemu_boots=2，旧结果缺失时拒绝发布。虚拟机不验证设备自动重启、通用服务状态迁移或生产硬件复位。

## Core 创建路径的 PID 有符号边界

宿主直接包含实际 kernel/core.c，替换页帧/页表与硬件调用，注入 next_pid=0x80000000。基线创建了 READY 任务却向调用方返回负值，记录 build/task-pid-baseline.txt。修复后 5022 项检查覆盖一千个固定种子状态、INT32_MAX-1/INT32_MAX 的正值成功和 BootArgs PID、后续永久拒绝且计数/整个任务表/页帧与地址空间调用均不变。另验证分配失败消耗旧标识、新创建不复用，以及非法计划、任务表满和 root-only 准入。

宿主为 LLP64，仅执行创建路径。TASK_STATUS 的 guest LP64 哨兵在该宿主会触发常量收窄诊断；测试对包含的 Core 关闭这一项警告，并明确不执行或验证该分支。本测试不代替真实页表/调度验证，也未在 VM 重复二十亿次创建。实际 guest 的正常/失败创建、native 进程容量与全部硬件回归由完整 VM 套件覆盖。

最终源码构建后完整 python tools/test.py --skip-build 通过（781.16 秒、131 次启动），日志 build/task-pid-final-test.txt；全部既有回归及六项摘要一致，发布门要求 task_pid_host_checks=5022，缺标记旧结果拒绝打包。

## 已安装调用预检与坏候选回退

基线真实 VM 中，runat /disk/etc installed 对无 namespace.read 的有效候选返回 DENIED，但已经 TRIAL/ROLLED BACK，候选丢失；日志 build/qemu-persist-invocation-preflight-baseline.log。新增 legacy/modern 各两次、合计四次 VM：0.1.0 初装和 0.2.0 升级时，runat/runenvat 能力拒绝、native 停止时 run/runenv 拒绝均保持整盘原字节和 PENDING；正确重试才健康确认。

随后安装 0.3.0 并重启，离线修改候选且修复事务校验、保留原选择器候选摘要。runat 的预览错误继续进入既有 TRIAL/ROLLBACK，拒绝坏候选并保留已确认 0.2.0；笔记和根/租约回收正常。原始构建盘保持原字节。

最终源码构建后完整 python tools/test.py --skip-build 通过（802.5 秒、135 次启动），日志 build/invocation-preflight-final-test.txt。5022 项 Core、97101 项监督、25080 项管理器及全部既有回归继续通过，六项摘要一致；发布门要求 invocation_preflight_qemu=true、invocation_preflight_qemu_boots=4，旧结果拒绝测试通过。

## RAM删除、忙引用与节点复用

17340项产品namespace/file_rpc检查覆盖只读/挂载边界、跨进程和dup忙引用、完整规范路径/Unicode/长度、畸形消息保护字节与无副作用，以及一千次数据清零、节点复用和旧句柄失效。未增加普通应用可写目录授权，真实fileprobe/readprobe以只读根发起OP_UNLINK，必须返回DENIED。

新增legacy/modern两次VM，每次四十次创建/删除，验证root shell的rm/mkdir、引号、只读与特殊对象拒绝、目录/非法路径/额外参数拒绝、真实应用IPC拒绝、笔记与盘字节保持和卸载/停止。最终完整python tools/test.py --skip-build通过（966.29秒、137次启动），日志build/ram-unlink-final-test.txt，原有全部回归和六项摘要通过。旧135VM结果被发布门实际拒绝；新结果要求unlink_host_assertions=17340、unlink_qemu=true、unlink_qemu_boots=2。

## 文件句柄规范值与固定元数据

旧分发器宿主基线build/file-rpc-high-handle-baseline.txt复现高位CLOSE关闭自己的有效目录但未清除根记录：OP_ROOT仍成功，openat实际STALE。不是跨进程提权。修复先拒绝高32位，再进行原所有者/代次检查；根记录与后端均保持。

6180项产品C宿主检查覆盖所有消费句柄操作的三类高位、规范长度、非空STAT输入拒绝、关闭/回收/错误所有者、保护字节及无副作用；一千次截断/定位后验证独立与dup偏移、当前大小，查询无后端I/O，目录/管道/控制台/只读及已失效磁盘的缓存元数据语义正确。

新增legacy/modern两个VM，实际fileprobe/readprobe发出高位READ/CLOSE，确认拒绝后根、偏移和正常读取保持；还验证32字节记录、根关闭后STALE、shell各种类型/引号/畸形路径、四十次查询及十二轮进程回收。持久笔记/盘字节和卸载停止保持。最终完整python tools/test.py --skip-build通过（965.9秒、139次启动），日志build/file-stat-final-test.txt，全部旧回归和六项摘要通过。发布门要求stat_host_assertions=6180、canonical_file_handles=true、stat_qemu=true、stat_qemu_boots=2，实际拒绝旧137VM结果。

## 内核能力值与浮点上下文

内核高位能力宿主基线直接调用原ipc.c，复现自己的能力被高位SEND/CLOSE/DERIVE/REVOKE当作别名，不宣称跨所有者提权。最终实际产品40908项IPC断言覆盖三类高位、五个操作、无用户复制/任务状态变化、一千次分配回收及合法IPC保持。两个新增legacy/modern VM各四十次真实签名probe进程，保留普通/PI/argv/env/目录文件调用与持久字节。

CPU基线在c565c40的两种真实VM中以连续签名应用复现XMM0跨退出泄漏。实际task_new模型新增一千次带脏FP槽复用，完整512字节初始映像和对齐检查使总数为8022。两个新增max CPU VM实际宣告AVX/XSAVE硬件但OSXSAVE关闭；每种十二组同时存活应用、每任务不同x87/MMX/XMM0–15与舍入/状态，经历yield及非主动让出的TSC循环；AVX/x87/待处理异常与超时仅回收原任务，随后新任务清零且服务/持久字节保持。

本地QEMU11.0.92 TCG对未屏蔽SSE 0/0仅记录0x1f01而无#XM。测试另外检查该标志跨yield保持，将两种传输的status-only观察写入总结果，不把它称为真实SIMD异常交付已通过。非零异常指针完整恢复受模型限制；恢复前清零环境，初始指针以FNSTENV实测；真实CPU语义仍待扩展验证，见CPU_CONTEXT.md。

最终完整python tools/test.py --skip-build通过（1074.85秒、143次启动），日志build/kernel-cpu-isolation-final-test.txt。全部既有宿主/VM回归及六项交付摘要一致；新版发布门拒绝缺少新标记的139VM结果，并要求实际能力/FP两组VM与SIMD观察记录。

## RAM空目录回收

11359项实际产品namespace/file_rpc宿主检查覆盖目录写权限、当前只读/挂载父节点和目标、跨所有者/dup忙引用、文件/子目录/深层后代非空拒绝、相邻前缀名称、UTF-8/长度/规范路径、高位与畸形请求无副作用；一千次目录及子文件创建/回收保证已关闭句柄仍失效。原17340项unlink及6180项元数据检查继续通过。

新增legacy/modern两个实际VM，各四十次目录/子文件回收，检查带引号路径、嵌套非空和相邻前缀、根/特殊/只读/挂载节点拒绝；十二轮fileprobe/readprobe发出真实RMDIR，确认只读根先返回DENIED、之后正常读取与引用回收仍可用。笔记、完整持久盘字节及卸载/停止保持。

最终完整python tools/test.py --skip-build通过（1166.82秒、145次启动），日志build/ram-rmdir-final-test.txt，全部旧回归及六项交付摘要一致。发布门实际拒绝旧143VM结果，要求rmdir_host_assertions=11359、rmdir_qemu=true、rmdir_qemu_boots=2。

## 外部助手审查

按用户授权调用已有 DeepSeek Harness。首次宽范围审查超时，没有采用其未完成输出；随后以低推理档审查一个文件写函数，11 秒完成且未使用工具。核实并修复空写返回错误，增加容量防御检查及对应宿主断言。审查回执在 `.cache/deepseek/runs/`，不包含提交或推送权限。

M5 再次使用低推理档，只审查设备授权 C 文件。核实其“旧驱动释放后仍映射 DMA 区”的发现，采用保留拥有者至地址空间销毁的方案，并增加真实双实例授权负例。其余意见分别由失败任务的完整地址空间回收和明确隔离保留策略覆盖，没有把所有建议直接当作已确认漏洞。

M6 尝试通过工作区桥调用 deepseek-flash 审查事务引擎。两次调用因 permission preset 初始化失败而分别超时，没有返回有效结果或工具调用；未采用任何未完成输出。实现由本次代码检查、共用 C 引擎故障测试和 QEMU 回归验证，不称为已经通过外部助手审查。

## PR #10 与主分支整合（2026-10-24）

重新构建后完整 `python tools/test.py --skip-build` 通过（1001.23 秒、149 次实际 VM 启动），日志 `build/pr10-merge-final-full.txt`。保留此前全部用途/轮换、native/调用、服务监督、文件、内核能力与 CPU 上下文回归，另含主分支持久化运行期撤销的四次 VM。七项交付摘要（EFI、两个盘、三个公钥、公开策略）与临时策略测试前逐项一致。

宿主包括 29 条撤销容量/幂等/隔离检查；29 组 Python 测试含用途策略与运行期拒绝层组合、已撤销签名者的签名损坏仍拒绝、不可变撤销不被空覆盖层恢复。事务/更新/截断三类故障场景保持通过，新增第五个事务键及数据布局使用主分支版本。备用 APP 密钥夹具替换已有磁盘条目，保留原 32 节点配额及持久挂载空间。

所有测试/VM 已结束，发布门要求两套信任测试及七项精确摘要。Windows CI 限时调整为 45 分钟以容纳完整套件；本地通过不代表托管 CI 已完成。先前 SDK 中止的套件与本轮交付无关，未混入合并。

## M7 网络纵向切片（2026-10-30）

宿主 `test_net` 直接链接实际 `services/net_proto.c`，共 14084 条检查：严格点分十进制解析/格式化拒绝空段、前导零、超界、非数字与越界字节；ARP 请求长度/固定前缀/目标 IP 及回复校验；IPv4 头长度/校验和/协议/源地址与 ICMP echo 校验；UDP 构包（以太网 14 + IPv4 20 + UDP 8 + 载荷、伪头校验和、缺省校验和合法而错误校验和拒绝）与解包（长度/端口/源地址/分片/截断拒绝）；TCP 构包（20 字节头、data offset、序号/确认/标志/窗口、强制校验和）与解包（data offset 下界与越界、端口、分片、协议、截断与篡改校验和拒绝）；另含 ICMP、UDP、TCP 各一千组固定种子随机往返与超长载荷拒绝。未覆盖真实网卡收发，网卡路径由 QEMU 场景验证。

新增 `tests/qemu/qemu_net_test.py`：128 MiB 无盘启动，`-netdev user,id=net0` 加 legacy virtio-net（`disable-modern=on,disable-legacy=off`），断言启动出现 `net: READY` 且没有存储就绪。串口依次执行 `ifconfig`（校验 `10.0.2.15/24`、网关 `10.0.2.2`、`255.255.255.0` 与 `link=up`）、`ping 10.0.2.2`（真实 ARP 后 ICMP 往返）、再次 `ifconfig`（正则校验 MAC `52:54:00:12:34:56` 与 RX/TX 计数增长）、再次 ping、`udp 10.0.2.2 56001 hello` 与 `tcp 10.0.2.2 56002 hello`（宿主 `127.0.0.1` 上的 UDP/TCP 回显服务经 slirp 映射往返 `ACK:hello`），最后 `shutdown` 并以退出码 1 结束。此前真实基线显示 RX 未跳过 10 字节 legacy `virtio_net_hdr`、TX 未添加该头，导致 ARP 可见而 ICMP 超时；修复后往返通过。

本轮 TCP 首次实测中，握手成功但回复超时：等待循环把“纯 ACK（0 载荷）”当成结束条件立即退出，导致对端数据段被丢弃；改为纯 ACK 继续等待、RST/FIN 才终止后才通过。该修复只在真实 QEMU 往返中暴露，宿主边界测试无法覆盖。

协议扩展的验证结果：`python3 tools/test.py --skip-build --host-only` 通过（47.55 秒），`net_host_checks=14084`；`python3 tests/qemu/qemu_net_test.py` 通过并输出 `NETWORK QEMU PASS (rx/tx advanced, ARP, ICMP, UDP and TCP over slirp)`，日志 `build/qemu-net.log`。限制：网络只在无盘启动接入，modern 网卡与 VT-d 隔离模式拒绝 legacy 设备，轮询无 IRQ，无 DHCP、无连接对象与 `/net` 命名空间视图，UDP/TCP 均为单次有界交换；图形、用户会话、多核与服务状态迁移未实现。本轮未重跑 150 VM 完整回归，M7 切片记录见 PROGRESS 第 66、67 项。

## 文件客户端 SDK 与作者修复基线（2026-10-24）

基于已合并 main f2e41bd / d6a71f2，执行完整 python tools/test.py（包含初始构建），1086.09 秒、151 次真实 VM 启动全部通过。该入口保留作者的独立构建进程，密钥轮换重建与默认策略恢复也实际完成，没有使用 --skip-build 绕开 Windows DLL 占用路径。

新增 SDK 有 416510 项产品 C 编解码宿主检查，替换 INT80 传输并与真实 namespace/file_rpc 组合；涵盖一千条正常序列、保护字节、畸形身份/头部/编号/操作码/计数/元数据、固定绝对期限、有界旧回复、错误不交付输出、重新初始化不复用编号、末次 64 位编号和耗尽无发送。边界计数由宿主注入，不宣称 guest 执行 2^64 次调用。

新增 legacy/modern 两个 VM 各六十次生产 SDK 应用：普通 run 无文件根，显式目录保持只读；验证 console、openat/read/write/seek/poll/enumerate/stat/close、拒绝截断/删除和父关闭后子文件可用、回收及原始盘字节保持。全部既有 CPU 隔离、服务监督、信任、更新、版本、DMA 和文件回归通过；七项精确交付摘要校验一致。

完整日志 build/file-client-author-main-full.txt，结果 build/file-client-author-test-results.json。打包门要求 file_client_host_assertions=416510、file_client_qemu=true、file_client_qemu_boots=2，旧的 149 VM 结果不能打包本轮成果。本地完整通过与新 PR 托管检查分别核对。

## 原始事务几何导入与首次升级（2026-10-24）

真实 8b704ae 事务引擎写出 generation 2 的 legacy-notes 后，旧的新读取器返回空 generation 1 / NOT_FOUND；修复后同一样本只读恢复 generation 2 / 完整 12 字节笔记，样本摘要不变。该对照记录在 .cache/txn-compat-baseline.json 与 .cache/txn-compat-fixed-reader-result.json。

永久宿主回归使用独立旧格式编码器，2046559 项检查覆盖一千个四槽/混合几何快照、0/1/127/128/129/16383/16384 字节、所有四槽、最高代次选择、同代次歧义、修复外层摘要后的内部摘要/路径/重复键/保留字节/未用槽/尾部拒绝、挂载字节不变和五键写出。首次旧转新提交在 261 个写/flush 边界注入三类故障，共 783 场景；恢复只能得到完整旧四键或完整新五键，未确认事务关闭后续准入。

六次新增 QEMU（每种传输三次）先准备已确认 0.2.0 应用和双槽/笔记，离线转为精确四记录旧几何；稳定挂载和运行 installed 不改任何字节。普通笔记写出迁移后的新几何，双槽/选择器保持原字节，再追加备用应用签名者的撤销键作为第五个文件。重启后笔记、版本、撤销保持，正常应用签名者仍可运行，原构建盘/只读底区不变。

完整 python tools/test.py 包含初始构建通过（1112.0 秒、157 VM），日志 build/txn-compat-final-full.txt，结果 build/txn-compat-test-results.json。全部 SDK、内核、CPU、服务、签名/用途/撤销、DMA、目录及旧事务/更新/截断回归与七项交付摘要一致。发布门要求本轮宿主计数、783 故障场景及六次 VM 标记；托管检查仍按最新提交单独核对。

## Windows SDK 宿主测试修复（2026-10-31）

读取 main 的 Actions 原始日志：Windows #54（run 41173097613）因 LLP64 的 32 位 long 断言无法编译，作者已在 f085ef1 修复；最新 #55（run 41189419413，e74cf26）构建完成后停在 `test_sdk.c` 的 strncmp 断言。本机使用同样的 `x86_64-windows-gnu -O2 -Wall -Wextra -Werror` 复现。逐项修正后又实际复现了分词重启和 `0xg` 结束偏移差异，没有把编译通过当成运行通过。

比较函数只比较结果的符号；不完整 `0x`/`0X`、正负号及前置空白使用独立的值/结束偏移向量；bsearch 对重复键核对返回值确实属于输入数组并等于 key，不要求两套实现选择相同副本。依据为 [WG14 N1570 草案](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf) 的 7.24.4、7.22.1.4 和 7.22.5.1。strtok_r 只在首次调用传入文本，其后始终传 NULL，保存指针不用于决定重新开始；调用约定见 [GNU libc 分词说明](https://www.gnu.org/software/libc/manual/2.23/html_node/Finding-Tokens-in-a-String.html)。SDK 产品实现保持原有语义。

新增 196608 条字节对/长度检查、4 条终止/前缀检查、7 条确定性分词检查、28 条不完整十六进制解析检查、104 条二分查找边界检查，保留全部原固定种子用例，总数 368490。Windows 的 `-O2`、`-O0 -fno-builtin`、`-O2 -fno-builtin -fsanitize=undefined -fno-sanitize-recover=all` 均实际运行通过，日志分别为 `build/sdk-ci-final.log`、`build/sdk-ci-unoptimized.log`、`build/sdk-ci-sanitized.log`。

本机 `python tools/test.py --host-only` 包含初始构建，175.29 秒通过（25 个 C 宿主程序、30 组 Python 用例）；日志 `build/windows-ci-host-20261031.log`，结果 `build/windows-ci-host-20261031-results.json` 记录七项交付摘要。此运行没有执行 QEMU 或发布打包，托管 Windows 完整套件及 Linux 宿主检查按本次修复提交单独核对，旧红色运行属于其历史提交。

## M0 清单源文件与独立 schema（2026-10-31）

`tests/integration/test_manifest_profile.py` 的 12 组专项通过，含 2000 组固定种子版本输入与独立 uint32 数值判据、schema 搜索语义下的终点、标识符/种类/ABI/能力与内存声明边界、JSON 重复键和非法 Unicode/数字、精确 256 KiB / 4096 节点 / 16 层 / 64 KiB 编码边界。隔离副本内没有 build、密钥或 C 共享库时仍可导出 schema 并验证两类源清单。

真实 CLI 用公开固定测试种子打包 static APP、relative-v1 APP 与模块，全部通过产品 C 验证，输出与原程序接口逐字节相同。默认 CLI 保留额外元数据；严格源模式关闭未知字段。畸形 JSON/源约定/编码越界在缺失 ELF 和种子之前报错，输出保持；声明内存低于实际映像时在真实打包阶段拒绝。专项日志 `build/manifest-profile-focused-20261031.log`。

包含初始构建的 `python tools/test.py --host-only` 完整通过；schema 按仓库 LF 行尾规范化后的最终运行耗时 76.11 秒（25 个 C 程序、42 组 Python 用例），日志 `build/manifest-profile-host-final-lf-20261031.log`。七项 EFI/磁盘/公钥/策略摘要与已验证的作者基线一致；本次本地未重复 VM。测试结果记录 schema 摘要，发布门核对此摘要及新宿主测试标记并把独立 schema 放入 ZIP；Windows 完整套件与 Linux CI 仍须按新提交核对。
