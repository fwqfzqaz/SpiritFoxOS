# SpiritFoxOS

<div align="center">

**灵狐操作系统**

一个从零构建的 x86_64 操作系统内核，采用 Multiboot2 引导，兼容 Linux ABI

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL%203.0-blue.svg)](LICENSE.txt)
[![Architecture](https://img.shields.io/badge/Arch-x86__64-green.svg)]()
[![Language](https://img.shields.io/badge/Language-C%2FASM-orange.svg)]()

</div>

---

## 目录

- [项目简介](#项目简介)
- [特性概览](#特性概览)
- [项目结构](#项目结构)
- [构建依赖](#构建依赖)
- [构建与运行](#构建与运行)
- [启动流程](#启动流程)
- [内核架构](#内核架构)
  - [内存管理](#内存管理)
  - [进程管理](#进程管理)
  - [文件系统](#文件系统)
  - [系统调用](#系统调用)
  - [设备驱动](#设备驱动)
  - [网络栈](#网络栈)
  - [图形与终端](#图形与终端)
  - [系统服务](#系统服务)
- [Shell 命令](#shell-命令)
- [开发与调试](#开发与调试)
- [许可证](#许可证)

---

## 项目简介

SpiritFoxOS 是一个教育性质的操作系统内核项目，从引导加载器到内核全部从零实现。内核运行在 x86_64 长模式下，通过 32 位 Multiboot2 引导器完成从保护模式到长模式的切换，最终进入 64 位内核。

项目实现了完整的操作系统基础设施：物理/虚拟内存管理、多进程调度、VFS 虚拟文件系统、FAT32 文件系统、AHCI 磁盘驱动、RTL8139 网络驱动、图形界面、交互式 Shell 以及包管理和沙箱等高级服务。

系统调用接口兼容 Linux x86-64 ABI，可加载并执行标准 ELF 格式的用户态程序。

---

## 特性概览

| 类别 | 特性 |
|------|------|
| **架构** | x86_64 长模式，Multiboot2 引导，32→64 位模式切换 |
| **内存** | 位图式物理页帧分配器、4 级页表 (PML4) 虚拟内存、内核堆 (kmalloc)、ELF 加载器 |
| **进程** | PCB 进程控制块、优先级调度、fork/exec/clone、信号、futex、COW 写时复制 |
| **系统调用** | 90+ 个系统调用，兼容 Linux x86-64 编号，含 SFK/Registry 自定义扩展 |
| **文件系统** | VFS 抽象层，支持 memfs / devfs / FAT32 / procfs / ramdisk，管道与符号链接 |
| **存储** | AHCI SATA 驱动，块设备抽象层，FAT32 读写 |
| **网络** | RTL8139 网卡驱动，基础 TCP/IP 协议栈，BSD socket API |
| **图形** | VBE 帧缓冲、Nuklear 即时模式 GUI、窗口管理器、终端模拟器 |
| **服务** | Shell、注册表 (Registry)、包管理器 (pkgmgr)、沙箱 (Sandbox)、自动运行 (autorun)、内核模块 |
| **硬件** | ACPI、APIC (本地/IO)、PCI 设备枚举、键盘、鼠标、定时器、串口 |

---

## 项目结构

```
SpiritFoxOS/
├── boot/
│   ├── multiboot/              # 32 位 Multiboot2 引导加载器
│   │   ├── boot32.S            # 汇编入口 (GRUB → 保护模式)
│   │   ├── multiboot32.c       # C 入口 (收集启动信息, 切换到长模式)
│   │   └── linker.ld           # 32 位链接脚本
│   └── uefi/                   # UEFI 引导 (开发中)
│       ├── boot.c
│       └── boot.h
├── kernel/
│   ├── include/                # 内核头文件 (50+ 个模块)
│   │   ├── acpi.h              # ACPI 表解析
│   │   ├── ahci.h              # AHCI SATA 控制器
│   │   ├── apic.h              # APIC 中断控制器
│   │   ├── blkdev.h            # 块设备抽象
│   │   ├── boot.h              # 启动信息结构体
│   │   ├── elf64.h             # ELF64 格式定义
│   │   ├── fb.h                # 帧缓冲
│   │   ├── futex.h             # 快速用户空间互斥锁
│   │   ├── gdt.h               # 全局描述符表
│   │   ├── idt.h               # 中断描述符表
│   │   ├── kmalloc.h           # 内核堆分配器
│   │   ├── memory.h            # 物理内存管理器
│   │   ├── mmu.h               # 虚拟内存/页表
│   │   ├── nuklear.h           # Nuklear GUI 库
│   │   ├── pci.h               # PCI 设备枚举
│   │   ├── process.h           # 进程控制块
│   │   ├── registry.h          # 注册表
│   │   ├── sandbox.h           # 沙箱权限
│   │   ├── shell.h             # 交互式 Shell
│   │   ├── syscall.h           # 系统调用定义
│   │   ├── vfs.h               # 虚拟文件系统
│   │   ├── vga.h               # VGA 图形
│   │   ├── window.h            # 窗口管理器
│   │   └── ...                 # 其他头文件
│   └── src/
│       ├── entry.c             # 内核入口 (_start64 → kernel_main)
│       ├── init.c              # 子系统分步初始化
│       ├── string.c            # 字符串/内存工具函数
│       ├── arch/x86_64/        # 架构相关代码
│       │   ├── acpi.c          #   ACPI 表解析 (RSDP/RSDT/MADT)
│       │   ├── apic.c          #   Local APIC & IO APIC
│       │   ├── boot32.S        #   32 位启动汇编
│       │   ├── gdt.c           #   GDT 加载与 TSS
│       │   ├── hal.c           #   硬件抽象层 (中断开关)
│       │   ├── idt.c           #   IDT 初始化与 ISR 注册
│       │   └── isr_stub.S      #   中断存根 (保存/恢复寄存器)
│       ├── drivers/            # 设备驱动
│       │   ├── ahci.c          #   AHCI SATA 控制器驱动
│       │   ├── blkdev.c        #   块设备抽象层
│       │   ├── keyboard.c      #   PS/2 键盘驱动
│       │   ├── mouse.c         #   PS/2 鼠标驱动
│       │   ├── pci.c           #   PCI 配置空间枚举
│       │   ├── rtl8139.c       #   RTL8139 以太网卡驱动
│       │   ├── serial.c        #   串口 (COM1) 驱动
│       │   └── timer.c         #   HPET/ACPI 定时器
│       ├── fs/                 # 文件系统实现
│       │   ├── vfs.c           #   VFS 核心实现 (路径解析/挂载/文件操作)
│       │   ├── fat32.c         #   FAT32 文件系统驱动
│       │   ├── memfs.c         #   内存文件系统 (根文件系统)
│       │   ├── devfs.c         #   设备文件系统 (/dev)
│       │   ├── procfs.c        #   进程信息文件系统 (/proc)
│       │   └── ramdisk.c       #   内存磁盘块设备
│       ├── mm/                 # 内存管理
│       │   ├── memory.c        #   物理页帧分配器 (位图)
│       │   ├── mmu.c           #   虚拟内存管理 (PML4 页表)
│       │   ├── kmalloc.c       #   内核堆分配器
│       │   └── elf_loader.c    #   ELF 可执行文件加载器
│       ├── net/                # 网络栈
│       │   └── net.c           #   RTL8139 驱动 + TCP/IP 协议栈
│       ├── proc/               # 进程管理
│       │   ├── process.c       #   进程创建/销毁/调度
│       │   ├── process_user.c  #   用户态进程加载
│       │   ├── scheduler.c     #   优先级调度器
│       │   ├── signal.c        #   POSIX 信号处理
│       │   └── futex.c         #   快速用户空间互斥锁
│       ├── services/           # 系统服务
│       │   ├── shell.c         #   交互式命令行 Shell (36 条命令)
│       │   ├── autorun.c       #   自动运行 (/etc/autorun.cfg)
│       │   ├── registry.c      #   注册表 (类 Windows 注册表)
│       │   ├── pkgmgr.c        #   包管理器 (DEB 包安装)
│       │   ├── sandbox.c       #   沙箱权限隔离
│       │   ├── module.c        #   内核模块加载
│       │   └── gunzip.c        #   gzip 解压
│       ├── syscall/            # 系统调用
│       │   ├── syscall.c       #   系统调用分发表
│       │   ├── syscall_file.c  #   文件 I/O 系统调用
│       │   ├── syscall_mem.c   #   内存管理系统调用
│       │   ├── syscall_process.c # 进程管理系统调用
│       │   ├── syscall_net.c   #   网络系统调用
│       │   ├── syscall_misc.c  #   杂项系统调用
│       │   └── syscall_internal.h
│       ├── video/              # 图形子系统
│       │   ├── fb.c            #   帧缓冲抽象
│       │   ├── vga.c           #   VGA 文本/图形模式
│       │   ├── nuklear_fb.c    #   Nuklear GUI 帧缓冲后端
│       │   ├── terminal.c      #   终端模拟器
│       │   └── window.c        #   窗口管理器
│       ├── test/               # 内核自测试
│       │   └── test.c
│       ├── kernel.ld           # 64 位链接脚本
│       ├── kernel32.ld         # 32 位链接脚本
│       └── osimage.ld          # OS 镜像链接脚本
├── tools/
│   ├── png2c.py                # Logo PNG → C 头文件转换工具
│   └── build_jre_deb.sh        # JRE DEB 包构建脚本
├── Makefile                    # 构建系统
├── logo.png                    # 启动 Logo
├── .gitignore                  # Git 忽略规则
├── LICENSE.txt                 # GPL-3.0 许可证
└── README.md                   # 本文件
```

---

## 构建依赖

| 工具 | 用途 | 安装 (Ubuntu/Debian) |
|------|------|---------------------|
| `gcc` / `x86_64-linux-gnu-gcc` | 交叉编译器 | `sudo apt install gcc gcc-multilib` |
| `nasm` | 汇编器 | `sudo apt install nasm` |
| `ld` / `objcopy` | 链接器与二进制工具 | `sudo apt install binutils` |
| `grub-mkrescue` | 生成 GRUB 可启动 ISO | `sudo apt install grub-pc-bin grub-common` |
| `xorriso` | ISO 生成依赖 | `sudo apt install xorriso` |
| `mtools` | FAT32 镜像操作 | `sudo apt install mtools` |
| `qemu-system-x86_64` | 运行与调试 | `sudo apt install qemu-system-x86` |
| `python3` | Logo 资源生成 | `sudo apt install python3` |

一键安装所有依赖：

```bash
sudo apt install gcc gcc-multilib nasm binutils grub-pc-bin grub-common xorriso mtools qemu-system-x86 python3
```

---

## 构建与运行

```bash
# 构建所有 (生成 GRUB 可启动 ISO)
make

# 仅构建内核 (需要先生成 Logo 头文件)
make logo    # 首次需要: logo.png → kernel/include/logo_data.h
make kernel

# 仅构建引导加载器
make loader

# 运行 (QEMU, 1GB 内存, 串口终端, 网络端口转发, AHCI 磁盘)
make run

# 日志模式 (串口输出到 build/serial.log, 无显示)
make run-log

# 调试模式 (GDB 远程调试, 端口 1234)
make run-debug

# 清理所有构建产物
make clean
```

### QEMU 运行配置

`make run` 启动的 QEMU 包含以下硬件模拟：

- **内存**: 1GB RAM
- **显示**: 串口终端 (stdio) + 标准 VGA
- **网络**: RTL8139 网卡，端口转发 `tcp::25565 → :25565`
- **存储**: AHCI 控制器 + 两个磁盘镜像
  - `build/fat32.img` — 512MB FAT32 磁盘 (自动创建)
  - `build/disk.img` — 16MB 原始磁盘 (自动创建)
- **串口**: COM1 → 标准输出 (用于内核日志和 Shell 交互)

---

## 启动流程

```
┌─────────────────────────────────────────────────┐
│  1. GRUB 加载 loader.elf (Multiboot2 协议)       │
│     └── 读取 /boot/grub/grub.cfg                 │
├─────────────────────────────────────────────────┤
│  2. boot32.S / multiboot32.c (32 位保护模式)      │
│     ├── 收集 Multiboot2 启动信息                   │
│     ├── 设置临时页表 (1GB identity mapping)        │
│     ├── 启用分页, 切换到 64 位长模式               │
│     └── 跳转至 _start64                           │
├─────────────────────────────────────────────────┤
│  3. _start64 (entry.c)                           │
│     ├── 设置内核栈 (0x800000)                     │
│     ├── 清零 BSS 段                               │
│     └── 调用 kernel_main(boot_info)              │
├─────────────────────────────────────────────────┤
│  4. kernel_main (entry.c → init.c)              │
│     ├── init_core()      — GDT, IDT, 内存, VGA  │
│     ├── init_hardware()  — ACPI, APIC, PCI,     │
│     │                       键盘, 鼠标           │
│     ├── init_storage()   — 块设备, AHCI,         │
│     │                       ramdisk, 终端         │
│     ├── init_filesystem()— VFS, memfs, devfs,    │
│     │                       FAT32, 挂载根文件系统 │
│     ├── init_services()  — kmalloc, 进程,        │
│     │                       系统调用, Registry,   │
│     │                       包管理, 沙箱, 网络    │
│     ├── init_fs_hierarchy()— FHS 目录, procfs    │
│     ├── hal_enable_interrupts()                  │
│     ├── autorun_execute()— 执行 /etc/autorun.cfg │
│     └── shell_run()      — 交互式 Shell          │
└─────────────────────────────────────────────────┘
```

---

## 内核架构

### 内存管理

**物理内存管理器 (PMM)** — `mm/memory.c`

- 基于位图的页帧分配器，管理最多 512MB 物理内存 (131072 个 4KB 页)
- 使用 16KB 静态位图，线性扫描分配空闲页
- 支持 `alloc_page()` / `alloc_pages(count)` / `free_page()` 接口
- 初始化时根据固件内存映射标记可用页，保留内核和栈区域

**虚拟内存 (MMU)** — `mm/mmu.c`

- 4 级页表 (PML4 → PDPT → PD → PT)，4KB 页面粒度
- 为每个进程创建独立地址空间
- 支持 `mmap` / `mprotect` / `munmap` 系统调用

**内核堆** — `mm/kmalloc.c`

- 内核动态内存分配器，提供 `kmalloc()` / `kfree()` 接口
- 基于块分配策略，支持不同大小的内存分配

**ELF 加载器** — `mm/elf_loader.c`

- 解析 ELF64 可执行文件，加载段到进程地址空间
- 设置进程入口点、栈和堆

**内核内存布局** — `kernel.ld`

| 区域 | 地址 | 说明 |
|------|------|------|
| 内核加载基址 | `0x100000` (1MB) | 传统 x86 内核加载位置 |
| 代码段 `.text` | 1MB 起 | 含 `.text.entry` (入口代码优先) |
| 只读数据 `.rodata` | 4K 对齐 | 常量数据 |
| 可写数据 `.data` | 4K 对齐 | 已初始化全局变量 |
| BSS `.bss` | 4K 对齐 | 未初始化数据 (`__bss_start` ~ `__bss_end`) |
| 内核栈 | `0x800000` (8MB) | 8MB-16MB 保留区域 |

### 进程管理

**进程控制块 (PCB)** — `include/process.h`

```c
typedef struct process {
    pid_t pid, ppid;           // 进程 ID
    int state;                  // READY/RUNNING/BLOCKED/ZOMBIE
    int flags;                  // KERNEL/SFK/DEB/COW 标志
    uid_t uid, euid;            // 用户身份
    gid_t gid, egid;
    int priority;               // 调度优先级
    uint64_t pml4;              // 页表物理地址
    uint64_t kernel_stack;      // 内核栈
    uint64_t entry_point;       // 入口地址
    uint64_t brk;               // 堆顶
    uint64_t stack_top;         // 用户栈顶
    int fd_table[256];          // 文件描述符表
    char cwd[256];              // 当前工作目录
    uint32_t signal_mask;       // 信号掩码
    void *signal_handlers[32];  // 信号处理函数
    uint32_t sfk_perms;         // 沙箱权限位掩码
    char sfk_pkg_id[64];        // 沙箱包 ID
    trap_frame_t *trap_frame;   // 中断/系统调用保存的寄存器
    struct process *parent, *child, *sibling; // 进程树
} process_t;
```

**调度器** — `proc/scheduler.c`

- 优先级调度算法，支持时间片轮转
- 通过 APIC 定时器中断触发调度
- 支持 `sleep` 阻塞与唤醒

**进程操作**

| 操作 | 说明 |
|------|------|
| `fork` | 创建子进程，支持 COW 写时复制 |
| `execve` | 加载 ELF 可执行文件替换当前进程映像 |
| `clone` | 创建线程 (共享地址空间) |
| `exit` / `wait` | 进程退出与父进程回收 |
| `signal` | POSIX 信号发送与处理 |
| `futex` | 快速用户空间互斥锁 |

### 文件系统

**VFS 抽象层** — `fs/vfs.c`

提供统一的文件系统接口，核心 API：

| API | 功能 |
|-----|------|
| `vfs_open()` / `vfs_close()` | 打开/关闭文件 |
| `vfs_read()` / `vfs_write()` | 读/写文件 |
| `vfs_seek()` | 文件定位 |
| `vfs_mkdir()` / `vfs_rmdir()` | 创建/删除目录 |
| `vfs_unlink()` | 删除文件 |
| `vfs_readdir()` | 读取目录项 |
| `vfs_mount()` / `vfs_unmount()` | 挂载/卸载文件系统 |
| `vfs_resolve_path()` | 路径解析 |
| `vfs_pipe()` | 创建管道 |
| `vfs_symlink()` / `vfs_readlink()` | 符号链接 |
| `vfs_dup()` / `vfs_dup2()` | 复制文件描述符 |

**支持的文件系统**

| 文件系统 | 挂载点 | 说明 |
|---------|--------|------|
| memfs | `/` | 内存文件系统，根文件系统 |
| devfs | `/dev` | 设备文件系统，自动枚举块设备/字符设备 |
| fat32 | `/mnt` | FAT32 文件系统，读写 AHCI 磁盘 |
| procfs | `/proc` | 进程信息文件系统 |
| ramdisk | — | 内存磁盘块设备 |

**默认目录结构 (FHS)**

```
/           memfs 根文件系统
├── bin/    用户命令
├── sbin/   系统命令
├── etc/    配置文件 (含 autorun.cfg)
├── dev/    设备文件 (devfs)
├── proc/   进程信息 (procfs)
├── mnt/    FAT32 挂载点
├── usr/    用户程序
│   ├── bin/
│   ├── lib/
│   └── share/
├── var/    可变数据
│   └── log/
├── opt/    可选软件
│   └── sfk/
├── home/   用户主目录
├── root/   超级用户主目录
├── tmp/    临时文件
└── sys/    系统信息
```

### 系统调用

系统调用采用 `syscall` 指令 (x86-64)，编号兼容 Linux ABI：

**文件 I/O (0-293)**

| 号 | 调用 | 号 | 调用 |
|----|------|----|------|
| 0 | read | 1 | write |
| 2 | open | 3 | close |
| 4-6 | stat/fstat/lstat | 8 | lseek |
| 9-12 | mmap/mprotect/munmap/brk | 16 | ioctl |
| 79-80 | getcwd/chdir | 83-84 | mkdir/rmdir |
| 87 | unlink | 89 | readlink |
| 90-91 | chmod/fchmod | 217 | getdents64 |
| 292-293 | dup3/pipe2 | | |

**进程管理 (39-234)**

| 号 | 调用 | 号 | 调用 |
|----|------|----|------|
| 39 | getpid | 56-58 | clone/fork/vfork |
| 59 | execve | 60-61 | exit/wait4 |
| 62 | kill | 102-108 | uid/gid 操作 |
| 158 | arch_prctl | 186 | gettid |
| 231 | exit_group | 234 | tgkill |

**信号处理 (13-131)**

| 号 | 调用 |
|----|------|
| 13-15 | rt_sigaction/rt_sigprocmask/rt_sigreturn |
| 127 | rt_sigpending | 130-131 | rt_sigsuspend/sigaltstack |

**网络 (41-55)**

| 号 | 调用 |
|----|------|
| 41-43 | socket/connect/accept |
| 44-45 | sendto/recvfrom |
| 48-55 | shutdown/bind/listen/getsockname/getpeername/socketpair/setsockopt/getsockopt |

**SFK 自定义 (600-604)**

| 号 | 调用 | 说明 |
|----|------|------|
| 600 | sfk_register_pkg | 注册沙箱包 |
| 601 | sfk_check_perm | 检查沙箱权限 |
| 602 | sfk_request_perm | 请求沙箱权限 |
| 603 | sfk_get_pkg_info | 获取包信息 |
| 604 | sfk_list_perms | 列出权限 |

**注册表自定义 (700-709)**

| 号 | 调用 | 说明 |
|----|------|------|
| 700-701 | reg_open / reg_close | 打开/关闭注册表键 |
| 702-704 | reg_read / reg_write / reg_delete | 读写删除注册表值 |
| 705-706 | reg_list / reg_exists | 列出/检查键 |
| 707-709 | reg_transaction_begin / commit / abort | 注册表事务 |

### 设备驱动

| 驱动 | 文件 | 说明 |
|------|------|------|
| AHCI SATA | `drivers/ahci.c` | AHCI 控制器初始化、命令队列、DMA 传输 |
| 块设备 | `drivers/blkdev.c` | 统一块设备抽象 (AHCI/Ramdisk) |
| PCI | `drivers/pci.c` | PCI 配置空间读写、设备枚举 |
| 键盘 | `drivers/keyboard.c` | PS/2 键盘中断处理、扫描码转换 |
| 鼠标 | `drivers/mouse.c` | PS/2 鼠标中断处理、移动/点击事件 |
| RTL8139 | `drivers/rtl8139.c` | Realtek 以太网卡驱动、收发环形缓冲区 |
| 串口 | `drivers/serial.c` | COM1 串口驱动，用于内核日志输出 |
| 定时器 | `drivers/timer.c` | HPET/ACPI 定时器，提供时钟中断和计时 |

### 网络栈

`net/net.c` 实现了基于 RTL8139 的基础 TCP/IP 协议栈：

- **链路层**: RTL8139 以太网帧收发
- **网络层**: IPv4 数据包处理
- **传输层**: TCP/UDP 协议
- **Socket API**: 兼容 BSD socket 接口 (socket/bind/listen/accept/connect/send/recv)

### 图形与终端

| 组件 | 文件 | 说明 |
|------|------|------|
| 帧缓冲 | `video/fb.c` | VBE 帧缓冲抽象，像素绘制 |
| VGA | `video/vga.c` | VGA 文本/图形模式管理 |
| Nuklear GUI | `video/nuklear_fb.c` | Nuklear 即时模式 GUI，帧缓冲后端 |
| 终端 | `video/terminal.c` | 终端模拟器 (光标/滚动/颜色/转义序列) |
| 窗口管理 | `video/window.c` | 图形窗口管理器 |

### 系统服务

| 服务 | 文件 | 说明 |
|------|------|------|
| Shell | `services/shell.c` | 交互式命令行，36 条内置命令 |
| Autorun | `services/autorun.c` | 启动时自动执行 `/etc/autorun.cfg` 中的命令 |
| Registry | `services/registry.c` | 类 Windows 注册表，键值存储 |
| 包管理 | `services/pkgmgr.c` | DEB 包安装/卸载/查询 |
| 沙箱 | `services/sandbox.c` | 基于权限位掩码的进程隔离 |
| 模块 | `services/module.c` | 内核模块加载/卸载 |
| 解压 | `services/gunzip.c` | gzip 解压缩 |

---

## Shell 命令

SpiritFoxOS 内置 36 条 Shell 命令：

### 系统信息

| 命令 | 说明 |
|------|------|
| `help` | 显示帮助信息 |
| `version` | 显示系统版本 |
| `about` | 关于 SpiritFoxOS |
| `uptime` | 系统运行时间 |
| `meminfo` | 内存使用信息 |
| `dmesg` | 内核日志 |
| `pcilist` | 列出 PCI 设备 |
| `blklist` | 列出块设备 |
| `ps` | 列出进程 |

### 文件操作

| 命令 | 说明 |
|------|------|
| `ls [path]` | 列出目录 |
| `cd <path>` | 切换目录 |
| `pwd` | 当前工作目录 |
| `cat <file>` | 显示文件内容 |
| `mkdir <dir>` | 创建目录 |
| `touch <file>` | 创建空文件 |
| `rm <file>` | 删除文件 |
| `rmdir <dir>` | 删除目录 |
| `cp <src> <dst>` | 复制文件 |
| `mv <src> <dst>` | 移动/重命名 |
| `stat <file>` | 显示文件信息 |
| `hexdump <file>` | 十六进制转储 |
| `writefile <file> <text>` | 写入文本到文件 |
| `mount <type> <path>` | 挂载文件系统 |
| `fileassoc` | 文件关联管理 |
| `vfstest` | VFS 测试 |

### 扩展功能

| 命令 | 说明 |
|------|------|
| `exec <elf>` | 执行 ELF 二进制文件 |
| `reg <op> <key>` | 注册表操作 |
| `pkg <op> <pkg>` | 包管理操作 |
| `sandbox` | 沙箱状态 |
| `autorun [path]` | 执行 autorun 配置 |
| `window` | 进入图形窗口管理器 |
| `lsmod` | 列出已加载模块 |
| `loadmod <mod>` | 加载内核模块 |
| `unloadmod <mod>` | 卸载内核模块 |

### 其他

| 命令 | 说明 |
|------|------|
| `echo <text>` | 打印文本 |
| `clear` | 清屏 |
| `reboot` | 重启系统 |

---

## 开发与调试

### GDB 远程调试

```bash
# 终端 1: 启动 QEMU (等待 GDB 连接)
make run-debug

# 终端 2: 连接 GDB
gdb build/kernel/kernel.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

### 串口日志

内核通过 COM1 串口输出详细日志。`make run` 直接在终端显示串口输出，`make run-log` 将日志写入 `build/serial.log`。

### 内核自测试

在 `kernel/src/test/test.c` 中包含 VFS、FAT32、Registry 等模块的测试用例。编译时定义 `KERNEL_SELFTEST` 宏可在启动时自动运行：

```bash
make clean
KERNEL_CFLAGS="-DKERNEL_SELFTEST" make
```

---

## 许可证

本项目基于 [GNU General Public License v3.0](LICENSE.txt) 许可证开源。
