# 文件截断与补零扩展

日期：2026-10-23。RAMFS 普通文件和 `/persist/notes` 支持显式改变文件长度；write 仍只覆盖写入部分并保留其余尾部。

```text
write /persist/notes abcdefghij
truncate /persist/notes 4       # 内容为 abcd
truncate /persist/notes 12      # 保留 abcd，后八字节补零
truncate /persist/notes 0       # 清空
```

shell 的 `truncate <现有路径> <字节数>` 只接受无符号十进制数字，检查 64 位溢出；不接受符号、后缀或额外参数，不隐式创建命名空间节点。RAMFS 上限为 1024 字节，笔记上限为 16384 字节。`/persist/notes` 是始终存在的虚拟文件，首次空文件截断到 0 不必创建事务键。

## 句柄与权限

`sf_ns_truncate(namespace, owner, handle, length)` 验证所有者、generation、写权限和目标类型。只读句柄返回 SF_DENIED，只读节点返回 SF_READ_ONLY，目录、管道和控制台返回 SF_INVALID。长度超出容量返回 SF_NO_SPACE；不在失败后发布新的节点长度。

操作只改变文件长度，保留所有打开描述的偏移，包括 dup 共享的偏移。缩短后偏移可以位于 EOF 之后，此时读返回 0；之后在旧偏移写入时，中间空洞补零。RAMFS 缩短时清零丢弃的内存尾部，扩展部分也清零，防止后续扩展重新暴露被截掉的内容。

命名空间 RPC 追加 `OP_TRUNCATE=21`：handle 是已有文件句柄，offset 是新的 64 位长度；成功 status=0，失败为错误码。消息布局和已有操作号不变。`runat` 仍只授予读权限，应用不能利用 truncate 绕过只读目录策略。实际 fileprobe/readprobe 在获得只读文件后尝试截断，必须被拒绝，原数据仍可读取。

NODE_DISK 通过 `external_truncate(index, length)` 回调处理，回调只有返回 0 才表示成功；正数被视为格式错误。持久化适配器仅接受笔记索引，A/B 槽、更新选择器和只读磁盘文件没有公开可写截断入口。

## 持久化与恢复

`sf_txn_resize` 与实际用户态服务共用同一事务引擎。它保留新旧长度公共前缀，把扩展部分初始化为零，然后调用现有双银行快照提交；格式和配额不变。暂存数据在 commit 重建银行之前复制到 next 状态，不依赖旧尾部残留为零。

已挂载卷中的同长度请求直接成功，不写盘、不 flush、不提高 generation；尚不存在的空事务键视为适配器的空文件，截断到 0 也不改盘。设备死亡或卷已关闭时仍拒绝操作，不能用幂等请求绕过失效检查。

改变长度持有真实 dirty 引用，最后一次 flush 成功后才公布内存状态和节点长度。任何写/flush 失败返回 SF_COMMIT_UNCERTAIN，并停止该卷后续读写与截断；dirty 引用仍被释放，关闭文件后可卸载。重挂载只能恢复完整旧文件或完整新文件；错误本身不保证新文件一定未落盘。失败时命名空间暂留原长度，但数据访问已被关闭，恢复时重新构造节点。

笔记操作不会修改应用槽、更新记录或已确认版本。银行仍是整个卷的快照；旧银行可能保留旧内容，因此截断不是安全擦除。未来损坏导致回退旧的完整快照时，笔记也按该快照恢复。本轮没有改变既有回放/损坏恢复边界，也没有物理断电证明。

## 验证入口

- `tests/host/test_truncate.c`：83 条命名空间检查，覆盖写权限/类型、所有者与 generation、缩短/补零/空洞、偏移与 dup、容量、后端失败和节点长度只在成功后更新。
- `tests/host/test_resize_txn.c`：真实 resize 与事务引擎执行缩短、扩展、清空、首次扩展四种转换，每种覆盖 261 个写/flush 边界 × 三种故障模式，共 3132 个故障场景。验证恢复长度、字节、零区、同长度不改盘和三个独立保护文件。
- `tests/qemu/qemu_truncate_test.py`：legacy 与 VT-d/modern 各八次真实 VM 启动。验证缩短、零扩展、清空、最大容量、重启、同长度与非法请求不改盘、只读应用 RPC 拒绝、应用槽和选择器不变，以及实际 OUT/FLUSH EIO 和银行中段写入时终止 VM 后恢复。

完整套件继续由 `python3 tools/test.py --skip-build` 运行，增加到 95 次 VM 启动。发布门要求 `truncate_host_assertions=83`、`resize_fault_scenarios=3132`、`truncate_qemu=true` 以及全部已有标记和精确镜像摘要。

最终产品源码构建后完整回归通过（386.03 秒），实际日志计数为 95 个 VM，输出 `build/truncate-final-test.txt`。新增事务测试完成 474481 条内部检查及上述 3132 个有界故障场景；缺少本轮标记的旧结果被打包门拒绝。
