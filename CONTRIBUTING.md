# 贡献与推送规范

本文件规定 SpiritFoxOS 的提交、推送、分支与 PR 门禁。目标不是增加流程负担，而是保证「可复现的测试证据 + 可审阅的原子提交 + 与代码同步的文档」。

## 许可证

- 项目以 **GPL-3.0-only** 发布，完整条款见 [LICENSE](LICENSE)。
- 提交 PR 即表示你同意该贡献按 GPL-3.0-only 授权，且你有权提交该代码。
- 第三方代码必须保留其原始许可证与版权声明；新引入依赖须在 PR 中说明许可证及与 GPLv3 的兼容性。
- 不引入来源不明、无许可证或许可证不兼容的代码、二进制或测试数据。

## 分支模型

| 分支 | 用途 | 规则 |
| --- | --- | --- |
| `main` | 始终可构建、测试可复现 | 禁止直接推送；只接受 PR 合并 |
| `feat/<topic>` | 新功能或纵向切片 | 从最新 `main` 创建，保持单一目标 |
| `fix/<topic>` | 缺陷修复 | 尽量附带最小复现或失败测试 |
| `docs/<topic>` | 纯文档整理 | 不夹带代码行为变更 |
| `chore/<topic>` | 构建、工具链、清理 | 不改变运行时语义 |

- 分支名使用小写 ASCII，`/` 分隔，避免空格与中文。
- 一个 PR 只做一个逻辑变更；跨阶段重构应拆分。
- 合并方式使用 **squash merge** 或 **rebase merge**，保持 `main` 线性可读；不使用 merge commit 把长分支历史带入 `main`。

## 提交信息

格式采用精简的 Conventional Commits：

```text
<type>(<scope>): <subject>

<body>

<footer>
```

- `type`：`feat`、`fix`、`docs`、`refactor`、`test`、`build`、`perf`、`chore`。
- `scope`：小写模块名，例如 `kernel`、`ipc`、`fs`、`native`、`sfpack`、`docs`。
- `subject`：英文祈使句，不超过 72 字符，句末不加句号。
- `body`：说明**为什么**改、影响范围与验证方式；不要复述 diff。
- `footer`：关联 `Refs #<issue>` / `Closes #<issue>`；破坏性变更写 `BREAKING CHANGE:` 并说明迁移方式。
- 示例：

```text
feat(native): add bounded import table for sf-libsf

Applications now declare up to 32 imports and are linked against the
single certified runtime library. ABI is append-only.

Refs #42
```

历史提交已使用英文祈使句，新提交保持一致。

## 提交粒度与文件卫生

- 一次提交只完成一个可独立回滚的变更；不要把重构、功能与格式化混在一起。
- 禁止提交：
  - `build/`、`.cache/`、`.tools/`、`rust/target/` 等生成物；
  - `.keys/`、任何私钥、口令、令牌或本地凭证；
  - 调试日志、崩溃转储、临时镜像与个人脚本。
- `.gitignore` 已覆盖上述路径；新增生成物时同步更新 `.gitignore`，不要用 `git add -f` 绕过。
- 修改头文件、结构体、常量或系统调用编号时，必须同步更新 ABI 基线并在 PR 中单独说明差异。

## 推送前检查

在本地依次执行：

```bash
git diff --check                 # 空白与冲突标记
python3 tools/test.py --host-only
```

涉及下列任一内容时，必须运行完整测试：

```bash
python3 tools/test.py            # 构建 + 宿主 + 集成 + QEMU
```

触发完整测试的条件：

- 修改 `kernel/`、`boot/`、`services/` 或 `include/`；
- 修改 ABI、系统调用、包格式、磁盘格式或服务协议；
- 修改构建脚本、签名/信任策略、测试框架；
- PR 触及持久化、更新、IOMMU、网络、SMP/线程或会话行为。

其他要求：

- 编译保持 `-Wall -Wextra -Werror` 零告警。
- 文档与代码同步：行为变化同时更新对应架构文档；阶段完成更新 `STATUS`/`ROADMAP`。
- 新增或删除文档时更新 `docs/README.md` 索引，并确认相对链接可解析。
- 提交前用 `git status` 与 `git diff --staged` 自查是否夹带无关文件。

## 推送与 PR 流程

1. 从最新 `main` 创建分支：`git switch -c feat/<topic> origin/main`。
2. 小步提交并本地验证；需要同步上游时使用 `git rebase origin/main`，不要用 merge 提交污染分支。
3. 推送：`git push -u origin feat/<topic>`。
4. 创建 PR，标题与提交 subject 一致；描述必须包含：
   - 变更目的与范围（一句话）；
   - 关键实现决策与放弃的方案；
   - 验证证据：命令、结果、测试数量或日志路径；
   - 兼容性：ABI/包格式/磁盘格式/协议是否变化及迁移方式；
   - 风险与未验证边界。
5. 等待 Linux 与 Windows CI 在**最新提交**上通过；旧提交的绿色结果不能沿用。
6. 至少一名维护者审阅通过后合并；作者不得自行绕过审阅直接推送 `main`。

### Force push 与特殊情况

- 只允许对自己的功能分支使用 `git push --force-with-lease`；禁止对 `main` 强推。
- 已公开审阅的提交尽量追加修复提交；仅在不破坏审阅记录时压缩历史。
- 紧急修复同样走 PR；如确需先合并，事后必须补测试与文档。

## 发布与标签

- 版本标签使用 `v0.x.y` 注解标签，附发布说明与已知限制。
- 发布前必须通过完整测试、发布门与离线 ZIP 校验；产物摘要记录在发布说明中。
- 发布标签一旦推送不再移动；修复使用新版本号。

## 安全与隐私

- 安全缺陷不要先开公开 Issue；使用仓库的私有安全报告渠道（GitHub Security Advisory）。
- 报告中包含受影响版本、复现步骤、影响范围与建议修复；避免提交真实凭证或用户数据。

## 文档规范

- 文档语言与现有文档保持一致（中文为主，标识符、命令与代码保持英文）。
- 架构文档描述**当前真实行为**；设计文档描述目标规范。两者冲突时先修代码或文档，不允许长期并存矛盾。
- 合并文档时保留必要的历史结论与验证边界，删除重复叙述；更新所有引用链接后再删除旧文件。
