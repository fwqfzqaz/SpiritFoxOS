# 应用调用接口与文件客户端 SDK

本文件合并原 native 参数说明与文件客户端 SDK 说明，描述 ring-3 应用可使用的调用数据与有界文件 RPC 客户端。

## Native 参数、显式环境与 BootArgs

`run app.sfk [参数...]` 和 `runat 只读目录 app.sfk [参数...]` 可向 native 应用传递参数，不增加应用权限或改变签名包内容。模块启动不接受额外参数。installed 入口使用同样流程，参数仅属于本次调用，不写入安装选择器或更新状态。

```text
run argprobe.sfk alpha "two words" '' a\ b
runat /disk/etc argprobe.sfk "directory args"
run installed "one invocation"
runenv MODE=demo "NOTE=two words" EMPTY= -- argprobe.sfk alpha
runenvat /disk/etc MODE=read -- argprobe.sfk granted
```

第一个示例输出 argc=5：argv[0] 是 `argprobe.sfk`，随后是 `alpha`、`two words`、空字符串和 `a b`。`argprobe.sfk` 是随构建提供的 SDK 示例应用，经普通文件 RPC 输出，不拥有额外系统权限。

### 兼容性与边界

当前最多八个参数（含 argv[0]）及八条环境变量；两个偏移表和全部 NUL 结尾字符串合计最多 512 字节。交互命令行仍有 191 字节的独立限制，超出时不继续接收字符。为了兼容旧程序，run/runat 未提供额外参数时保留 argc=envc=0 和 96 字节 BootArgs；显式参数调用及 runenv/runenvat 均包含 argv[0]。installed 的 argv[0] 保留调用时的 `installed` 或 `/installed.sfk` 名称。

词法支持空格/制表符分隔、单双引号和反斜线转义；单引号内反斜线是普通字符，双引号或未引用时反斜线转义下一个字符。相邻引用片段连接，不做变量、通配符、命令替换或管道展开。未闭合引号、末尾反斜线、参数超限等在获取目录能力及开始更新试运行之前拒绝，不创建子进程或写 TRYING。

字符串是有界的 NUL 结尾字节序列；交互控制台目前只接收 ASCII。传入路径字符串或环境值不会变成文件能力，目录委派的只读授权和清单 namespace.read 检查保持有效。

### 显式环境

runenv/runenvat 只接收 `--` 之前显式给出的 NAME=VALUE，不继承 Windows 宿主、管理器或上次进程的环境。NAME 为 1 至 63 个 ASCII 字母/数字/下划线，首字符不能是数字；名字大小写敏感，重复名字拒绝。VALUE 可为空并可包含更多等号；`EMPTY=` 与未设置不同。引号和转义使用上述词法，不展开 `$NAME`。非法名字、重复项、数量/载荷超限或缺少分隔符在获取目录和更新 TRYING 之前拒绝，原磁盘状态不变。

runenvat 的首项为显式只读目录，其余语义与 runenv 相同。环境仅属于当前调用，不写入更新选择器、磁盘或全局配置；模块不接受带显式调用载荷的启动。旧应用若硬性只接受 96 字节 BootArgs，应继续无参数/无环境调用，或采用新的 SDK 校验。

### 布局与 SDK

`SfBootArgs` 固定头仍为 96 字节，size 和入口第二个参数表示本次对象总长度。argc 非零时 argv_offset=96；envc 非零时 env_offset=96+argc*4，对应计数为零时偏移也为零。两个表依次包含 argc、envc 个小端 u32 偏移，随后先 argv、再环境字符串紧密排列；偏移相对于整个 BootArgs。对象总长为 96 至 608 字节，存放在原先的只读/NX `SF_ARGS` 页内。

内核只接受规范连续布局：数量、保留字段和总长有界，每个偏移恰好指向下一段字符串，不指向头或表、不重叠、完整终止且无尾随内容。它复制载荷到新进程专属页，不把调用者指针交给子进程。名字语法和唯一性由用户态加载器及 SDK 校验，不作为内核权限策略。未使用的页内空间为零；修改参数或环境字符串触发用户态页故障，不影响其他任务。

应用可使用 `include/arguments.h`：

