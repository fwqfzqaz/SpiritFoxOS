# SpiritFoxOS

<div align="center">

**灵狐操作系统**

一个从零构建的 x86_64 操作系统内核，采用 Multiboot2 / UEFI 双引导，兼容 Linux ABI

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
  - [对称多处理 (SMP)](#对称多处理-smp)
- [用户态支持](#用户态支持)
- [Shell 命令](#shell-命令)
- [开发与调试](#开发与调试)
- [许可证](#许可证)

---

## 项目简介

SpiritFoxOS 是一个教育性质的操作系统内核项目，从引导加载器到内核全部从零实现。内核运行在 x86_64 长模式下，支持两种引导方式：

- **BIOS 引导**：通过 32 位 Multiboot2 引导器完成从保护模式到长模式的切换
- **UEFI 引导**：自包含 UEFI bootloader，无需 gnu-efi 依赖，直接进入 64 位长模式

项目实现了完整的操作系统基础设施：物理/虚拟/SLAB 内存管理、多进程调度与 SMP、VFS 虚拟文件系统、FAT32 文件系统、AHCI 磁盘驱动、分层网络协议栈 (RTL8139 + TCP/IP + ICMP)、图形界面、交互式 Shell 以及包管理和沙箱等高级服务。

系统调用接口兼容 Linux x86-64 ABI，可加载并执行标准 ELF 格式的用户态程序。项目同时包含一个轻量级 libc 实现，支持用户态 C 程序开发。

---

## 特性概览

| 类别 | 特性 |
|------|------|
| **架构** | x86_64 长模式，Multiboot2 / UEFI 双引导，32→64 位模式切换 |
| **内存** | 位图式物理页帧分配器、4 级页表 (PML4) 虚拟内存、内核堆 (kmalloc)、SLAB 分配器、ELF 加载器 |
| **进程** | PCB 进程控制块、优先级调度、fork/exec/clone、信号、futex、COW 写时复制 |
| **SMP** | 多核启动 (AP Trampoline)、Per-CPU 数据、自旋锁、IPI 中断 |
| **系统调用** | 150+ 个系统调用定义，兼容 Linux x86-64 编号，含 SFK/Registry 自定义扩展 |
| **文件系统** | VFS 抽象层，支持 memfs / devfs / FAT32 / procfs / ramdisk，管道与符号链接 |
| **存储** | AHCI SATA 驱动，块设备抽象层，FAT32 读写 |
| **网络** | RTL8139 网卡驱动，分层 TCP/IP 协议栈 (Ethernet/ARP/IPv4/ICMP/TCP/UDP)，BSD socket API，ping 支持 |
| **图形** | VBE 帧缓冲、Nuklear 即时模式 GUI、窗口管理器、终端模拟器 |
| **服务** | Shell、注册表 (Registry)、包管理器 (pkgmgr)、沙箱 (Sandbox)、自动运行 (autorun)、内核模块 |
| **用户态** | 轻量级 libc (crt0 + 系统调用封装)、测试 ELF 程序 |
| **硬件** | ACPI、APIC (本地/IO)、PCI 设备枚举、键盘、鼠标、定时器、串口 |

---

## 项目结构

```
SpiritFoxOS/
├── boot/
│   ├── multiboot/              # 32 位 Multiboot2 引导加载器 (BIOS)
│   │   ├── boot32.S            #   汇编入口 (GRUB → 保护模式)
│   │   ├── multiboot32.c       #   C 入口 (收集启动信息, 切换到长模式)
│   │   └── linker.ld           #   32 位链接脚本
│   └── uefi/                   # UEFI 引导加载器
│       ├── boot.c              #   自包含 UEFI bootloader (无 gnu-efi)
│       ├── boot.h              #   UEFI 类型与协议定义
│       ├── crt0-efi-x86_64.S   #   UEFI 入口汇编
│       ├── gen_reloc.py        #   PE 重定位表生成脚本
│       ├── linker.ld           #   UEFI 链接脚本
│       └── test_efi.c          #   UEFI 测试代码
├── kernel/
│   ├── include/                # 内核头文件 (55+ 个模块)
│   │   ├── acpi.h              #   ACPI 表解析
│   │   ├── ahci.h              #   AHCI SATA 控制器
│   │   ├── apic.h              #   APIC 中断控制器
│   │   ├── autorun.h           #   自动运行服务
│   │   ├── blkdev.h            #   块设备抽象
│   │   ├── boot.h              #   启动信息结构体
│   │   ├── clone.h             #   Linux clone 标志定义
│   │   ├── console.h           #   控制台输出
│   │   ├── devfs.h             #   设备文件系统
│   │   ├── elf64.h             #   ELF64 格式定义
│   │   ├── errno.h             #   错误码定义
│   │   ├── fat32.h             #   FAT32 文件系统
│   │   ├── fb.h                #   帧缓冲
│   │   ├── futex.h             #   快速用户空间互斥锁
│   │   ├── gdt.h               #   全局描述符表
│   │   ├── gunzip.h            #   gzip 解压
│   │   ├── hal.h               #   硬件抽象层
│   │   ├── idt.h               #   中断描述符表
│   │   ├── init.h              #   子系统初始化声明
│   │   ├── keyboard.h          #   PS/2 键盘
│   │   ├── kmalloc.h           #   内核堆分配器
│   │   ├── memfs.h             #   内存文件系统
│   │   ├── memory.h            #   物理内存管理器
│   │   ├── mmu.h               #   虚拟内存/页表
│   │   ├── module.h            #   内核模块
│   │   ├── mouse.h             #   PS/2 鼠标
│   │   ├── net.h               #   网络子系统
│   │   ├── net_arp.h           #   ARP 协议
│   │   ├── net_eth.h           #   以太网层
│   │   ├── net_icmp.h          #   ICMP 协议
│   │   ├── net_ip.h            #   IPv4 协议
│   │   ├── net_socket.h        #   Socket API
│   │   ├── net_tcp.h           #   TCP 协议
│   │   ├── net_udp.h           #   UDP 协议
│   │   ├── net_utils.h         #   网络校验和等工具
│   │   ├── nuklear.h           #   Nuklear GUI 库
│   │   ├── pci.h               #   PCI 设备枚举
│   │   ├── pkgmgr.h            #   包管理器
│   │   ├── process.h           #   进程控制块与调度器 API
│   │   ├── procfs.h            #   进程信息文件系统
│   │   ├── ramdisk.h           #   内存磁盘块设备
│   │   ├── registry.h          #   注册表
│   │   ├── rtl8139.h           #   RTL8139 网卡驱动
│   │   ├── sandbox.h           #   沙箱权限隔离
│   │   ├── serial.h            #   串口驱动
│   │   ├── sfk_perms.h         #   SFK 权限位定义
│   │   ├── shell.h             #   交互式 Shell
│   │   ├── slab.h              #   SLAB 分配器
│   │   ├── smp.h               #   对称多处理
│   │   ├── string.h            #   字符串/内存工具函数
│   │   ├── syscall.h           #   系统调用定义
│   │   ├── terminal.h          #   终端模拟器
│   │   ├── timer.h             #   定时器
│   │   ├── vfs.h               #   虚拟文件系统
│   │   ├── vga.h               #   VGA 图形
│   │   └── window.h            #   窗口管理器
│   └── src/
│       ├── entry.c             #   内核入口 (_start64 → kernel_main)
│       ├── init.c              #   子系统分步初始化
│       ├── string.c            #   字符串/内存工具函数
│       ├── kernel.ld           #   64 位链接脚本
│       ├── kernel32.ld         #   32 位链接脚本
│       ├── osimage.ld          #   OS 镜像链接脚本
│       ├── arch/x86_64/        #   架构相关代码
│       │   ├── acpi.c          #     ACPI 表解析 (RSDP/RSDT/MADT)
│       │   ├── apic.c          #     Local APIC & IO APIC
│       │   ├── ap_trampoline.S #     AP 启动跳板代码 (raw binary)
│       │   ├── boot32.S        #     32 位启动汇编
│       │   ├── gdt.c           #     GDT 加载与 TSS
│       │   ├── hal.c           #     硬件抽象层 (中断开关)
│       │   ├── idt.c           #     IDT 初始化与 ISR 注册
│       │   ├── isr_stub.S      #     中断存根 (保存/恢复寄存器)
│       │   └── smp.c           #     SMP 多核启动与 IPI
│       ├── drivers/            #   设备驱动
│       │   ├── ahci.c          #     AHCI SATA 控制器驱动
│       │   ├── blkdev.c        #     块设备抽象层
│       │   ├── keyboard.c      #     PS/2 键盘驱动
│       │   ├── mouse.c         #     PS/2 鼠标驱动
│       │   ├── net/            #     网络驱动
│       │   │   └── rtl8139.c   #       RTL8139 以太网卡驱动
│       │   ├── pci.c           #     PCI 配置空间枚举
│       │   ├── serial.c        #     串口 (COM1) 驱动
│       │   └── timer.c         #     HPET/ACPI 定时器
│       ├── fs/                 #   文件系统实现
│       │   ├── vfs.c           #     VFS 核心实现 (路径解析/挂载/文件操作)
│       │   ├── fat32.c         #     FAT32 文件系统驱动
│       │   ├── memfs.c         #     内存文件系统 (根文件系统)
│       │   ├── devfs.c         #     设备文件系统 (/dev)
│       │   ├── procfs.c        #     进程信息文件系统 (/proc)
│       │   └── ramdisk.c       #     内存磁盘块设备
│       ├── mm/                 #   内存管理
│       │   ├── memory.c        #     物理页帧分配器 (位图)
│       │   ├── mmu.c           #     虚拟内存管理 (PML4 页表)
│       │   ├── kmalloc.c       #     内核堆分配器
│       │   ├── slab.c          #     SLAB 分配器
│       │   └── elf_loader.c    #     ELF 可执行文件加载器
│       ├── net/                #   分层网络协议栈
│       │   ├── eth.c           #     以太网帧收发
│       │   ├── arp.c           #     ARP 地址解析协议
│       │   ├── ip.c            #     IPv4 数据包处理
│       │   ├── icmp.c          #     ICMP 协议 (Echo Request/Reply)
│       │   ├── tcp.c           #     TCP 协议 (状态机/滑动窗口/重传)
│       │   ├── udp.c           #     UDP 协议
│       │   ├── socket.c        #     BSD Socket API 实现
│       │   └── net_utils.c     #     校验和计算等网络工具
│       ├── proc/               #   进程管理
│       │   ├── process.c       #     进程创建/销毁/调度
│       │   ├── process_user.c  #     用户态进程加载
│       │   ├── scheduler.c     #     优先级调度器
│       │   ├── signal.c        #     POSIX 信号处理
│       │   └── futex.c         #     快速用户空间互斥锁
│       ├── services/           #   系统服务
│       │   ├── shell.c         #     交互式命令行 Shell (38 条命令)
│       │   ├── autorun.c       #     自动运行 (/etc/autorun.cfg)
│       │   ├── registry.c      #     注册表 (类 Windows 注册表)
│       │   ├── pkgmgr.c        #     包管理器 (DEB 包安装)
│       │   ├── sandbox.c       #     沙箱权限隔离
│       │   ├── module.c        #     内核模块加载
│       │   └── gunzip.c        #     gzip 解压
│       ├── syscall/            #   系统调用
│       │   ├── syscall.c       #     系统调用分发表
│       │   ├── syscall_file.c  #     文件 I/O 系统调用
│       │   ├── syscall_mem.c   #     内存管理系统调用
│       │   ├── syscall_process.c #   进程管理系统调用
│       │   ├── syscall_net.c   #     网络系统调用
│       │   ├── syscall_misc.c  #     杂项系统调用
│       │   └── syscall_internal.h #  内部头文件
│       ├── video/              #   图形子系统
│       │   ├── fb.c            #     帧缓冲抽象
│       │   ├── vga.c           #     VGA 文本/图形模式
│       │   ├── nuklear_fb.c    #     Nuklear GUI 帧缓冲后端
│       │   ├── terminal.c      #     终端模拟器
│       │   └── window.c        #     窗口管理器
│       └── test/               #   内核自测试
│           └── test.c          #     VFS / FAT32 / Registry 测试
├── libc/                       # 轻量级 C 标准库
│   ├── Makefile                #   libc 构建脚本
│   ├── crt1.S                  #   C 运行时启动代码
│   ├── crti.S                  #   初始化段 (.init/.fini)
│   ├── crtn.S                  #   初始化段结尾
│   ├── arch/x86_64/
│   │   └── syscall.h           #   系统调用号 (用户态)
│   ├── dl/
│   │   └── ldso.c              #   动态链接器占位
│   ├── include/                #   libc 头文件
│   │   ├── ctype.h             #   字符分类
│   │   ├── dirent.h            #   目录项
│   │   ├── errno.h             #   错误码
│   │   ├── fcntl.h             #   文件控制
│   │   ├── mman.h              #   内存管理
│   │   ├── signal.h            #   信号
│   │   ├── stdarg.h            #   可变参数
│   │   ├── stdbool.h           #   布尔类型
│   │   ├── stddef.h            #   标准定义
│   │   ├── stdint.h            #   整数类型
│   │   ├── stdio.h             #   标准 I/O
│   │   ├── stdlib.h            #   标准库
│   │   ├── string.h            #   字符串操作
│   │   ├── sys/stat.h          #   文件状态
│   │   ├── sys/types.h         #   系统类型
│   │   ├── time.h              #   时间
│   │   └── unistd.h            #   POSIX 标准
│   └── src/                    #   libc 源文件
│       ├── internal.h          #     内部定义
│       ├── ctype.c             #     字符分类实现
│       ├── dirent.c            #     目录操作
│       ├── fcntl.c             #     文件操作
│       ├── malloc.c            #     内存分配
│       ├── signal.c            #     信号操作
│       ├── start.c             #     程序入口
│       ├── stdio.c             #     标准 I/O 实现
│       ├── stdlib.c            #     标准库实现
│       ├── string.c            #     字符串实现
│       ├── syscall.c           #     系统调用封装
│       ├── time.c              #     时间函数
│       └── unistd.c            #     POSIX 函数
├── tests/                      # 用户态测试程序
│   ├── test_hello.c            #   Hello World 测试程序
│   ├── test_crt.S              #   最小 C 运行时
│   └── test.ld                 #   测试链接脚本
├── tools/
│   ├── png2c.py                #   Logo PNG → C 头文件转换工具
│   └── build_jre_deb.sh        #   JRE DEB 包构建脚本
├── Makefile                    # 主构建系统
├── logo.png                    #   启动 Logo
├── LICENSE.txt                 #   GPL-3.0 许可证
├── .gitignore                  #   Git 忽略规则
└── README.md                   #   本文件
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
| `ovmf` | UEFI 固件 (QEMU 运行 UEFI) | `sudo apt install ovmf` |
| `python3` | Logo 资源生成 / UEFI 重定位 | `sudo apt install python3` |

一键安装所有依赖：

```bash
sudo apt install gcc gcc-multilib nasm binutils grub-pc-bin grub-common xorriso mtools qemu-system-x86 ovmf python3
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

# 构建 UEFI 引导加载器
make uefi-bootloader

# 构建 UEFI 磁盘镜像
make uefi-image

# 构建 BIOS+UEFI 双启动 ISO
make dual-iso

# 构建用户态测试 ELF
make test-elf

# 构建 libc
make libc

# ---- 运行 (QEMU) ----

# BIOS 模式运行 (1GB 内存, 串口终端, 网络, AHCI 磁盘)
make run

# 日志模式 (串口输出到 build/serial.log, 无显示)
make run-log

# 调试模式 (GDB 远程调试, 端口 1234)
make run-debug

# UEFI 模式运行
make run-uefi

# UEFI 模式从双启动 ISO 运行
make run-uefi-iso

# 制作 USB 启动盘 (实体机)
make usb DEVICE=/dev/sdX

# 清理所有构建产物
make clean
```

### QEMU 运行配置

`make run` 启动的 QEMU 包含以下硬件模拟：

- **内存**: 1GB RAM
- **显示**: 串口终端 (stdio) + 标准 VGA
- **网络**: RTL8139 网卡，端口转发 `tcp::25565 → :25565`
- **存储**: AHCI 控制器 + 两个磁盘镜像
  - `build/fat32.img` — 512MB FAT32 磁盘 (自动创建，含测试程序)
  - `build/disk.img` — 16MB 原始磁盘 (自动创建)
- **串口**: COM1 → 标准输出 (用于内核日志和 Shell 交互)

`make run-uefi` 使用 OVMF 固件，2GB 内存，端口转发 `tcp::25566 → :25565`。

---

## 启动流程

```
┌─────────────────────────────────────────────────┐
│  1a. GRUB 加载 loader.elf (Multiboot2 协议)      │
│      └── 读取 /boot/grub/grub.cfg                │
│  1b. UEFI 加载 BOOTX64.EFI → kernel.elf         │
│      └── 自包含 UEFI bootloader                   │
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
│     ├── init_core()      — 串口, GDT, IDT,       │
│     │                       内存, PCI, VGA       │
│     ├── init_hardware()  — ACPI, APIC, SMP,      │
│     │                       定时器, 键盘, 鼠标    │
│     ├── init_storage()   — 块设备, AHCI,         │
│     │                       ramdisk, 终端         │
│     ├── init_filesystem()— VFS, memfs, devfs,    │
│     │                       FAT32, 挂载根文件系统 │
│     ├── init_services()  — kmalloc, SLAB, 进程,  │
│     │                       系统调用, Registry,   │
│     │                       包管理, 沙箱, 网络栈  │
│     ├── init_fs_hierarchy()— FHS 目录, procfs    │
│     ├── hal_enable_interrupts()                  │
│     ├── [ICMP 自测] — ping QEMU 网关验证网络     │
│     ├── [内核自测] — KERNEL_SELFTEST 宏控制       │
│     ├── shell_init()                             │
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

**SLAB 分配器** — `mm/slab.c`

- 8 个缓存组，对象大小 16B ~ 2048B
- 每个缓存维护 partial / full / free 三个 slab 链表
- 提供 `slab_alloc()` / `slab_free()` / `slab_realloc()` 接口

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
    int             pid, ppid;         // 进程 ID
    int             state;             // READY/RUNNING/BLOCKED/ZOMBIE
    uint32_t        flags;             // KERNEL/SFK/DEB/COW 标志
    uid_t           uid, euid;         // 用户身份
    gid_t           gid, egid;
    int             priority;          // 调度优先级
    uint64_t        pml4;              // 页表物理地址
    void           *kernel_stack;      // 内核栈
    uint64_t        entry_point;       // 入口地址
    uint64_t        brk;               // 堆顶
    uint64_t        stack_top;         // 用户栈顶
    uint64_t        mmap_base;         // mmap 基地址
    vfs_file_t     *fd_table[256];     // 文件描述符表
    char            cwd[256];          // 当前工作目录
    uint64_t        signal_mask;       // 信号掩码
    uint64_t        signal_handlers[32]; // 信号处理函数
    uint32_t        sfk_perms;         // 沙箱权限位掩码
    char            sfk_pkg_id[64];    // 沙箱包 ID
    uint64_t        clear_tid_address; // set_tid_address
    trap_frame_t   *trap_frame;        // 中断/系统调用保存的寄存器
    uint64_t        kernel_rsp;        // 上下文切换保存的内核 RSP
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
| 22 | pipe | 32-33 | dup/dup2 |
| 72 | fcntl | 78 | getdents |
| 79-80 | getcwd/chdir | 82 | rename |
| 83-84 | mkdir/rmdir | 87 | unlink |
| 88-89 | symlink/readlink | 90-91 | chmod/fchmod |
| 217 | getdents64 | 292-293 | dup3/pipe2 |

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

**时间 (96-229)**

| 号 | 调用 |
|----|------|
| 96-97 | gettimeofday/settimeofday |
| 201 | time | 228-229 | clock_gettime/clock_getres |

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
| RTL8139 | `drivers/net/rtl8139.c` | Realtek 以太网卡驱动、收发环形缓冲区 |
| 串口 | `drivers/serial.c` | COM1 串口驱动，用于内核日志输出 |
| 定时器 | `drivers/timer.c` | HPET/ACPI 定时器，提供时钟中断和计时 |

### 网络栈

`net/` 目录实现了分层 TCP/IP 协议栈：

| 层级 | 文件 | 说明 |
|------|------|------|
| 链路层 | `net/eth.c` | 以太网帧封装与解析 |
| 地址解析 | `net/arp.c` | ARP 请求/应答，IP-MAC 映射缓存 |
| 网络层 | `net/ip.c` | IPv4 数据包处理与转发 |
| ICMP | `net/icmp.c` | ICMP Echo Request/Reply (ping) |
| 传输层 | `net/tcp.c` | TCP 连接状态机、滑动窗口、超时重传、拥塞控制 |
| 传输层 | `net/udp.c` | UDP 数据报收发 |
| Socket | `net/socket.c` | BSD socket API (socket/bind/listen/accept/connect/send/recv) |
| 工具 | `net/net_utils.c` | 校验和计算 (含奇数字节处理) |

网卡驱动位于 `drivers/net/rtl8139.c`，负责以太网帧的 DMA 收发。

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
| Shell | `services/shell.c` | 交互式命令行，38 条内置命令 |
| Autorun | `services/autorun.c` | 启动时自动执行 `/etc/autorun.cfg` 中的命令 |
| Registry | `services/registry.c` | 类 Windows 注册表，键值存储 |
| 包管理 | `services/pkgmgr.c` | DEB 包安装/卸载/查询 |
| 沙箱 | `services/sandbox.c` | 基于权限位掩码的进程隔离 |
| 模块 | `services/module.c` | 内核模块加载/卸载 |
| 解压 | `services/gunzip.c` | gzip 解压缩 |

### 对称多处理 (SMP)

`arch/x86_64/smp.c` 实现多核支持：

- **AP 启动**：通过 AP Trampoline (`ap_trampoline.S`) 唤醒应用处理器
- **Per-CPU 数据**：通过 GS 段寄存器访问当前 CPU 的 `cpu_local_t`
- **自旋锁**：基于票据锁 (ticket lock) 实现
- **IPI**：支持向特定 CPU 或广播发送处理器间中断
- 最多支持 256 个 CPU

---

## 用户态支持

### libc — 轻量级 C 标准库

项目包含一个面向 SpiritFoxOS 的轻量级 libc 实现，位于 `libc/` 目录：

- **crt0**: `crt1.S` / `crti.S` / `crtn.S` — 程序启动/终止代码
- **系统调用封装**: `src/syscall.c` — 通过 `syscall` 指令调用内核
- **标准 I/O**: `src/stdio.c` — printf, fopen, fread, fwrite 等
- **标准库**: `src/stdlib.c` — atoi, malloc (通过 brk), exit 等
- **字符串**: `src/string.c` — strlen, strcpy, memcpy 等
- **POSIX**: `src/unistd.c` — read, write, close, fork, exec 等
- **文件操作**: `src/fcntl.c` — open, creat, fcntl
- **内存管理**: `src/malloc.c` — 基于 brk 的用户态 malloc/free
- **信号**: `src/signal.c` — signal, kill, sigaction
- **时间**: `src/time.c` — time, clock_gettime
- **目录**: `src/dirent.c` — opendir, readdir, closedir

### 测试程序

`tests/` 目录包含用户态测试 ELF 程序：

- `test_hello.c` — Hello World 测试程序
- `test_crt.S` — 最小 C 运行时启动代码
- `test.ld` — 测试链接脚本

在 Shell 中运行：`exec /mnt/bin/test_hello`

---

## Shell 命令

SpiritFoxOS 内置 38 条 Shell 命令：

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

### 网络

| 命令 | 说明 |
|------|------|
| `ifconfig` | 显示网络配置 |
| `ping <ip>` | 发送 ICMP Echo 请求 |

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

### UEFI 调试

UEFI 模式需要安装 OVMF 固件：

```bash
sudo apt install ovmf
make run-uefi
```

---

## 许可证

本项目基于 [GNU General Public License v3.0](LICENSE.txt) 许可证开源。
