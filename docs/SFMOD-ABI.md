# sfmod ABI 1.5 与服务接口

本 ABI（0x00010005）面向 x86_64、小端、受信任 Ring 0 插件。SDK 是 `kernel/include/sfmod.h`。
1.5 在 API 表尾增加受控 block_read(index,lba,count,data,capacity)，表大小 96 字节，
该指针偏移 88；包版本同步提高，1.4 包/模块被拒绝，必须整体重编译。
设备注册、文件路由及 PCI 接口布局保持不变；块协议独立升级至 version=2。
代码入口通过核心提供的 `sfm_api_t` 表调用服务；不解析任意内核符号，不链接宿主 libc。
API 的 `abi` 和 `size` 描述接口版本。需要扩展语义时提高 ABI 版本，并补兼容测试。

## 二进制与启动包

`make modules` 用 freestanding C 编译和 `ld -r` 生成 ELF64 ET_REL `.sfmod`。
分配 section 支持 PROGBITS/NOBITS，含 `.text/.rodata/.data/.bss`，
以及编译器生成的只读、不可执行 NOTE 元数据。
`.sfmod_meta` 固定 96 字节，`.sfmod_init` 为两个经过重定位的 64 位函数指针，
`.sfmod_deps` 是最多 8 个 32 字节依赖名称。SDK 宏自动生成这些 section。

加载器最多接受 128 个 section 和 2 MiB 加载内存，拒绝 TLS、压缩 section、
REL 隐式 addend、COMMON、用于重定位的未定义强符号以及不支持的重定位。支持
R_X86_64_64、PC32、PLT32、32、32S，检查写入边界和有符号/无符号溢出。
入口必须指向该模块加载后的可执行 section。初期权限声明必须为零，非零直接拒绝；
没有以软件检查声称能拦截 Ring 0 的任意内存或 I/O 指令。

`modules/manifest.txt` 每行是模块名和源码。`tools/pack-modules.py` 生成可重复的
SFPK 包：16 字节头 `<magic, abi, count, total_size>`，其后每个目录项为
`<name[32], uint64 offset, uint64 size>`，数据按 8 字节对齐。目录项与 ELF 名称须相符，
数据不能越界/重叠。最多 16 个模块，总包至多 16 MiB。
包通过 NASM incbin 内嵌在内核，只读启动 initrd 不依赖 VFS/磁盘驱动。
改变模块、SDK、打包工具或清单都会重新生成包并链接 EFI。

所有 ELF 和完整依赖图验证通过后，按拓扑顺序初始化，同级按 metadata.priority。
初始化失败调用该模块 exit，再逆序退出此前启动的模块，回收端点和加载内存。
模块必须在 exit 中释放自己通过 API 分配的资源；不允许后台线程继续执行已卸载代码。
当前装卸仅供串行启动/管理上下文使用，不支持并发热插拔。

## 端点与消息

端点句柄的低 32 位是槽号，高 32 位是代数。失效句柄返回 ENOENT，代数耗尽的槽
不再复用。最多 32 个端点，每个端点 4 个消息槽。消息包含固定 64 字节头与最多
4096 字节载荷，协议不传裸指针。owner 是模块装载序号；保留的 core owner 用于
内核测试。owner 防误用，不是不能伪造的能力。

- `call(dst, req, resp)` 是立即执行的内核内 RPC；调用前固定引用，释放端点锁后才
  调用处理器。禁止同一端点重入，返回 EBUSY，避免递归环和服务状态被并发改写。
  不提供线程切换、等待或超时；IRQ 上下文不能调用服务。
- `send` 复制消息到有界 FIFO，满返回 EAGAIN；`recv` 由所属 owner 读取，空返回
  EAGAIN，不等待。不丢弃旧消息。
- 函数返回 0 表示传输/处理成功；负值是 Linux errno 的相反数。响应的 result
  表示操作结果，错误时同步写入负 errno；src/dst/sequence 由核心回填。
- endpoint_destroy 在有调用进行时返回 EBUSY。模块卸载同时检查依赖和进行中的调用。

## 文件服务

`fs.vfs` 依赖 `fs.tmpfs` 和 `dev.registry`，固定启动挂载表把 `/modules`、`/tmp`、`/run`
路由到三个独立 tmpfs 根，把 `/dev` 路由到统一目录，特殊设备数据再进 dev.tmpfs。
绝对 OPEN/MKDIR 使用最长组件前缀匹配（`/tmp-other` 不属于 `/tmp`）；
相对及其他句柄请求按 route 转发。uint64_t route 为 0=tmpfs、1=devfs，其他值
返回 EINVAL；响应返回实际 route。文件身份为 (route, handle)，编号不是安全能力。
尚无通用动态挂载协议；SFM_DEPENDS 支持多个依赖名称。