```c
if (sf_boot_arguments_validate(boot, bytes)) return SF_ABI_MISMATCH;
const char *first = sf_argument(boot, bytes, 0);
const char *mode = sf_environment_value(boot, bytes, "MODE");
/* 无参数的兼容调用或索引越界时返回 NULL。 */
```

`sf_invocation_pack` 为加载器构造 argv 和环境，`sf_arguments_pack` 是无环境的兼容包装。`sf_arguments_validate` 验证规范载荷，SDK 与 Core 0 使用同一边界检查代码；`sf_environment` 按索引返回 NAME=VALUE，`sf_environment_value` 按名字返回值，未设置返回 NULL、空值返回非 NULL 的空串。`SfLoadPlan` 因内嵌调用载荷而扩展，属于尚未冻结的装载服务 ABI；管理器、内核和服务已同步重建。包容器、签名协议和 96 字节 BootArgs 头未修改。

### 验证

`tests/host/test_arguments.c` 含 8155 项检查，包括 512 字节边界、1000 次固定种子往返、偏移/数量/长度/终止符破坏、头部别名、不一致环境字段及词法规则。`tests/host/test_environment.c` 另有 12034 项检查，含显式查询、空/缺失区别、名字/重复项、最大 512 字节环境和 1000 次往返。真实 syscall 负例验证内核拒绝参数/环境数量溢出、保留位和指向头部的偏移。

`tests/qemu/qemu_arguments_test.py` 在 legacy 与 VT-d/modern 各启动一次，验证默认无参数布局、引号/空参数/转义、八参数上限、runat、installed 健康确认、非法词法不写盘、只读参数页故障及进程重用后的清理。调用参数不进入持久化磁盘；示例应用故障后回显与后续进程继续运行。

`tests/qemu/qemu_environment_test.py` 再各启动一次，验证显式值/不展开/空值、双计数上限、runenvat、非法/重复名字不启动试运行、环境页只读、进程重用后无残留及环境不落盘。

## 应用可用的内存接口

native 应用经 `sf-libsf.sfmod` 绑定 `malloc`/`free`/`calloc`/`realloc`，由库内基于 `SYS_MMAP` 的首次适配堆提供；映射失败返回 NULL，不改变应用原有段布局。ring-3 服务与模块可直接使用 `include/libsf.h` 的 `sf_mmap`/`sf_munmap`，地址窗口为 `[0x74000000, 0x78000000)`、页粒度、独占且清零。`sf_shm_create/map/grant/close` 面向服务间只读数据传递（固定 8 槽 × 8 MiB），普通应用没有这些导入；共享页的记账归创建者 PID，进程退出时先撤引用再回收页。

## Native 文件客户端 SDK

公开头为include/sf_file.h，实现位于services/file_client.c。它封装文件RPC，提供同一所有者内的句柄复制，保持原有权限。当前native进程单线程，一个私有回复端点只由一个客户端串行使用；不要混用自行构造的RPC、复制上下文后并发读取同一队列。

### 使用

应用通过sf_boot_arguments_validate验证私有BootArgs后，以manager和service端点初始化SfFileClient，超时范围1..60000毫秒。当前服务仍由PID1的sf-init提供，BootArgs.root_cap仍为0，目录从OP_ROOT取得。声明namespace.read还需要受信任shell显式runat/runenvat选择目录；普通run没有文件根。

```c
#include "sf_file.h"

SfFileClient files;
if (sf_file_client_init(&files, boot->manager, boot->service, 3000)) return 1;
int64_t root = sf_file_root(&files);
if (root < 0) return root;
int64_t file = sf_file_openat(&files, root, "notes", SF_OPEN_READ);
if (file < 0) { sf_file_close(&files, root); return file; }
uint8_t data[256];
int64_t count = sf_file_read(&files, file, data, sizeof(data));
sf_file_close(&files, file);
sf_file_close(&files, root);
return count < 0 ? count : 0;
```

链接现有user-common对象与file_client.c即可；构建工具只把SDK对象加入新的fileclient.sfk探针，不增加其他包或持久化16KiB槽的体积。可实际运行：run fileclient.sfk验证无根；runat /tmp fileclient.sfk、runat /disk/etc fileclient.sfk readme.txt和runat /persist fileclient.sfk验证显式只读根。

