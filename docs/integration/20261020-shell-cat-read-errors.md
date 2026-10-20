# 2026-10-20 对接：shell cat 读错误传播

## 基线与范围

开始前 fetch upstream/origin 并检查接口文档与 NEXT。作者已合并 PR #2，
本修复基于 upstream/main `4a5196952ecc3dd049f14c004f284ac72cad2226`，
独立分支为 `codex/shell-cat-read-errors`。前序 VFS 边界修复在个人草稿 PR #3，
不包含在此分支。本次只处理 shell 内置 `cat` 对读错误的返回值。

## 问题与行为

原 `cmd_cat` 使用 `while ((n = vfs_read(...)) > 0)`，把所有 `n <= 0` 都当 EOF，
忽略负数错误，最后仍返回成功。现在分开处理：`n == 0` 是正常 EOF，`n < 0` 打印读错误并返回 -1。
若读到部分数据后失败，会保留已经输出的数据并返回失败。

失败后始终关闭 cat 自己打开的文件；若 cat 使用 shell 借用的 stdin，不关闭该 fd。
成功 EOF、输出行为和 stdin 生命周期不变。没有新增服务/API，也不改变模块 ABI 1.5 或块消息 v2。

## 回归与接入

`tests/test_shell_cat.c` 直接编译调用生产 `cmd_cat`，通过小型可控 VFS 桩设置读结果，覆盖：
- 初次 read 返回错误，以及输出部分数据后 read 返回错误；两者均返回失败。
- 普通文件错误路径关闭自有 fd 一次；stdin 错误路径不关闭借用 fd。
- 普通文件和 stdin 正常 EOF 均成功；stdout 数据与读到的数据一致。

`make test-shell-cat SHELL_CAT_ARGS=--baseline` 在修复前实测到断言失败，生产 `cmd_cat` 确实
把 read 错误报告为成功；加 `--baseline` 时输出 `reproduced: cat returns success after a VFS read error`。
修复后的 `make test-shell-cat` 通过 UBSan，输出 `shell cat read-error and EOF paths passed`。

双 Linux 完整验证由 pre-push 对最终提交执行：`bash tools/check-linux.sh`，覆盖此回归、其他宿主
测试、完整编译和 UEFI/Wayland 启动。提交/PR 的实际结果以最终 SHA 的回执和 GitHub CI 为准；
本说明不把尚未执行的最终提交门禁提前记作通过。宿主桩不验证真机、调度或所有 VFS 后端。

## 后续

本分支只交付 S1，完成后停止。其他小模型候选不在本次实现范围内；后续按用户选择逐项进行。
