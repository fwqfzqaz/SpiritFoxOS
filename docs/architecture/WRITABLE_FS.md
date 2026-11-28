# 通用可写文件系统 SFRWFS1（格式核心 v1 + sf-fs 集成）

2026-11-27。SFRWFS1 是仓库内第一个通用可写文件系统格式，实现于
[include/sfrw.h](../../include/sfrw.h) 与 [services/sfrw.c](../../services/sfrw.c)，
由宿主 [test_sfrw.c](../../tests/host/test_sfrw.c) 在内存设备上验证。v1 交付格式
核心库；v1.1 在不改变磁盘格式与既有 API 语义的前提下修复 fsck/提交缺陷，并补齐
`link`/`chmod`/`chown`。2026-11-28 完成 **Phase 1 sf-fs 集成**：专用可写盘的
SFRWFS1 卷以 `/disk` 挂载，可读写已有文件，`mkfs /disk` 格式化空白盘，并在 QEMU
中验证重启后持久化。已有的 SFRFS1/SFRFS2 只读格式与双银行事务卷见
[STORAGE](STORAGE.md) 与 [PERSISTENCE](PERSISTENCE.md)。

设计原则与本仓库其余部分一致：容量有界、错误显式、失败关闭；每个写/刷盘边界都
可注入故障，验证崩溃后只能观察到旧状态或新状态之一。

## 布局

块大小固定 4096 B，设备按块读写。格式化容量 `block_count ∈ [128, 65536]`，
即 512 KiB 至 256 MiB。

| 区域 | 块 | 说明 |
| --- | --- | --- |
| 主超级块 | 0 | 4064 B 内容 + 32 B sha256 |
| 备份超级块 | 1 | 内容与块 0 相同 |
| inode 表 | 2..33 | 32 块 × 32 inode × 128 B = 1024 inode |
| 块位图 | 34..35 | 2 块，共 65536 位 |
| 日志 | 36..99 | 头块 36，payload 从块 37 起最多 16 个 |
| 数据区 | 100.. | 目录块、文件数据、间接块 |

所有整数小端。超级块字段：`magic "SFRWFS1\0" @0`、`version=1 @8`、
`block_bytes=4096 @12`、`block_count @16`、`max_inodes=1024 @20`、
`inode_table_block=2 @24`、`bitmap_block=34 @28`、`journal_block=36 @32`、
`journal_blocks=64 @36`、`data_block=100 @40`、`generation u64 @44`、
`root_inode=1 @52`、`free_blocks @56`、`free_inodes @60`、
`default_quota_blocks @64`、`default_quota_inodes @68`、`quota_count @72`、
保留 @76、`quota[8]{uid,blocks,inodes,reserved} @80..207`，其余清零，
`sha256(前 4064 B) @4064`。

inode 128 B：`mode u16 @0`（`0x4000` 目录 / `0x8000` 文件，低 12 位权限）、
`links u16 @2`、`uid @4`、`gid @8`、`size u64 @12`、`flags @20`、
`direct[12] @24`、`indirect @72`、保留 52 B。inode 0 保留，inode 1 为根目录。

