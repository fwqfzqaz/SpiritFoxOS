# Native文件客户端SDK

公开头为include/sf_file.h，实现位于services/file_client.c。它封装文件RPC，提供同一所有者内的句柄复制，保持原有权限。当前native进程单线程，一个私有回复端点只由一个客户端串行使用；不要混用自行构造的RPC、复制上下文后并发读取同一队列。

## 使用

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

## 函数与返回

| 接口 | 成功返回 |
| --- | --- |
| sf_file_root / sf_file_console / sf_file_openat / sf_file_dup | 非零规范32位命名空间句柄，扩展为int64_t |
| sf_file_read / sf_file_pread / sf_file_write / sf_file_enumerate | 实际字节数，一次最多256；调用者按需循环 |
| sf_file_stat | 0，写入32字节SfFileStat |
| sf_file_seek | 绝对偏移；不提供SEEK_CUR/END |
| sf_file_poll | 当前协议的0..7事件掩码 |
| sf_file_close / truncate / unlink / rmdir | 0 |

所有RPC函数返回显式int64_t，避免宿主LLP64的long窄化；错误为既有负值。句柄参数接收uint64_t并在窄化前拒绝非零高32位。OPEN权限为SF_OPEN_READ/WRITE/NONBLOCK/DIRECTORY，对应既有协议位；SF_TYPE_*对应五种元数据类型，其他语义见NAMESPACE.md。缓存metadata/poll不能作为设备健康证明。

read/write/enumerate请求超过256时只发送一段，允许部分结果，不自动组合多次操作。零字节调用仍由服务检查句柄与权限；只读文件的零字节write也返回DENIED。路径有界编码并包含唯一末尾NUL，规范UTF-8/相对路径与实际权限继续由服务检查。输出是字节流，没有自动字符串终止符；错误不会把响应数据复制到调用者缓冲或元数据记录。

SDK包含write/truncate/unlink/rmdir编解码，但当前普通应用仍只能获得只读目录，只有控制台写入可用。宿主内部构造可写目录来检查协议，不代表guest新增了可写委派。create、跨所有者能力转移或挂载接口仍为后续工作。

## 句柄复制

`sf_file_dup(&files, handle)` 使用新登记的 `OP_DUP=25`。请求只携带原句柄，length/mode/offset 必须为零；服务先验证规范32位句柄及所有者，随后分配一个新句柄。成功回复 status 为0、新句柄与原值不同，其余上述字段为零；客户端检查这些约束，异常回复停止该上下文。

两个句柄引用同一打开描述，保留类型、读写权限与共享偏移；任一 seek/read/write 对另一方可见。关闭一个只释放一个引用，另一个继续有效；最后引用才释放挂载租约或管道写描述。复制已打开文件仍会阻止 unlink，复制管道写句柄不会虚增 writer 数量。普通应用复制只读文件后仍不能写或截断。

目录副本保留原目录权限。关闭默认根会继续清空 OP_ROOT 查询结果，显式保留的目录副本仍能 openat；它不会重新发布默认根。复制只在当前所有者内完成，不把句柄转交给其他 PID。槽位耗尽返回 NO_SPACE，原句柄和已有引用保持。

这项是额外分配引用的操作。超时或回复丢失时，副本可能已创建，不能盲目重试；进程回收会释放所有该所有者的句柄。新复制请求沿用原有单调编号、固定截止时间与认证回复规则。

## 回复与超时

每次调用使用进程内单调64位请求编号，重新初始化客户端也不重置编号，耗尽时在任何系统调用前返回NO_SPACE。实际接收者身份必须为PID1，header.flags也必须一致；随后检查头部大小、协议、载荷、无能力转移、编号与操作码。只在成功响应满足该操作的句柄、长度、计数或元数据约束后交付结果。

单次请求只计算一个经过溢出检查的绝对截止时间。SEND失败不入队，可以返回错误；RECV错误或异常回复保存到client.error，后续调用直接返回该错误。正常服务错误，例如DENIED、STALE，不毒化客户端。显式重新初始化时编号继续递增，最多跳过八条已认证旧回复，仍使用当前请求原截止时间；超过上限、未来/零编号或格式错误都会拒绝。

超时不取消已入队操作，不能承诺没有副作用，也不能据此盲目重试写入。丢失的OPEN或DUP回复可能保留描述/引用直至进程回收；遇到不确定超时时应结束进程并由已有回收路径释放引用。通用取消、异步/多线程和独立命名空间服务仍为后续工作。

## 不改变共享偏移的读取

`sf_file_pread(client, handle, output, bytes, offset)` 以完整 uint64_t 绝对字节偏移读取 RAM 文件、只读磁盘文件或持久笔记，一次最多 256 字节。`OP_PREAD=26` 沿用现有消息，handle 为规范句柄，offset 为绝对位置，length 为容量，mode 必须为 0；成功回复保留 handle/offset/mode，并以 status 和 length 返回实际字节数。SDK 独立检查这些字段，异常成功回复停止客户端，错误不交付输出。

成功、EOF 或失败均不改变原描述及其所有副本的共享游标。完整 64 位偏移不窄化；超过末尾返回 0。非空磁盘请求即使在 EOF 也会进入原后端检查设备/卷健康，没有缓存元数据捷径。零字节调用仍验证句柄、读权限和对象类型，但不访问后端；管道/控制台返回 NOT_SEEKABLE，目录返回 INVALID。该接口不增加权限，不提供定位写入或并发/取消保证。

## 验证

完整 `python tools/test.py` 包含初始构建验证通过（1086.09 秒、151 VM），日志 `build/file-client-author-main-full.txt`，七项交付摘要一致。416510 项宿主检查与两种传输专项均已通过。宿主只替换INT80传输，直接包含实际SDK编解码；还与真实namespace/file_rpc组合验证读写权限、偏移、元数据和关闭。覆盖一千条正常序列、保护字节、非法本地参数、伪造身份/头/编号/操作码/计数/元数据、固定截止时间与有界旧回复、超时后停止、跨重新初始化编号不复用及末个64位编号。边界计数在宿主注入，不宣称guest执行2^64次调用。

实际fileclient.sfk使用生产INT80，在legacy/modern中各六十次RAM/磁盘/持久化只读目录调用，验证控制台、元数据、读取/定位/列举/poll、关闭、合法服务错误后可用、只读修改拒绝与进程回收。原始高位文件RPC探针保持，继续单独验证服务防御，不让SDK本地拒绝代替服务器测试。

后续复制扩展有独立 `test_file_dup`：23068 项宿主检查直接包含产品 SDK 并连接真实 namespace/file_rpc；验证共享偏移、所有者/权限、默认根关闭与目录副本、挂载/管道/控制台生命周期、退役槽位、1000 次复用和异常成功回复后的停止/进程回收。生产探针增加 `FILE DUP APP PASSED`，同一 legacy/modern 专项逐次要求该标记，并保留只读盘字节与卸载租约检查；完整回归以当前 TESTING/PROGRESS 记录为准。
