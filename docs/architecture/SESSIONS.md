# 账户与多用户会话

日期：2026-11-07。控制台仍只由 PID 1 的 sf-init 读取，因此本轮不引入多终端进程，而是在单个控制台上复用有界会话：账户在构建期嵌入签名的 sf-init，会话表保存最多四个已认证身份，命名空间按 uid/gid 执行所有者/组/其他权限。

## 账户数据库

`config/accounts.txt` 是唯一的账户来源，格式为 `name:uid:gid:password`，每行一条，空行与 `#` 注释被忽略。`tools/build.py` 在打包 sf-init 前生成 `build/accounts_table.h`：

- 名称限 1–8 个小写 ASCII 字符，保证用户主目录名是规范命名空间路径；uid/gid 必须为 0–65535。
- 最多 8 条；名称与 uid 必须唯一；必须且只能有一个 `root:0:0`。
- 口令限 1–32 字节。构建期先计算 `digest=sha256(salt||password)`，再做 256 轮 `sha256(digest||salt)`，只把摘要和盐写入镜像，明文不进入产物。
- 当前盐由 `sha256("spiritfox-account:"+name)` 的前 16 字节确定性派生，便于重复构建；生产部署必须改为随机盐并轮换开发口令。

运行期 `services/accounts.c` 从 `SF_ACCOUNT_BUILTIN_TEXT` 严格解析同一格式：拒绝 CR、重复名称/uid、非法十六进制与长度、额外字段、超过 8 条以及缺失/重复 root。任一错误使 sf-init 启动断言失败，而不是带着不完整账户表继续运行。宿主测试直接链接该模块并以固定向量验证解析与加盐摘要。

## 会话表

`services/session.c` 维护 `SF_SESSION_MAX=4` 个固定槽位，每槽保存 uid、gid、名称、主目录和已认证标志。启动时 `identity_setup` 用内置 root 账户建立一个 uid 0 会话并立即成为当前会话，因此既有 `fox> ` 提示与全部 root 命令保持不变。

| 命令 | 行为 |
| --- | --- |
| `login <user> [password]` | 省略口令时先打印 `password: ` 并隐藏回显（≤32 字节，支持退格）；认证成功占用首个空槽，输出 uid/gid/会话号/主目录 |
| `logout` | 关闭当前会话，回收该会话 owner 的全部命名空间句柄，再附着到剩余的最高槽位 |
| `whoami` | 输出当前名称、uid、gid、会话号与主目录；无会话时返回 `whoami: no active session` |
| `users` | 列出构建期账户及其主目录 |
| `sessions` | 列出占用槽位、使用者、uid 与 active 标记 |

认证失败（未知用户、错误口令、超长名称）统一返回 `login: authentication failed` 或 `login: invalid user name`，不区分“用户不存在”和“口令错误”；四个槽位全部占用返回 `login: no free session slot`。没有会话时只接受 `help/login/logout/whoami/users/sessions`，其他命令返回 `login required; type login <user>`；`logout` 关闭 root 会话后提示符变为 `login> `。

会话不是进程：同一用户可占多个槽位，`logout` 只关闭当前槽，不结束其他会话。当前没有 `su`/切换命令，用户通过登出再登录改变身份。

## 身份传播

- 每个子进程记录创建时会话的 uid/gid（`Child.uid/gid`），后续文件 RPC 由内核返回的真实发送者 PID 映射回该身份，不接受消息自报身份。
- 命名空间句柄 owner 使用高位标签 `SF_SESSION_OWNER_TAG (0x80000000)|session_id`，与 PID owner 空间分离；会话登出按该 owner 回收句柄，避免 PID 复用或跨会话句柄越权。
- shell 的 `ls/cat/stat/write/truncate/rm/mkdir/rmdir/run/runat/runenv` 均以当前会话身份打开；`write` 对不存在的路径以会话身份和 0644 创建。

## 文件权限

`SfNode` 新增 `owner_uid`、`owner_gid` 与九位 `mode`（rwxrwxrwx）。uid 0 绕过全部检查；其他用户按属主/同组/其他依次取 6/3/0 位：

- 打开目标需要 read=4（读）或 write=2（写），目录另需 execute=1。
- 从委派根（不含）到目标直接父目录的每一层目录都必须授予 execute，私有主目录不能被更深的规范路径绕过。
- 创建与删除要求直接父目录同时具备 write 与 execute；删除仍保留原有只读、挂载与打开引用检查。
- 默认模式：目录 0755、文件 0644。启动时为每个账户建立 `/home/<name>` 并置为 `0700`（属主该账户）；`/tmp` 为 0777；`/dev/console` 与 `/run/pipe` 为 0666，供所有账户使用。
- 该模型没有 sticky 位：`/tmp` 的 0777 允许任一账户删除他人的临时文件。需要隔离的私有数据应放在自己的 `0700` 主目录。
- `/proc`、`/sys`、`/net` 的只读投影目录继续由 `sf_ns_protect` 保护；普通账户可读，但不能写入或创建。

服务端强制：`sf_file_dispatch_as(ns,owner,root,m,uid,gid)` 在分发 `OP_OPEN/OP_OPENAT/OP_UNLINK/OP_RMDIR` 前执行上述检查；旧 `sf_file_dispatch` 以 uid/gid=0 委托，只供既有 root 测试使用。

## 管理员命令门

`root_only` 前缀表在命令分发前拒绝非 root 会话，返回 `denied: root session required`，不执行处理函数。当前包括 `persist/install/revoke-key/trust/dhcp/mount/unmount/start|stop disk|echo|native/*-test/priority/cap-limit/page-limit/threads/cpu-budget/echo|native-*注入/selftest/shutdown` 等开发与管理命令；`run installed` 也要求 root 会话。普通账户仍可运行未安装的签名应用、读写自己的文件和使用网络查询命令。

## 限制与后续

- 单控制台串行复用，不能同时从两个会话输入；没有多终端、窗口或后台会话。
- 账户表只在构建期确定，运行期不能增删用户或修改口令；没有组管理、ACL、sticky 位与审计日志。
- 口令输入仅隐藏回显，不提供锁定/退避、长度之外的强度规则或加密存储之外的运行时保护；进程内明文只存在于输入缓冲区，提交后清零。
- 主目录内容在 RAMFS 中，重启后丢失；持久化卷仍是五文件笔记原型，不按用户分区。
- 后续可选方向：随机盐与账户轮换工具、`su`/会话切换、按用户持久化命名空间、组与 ACL、登录失败节流与审计。

## 验证

- `tests/host/test_accounts.c`：33 项，覆盖解析、注释/空行、唯一性与 root 不变量、名称/uid/摘要/字段边界，以及加盐迭代摘要的接受/拒绝。
- `tests/host/test_sessions.c`：25 项，覆盖 root 引导、登录成功/失败、槽位上限、登出重新附着与 owner 标签。
- `tests/host/test_ns_identity.c`：42 项，覆盖所有者/组/其他位、逐层 execute、创建与删除守卫、uid 0 绕过及无 sticky 的 `/tmp` 语义。
- `tests/qemu/qemu_user_test.py`：legacy 与 modern 各一次真实启动，验证 root 自动会话、隐藏口令登录、`users/sessions/whoami`、`/home` 隔离、非 root 管理命令拒绝、会话上限、登出重附着、无会话控制台，以及会话与主目录操作不改写持久盘字节。