目录项 128 B：`inode u32 @0`、`name_len u8 @4`、`type u8 @5`（1 文件 / 2 目录）、
保留 u16、`name[120] @8`；`inode==0` 表示空槽。名字长度 1..95，仅可打印 ASCII，
禁止 `/`、`\`、`.`、`..`。根目录含 `.`/`..` 且都指向 inode 1；目录大小按整块计，
`dir_add` 在现有目录块填满时分配新块。

日志头块 36：`magic "SFRWJ1\0\0" @0`、`version=1 @8`、`state=1 @12`（已提交）、
`seq u64 @16`、`count u32 @24`、保留 @28、`targets[16] u32 @32`、
`lengths[16] u32 @96`（固定 4096）、`payload_sha256[16][32] @160..671`，
其余清零，`sha256(前 4064 B) @4064`；payload 依槽位存放于块 37 起。

## 限额

| 项 | 上限 |
| --- | --- |
| 卷容量 | 65536 块（256 MiB） |
| inode | 1024（inode 0 保留，inode 1 为根） |
| 单文件大小 | 1036 块 = 4 243 456 B（12 直接 + 1024 间接） |
| 名字长度 | 95 B |
| 路径长度 | 255 B |
| 单次提交日志目标 | 16 |
| 配额覆盖项 | 8 |
| 跟踪 uid 数 | 16 |

## 写路径与提交协议

**COW 写**：写文件时，对每个被触及的逻辑块分配新块并写入内容（部分块先读旧内容
再覆盖）；inode 与间接块改指向新块，旧块只在同一提交的位图中释放。数据块先写并
`flush`，随后只提交元数据，因此崩溃后只可能看到旧指针或新指针，不会出现半写数据。
`truncate` 只改元数据并释放块，扩展产生稀疏洞（读到零）。

**有界写回窗口（v1.2）**：`sf_rw_write` 不再每次调用都提交。数据块仍直接写到最终
位置，元数据在日志暂存区累积；累计写入达到 `SF_RW_COMMIT_BLOCKS`（128 块）或暂存
目标接近 16 槽上限时自动提交。`sf_rw_fsync`、卷停止以及任何元数据操作
（`create`/`mkdir`/`unlink`/`rmdir`/`rename`/`link`/`truncate`/`chmod`/`chown`）
都会先原子提交，保证元数据立即可见。进程或电源崩溃最多丢失上次提交之后的写入
（≤128 块），文件系统保持旧的一致状态，fsck 回收未被引用的已写数据块。

**日志提交**（[services/sfrw.c](../../services/sfrw.c) 的 `commit`）：被修改的
位图块、inode 表块、超级块（块 0/1）都作为日志目标写入 payload；提交顺序为

1. 若磁盘日志头尚未清空，先写零头并刷盘；
2. 写 payload 到日志槽，记录目标、长度与 sha256；
3. `flush`（payload 先于头落盘）；
4. 写已提交日志头（`state=1`、`seq=generation`），`flush`；
5. 逐目标读回 payload 并写到目标块，`flush`；
6. 写零头，`flush`。

若提交目标集将超过 16 槽（`journal_count+4>16`），立即置未挂载并返回
`SF_COMMIT_UNCERTAIN`，不接受后续操作，重挂载得到旧状态，不以未提交内存状态继续。

恢复时若日志头 magic 与校验和有效且 `state==1`，逐项校验目标（不得落入日志区）、
长度与 payload 哈希后幂等重放并清头；撕裂或校验不符的头被忽略。任一步设备失败即
置 `mounted=0`、返回 `SF_COMMIT_UNCERTAIN`，不再接受后续操作。`generation` 每次
提交 +1，主/备超级块同步更新。

## 挂载校验

`sf_rw_mount` 首选块 0，校验和失败时回退块 1；两块都坏返回 `SF_BAD_FORMAT`。
校验 magic、版本、块大小、几何常量（inode 表/位图/日志/数据区起始块、根 inode、
保留字段、配额项）与容量范围；重放有效日志；再校验位图保留位、`free_blocks`/
`free_inodes` 与位图及 inode 实际一致、inode 字段与块指针合法。严格校验失败一律
返回 `SF_BAD_FORMAT`，交给 fsck 修复。挂载扫描全部 inode 统计每 uid 占用，超出
16 个 uid 返回 `SF_NO_SPACE`。

## 路径、权限与目录

- 路径必须为绝对路径，总长 ≤255，无空段、`.`、`..`、反斜线、尾随 `/`，各段
  名字合法；`name_ok`/`path_ok` 统一裁决。
- 逐级解析目录需 `x`；在父目录创建/删除/改名需 `w|x`；读文件需读位，写/截断需
  写位。uid 0 绕过全部权限。
- v1 只比较 owner 位与 other 位（**不使用组位与 gid 语义**），owner 匹配用 owner
  位，否则用 other 位。
- `mkdir` 初始化 `.`/`..` 并更新父目录链接计数；`rmdir` 只允许空目录（仅含
  `.`/`..`）；`unlink`/`rmdir` 释放 inode 并在提交中维护 `free_inodes`。
- `rename` 在一次提交内暂存源/目标目录块、相关 inode、位图与超级块；支持同目录
  改名、跨目录、替换已存在文件、替换空目录；移动目录时更新其 `..` 与双方链接
  计数；拒绝把目录移入自身子树、拒绝替换非空目录。
- `sf_rw_link(existing,newpath,uid)` 为普通文件创建硬链接：要求目标父目录 `w|x`、
  目标名不存在、链接源为普通文件，单次提交内 `dir_add` 并 `links++`。
  `unlink` 与 `rename` 替换目标时按链接计数处理：仅最后一个链接释放数据块与
  inode，其余只递减计数。
- `sf_rw_chmod(ino,uid,mode)` 仅接受低 9 位权限位，拒绝设置类型位；uid 0 或文件
  所有者可调用。`sf_rw_chown(ino,uid,new_uid,new_gid)` 仅 uid 0；变更所有者前按
  目标 uid 校验配额并确认可跟踪，再迁移 `usage`，配额不足返回 `SF_NO_SPACE` 且
  不改动 inode。

## 配额

默认配额（块/inode）由 `SfrwFormatOptions` 指定，另有最多 8 个 uid 覆盖项。
uid 0 豁免。创建 inode 时检查 inode 配额，分配块时按 COW 净增减检查块配额，超限
返回 `SF_NO_SPACE`；`sf_rw_set_quota` 仅 uid 0 可调用。

## fsck

`sf_rw_fsck` 先宽松加载并重放日志，然后：

1. 从根目录 BFS 标记可达 inode，删除坏目录项，统计子目录数；目录块指针必须落在
   数据区，越界或指向元数据区的块只报告并跳过，绝不读写元数据块；直接块可用新
   分配的数据块替换并写回 inode；
2. 只为可达 inode 建立引用位图，检测重复块（低 inode 优先；对重复的文件 inode
   清除其直接/间接指针以便修复，目录重复块只报告不清除指针）；`size` 之外的
   direct/间接指针先清零且不计引用，避免不可达块被永久占用；
3. 清理不可达 inode、重建位图与 `free_blocks`/`free_inodes` 及配额占用、修正
   目录链接计数（按目录项计数，天然支持硬链接多引用）；
4. 按每批 ≤16 个日志目标分批提交，操作幂等。

报告 `SfckReport` 含 `checked_inodes`、`used_inodes`、`used_blocks`、`free_blocks`、
`free_inodes`、`lost_inodes`、`unreachable_blocks`、`duplicate_blocks`、
`bad_entries`、`fixed_links`、`repaired`。fsck 以严格 `super_load` 加载超级块，若
超级块计数已严重越界（如 `free_blocks` 超过数据区容量）则无法修复，需重新格式化。

## 实现结构

`Sfrw`（约 40 KiB）含设备 vtable、几何/计数、`bitmap[8192]`、4 KiB 读写缓冲、
日志暂存表、`usage[16]`、COW 的 `old_block/new_block[1036]`，以及 fsck 用的
`ref[8192]`、`reach[128]`、`refs/subdirs/queue[1024]`。仅依赖 `sf.h`/`package.h`
声明（`memcpy/memset/strlen/strcmp`、`sf_sha256`），不引入 `<string.h>`。

## 验证边界

宿主 [test_sfrw.c](../../tests/host/test_sfrw.c) 使用 512 块（2 MiB）内存设备，
故障模型与 [test_txn.c](../../tests/host/test_txn.c) 相同：`stable/cache/baseline`
三镜像 + dirty 位图，`fail_at` 指定第几次写/刷盘失败，`mode 0/1/2` 表示不落盘、
半块撕裂落盘、完整落盘。覆盖：

- 格式化参数负例、挂载、根目录 `.`/`..`、坏主超级块回退备份、双坏拒绝；
- create/write/read、跨 12 直接块与间接块、稀疏洞读零、truncate 缩/扩、
  单文件上限与卷容量上限；
- 目录嵌套、readdir、rmdir 非空失败、unlink、重复创建、缺失路径、非法名字/路径；
- 权限 owner/other、目录 `x`、父目录 `w|x`、uid 0 绕过；
- 配额 inode/块超限返回 `SF_NO_SPACE`、uid 0 不受限、非 root 不能 `set_quota`；
- rename 同目录/跨目录/替换文件/替换空目录/移动目录 `..` 正确/拒绝移入子树/
  拒绝覆盖非空目录；
- fsck 干净卷零修复，以及坏指针/重复块/不可达 inode/坏目录项注入后修复；
- 硬链接（同/跨目录创建、目标已存在、目录源、非属主拒绝、计数持久化与最后一个
  链接释放）、`chmod` 权限与类型位/非属主拒绝、`chown` 仅 uid 0、配额不足失败且
  不改动；
- fsck 坏目录块指向 inode 表时不改写元数据、可替换修复；`size` 之外指针清零且不
  计入重复块；
- 崩溃一致性：以 create+write 与 rename 为样本，对每个写/刷盘边界 × 3 种故障
  模式注入，另以固定种子伪随机采样 256 边界 × 2 场景；重挂载（必要时 fsck）后
  断言“旧状态或新状态”之一，内容不得混合；日志已提交后中途失败必须重放为新状态。

测试以 `SFRW PASS: N checks, ...` 输出，并由 [tools/test.py](../../tools/test.py)
的 `host_test('test_sfrw', ...)` 纳入宿主回归。

## sf-fs 集成（Phase 1/2）

Phase 1 覆盖“挂载 + 已有文件读写”；Phase 2 把目录创建/删除/改名与目录内建文件
打通到 namespace 树，`/disk` 成为真正可写的挂载点。

- **IPC**：`include/sf.h` 追加 `OP_FS_BLOCK_WRITE=117`、`OP_FS_BLOCK_FLUSH=118`、
  `OP_FS_FORMAT=119`；`include/fs_format.h` 新增 `SF_FS_FORMAT_SFRWFS1=3`。ABI 快照
  工具不收录这些操作码，`abi/x86_64-current.json` 保持不变。
- **块适配**：[services/fs_rw.h](../../services/fs_rw.h) 提供 4096 B↔512 B 适配，
  每块按 8 个 512 B 扇区转换，扇区再拆为两次 256 B 半块请求（`mode∈{0,256}`）。
- **sf-fs**：[fs_main.c](../../services/fs_main.c) 常驻只保存 `Sfrw` 卷状态；只读镜像
  的整文件校验缓冲按挂载时的最大文件 `sf_mmap`/`sf_munmap` 临时映射，避免为 SFRWFS1
  常驻 1 MiB，进程载入从约 1.2 MiB 降到约 292 KiB。`OP_FS_MOUNT` 先探测扇区 0 的
  `SFRWFS1\0`，命中则挂载并发布，请求 `handle` 承载可写标志，回复 `mode=3`、
  `handle=0`；`OP_FS_FORMAT` 要求未挂载、4096 B 对齐、块数在 128..65536、扇区 0
  全零，格式化后写入 `/etc/readme.txt` 种子；`OP_FS_STOP` 先 `fsync` 再解除发布。
- **namespace**：[namespace_runtime.c](../../services/namespace_runtime.c) 以
  `0x80000000|inode` 识别 SFRWFS1 节点（显式排除 `UINT32_MAX` 旧 broker 哨兵），
  后端以 uid 0 调 `sf_rw_read/write/truncate`；`sf_nsv_publish_rw` 以 BFS 发布
  `/disk` 树，根（inode 1）与所有目录/文件均带该标记，`readonly=writable?0:1`。
  根前台只声明读后端，因此挂载可写卷时由运行时直接安装自身的读/写/截断后端。
- **目录变更**：[namespace.c](../../services/namespace.c) 通过 `SfNsDirBackend`
  钩子把 `mkdir/create/rmdir/unlink/rename` 转发到同进程的 `Sfrw`，先落盘再改
  内存树：`sf_ns_create_owned` 预留节点槽后调用 `sf_rw_mkdir/sf_rw_create` 取回
  inode；`remove_node_as` 在句柄占用、类型与非空检查通过后调用
  `sf_rw_rmdir/sf_rw_unlink`；`sf_ns_rename_as` 校验两条路径同挂载、拒绝把目录
  移入自身子树，提交 `sf_rw_rename` 后再搬移节点及全部后代路径。非空目录在
  namespace 层返回 `SF_BUSY`，后端仍以 `SF_INVALID` 兜底；只读挂载在
  `rw_ready` 处返回 `SF_READ_ONLY`，不会产生仅存在于内存的“改名”。
- **协议**：`OP_NS_RENAME=229` 追加在 `OP_NS_ENUMERATE_AT` 之后，携带
  owner/uid/gid 与两条绝对路径；`fs_main.c` 的 namespace 路由与
  `storage.c` 的持久化闸门同步纳入该操作码，避免落入块后端而返回
  `SF_BAD_FORMAT`。`sf_nsv_detach_rw` 先 `sf_ns_unmount(tag=1)` 再清空卷，
  同一启动内可重新 `mount /disk`。
- **storage**：[storage.c](../../services/storage.c) 的 broker 仅接受来自 `fs_pid`
  的写/刷盘请求，要求 `write_grant && driver_pid`；写请求校验 512 B 对齐、
  `length==256`、半块偏移与容量边界后组装驱动 `OP_BLOCK_WRITE`。SFRWFS1 盘跳过节
  2048 的 `SFTXN1` 校验并把写范围设为整盘；`sf_storage_format()` 供 `mkfs` 调用。
- **init**：新增 `mkfs /disk` 与 `mv <from> <to>`；SFRWFS1 盘跳过 persist attach、
  存储写测试与 Linux 兼容库断言（空白卷无这些库），避免误判。
- **QEMU**：[qemu_sfrwfs_test.py](../../tests/qemu/qemu_sfrwfs_test.py) 以 4 MiB
  空白可写盘启动，`mkfs` → `cat` 种子 → `write` 修改 → `mkdir /disk/docs` →
  `write /disk/docs/note.txt` → `mv` 改名 → 拒绝非空 `rmdir` 与移入子树 →
  `unmount`；同镜像重启读回改名后的内容，再跨启动删除目录并在最终启动断言
  `stat /disk/docs` 为 `SF_NOT_FOUND`，全过程断言镜像哈希变化。
- **宿主覆盖**：[test_namespace_rw.c](../../tests/host/test_namespace_rw.c) 以真实
  格式化卷驱动客户端 → runtime → namespace 全链路，覆盖建目录/建文件、读写落盘、
  改名、非空目录拒绝、移入子树拒绝、只读挂载拒绝与重挂载可见性。

挂载时的 inode 校验按 inode 表块批读：每 32 个 inode 只读一次 4 KiB 块，语义与逐个
`inode_read` 相同，但在 QEMU/TCG 下把 1024 次块读降为 32 次。

## 非目标

- Phase 1 不实现 SFRWFS1 上的目录创建/删除/改名、符号链接、并发/多线程挂载、
  扩展属性与 ACL；目录条目发布为只读。
- 格式核心不提供崩溃后自动 fsck；`sf-fs` 挂载失败时显式 `sf_rw_fsck` 后重试一次。