### 函数与返回

| 接口 | 成功返回 |
| --- | --- |
| sf_file_root / sf_file_console / sf_file_openat / sf_file_dup | 非零规范32位命名空间句柄，扩展为int64_t |
| sf_file_read / sf_file_pread / sf_file_write / sf_file_enumerate | 实际字节数，一次最多256；调用者按需循环 |
| sf_file_enumerate_at | 0 表示目录结束；正值为单个名称字节数（含末尾 NUL）；容量不足返回 SF_NO_SPACE |
| sf_file_stat | 0，写入32字节SfFileStat |
| sf_file_seek | 绝对偏移；不提供SEEK_CUR/END |
| sf_file_poll | 当前协议的0..7事件掩码 |
| sf_file_close / truncate / unlink / rmdir | 0 |

所有RPC函数返回显式int64_t，避免宿主LLP64的long窄化；错误为既有负值。句柄参数接收uint64_t并在窄化前拒绝非零高32位。OPEN权限为SF_OPEN_READ/WRITE/NONBLOCK/DIRECTORY，对应既有协议位；SF_TYPE_*对应目录、文件、管道、控制台、磁盘与只读投影六类元数据，其他语义见NAMESPACE.md。缓存metadata/poll不能作为设备健康证明。

`SF_TYPE_PROJECTION=6` 命名并接受服务端已有的投影类型，不新增节点类型或消息布局。取得显式只读目录后，应用可对 `/proc` 或 `/sys` 投影执行 stat/read/pread/seek；普通 run 仍没有目录根。投影 `size=0` 表示不提供固定内容长度，不能用它代替 read 的结果判断 EOF。当前 flags 保持只读值 1，内容由每次读取时的后端生成。

命名空间 seek 和文件分发器也使用显式 int64_t 状态，保证 Windows 宿主不会把合法的 0x80000000–0xffffffff 偏移返回成负数。投影允许绝对偏移 0–UINT32_MAX；更大偏移返回 INVALID，保持原共享偏移，普通权限/陈旧句柄检查保持。可用 `runat /proc fileclient.sfk --projection` 与 `runat /sys fileclient.sfk --projection modules` 检查 SDK 元数据、dup 共享偏移、边界 seek 和只读拒绝。

目录游标 `sf_file_enumerate_at(c, h, index, name, bytes)` 不保留服务端状态：每次按零基序号取一个直接子项，调用者递增 `index` 直到返回 0。名称以单个 NUL 结束，返回值包含该 NUL；序号上限为命名空间节点容量（128），越界序号、非目录句柄和陈旧句柄分别返回 SF_INVALID、SF_INVALID、SF_STALE_HANDLE。与一次性 enumerate 相比，游标不需要调用者一次容纳全部名称。

read/write/enumerate请求超过256时只发送一段，允许部分结果，不自动组合多次操作。零字节调用仍由服务检查句柄与权限；只读文件的零字节write也返回DENIED。路径有界编码并包含唯一末尾NUL，规范UTF-8/相对路径与实际权限继续由服务检查。输出是字节流，没有自动字符串终止符；错误不会把响应数据复制到调用者缓冲或元数据记录。

SDK包含write/truncate/unlink/rmdir编解码，但当前普通应用仍只能获得只读目录，只有控制台写入可用。宿主内部构造可写目录来检查协议，不代表guest新增了可写委派。create、跨所有者能力转移或挂载接口仍为后续工作。

### 句柄复制

`sf_file_dup(&files, handle)` 使用新登记的 `OP_DUP=25`。请求只携带原句柄，length/mode/offset 必须为零；服务先验证规范32位句柄及所有者，随后分配一个新句柄。成功回复 status 为0、新句柄与原值不同，其余上述字段为零；客户端检查这些约束，异常回复停止该上下文。

两个句柄引用同一打开描述，保留类型、读写权限与共享偏移；任一 seek/read/write 对另一方可见。关闭一个只释放一个引用，另一个继续有效；最后引用才释放挂载租约或管道写描述。复制已打开文件仍会阻止 unlink，复制管道写句柄不会虚增 writer 数量。普通应用复制只读文件后仍不能写或截断。