| 请求 | 头部字段 | 载荷/响应 |
| --- | --- | --- |
| FS_ROOT | offset=0..2，仅 tmpfs 后端 | 取得预建根节点；fs.vfs 不对外转发此管理操作 |
| FILE_OPEN | handle=目录，flags 的 CREATE 位可创建 | NUL 结尾相对路径（VFS 也接受绝对路径）；响应 handle、flags=节点类型、result=长度 |
| FILE_MKDIR | 同 OPEN | 只创建最后一级，重复返回 EEXIST |
| FILE_READDIR | handle=目录，offset=条目序号 | result=1、handle、flags、offset=节点长度、NUL 结尾名称；末尾 result=0 |
| FILE_RETAIN / RELEASE | handle | 打开文件描述的引用，与目录链接分开计数；多余 RELEASE 返回 EBADF |
| FILE_SYMLINK | handle=父目录 | 名称 NUL + 目标 NUL；响应节点句柄，符号链接由兼容 VFS 解析 |
| FILE_WRITE | handle、offset | data/size；响应 result=写入字节数 |
| FILE_READ | handle、offset | 载荷为 uint32 期望长度；响应 data/size/result |
| FILE_STAT | handle | 响应 result=文件大小、flags=节点类型 |
| FILE_SEEK | handle、flags=whence、offset=有符号偏移的位表示 | 设备支持 0..2 并返回位置 0；tmpfs 仍由兼容桥计算位置 |
| FILE_TRUNCATE | handle、offset=新长度 | 缩短后扩展的内容为零 |
| FILE_REMOVE | handle | 解除命名链接；无打开引用才回收，非空目录返回 ENOTEMPTY，根返回 EBUSY |

tmpfs 三个根共用 64 个节点配额（含根和目录）；/modules 单文件至多 64 KiB，
/tmp、/run 保留旧 memfs 的 1 MiB 上限。数据按需以 4 KiB 为粒度扩充，扩充分配失败
时保留原数据和长度，不预先为每个文件占满上限。名称最多 63 字节、路径最多 255 字节；消息相对路径拒绝空组件、
点/双点、内嵌 NUL、控制字符和过长组件。兼容 VFS 负责规范化普通程序路径。
写入空洞为零，缩短后再扩展的内容为零，读到末尾返回 0。句柄带代数；unlink 后
已 RETAIN 的节点保持有效，最后 RELEASE 回收，再创建同名文件得到不同节点。
OPEN 只查找/创建节点，不隐式 RETAIN；每个独立打开文件描述 RETAIN 一次，dup/fork
共享描述，最后一次 close/进程退出/exec 关闭引用时 RELEASE。没有 RETAIN 的内部临时
句柄只在命名链接存在期间有效。暂未实现 rename、完整权限主体、持久化、页缓存、
共享内存传输或完整 POSIX 语义。模块接口尚处于内部实验 ABI，后续扩展须版本化。

`kernel/src/fs/sfmodfs.c` 是兼容桥：旧 VFS 缓存公开目录项，节点和数据调用消息服务。
它把 `/modules`、`/tmp`、`/run` 挂入现有路径树，普通 shell 和 Linux ABI 的文件调用、
mkdir/rmdir/getdents64 进入模块；符号链接走原 VFS 解析路径。Linux 打开标志与 VFS
常量对齐，支持截断和每次写前定位末尾的追加模式。单次最多 4 KiB，返回短读/写。
桥的共享 RPC 缓冲区在不阻塞的短临界区中串行使用，使关闭引用不再依赖临时内存分配。
VFS 的命名空间修改、open、目录枚举和最后一次 close 使用同一命名空间锁，避免目录项
尚未固定引用就被 unlink 回收。这些同步回调不得睡眠；未来阻塞服务必须改用可睡眠的
引用/锁协议，当前未据此宣称完整 SMP 文件系统并发支持。
模块的直接内部测试文件不自动成为旧 VFS 的目录项；迁移期以兼容桥为公共入口。
挂载后本阶段不提供卸载/服务替换操作，避免仍有文件引用时释放代码。

`dev.tmpfs` 的 ROOT 仅接受 offset=0，固定节点类型为 DIR 和新增 CHARDEV=4。
OPEN/STAT/READDIR 的字符设备响应 offset 编码设备号（null=0x103、zero=0x105），
文件大小 result 保持 0；READDIR 的 result=1 表示存在条目。OPEN 不隐式 RETAIN。
设备支持短读写、引用计数和位置归零，名字空间不允许变更。
旧 devfs 只将 null/zero 绑定到消息服务，保留硬件节点；ioctl 返回 ENOTTY、mmap
返回 ENODEV，尚无 poll 或用户态驱动。详见 [本轮对接](integration/20261019-devfs-services.md)。

