# VFS 默认文件读写边界修复

日期：2026-10-20，Asia/Shanghai。基线为作者已合并 PR #2 的 main：
`4a5196952ecc3dd049f14c004f284ac72cad2226`。
本轮仅修复可复现的文件读写问题，不继续旧交接中的设备迁移计划。

## 问题与行为

- 默认内存文件读：seek 超过 EOF 后，`size - offset` 无符号下溢，可能返回文件外数据。
  现在先判断 offset 是否到达/超过 EOF，返回 0，保持 offset 和目标缓冲区不变。
- 默认单页文件写：seek 超过 PAGE_SIZE 后，`PAGE_SIZE - offset` 下溢，导致巨大长度 memcpy；
  原先 `offset + count` 也可能溢出。现在先判断容量边界，再按剩余容量截短请求。
  到达/超过容量仍按已有容量耗尽约定返回 0；正常覆盖和末页部分写入保持可用。

仅修改 VFS 无 inode read/write 回调的普通文件后备路径。
模块/文件系统自身回调、FAT32、tmpfs、设备、SHM 的读写语义不变。
不增加接口或权限，不改变模块 ABI 1.5 / 块消息 v2。

## 接入与验证

1. `tests/test_vfs_bounds.c` 直接链接生产 `kernel/src/fs/vfs.c`，仅为无关设备和进程路径提供
   宿主桩。测试不执行内核特权指令，不验证调度或并发。
2. `make test-vfs-bounds` 使用 ASan/UBSan，覆盖 EOF、超 EOF、空文件、零长度请求、
   PAGE_SIZE 和超容量 offset、UINT64_MAX offset、SIZE_MAX count、末页部分写及正常覆盖。
3. `tools/check-linux.sh` 将该目标纳入 Ubuntu/Arch 检查；其余模块、FAT32、RAMDISK、
   ATA 测试及完整编译、UEFI/Wayland 启动回归继续保留。

实际结果：在修复前的生产 VFS 上，读测试因未返回 EOF 失败；写测试由 ASan 报告
`negative-size-param (size=-1)`，定位到 vfs_write 的 memcpy。修复后 Ubuntu 宿主目标通过，
两个成功标记为 `VFS EOF bounds passed` 和 `VFS single-page write bounds passed`。

最终提交的 Ubuntu/Arch 完整检查由 pre-push 对该提交执行；结果以验证回执和新 PR CI
为准。本说明不把尚未执行的完整检查写成通过。原 VFS 的调试打印/未用参数编译警告
未在本轮顺带重构。宿主测试不能证明真机、SMP 或所有文件系统正确。

## 后续

等待作者确认优先级。本轮交付到个人 Fork 的独立修复 PR 后停止扩展；不从旧 NEXT
自动继续 AHCI 写盘、协议扩展或微内核迁移。其他审计线索需先复现并单独评估。
