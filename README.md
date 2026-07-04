# SpiritFoxOS

一个基于 x86_64 架构的自制操作系统内核，使用 Multiboot2 引导，采用 GPL-3.0 许可证发布。

## 特性

- **x86_64 内核** — 从 32 位 Multiboot 引导进入 64 位长模式
- **内存管理** — 物理页帧分配器、虚拟内存 (MMU)、内核堆 (kmalloc)
- **进程管理** — 多进程调度、信号、futex、用户态进程加载 (ELF)
- **系统调用** — Linux ABI 兼容的系统调用接口（文件、内存、进程、网络等）
- **文件系统** — VFS 虚拟文件系统层，支持 memfs / devfs / fat32 / procfs / ramdisk
- **存储驱动** — AHCI SATA 驱动、块设备抽象层
- **网络栈** — RTL8139 网卡驱动、基础 TCP/IP 网络栈
- **图形界面** — VBE/VGA 帧缓冲、Nuklear GUI、窗口管理
- **终端** — 内置终端模拟器、交互式 Shell
- **服务框架** — 注册表 (Registry)、包管理器 (pkgmgr)、沙箱 (Sandbox)、自动运行 (autorun)
- **硬件支持** — ACPI、APIC、PCI 枚举、键盘、鼠标、定时器、串口

## 项目结构

```
SpiritFoxOS/
├── boot/
│   ├── multiboot/          # 32 位 Multiboot2 引导加载器
│   │   ├── boot32.S        # 汇编入口
│   │   ├── multiboot32.c   # C 入口，切换到长模式
│   │   └── linker.ld       # 32 位链接脚本
│   └── uefi/               # UEFI 引导 (开发中)
├── kernel/
│   ├── include/            # 内核头文件
│   └── src/
│       ├── arch/x86_64/    # 架构相关 (GDT, IDT, ACPI, APIC, HAL)
│       ├── drivers/        # 设备驱动 (AHCI, keyboard, mouse, PCI, RTL8139, serial, timer)
│       ├── fs/             # 文件系统 (VFS, FAT32, memfs, devfs, procfs, ramdisk)
│       ├── mm/             # 内存管理 (物理/虚拟内存, kmalloc, ELF 加载器)
│       ├── net/            # 网络栈
│       ├── proc/           # 进程管理 (调度器, 信号, futex)
│       ├── services/       # 系统服务 (Shell, Registry, pkgmgr, Sandbox, autorun, module)
│       ├── syscall/        # 系统调用
│       ├── video/          # 图形 (帧缓冲, VGA, Nuklear GUI, 窗口, 终端)
│       ├── test/           # 内核自测试
│       ├── entry.c         # 内核入口 (_start64 → kernel_main)
│       ├── init.c          # 子系统初始化
│       ├── string.c        # 字符串工具
│       └── kernel.ld       # 64 位链接脚本
├── tools/
│   ├── png2c.py            # Logo PNG → C 头文件转换
│   └── build_jre_deb.sh    # JRE deb 包构建脚本
├── Makefile                # 构建系统
├── logo.png                # 启动 Logo
└── LICENSE.txt             # GPL-3.0 许可证
```

## 构建依赖

- `gcc` / `x86_64-linux-gnu-gcc` — 交叉编译器
- `nasm` — 汇编器
- `ld` / `x86_64-linux-gnu-ld` — 链接器
- `objcopy` — 二进制转换
- `grub-mkrescue` — 生成可启动 ISO
- `xorriso` — GRUB ISO 依赖
- `qemu-system-x86_64` — 运行与调试（可选）
- `python3` — Logo 资源生成

## 构建与运行

```bash
# 构建所有 (生成 ISO)
make

# 仅构建内核
make kernel

# 运行 (QEMU, 需要 sudo 权限创建磁盘镜像)
make run

# 调试模式 (GDB 远程调试)
make run-debug

# 日志模式 (串口输出到文件)
make run-log

# 清理构建产物
make clean
```

## 启动流程

1. GRUB 加载 `loader.elf` (32 位 Multiboot2 引导器)
2. 引导器收集启动信息，切换到 64 位长模式
3. 跳转至内核 `_start64` 入口，清零 BSS 段后调用 `kernel_main`
4. `kernel_main` 按序初始化各子系统：
   - 核心 (GDT, IDT, 内存, VGA)
   - 硬件 (ACPI, APIC, PCI, 键盘, 鼠标)
   - 存储 (块设备, AHCI, ramdisk, 终端)
   - 文件系统 (VFS, memfs, devfs, FAT32)
   - 服务 (kmalloc, 进程, 系统调用, Registry, 包管理, 沙箱, 网络)
   - 目录结构 (FHS 标准目录)
5. 启用中断，执行 autorun 命令，进入交互式 Shell

## 许可证

本项目基于 [GPL-3.0](LICENSE.txt) 许可证开源。