可在交互 shell 读取 `cat /modules/README`。默认 autorun 先运行
`/mnt/bin/test_sfmod_file`，通过实际用户态调用验证文件读写与截断，再启动 Wayland。

## 测试实现

设备注册协议 version=1，record=328 字节，包含路径、逻辑归属、后端编号与元数据，
不传指针。启动登记真实节点后封存，公共目录枚举和消息枚举共用此表。
动态注销/热替换尚未实现，旧硬件 I/O 留在 VFS 适配器。详见
[设备注册对接](integration/20261019-device-registry.md)。

PCI 使用独立 version=1 协议，record=176 字节、query=16 字节，支持 ENUM/FIND/GET。
端点代数标识 snapshot，(snapshot,index) 标识设备引用；不传指针，未知 BAR 大小
不探测。核心 API 只复制缓存，模块负责快照/查询；Bochs DRM 从服务选择 VGA。
具体布局、错误与边界见 [PCI 对接](integration/20261019-pci-snapshot.md)。

块设备 `dev.block` 使用独立 version=2 协议、64 字节固定记录。
原记录的 reserved[0]（偏移 56）改为 geometry_source，偏移 60 保持为零保留项。
来源 UNKNOWN=0、SOFTWARE=1（仅 RAMDISK）、ATA=2（仅 AHCI），未知来源须带
GEOMETRY_UNVERIFIED，已知来源不得带该标志。所有消息 flags=version、route=0，REGISTER 接收记录，SEAL
冻结，ENUM 按清单序号读取，GET 按旧块设备槽号查询且必须携带 snapshot 端点句柄。
响应 offset 始终为清单序号，record.index 才是旧槽号（有空洞时两者不同）。
ENUM 的 offset=count 为 EOF，超过 count 为 EINVAL；GET 不存在槽/失效快照为
ENOENT，槽越界为 EINVAL。未冻结查询 EAGAIN，重复冻结 EBUSY，冻结后登记 EROFS。
记录不包含指针。READ=0x5004：handle=snapshot、offset=LBA，载荷 16 字节
`{uint32 index,count; uint64 reserved=0}`。count 非零，最多 4096 字节，来源/容量须已知；
响应 result=size=字节数，handle=snapshot，offset=LBA。未知容量 ENODEV，越界/长度
错误 EINVAL，OOM ENOMEM，底层忙 EBUSY，其他底层错误 EIO；错误 size=0，载荷清零。
WRITE=0x5005 明确 EROFS；没有新增核心写入口。此限制仅针对新消息路径，原 FAT32
兼容写路径仍存在。核心 API 再检查冻结状态、真实设备/来源及长度/容量，不直接将模块
缓冲区交给 DMA。底层错误隔离页，忙不隔离，可重试；没有超时复位/热恢复协议。
详见 [本轮只读块对接](integration/20261019-block-read.md)。历史 v1 清单说明保留供追溯。

FAT32 实际读已使用 kernel-only block_client（不改变 ABI）：固定快照，最多 64 KiB
拆分 READ，核对每段响应，任意失败清空完整输出；EBUSY/EAGAIN 不自动重试。
sfm_call 在 transport 拒绝时不保证清空 response，客户端必须先清空并先检查返回码。
详见 [FAT32 读取对接](integration/20261019-fat32-read.md)。

`make test-modules` 使用 HOST_CC 编译带 UBSan 的 Linux x86_64 宿主测试，模块保持
目标编译器。测试用 MAP_32BIT 分配可执行内存，以匹配内核低地址重定位约束；这个
测试适配不是生产加载器的内存保护实现。
测试涵盖每一种截断长度、坏 section/重定位、越界/重叠包、缺失与循环依赖、
真实 init 失败、各分配点 OOM 回滚、队列满/空、owner/旧句柄、文件读写、目录/独立根、
unlink/retain/release 生命周期、节点耗尽及首次写入 OOM 后恢复、依赖卸载。
QEMU 使用同一 selftest 调用实际模块，并通过用户态程序检查公共路径、目录枚举、
独立根、截断/追加、旧打开文件及重复回收，另进行 16 次 fork/exit/wait 压力测试。

下一阶段要先将端点所有权改成真正的能力/进程上下文，建立阻塞与取消协议、
独立地址空间和 IRQ/DMA 边界，再迁移设备驱动。当前形式不能隔离恶意或崩溃模块。
