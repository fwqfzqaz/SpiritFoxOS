# Native relative-v1 子集

设计依据为原始设计说明 9.4；包头、SFK1 容器和签名消息保持 v1。旧静态包没有 native_profile（或显式 static-v1），继续在 0x40000000 运行。relative-v1 用于 APP（可绑定一个认证运行时库）和 library 角色模块；不支持 TLS、异常展开、通用 ELF 执行、多库依赖图、惰性绑定、符号插入或版本化符号。

## 生成与子表

工具链用 -fpie/-pie 和 services/user-pie.ld 生成 x86_64 ET_DYN 中间产物，映像 RVA 从零开始。sfpack 接受有界 PT_LOAD，拒绝解释器、TLS、依赖库、未定义导入、REL/RELR、非 RELATIVE 或带符号索引的重定位。ELF 不进入 Core 0。

源清单指定 native_profile 为 relative-v1。sfpack 从 ELF RELA 提取符号索引为零的 R_X86_64_RELATIVE，把 24 字节 ELF 项转换为 16 字节 target_rva:u64、addend:i64，小端编码，按目标排序且拒绝重复。它生成一个 RELOC 段与 relative_relocations 清单对象：section 为该段的零基索引，offset=0，bytes 为整段长度。源清单不能覆盖生成的子表。

当前子集要求恰好一个 RELOC，完整声明该段，不允许隐藏项或第二个空段。相对项长度是 16 的倍数，最多 256 项（4096 字节），空表可用。段为非映射元数据，rva=0、alignment=8、flags=0、memory_bytes=file_bytes。`relative_relocations` 和 `import_relocations` 均由打包器生成，作者提供即拒绝；未知 native_profile 也拒绝。旧静态配置继续拒绝非空 RELOC。

签名校验后，每个目标必须严格递增、8 字节对齐，且完整八字节槽落在 RW_DATA 的实际 memory_bytes 内，允许写 BSS。不能写代码、常量、段间隙或页尾对齐扩展。当前 addend 限定非负且位于某个映射段实际内存范围内；负数、映像外、段间隙和尾后指针不属于这个子集。写入值为 image_base+addend，上限保证不溢出。

## 导入表与运行时库

导入不是 ELF 未定义符号。应用清单可声明最多 32 个 `native_imports` 项 `{name, abi, slot}`，`name` 为被调用函数名，`abi` 目前固定为 `libsf-posix-1`，`slot` 是应用 ELF 中一个全局、8 字节、位于 RW 段的 OBJECT 符号名。sfpack 从符号表解析该槽并写入 `slot_rva`；源清单不能自行提供 RVA。槽必须 8 字节对齐、完整落在 RW_DATA 的 memory_bytes 内，重复名称或重复槽均拒绝。同名函数指针对象由应用定义，例如 `include/posix_imports.h` 中的 `__sf_import_open`。

运行时库是 `native_role` 为 `library` 的 relative-v1 模块（kind=1），可声明最多 64 个 `native_exports` 项 `{name, abi}`；sfpack 要求对应全局 FUNC 符号位于 RX 段并写入 `rva`。库不得有导入、导入重定位或相对外部的符号索引，也不得作为应用入口运行。当前只支持一个库角色：`sf-libsf.sfmod`，模块 ID `org.spiritfox.posix`，ABI `libsf-posix-1`。

带导入的 APP 必须在清单中声明 `native_library`（当前唯一值为 `org.spiritfox.posix`）。sfpack 把导入重定位写成 16 字节记录 `target_rva:u64, import_index:u32, reserved:u32`，与相对记录同处唯一的 RELOC 段并紧接其后；`import_relocations` 的 `offset` 等于相对记录字节数，`bytes` 为导入记录长度。记录按槽地址严格递增且唯一，`target_rva` 必须等于对应项的 `slot_rva`，`import_index` 必须唯一，且不得与相对重定位目标重叠。

装载时 sf-init 认证同一包快照后，按 `native_library` 查找并认证库包，要求模块 ID/角色/ABI/导出数与清单一致。`sf_native_link` 把应用段与库段并入一个计划：库段从 `page_up(应用页对齐总长)` 的偏移开始，应用与库的相对修正分别加上 0 与库偏移，导入槽写入 `image_base + 库偏移 + export_rva`。因此槽中只有本地址空间内的库函数指针，从不写入另一进程地址。合并后最多 8 个映射段；找不到匹配的（名称, ABI）导出返回 `SF_DEPENDENCY`。

## POSIX 子集

`sf-libsf.sfmod` 导出有界 POSIX 兼容函数：`open`、`read`、`write`、`close`、`lseek`、`fstat`、`stat`、`getpid`、`_exit`、`sleep`、`printf`、`puts`、`getenv`、`__errno_location`、`strerror`。它复用现有文件 RPC、启动参数与 SDK，不引入 ELF 装载器、libc、malloc、线程、信号、fork/exec、管道或动态数据段。

能力模型不变：普通应用只读，`O_CREAT`/`O_TRUNC` 在导入层返回 `EACCES`；符号错误映射到 `ENOENT`/`EACCES`/`ENOSPC`/`EBADF`/`EINVAL`/`EIO`。`/dev/console` 的写打开映射到进程控制台通道（`OP_OPEN`），仍由服务端只允许该精确路径与写模式。相对路径基于 `runat` 授予的目录根，`stat` 读取同一文件描述符的固定元数据。

