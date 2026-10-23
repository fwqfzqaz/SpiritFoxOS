# 测试记录

日期：2026-10-23；宿主 Windows x64；Zig 0.13.0；QEMU TCG、q35、qemu64+NX、单核。

执行 `python tools/test.py`，构建时启用 `-Wall -Wextra -Werror`；自动测试完整运行于 128、256、512 MiB。输出日志与机器结果保存在忽略提交的 `build/`。

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

最终源码已构建后，`python tools/test.py --skip-build` 完整通过，耗时 227.14 秒，输出为 `build/update-final-test.txt`。最新结果含 `update_fault_scenarios=3132`、`update_health_qemu=true`。打包门同时要求完整更新回归和精确镜像摘要；串口输出仍在 `qemu-persist-*.log`。协议和不包含的数据副作用回滚见 `UPDATES.md`。

## 已确认版本策略

在上一轮六十一次启动上，legacy 与 VT-d/modern 各增加七次，共七十五次。每种传输都覆盖：

- 0.2.0 已确认时，拒绝 0.1.0；精确重装 0.2.0 返回 UNCHANGED，整份磁盘字节和 generation 不变。
- 0.3.0 页故障候选失败后下限仍为 0.2.0；健康 0.3.0 确认后才提高下限。此后拒绝相同版本的不同包，重启仍保留 0.3.0。
- 四个签名正确但版本不合规范的包（缺分量、前导零、溢出、预发布后缀）均拒绝安装，磁盘不变。
- 人工构造旧格式 PENDING 状态，候选为低版本、同版本不同摘要或非法版本。installed 执行时仍拒绝，不执行入口，持久回退已确认包。
- 继续执行全部确认写 EIO、试运行中断、银行损坏和重启恢复用例；版本下限与已确认包共用同一事务结果，没有第二份计数器需要协调。

最终源码已构建后，`python tools/test.py --skip-build` 完整通过（271.37 秒）。结果同时具有 `version_policy_host=true`、`version_policy_qemu=true`；打包门拒绝缺失这些标记的旧结果。完整本轮日志为 `build/version-final-test.txt`。此策略不提供离线旧盘回放保护、TPM 可信计数或密钥撤销。

## 权限边界与驱动失效回归

基于 `9a25ac6` 的审查与修复见 `SECURITY.md`。新增宿主测试直接链接真实内核实现：908 条 IPC 检查覆盖端点循环创建/关闭、派生和跨任务引用、撤销唤醒、退出清理及 generation 退休；42 条内存检查覆盖跨页/只读/范围边界、MMIO 拒绝且不产生部分复制、普通 DMA RAM 正常复制。审查基线在新增两组测试分别失败，修复后全部通过。

所有启动 VM 中，非特权应用执行 128 次端点创建/关闭，退出 -7 的应用及时返回该退出码，非 canonical RSP 在返回用户态前被隔离。modern 驱动实际使用四页设备 MMIO 作为 LOG/SEND 指针，另验证拒绝的 RECV 不消费消息也不安装阻塞缓冲，随后继续正常 I/O。

在上一轮七十五次基础上增加两个可写盘 VM，共七十七次：legacy 与 VT-d/modern 均先确认应用与笔记，再以 -7 终止驱动，验证已打开笔记句柄、写入、installed、安装和状态全部失败关闭，盘字节不变，dirty 引用可以释放。先运行 bundle 应用以复用死亡任务槽，再卸载/停止/重启/重挂载驱动，恢复原数据和应用，generation 保持 5；modern 再触发真实 DMA 故障，确认隔离后的持久化入口仍拒绝缓存。

最终源码构建后，`python tools/test.py --skip-build` 完整通过（287.34 秒），输出 `build/security-final-test.txt`，实际日志计数为 77 个 VM。结果包含 `security_host=true`、`security_qemu=true`、`ipc_host_assertions=908`、`memory_host_assertions=42` 以及全部已有完整回归标记与镜像摘要。打包门对缺少安全标记的旧结果作拒绝测试，通过后才生成发布包。当前仅报告本地测试结果。

