# 下一步开发交接

更新时间：2026-10-20，Asia/Shanghai。
基线：作者已合并 PR #2，upstream/main=`4a5196952ecc3dd049f14c004f284ac72cad2226`。
本次在 `codex/shell-cat-read-errors` 上单独处理清单 S1，推送目标为个人 Fork 的新分支/PR。

## 本轮授权与修改

用户用“111”选择 S1，后续明确说“按照你上面的方案修”。本轮只改 shell `cat` 读错误返回：
read=0 正常 EOF、read<0 打印错误并返回失败；失败时关闭 cat 自己打开的 fd，不关闭借用 stdin。
没有改变模块 ABI/块消息、文件系统后端或架构路线。

实现与验收见 [本轮对接](../integration/20261020-shell-cat-read-errors.md)。

## 定向验证状态

- 修复前，用生产 `cmd_cat` 加可控 VFS 桩复现：read 返回错误时错误处理断言失败，
  baseline 模式直接确认命令返回成功，符合原 bug。
- 修复后，同一生产函数宿主测试覆盖立即错误/中途错误、普通文件/借用 stdin 清理，
  正常 EOF 和输出；`make test-shell-cat` 通过 UBSan。
- 工作区/最终提交完整 Ubuntu 与 Arch 检查尚待 pre-push 执行；推送后查看该 PR 的双 Linux CI。
  不借用旧分支或 PR #3 的验证结果代替本 SHA 检查。

## 后续和停止位置

本轮只交付 S1，之后停止并等用户选择。小模型清单 S2/S3/S4/R1/R2 仍是候选，未由这轮验证；
尤其 cp/mv 错误路径需使用临时内存测试，不要用真实资料制造失败。作者以前提出的设备与架构待办
仍暂停，不从旧 NEXT 自动启动。

## 协作约束

- 新任务先 fetch、读 integration/NEXT，基于当前作者 main；每次提交同时更新 integration 和 NEXT。
- 只推个人 Fork 的新分支，以独立 PR 供作者审阅；不推作者代码分支，不自行合并。
- pre-push 必须完整验证最终提交，保留 hook。源码 UTF-8/LF、Makefile Tab 不变。
- 额度保护继续生效：每次修改/构建/提交前检查可用窗口，任一剩余少于 5% 时保存并停止。