目录副本保留原目录权限。关闭默认根会继续清空 OP_ROOT 查询结果，显式保留的目录副本仍能 openat；它不会重新发布默认根。复制只在当前所有者内完成，不把句柄转交给其他 PID。槽位耗尽返回 NO_SPACE，原句柄和已有引用保持。

这项是额外分配引用的操作。超时或回复丢失时，副本可能已创建，不能盲目重试；进程回收会释放所有该所有者的句柄。新复制请求沿用原有单调编号、固定截止时间与认证回复规则。

### 回复与超时

每次调用使用进程内单调64位请求编号，重新初始化客户端也不重置编号，耗尽时在任何系统调用前返回NO_SPACE。实际接收者身份必须为PID1，header.flags也必须一致；随后检查头部大小、协议、载荷、无能力转移、编号与操作码。只在成功响应满足该操作的句柄、长度、计数或元数据约束后交付结果。

单次请求只计算一个经过溢出检查的绝对截止时间。SEND失败不入队，可以返回错误；RECV错误或异常回复保存到client.error，后续调用直接返回该错误。正常服务错误，例如DENIED、STALE，不毒化客户端。显式重新初始化时编号继续递增，最多跳过八条已认证旧回复，仍使用当前请求原截止时间；超过上限、未来/零编号或格式错误都会拒绝。

超时不取消已入队操作，不能承诺没有副作用，也不能据此盲目重试写入。丢失的OPEN或DUP回复可能保留描述/引用直至进程回收；遇到不确定超时时应结束进程并由已有回收路径释放引用。通用取消、异步/多线程和独立命名空间服务仍为后续工作。

### 不改变共享偏移的读取

`sf_file_pread(client, handle, output, bytes, offset)` 以完整 uint64_t 绝对字节偏移读取 RAM 文件、只读磁盘文件或持久笔记，一次最多 4096 字节。`OP_PREAD=26` 沿用现有消息，handle 为规范句柄，offset 为绝对位置，length 为容量，mode 必须为 0；成功回复保留 handle/offset/mode，并以 status 和 length 返回实际字节数。SDK 独立检查这些字段，异常成功回复停止客户端，错误不交付输出。

成功、EOF 或失败均不改变原描述及其所有副本的共享游标。完整 64 位偏移不窄化；超过末尾返回 0。非空磁盘请求即使在 EOF 也会进入原后端检查设备/卷健康，没有缓存元数据捷径。零字节调用仍验证句柄、读权限和对象类型，但不访问后端；管道/控制台返回 NOT_SEEKABLE，目录返回 INVALID。该接口不增加权限，不提供定位写入或并发/取消保证。

### 验证

完整 `python tools/test.py` 包含初始构建验证通过（1086.09 秒、151 VM），日志 `build/file-client-author-main-full.txt`，七项交付摘要一致。416510 项宿主检查与两种传输专项均已通过。宿主只替换INT80传输，直接包含实际SDK编解码；还与真实namespace/file_rpc组合验证读写权限、偏移、元数据和关闭。覆盖一千条正常序列、保护字节、非法本地参数、伪造身份/头/编号/操作码/计数/元数据、固定截止时间与有界旧回复、超时后停止、跨重新初始化编号不复用及末个64位编号。边界计数在宿主注入，不宣称guest执行2^64次调用。

实际fileclient.sfk使用生产INT80，在legacy/modern中各六十次RAM/磁盘/持久化只读目录调用，验证控制台、元数据、读取/定位/列举/poll、关闭、合法服务错误后可用、只读修改拒绝与进程回收。原始高位文件RPC探针保持，继续单独验证服务防御，不让SDK本地拒绝代替服务器测试。

后续复制扩展有独立 `test_file_dup`：23068 项宿主检查直接包含产品 SDK 并连接真实 namespace/file_rpc；验证共享偏移、所有者/权限、默认根关闭与目录副本、挂载/管道/控制台生命周期、退役槽位、1000 次复用和异常成功回复后的停止/进程回收。生产探针增加 `FILE DUP APP PASSED`，同一 legacy/modern 专项逐次要求该标记，并保留只读盘字节与卸载租约检查；完整回归以当前 TESTING/PROGRESS 记录为准。
