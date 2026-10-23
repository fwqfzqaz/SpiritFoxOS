# Native 参数、显式环境与 BootArgs

`run app.sfk [参数...]` 和 `runat 只读目录 app.sfk [参数...]` 可向 native 应用传递参数，不增加应用权限或改变签名包内容。模块启动不接受额外参数。installed 入口使用同样流程，参数仅属于本次调用，不写入安装选择器或更新状态。

```text
run argprobe.sfk alpha "two words" '' a\ b
runat /disk/etc argprobe.sfk "directory args"
run installed "one invocation"
runenv MODE=demo "NOTE=two words" EMPTY= -- argprobe.sfk alpha
runenvat /disk/etc MODE=read -- argprobe.sfk granted
```

第一个示例输出 argc=5：argv[0] 是 `argprobe.sfk`，随后是 `alpha`、`two words`、空字符串和 `a b`。`argprobe.sfk` 是随构建提供的 SDK 示例应用，经普通文件 RPC 输出，不拥有额外系统权限。

## 兼容性与边界

当前最多八个参数（含 argv[0]）及八条环境变量；两个偏移表和全部 NUL 结尾字符串合计最多 512 字节。交互命令行仍有 191 字节的独立限制，超出时不继续接收字符。为了兼容旧程序，run/runat 未提供额外参数时保留 argc=envc=0 和 96 字节 BootArgs；显式参数调用及 runenv/runenvat 均包含 argv[0]。installed 的 argv[0] 保留调用时的 `installed` 或 `/installed.sfk` 名称。

词法支持空格/制表符分隔、单双引号和反斜线转义；单引号内反斜线是普通字符，双引号或未引用时反斜线转义下一个字符。相邻引用片段连接，不做变量、通配符、命令替换或管道展开。未闭合引号、末尾反斜线、参数超限等在获取目录能力及开始更新试运行之前拒绝，不创建子进程或写 TRYING。

字符串是有界的 NUL 结尾字节序列；交互控制台目前只接收 ASCII。传入路径字符串或环境值不会变成文件能力，目录委派的只读授权和清单 namespace.read 检查保持有效。

## 显式环境

runenv/runenvat 只接收 `--` 之前显式给出的 NAME=VALUE，不继承 Windows 宿主、管理器或上次进程的环境。NAME 为 1 至 63 个 ASCII 字母/数字/下划线，首字符不能是数字；名字大小写敏感，重复名字拒绝。VALUE 可为空并可包含更多等号；`EMPTY=` 与未设置不同。引号和转义使用上述词法，不展开 `$NAME`。非法名字、重复项、数量/载荷超限或缺少分隔符在获取目录和更新 TRYING 之前拒绝，原磁盘状态不变。

runenvat 的首项为显式只读目录，其余语义与 runenv 相同。环境仅属于当前调用，不写入更新选择器、磁盘或全局配置；模块不接受带显式调用载荷的启动。旧应用若硬性只接受 96 字节 BootArgs，应继续无参数/无环境调用，或采用新的 SDK 校验。

## 布局与 SDK

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

## 验证

`tests/test_arguments.c` 含 8155 项检查，包括 512 字节边界、1000 次固定种子往返、偏移/数量/长度/终止符破坏、头部别名、不一致环境字段及词法规则。`tests/test_environment.c` 另有 12034 项检查，含显式查询、空/缺失区别、名字/重复项、最大 512 字节环境和 1000 次往返。真实 syscall 负例验证内核拒绝参数/环境数量溢出、保留位和指向头部的偏移。

`tools/qemu_arguments_test.py` 在 legacy 与 VT-d/modern 各启动一次，验证默认无参数布局、引号/空参数/转义、八参数上限、runat、installed 健康确认、非法词法不写盘、只读参数页故障及进程重用后的清理。调用参数不进入持久化磁盘；示例应用故障后回显与后续进程继续运行。

`tools/qemu_environment_test.py` 再各启动一次，验证显式值/不展开/空值、双计数上限、runenvat、非法/重复名字不启动试运行、环境页只读、进程重用后无残留及环境不落盘。