## 准备与隔离

sf-native 独立认证相同包快照，返回至多五个段、profile、基址和重定位偏移/数量，240 字节计划装入一条复制消息。sf-init 将所有字段与同一认证快照比较，再由用户态 sf_native_image 清零暂存区、复制文件字节并应用修正。暂存区为 1280 KiB，映像 RVA 上限 2 MiB；因此单个映像的实际可用上限是 1280 KiB（按页对齐累计所有映射段，含绑定库），当前应用映像仅 8–20 KiB。用户映像按函数和只读数据分区并启用 `--gc-sections`，使含完整认证代码的 sf-native 保持在 168 KiB、其余模块不超过 13 KiB。带导入的应用由 `sf_native_link` 在同一暂存区追加库段并绑定槽，应用与库合计最多八个映射段。包原字节及签名不被修改。

加载器为 relative-v1 应用交替选择 0x44000000 和 0x46000000，这是确定性装载策略，不提供安全随机化。Core 0 独立验证 image_base：静态基址，或 [0x44000000,0x4fe00000] 内 2 MiB 对齐的基址；整个最大映像位于 DMA 区之前，与栈、BootArgs、设备 MMIO、bundle 不重叠。引导任务只允许静态基址。Core 0 校验段、入口、权限与源内存后复制到子进程私有页；没有重定位解释器或共享可写代码别名。

SfLoadPlan 增加 image_base，零值为旧静态默认；SfNativePlan 扩为 240 字节。这两项属于尚未冻结的内部装载 ABI，所有服务已同步构建。96 字节 BootArgs 头和显式 argv/env 格式不变。

native 的 RPC 回复使用独立端点，普通应用没有其发送能力；运行时重启会撤销并重建该端点。原共享管理端点会在并发启动时把已有子进程的文件请求误当成装载回复。现有子进程请求留在应用队列中，等待装载完成后由原处理流程服务，不影响身份/请求 ID 核对。

## 示例与回归

pieprobe.sfk 含三个真实重定位槽，分别指向 RW 数据、代码函数和 BSS；验证初值、零填充、函数指针调用及每个进程的独立修改。可运行 run pieprobe.sfk、runenv MODE=demo -- pieprobe.sfk --require-mode、run pieprobe.sfk --write-code，最后一个触发隔离页故障。

posixprobe.sfk 有 15 个导入槽、无相对项，经 `runat /tmp posixprobe.sfk` 绑定 `sf-libsf.sfmod`：验证控制台写、文件描述符元数据、相对路径读取、缺失路径 `ENOENT`、`printf`/`puts` 与只读拒绝语义。`test_native.py` 另用宿主导出 `sf_image_link` 检查槽值等于 `image_base+库偏移+导出RVA`，并构造缺失导出返回 `SF_DEPENDENCY`。

tests/integration/test_native.py 六组宿主测试直接调用产品 C 验证器与映像准备器，检查精确页字节、三个合法基址、容量失败不改输出、0/1/256 项、签名有效的非法目标/加数/排序/子表、导入/导出表的槽与 RVA 约束及 500 次固定种子已签名记录变异。仍落在合法范围的变异按独立目标范围计算允许通过，不能把签名者有权更改的合法记录误称为攻击。

tests/qemu/qemu_native_test.py 在 legacy 和 VT-d/modern 各执行初次启动与重启，共四次：并发两个私有映像、两个实际基址、代码页写拒绝、argv/env、非法签名包拒绝且磁盘不变、PI 应用安装健康确认、连续回收、重启后再次执行。`qemu_posix_test.py` 在真实访客中运行 posixprobe 并核对 15 个按名/ABI 绑定、控制台/元数据/errno 与退出 0。全套结果以 TESTING.md 和 build/test-results.json 为准；专项通过不代表整个路线图已完成。

## 启动意图与认证类型

管理器完成包签名、key_id 与用途认证后，先把容器类型和调用方意图比较，再准备 native 映像或分配特定服务授权。模块、设备、echo 入口只接受 MODULE，run/runat/runenv/runenvat 只接受 APP；不符返回 SF_DENIED。没有附加 argv 或环境时也执行该检查。已安装路径原有 APP 策略保持。

类型由签名容器 magic/kind 决定：有效 APP 放在 .sfmod 文件中仍可作为应用运行；有效 MODULE 改名 .sfk 不能进入应用路径。bundle 与 runtime 私有回复的特殊授权同时要求 MODULE 类型及受控内置 sf-native 名称。文件名仍用于查找，不作为类型证明。

## 运行时恢复与已有进程

内置 native 的启动分配失败会撤销两个私有端点并回退登记；死亡或有界健康失败关闭新应用准入，原应用保留已复制的私有映像和 namespace RPC。应用租约全部释放后才创建新运行时，使用新 PID 和新端点。健康往返不改变 OP_NATIVE_STAGE 快照；不会自动重放中断的 PREPARE/上传。状态、退避预算、root 诊断和适用边界见 LIFECYCLE.md。