## 目录能力与 openat

新增 108 条宿主检查直接链接 `services/namespace.c`：目录类型、根锚定、路径逃逸/别名拒绝、所有者与 generation、读写权限衰减、UTF-8 与 95 字节边界、独立/共享偏移、父关闭后子句柄、进程回收、挂载准入和容量失败时引用回收。包测试增加第十组，覆盖签名 namespace.read 请求的十种类型/数组/精确匹配组合及错误公钥。

每次正常启动实际运行只读目录授权应用。另在上一轮 77 次 VM 基础上增加 legacy/modern 两个目录能力专项 VM，共 79 次：无授权时拒绝；runat 成功相对读文件；拒绝无请求声明、模块、缺少应用与非法目录；连续 40 次故意遗留文件的进程仍全部清理；页故障和超时后可卸载/重启/重挂载并再次读取。已有安全 VM 同时验证只读授权读取持久化笔记不改盘、已安装包不能自动获得未声明能力、驱动死亡后应用读取被拒绝。

最终产品源码构建后，`python tools/test.py --skip-build` 完整通过（320.67 秒），日志 `build/openat-final-test.txt`。共 108 条新增 openat 检查、950 条已有内核检查、151 条基础/存储断言、229 条 IOMMU、4069 条版本、783 个事务与 3132 个更新故障场景、15 组 Python 测试和 79 个 VM。发布门要求 `openat_host_assertions=108`、`openat_qemu=true` 及全部已有标记与精确镜像摘要；缺标记的旧结果拒绝测试通过。

## 统一截断与补零扩展

新增 83 条命名空间检查：句柄所有者/generation、写权限、只读节点与特殊对象拒绝、缩短/补零/空洞、dup 共享偏移保持、容量边界、后端失败时节点长度不变。真实 resize/事务引擎完成 474481 条检查，在缩短、扩展、清空、首次扩展四种转换的 261 个写/flush 边界分别注入三种故障，共 3132 个场景；恢复的数据只能是完整旧版或新版，三个保护文件保持原字节，同长度操作不写盘。

在上一轮 79 次 VM 上，legacy 与 VT-d/modern 各新增八次，共 95 次。验证最大容量、零扩展、清空、截断后再扩展不会暴露旧尾部、重启恢复、非法参数和同长度操作整盘不变、只读文件应用 RPC 拒绝、已安装包与更新/版本记录不变。实际 OUT/FLUSH EIO 不输出成功，卷关闭并释放 dirty 资源；银行中段暂停后终止 VM，重新启动恢复已确认文件与应用。原始构建盘和只读底区保持不变。

最终产品源码构建后，`python tools/test.py --skip-build` 完整通过（386.03 秒），日志 `build/truncate-final-test.txt`，机器结果包含 truncate_host_assertions=83、resize_fault_scenarios=3132、truncate_qemu=true 及全部已有标记和精确镜像摘要。打包门对缺少新增标记的旧结果作拒绝测试。详细语义及安全擦除限制见 `TRUNCATE.md`。

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

## 外部助手审查

按用户授权调用已有 DeepSeek Harness。首次宽范围审查超时，没有采用其未完成输出；随后以低推理档审查一个文件写函数，11 秒完成且未使用工具。核实并修复空写返回错误，增加容量防御检查及对应宿主断言。审查回执在 `.cache/deepseek/runs/`，不包含提交或推送权限。

M5 再次使用低推理档，只审查设备授权 C 文件。核实其“旧驱动释放后仍映射 DMA 区”的发现，采用保留拥有者至地址空间销毁的方案，并增加真实双实例授权负例。其余意见分别由失败任务的完整地址空间回收和明确隔离保留策略覆盖，没有把所有建议直接当作已确认漏洞。

M6 尝试通过工作区桥调用 deepseek-flash 审查事务引擎。两次调用因 permission preset 初始化失败而分别超时，没有返回有效结果或工具调用；未采用任何未完成输出。实现由本次代码检查、共用 C 引擎故障测试和 QEMU 回归验证，不称为已经通过外部助手审查。
