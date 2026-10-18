# sfmod ABI 1 与服务接口

本 ABI 面向 x86_64、小端、受信任 Ring 0 插件。SDK 是 `kernel/include/sfmod.h`。
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

`fs.vfs` 依赖 `fs.tmpfs`，将 `/modules/<name>` 的 OPEN 路由到 tmpfs，其他请求使用
已返回的句柄。首版只有一个挂载后端，不宣称已经实现通用多后端挂载表。

| 请求 | 头部字段 | 载荷/响应 |
| --- | --- | --- |
| FILE_OPEN | flags 的 CREATE 位可创建 | NUL 结尾名称；响应 handle |
| FILE_WRITE | handle、offset | data/size；响应 result=写入字节数 |
| FILE_READ | handle、offset | 载荷为 uint32 期望长度；响应 data/size/result |
| FILE_STAT | handle | 响应 result=文件大小 |
| FILE_TRUNCATE | handle、offset=新长度 | 缩短后扩展的内容为零 |
| FILE_REMOVE | handle | 删除并使旧句柄失效 |

tmpfs 最多 16 个文件，每个 64 KiB，名称最多 31 字节，不含 `/` 和控制字符。
写入空洞为零，读到末尾返回 0；偏移和长度先检查再相加。文件句柄也带代数。
暂未实现目录、rename、权限主体、持久化、页缓存、共享内存传输或 POSIX 的
unlink 后继续访问旧打开文件语义。模块接口尚处于内部实验 ABI，后续扩展须版本化。

`kernel/src/fs/sfmodfs.c` 是兼容桥：目录项由旧 VFS 管理，数据调用消息服务。
它把 `/modules` 挂入现有路径树，普通 shell 和 Linux ABI 的 open/read/write/lseek/
ftruncate/unlink 能使用该路径。单次最多 4 KiB，返回短读/写；禁止建立子目录。
模块的直接内部测试文件不自动成为旧 VFS 的目录项；迁移期以兼容桥为公共入口。
挂载后本阶段不提供卸载/服务替换操作，避免仍有文件引用时释放代码。

可在交互 shell 读取 `cat /modules/README`。默认 autorun 先运行
`/mnt/bin/test_sfmod_file`，通过实际用户态调用验证文件读写与截断，再启动 Wayland。

## 测试实现

`make test-modules` 使用 HOST_CC 编译带 UBSan 的 Linux x86_64 宿主测试，模块保持
目标编译器。测试用 MAP_32BIT 分配可执行内存，以匹配内核低地址重定位约束；这个
测试适配不是生产加载器的内存保护实现。
测试涵盖每一种截断长度、坏 section/重定位、越界/重叠包、缺失与循环依赖、
真实 init 失败、各分配点 OOM 回滚、队列满/空、owner/旧句柄、文件读写与依赖卸载。
QEMU 使用同一 selftest 调用实际模块，并通过用户态程序检查公共路径，并进行 16 次 fork/exit/wait 压力测试。

下一阶段要先将端点所有权改成真正的能力/进程上下文，建立阻塞与取消协议、
独立地址空间和 IRQ/DMA 边界，再迁移设备驱动。当前形式不能隔离恶意或崩溃模块。
