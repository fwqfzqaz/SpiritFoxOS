# Native relative-v1 子集

设计依据为原始设计说明 9.4；包头、SFK1 容器和签名消息保持 v1。旧静态包没有 native_profile（或显式 static-v1），继续在 0x40000000 运行。relative-v1 仅用于 APP，libsf 静态链接；不支持动态库、本地导入、TLS、异常展开或通用 ELF 执行。

## 生成与子表

工具链用 -fpie/-pie 和 services/user-pie.ld 生成 x86_64 ET_DYN 中间产物，映像 RVA 从零开始。sfpack 接受有界 PT_LOAD，拒绝解释器、TLS、依赖库、未定义导入、REL/RELR、非 RELATIVE 或带符号索引的重定位。ELF 不进入 Core 0。

源清单指定 native_profile 为 relative-v1。sfpack 从 ELF RELA 提取符号索引为零的 R_X86_64_RELATIVE，把 24 字节 ELF 项转换为 16 字节 target_rva:u64、addend:i64，小端编码，按目标排序且拒绝重复。它生成一个 RELOC 段与 relative_relocations 清单对象：section 为该段的零基索引，offset=0，bytes 为整段长度。源清单不能覆盖生成的子表。

当前子集要求恰好一个 RELOC，完整声明该段，不允许隐藏项或第二个空段。长度是 16 的倍数，最多 256 项（4096 字节），空表可用。段为非映射元数据，rva=0、alignment=8、flags=0、memory_bytes=file_bytes。native_imports 和 import_relocations 字段出现即拒绝；未知 native_profile 也拒绝。旧静态配置继续拒绝非空 RELOC。

签名校验后，每个目标必须严格递增、8 字节对齐，且完整八字节槽落在 RW_DATA 的实际 memory_bytes 内，允许写 BSS。不能写代码、常量、段间隙或页尾对齐扩展。当前 addend 限定非负且位于某个映射段实际内存范围内；负数、映像外、段间隙和尾后指针不属于这个子集。写入值为 image_base+addend，上限保证不溢出。

## 准备与隔离

sf-native 独立认证相同包快照，返回至多五个段、profile、基址和重定位偏移/数量，240 字节计划装入一条复制消息。sf-init 将所有字段与同一认证快照比较，再由用户态 sf_native_image 清零暂存区、复制文件字节并应用修正。暂存区总上限仍为 1 MiB，映像 RVA 上限 2 MiB。包原字节及签名不被修改。

加载器为 relative-v1 应用交替选择 0x44000000 和 0x46000000，这是确定性装载策略，不提供安全随机化。Core 0 独立验证 image_base：静态基址，或 [0x44000000,0x4fe00000] 内 2 MiB 对齐的基址；整个最大映像位于 DMA 区之前，与栈、BootArgs、设备 MMIO、bundle 不重叠。引导任务只允许静态基址。Core 0 校验段、入口、权限与源内存后复制到子进程私有页；没有重定位解释器或共享可写代码别名。

SfLoadPlan 增加 image_base，零值为旧静态默认；SfNativePlan 扩为 240 字节。这两项属于尚未冻结的内部装载 ABI，所有服务已同步构建。96 字节 BootArgs 头和显式 argv/env 格式不变。

native 的 RPC 回复使用独立端点，普通应用没有其发送能力；运行时重启会撤销并重建该端点。原共享管理端点会在并发启动时把已有子进程的文件请求误当成装载回复。现有子进程请求留在应用队列中，等待装载完成后由原处理流程服务，不影响身份/请求 ID 核对。

## 示例与回归

pieprobe.sfk 含三个真实重定位槽，分别指向 RW 数据、代码函数和 BSS；验证初值、零填充、函数指针调用及每个进程的独立修改。可运行 run pieprobe.sfk、runenv MODE=demo -- pieprobe.sfk --require-mode、run pieprobe.sfk --write-code，最后一个触发隔离页故障。

tests/integration/test_native.py 六组宿主测试直接调用产品 C 验证器与映像准备器，检查精确页字节、三个合法基址、容量失败不改输出、0/1/256 项、签名有效的非法目标/加数/排序/子表、ELF 导入拒绝及 500 次固定种子已签名记录变异。仍落在合法范围的变异按独立目标范围计算允许通过，不能把签名者有权更改的合法记录误称为攻击。

tests/qemu/qemu_native_test.py 在 legacy 和 VT-d/modern 各执行初次启动与重启，共四次：并发两个私有映像、两个实际基址、代码页写拒绝、argv/env、非法签名包拒绝且磁盘不变、PI 应用安装健康确认、连续回收、重启后再次执行。全套结果以 TESTING.md 和 build/test-results.json 为准；专项通过不代表整个路线图已完成。

## 启动意图与认证类型

管理器完成包签名、key_id 与用途认证后，先把容器类型和调用方意图比较，再准备 native 映像或分配特定服务授权。模块、设备、echo 入口只接受 MODULE，run/runat/runenv/runenvat 只接受 APP；不符返回 SF_DENIED。没有附加 argv 或环境时也执行该检查。已安装路径原有 APP 策略保持。

类型由签名容器 magic/kind 决定：有效 APP 放在 .sfmod 文件中仍可作为应用运行；有效 MODULE 改名 .sfk 不能进入应用路径。bundle 与 runtime 私有回复的特殊授权同时要求 MODULE 类型及受控内置 sf-native 名称。文件名仍用于查找，不作为类型证明。

## 运行时恢复与已有进程

内置 native 的启动分配失败会撤销两个私有端点并回退登记；死亡或有界健康失败关闭新应用准入，原应用保留已复制的私有映像和 namespace RPC。应用租约全部释放后才创建新运行时，使用新 PID 和新端点。健康往返不改变 OP_NATIVE_STAGE 快照；不会自动重放中断的 PREPARE/上传。状态、退避预算、root 诊断和适用边界见 LIFECYCLE.md。
