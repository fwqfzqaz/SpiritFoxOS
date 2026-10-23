# Native 命令行参数与 BootArgs

`run app.sfk [参数...]` 和 `runat 只读目录 app.sfk [参数...]` 可向 native 应用传递参数，不增加应用权限或改变签名包内容。模块启动不接受额外参数。installed 入口使用同样流程，参数仅属于本次调用，不写入安装选择器或更新状态。

```text
run argprobe.sfk alpha "two words" '' a\ b
runat /disk/etc argprobe.sfk "directory args"
run installed "one invocation"
```

第一个示例输出 argc=5：argv[0] 是 `argprobe.sfk`，随后是 `alpha`、`two words`、空字符串和 `a b`。`argprobe.sfk` 是随构建提供的 SDK 示例应用，经普通文件 RPC 输出，不拥有额外系统权限。

## 兼容性与边界

当前最多八个参数，包含 argv[0]；偏移表和全部 NUL 结尾字符串合计最多 512 字节。交互命令行仍有 191 字节的独立限制，超出时不继续接收字符。为了兼容旧的无参数程序，未提供额外参数时保留 argc=0、argv_offset=0 和 96 字节 BootArgs；一旦显式提供参数，则包含 argv[0]。installed 的 argv[0] 保留调用时的 `installed` 或 `/installed.sfk` 名称。

词法支持空格/制表符分隔、单双引号和反斜线转义；单引号内反斜线是普通字符，双引号或未引用时反斜线转义下一个字符。相邻引用片段连接，不做变量、通配符、命令替换或管道展开。未闭合引号、末尾反斜线、参数超限等在获取目录能力及开始更新试运行之前拒绝，不创建子进程或写 TRYING。

本阶段不传递环境变量，envc/env_offset 必须为零。字符串是有界的 NUL 结尾字节序列；交互控制台目前只接收 ASCII。传入路径字符串不会变成文件能力，runat 的只读授权和清单 namespace.read 检查保持有效。

## 布局与 SDK

`SfBootArgs` 固定头仍为 96 字节，size 和入口第二个参数现在表示本次对象总长度。argc 非零时 argv_offset=96，随后为 argc 个小端 u32 偏移，再按参数顺序紧密排列 NUL 结尾字符串；偏移相对于整个 BootArgs 对象。对象总长为 96 至 608 字节，存放在原先的只读/NX `SF_ARGS` 页内。

内核只接受规范连续布局：数量和总长有界、每个偏移恰好指向下一段字符串、不指向头或表、不重叠、完整终止且无尾随内容。内核复制已校验载荷到新进程专属页，不把调用者指针交给子进程。未使用的页内空间为零；修改参数字符串触发用户态页故障，不影响其他任务。

应用可使用 `include/arguments.h`：

```c
if (sf_boot_arguments_validate(boot, bytes)) return SF_ABI_MISMATCH;
const char *first = sf_argument(boot, bytes, 0);
/* 无参数的兼容调用或索引越界时返回 NULL。 */
```

`sf_arguments_pack` 为用户态加载器构造 `SfArguments`，`sf_arguments_validate` 验证规范载荷，SDK 与 Core 0 使用同一边界检查代码。`SfLoadPlan` 因内嵌参数载荷而扩展，属于尚未冻结的装载服务 ABI；当前管理器、内核和服务已同步重建。包容器、签名协议和 96 字节 BootArgs 头未修改。旧应用若自行强制总长度必须等于 96，只能继续使用无参数方式；显式参数调用应采用新的校验助手。

## 验证

`tests/test_arguments.c` 含 8155 项检查，包括 512 字节边界、1000 次固定种子往返、偏移/数量/长度/终止符破坏、头部别名、环境字段拒绝及词法规则。真实 syscall 负例验证内核拒绝参数数量溢出和偏移指向头部。

`tools/qemu_arguments_test.py` 在 legacy 与 VT-d/modern 各启动一次，验证默认无参数布局、引号/空参数/转义、八参数上限、runat、installed 健康确认、非法词法不写盘、只读参数页故障及进程重用后的清理。调用参数不进入持久化磁盘；示例应用故障后回显与后续进程继续运行。
