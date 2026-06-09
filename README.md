# SpiritFoxOS

**版本**: 1.0.0
**架构**: x86_64 (Long Mode)
**引导协议**: Multiboot2 (BIOS) / UEFI (默认)
**许可证**: GNU General Public License v3.0 (GPLv3)

SpiritFoxOS 是一个从零编写的 x86_64 操作系统，具备完整的双模式启动流程、硬件抽象层、内存管理、进程调度、图形用户界面（GUI）、文件系统、USB 驱动和交互式 Shell。

---

## 目录

- [项目概述](#项目概述)
- [项目结构](#项目结构)
- [构建与运行](#构建与运行)
- [启动流程](#启动流程)
- [内核架构](#内核架构)
  - [引导与长模式切换](#引导与长模式切换)
  - [全局描述符表 (GDT)](#全局描述符表-gdt)
  - [中断描述符表 (IDT)](#中断描述符表-idt)
  - [可编程中断控制器 (PIC)](#可编程中断控制器-pic)
  - [可编程间隔定时器 (PIT)](#可编程间隔定时器-pit)
  - [物理内存管理 (PMM)](#物理内存管理-pmm)
  - [虚拟内存管理 (VMM)](#虚拟内存管理-vmm)
  - [进程调度器](#进程调度器)
  - [系统调用接口](#系统调用接口)
  - [PCI 总线枚举](#pci-总线枚举)
  - [ATA 磁盘驱动](#ata-磁盘驱动)
  - [xHCI / USB 驱动](#xhci--usb-驱动)
  - [SFS 文件系统](#sfs-文件系统)
  - [权限管理与加密](#权限管理与加密)
  - [设备树与健康检查](#设备树与健康检查)
  - [内核日志系统](#内核日志系统)
  - [键盘驱动](#键盘驱动)
  - [鼠标驱动](#鼠标驱动)
  - [串口通信](#串口通信)
  - [VGA 文本模式](#vga-文本模式)
  - [帧缓冲与图形模式](#帧缓冲与图形模式)
  - [字体渲染](#字体渲染)
  - [图形用户界面 (GUI)](#图形用户界面-gui)
  - [命令行 Shell](#命令行-shell)
  - [Init 进程与安全模式](#init-进程与安全模式)
- [公共头文件](#公共头文件)
- [链接脚本](#链接脚本)
- [已实现功能清单](#已实现功能清单)

---

## 项目概述

SpiritFoxOS 是一个 64 位爱好操作系统，运行在 x86_64 架构上。系统支持 **两种启动模式**：

- **BIOS 模式**：通过 GRUB 引导加载程序以 Multiboot2 协议启动，从 32 位保护模式切换到 64 位长模式
- **UEFI 模式**（默认）：通过 UEFI 固件直接加载为 PE/COFF 可执行文件，使用 Microsoft x64 调用约定

系统支持两种显示模式：
- **VGA 文本模式**：80x25 字符终端，提供功能完整的命令行 Shell
- **图形帧缓冲模式**：1024x768 分辨率（或 GOP 自适应），32 位色深，带有桌面环境、窗口系统、动画效果和图形终端

---

## 项目结构

```
SpiritFoxOS/
├── Makefile                  # 构建系统 (支持 BIOS + UEFI 双模式)
├── linker.ld                 # BIOS 模式链接脚本
├── linker_efi.ld             # UEFI 模式链接脚本
├── scripts/
│   ├── elf2pe.py             # ELF → UEFI PE 转换工具
│   └── ...                   # 其他 PE 调试/分析工具
├── iso/
│   └── boot/
│       ├── grub/
│       │   └── grub.cfg      # GRUB 引导配置 (BIOS)
│       ├── kernel.elf        # BIOS 内核 ELF (构建产物)
│       └── kernel.efi        # UEFI 内核 EFI (构建产物)
├── build/                    # 构建输出目录
└── src/
    ├── boot/
    │   ├── boot.asm          # Multiboot2 头 + 32位入口 + 长模式切换
    │   ├── efi_boot.c        # UEFI 入口点 (EFIAPI, MS ABI)
    │   └── test_mingw2.c     # 最小 mingw 应用 (PE 模板)
    ├── include/
    │   ├── io.h              # I/O 端口操作内联函数
    │   ├── multiboot2.h      # Multiboot2 数据结构定义
    │   ├── efi.h             # UEFI 类型定义与协议头
    │   ├── bootinfo.h        # 统一启动信息结构
    │   ├── stdarg.h          # 可变参数支持
    │   ├── stddef.h          # 基本类型定义 (size_t, NULL)
    │   ├── stdint.h          # 标准整数类型
    │   └── string.h          # 字符串操作 + snprintf
    └── kernel/
        ├── isr.asm           # 中断服务例程汇编存根
        ├── kernel.c          # 内核主入口 + 启动阶段管理
        ├── init.c            # Init 进程 + 安全模式
        ├── gdt.c / gdt.h     # 全局描述符表 (含 TSS)
        ├── idt.c / idt.h     # 中断描述符表 + ISR 分发
        ├── pic.c / pic.h     # 8259 PIC 驱动
        ├── pit.c / pit.h     # PIT 定时器驱动 (100Hz)
        ├── pmm.c / pmm.h     # 物理内存管理器 (位图法)
        ├── vmm.c / vmm.h     # 虚拟内存管理器 (4 级页表)
        ├── scheduler.c / scheduler.h # 轮转调度器
        ├── syscall.c / syscall.h     # 系统调用接口 (int 0x80)
        ├── keyboard.c / keyboard.h  # PS/2 键盘驱动
        ├── mouse.c / mouse.h        # PS/2 鼠标驱动
        ├── serial.c / serial.h      # 串口通信 (COM1/COM2)
        ├── vga.c / vga.h            # VGA 文本模式驱动
        ├── fb.c / fb.h              # 帧缓冲驱动 + 双缓冲
        ├── font.c / font.h          # 8x16 位图字体渲染
        ├── gui.c / gui.h            # 图形用户界面 (SFGUI 2.0)
        ├── shell.c / shell.h        # 交互式命令行 Shell (sfsh)
        ├── pci.c / pci.h            # PCI 总线枚举与配置
        ├── ata.c / ata.h            # ATA/PATA 磁盘驱动
        ├── usb.c / usb.h            # USB 核心协议栈 + HID
        ├── xhci.c / xhci.h          # xHCI 主机控制器驱动
        ├── sfs.c / sfs.h            # SFS1 文件系统
        ├── devtree.c / devtree.h    # 设备树注册与健康检查
        ├── perm.c / perm.h          # 权限管理器 + 应用沙箱
        ├── crypto.c / crypto.h      # 加密原语 (密钥对/签名)
        ├── log.c / log.h            # 内核日志系统 (5 级)
        └── splash_logo.h           # 启动 Logo 位图数据
```

---

## 构建与运行

### 依赖工具

| 工具 | 用途 | 必需 |
|------|------|------|
| `nasm` | 汇编器 (ELF64) | BIOS + UEFI |
| `gcc` / `x86_64-elf-gcc` | C 编译器 (freestanding) | BIOS + UEFI |
| `ld` / `x86_64-elf-ld` | 链接器 | BIOS + UEFI |
| `x86_64-w64-mingw32-gcc` | UEFI PE 模板生成 | UEFI |
| `python3` | ELF→PE 转换脚本 | UEFI |
| `grub-mkrescue` | 创建可引导 ISO | BIOS |
| `xorriso`, `mtools` | ISO 创建依赖 | BIOS (ISO) |
| `qemu-system-x86_64` | 模拟器运行与调试 | 测试 |
| OVMF 固件 (`OVMF_CODE.fd`) | UEFI 固件 | UEFI |

### 编译命令

```bash
make            # 构建 UEFI 模式 (默认)
make bios        # 构建 BIOS/Multiboot2 模式
make uefi        # 同 make (UEFI 模式)
make iso         # 创建 UEFI 可引导 ISO
make iso-bios     # 创建 BIOS 可引导 ISO (GRUB)
make run         # QEMU 运行 UEFI 模式
make run-bios     # QEMU 运行 BIOS 模式
make debug       # QEMU 运行 + 串口调试输出 (BIOS)
make debug-uefi  # QEMU 运行 + 串口调试输出 (UEFI)
make clean       # 清理构建产物
```

### 编译器标志

```makefile
# 公共标志 (所有模式共用)
-ffreestanding -fno-stack-protector
-mno-mmx -mno-sse -mno-sse2 -mno-sse3
-m64 -Wall -Wextra -Werror
-nostdlib -nostdinc -fno-builtin -nodefaultlibs

# BIOS 模式额外标志
-fno-pie -fno-pic -mcmodel=small

# UEFI 模式额外标志
-fpic -mno-red-zone -DBOOT_MODE_UEFI
# efi_boot.c 额外: -mabi=ms (Microsoft x64 calling convention)
```

### QEMU 快速测试

```bash
# BIOS 模式测试 (推荐，无需 OVMF)
make run-bios

# 或手动指定:
qemu-system-x86_64 -cdrom build/spiritfoxos_bios.iso \
    -m 128M -serial stdio

# UEFI 模式测试 (需要 OVMF)
make run
```

---

## 启动流程

SpiritFoxOS 采用分阶段启动架构（Phase I → Phase J → Phase K → Phase N-P → Phase Q）:

```
┌─────────────────────────────────────────────────────┐
│                  引导加载程序                        │
│  BIOS: GRUB → Multiboot2 → boot.asm               │
│  UEFI:  Firmware → efi_boot.c (PE)                │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│  Phase I:  系统内核启动 (kernel_main)                │
│  I1: 保存启动信息                                    │
│  I2: PMM + VMM 内存管理初始化                        │
│  I4: IDT + PIC 中断初始化                            │
│  I5: GDT + TSS 全局描述符表                           │
│  I6: Syscall 系统调用接口                             │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│  Phase J:  设备自检                                  │
│  J1: PCI 总线枚举                                   │
│  J2: CPU 核心检测 (CPUID)                           │
│  J3: 内存控制器信息                                   │
│  J4: ATA 存储设备探测                                │
│  J5: PS/2 键盘/鼠标初始化                            │
│  J6: VGA/帧缓冲显示初始化                            │
│  J7: 网络设备检测                                    │
│  J8: 汇总设备到系统设备树                             │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│  Phase K:  设备自检结果检查                          │
│  通过 → N: 挂载文件系统                               │
│  失败 → L+M: 输出错误信息 → 安全模式                  │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│  Phase N-P: 文件系统和进程管理                        │
│  N: SFS 文件系统挂载                                 │
│  O: 进程调度器 + 权限管理器初始化                     │
│  P: Init 进程创建                                   │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│  Phase Q:  命令行界面                                │
│  图形模式 → GUI 桌面 + 窗口系统 + 图形终端           │
│  文本模式 → VGA Shell (sfsh)                        │
└─────────────────────────────────────────────────────┘
```

---

## 内核架构

### 引导与长模式切换

**文件**: `src/boot/boot.asm` (BIOS) / `src/boot/efi_boot.c` (UEFI)

**BIOS 引导路径**:
- **Multiboot2 头**：魔数 `0xE85250D6`，请求 1024x768x32bpp 帧缓冲
- **页表设置**：5 个 4KB 页表 (PML4 -> PDPT -> PD)，使用 2MB 大页映射前 4GB 物理内存
  - `PML4[0]` -> 低 4GB 身份映射
  - `PML4[256]` -> 高半核映射 (0xFFFFFFFF80000000)
- **GDT64**：Null + 64位代码段 + 数据段，远跳转进入长模式

**UEFI 引导路径**:
- **入口**: `efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE*)` 使用 Microsoft x64 ABI
- **GOP 初始化**: 自动选择 >= 1024x768 的最佳显示模式
- **ACPI RSDP**: 从配置表中查找 ACPI 2.0 RSDP 指针
- **内存映射**: 调用 `GetMemoryMap()` 获取 UEFI 内存描述符
- **ExitBootServices**: 退出 UEFI 启动服务后跳转到内核

### 全局描述符表 (GDT)

**文件**: `src/kernel/gdt.c`, `src/kernel/gdt.h`

| 索引 | 选择子 | 描述 |
|------|--------|------|
| 0 | 0x00 | 空描述符 |
| 1 | 0x08 | 64 位内核代码段 (Execute/Read, Long mode) |
| 2 | 0x10 | 内核数据段 (Read/Write) |
| 3 | 0x18 | 64 位用户代码段 |
| 4 | 0x20 | 用户数据段 |
| 5-6 | 0x28 | TSS 描述符 (16 字节，64 位模式) |

初始化时重新加载所有段选择子 (DS, ES, FS, GS, SS, CS)，并使用 `ltr` 加载 TSS。

### 中断描述符表 (IDT)

**文件**: `src/kernel/idt.c`, `src/kernel/idt.h`, `src/kernel/isr.asm`

- 256 个中断向量，前 48 个已注册
- 每个门描述符 16 字节 (64 位模式)
- CPU 异常 (0-31)：除零、缺页、通用保护故障等
- 硬件中断 (32-47)：PIT、键盘、鼠标、串口等
- 系统调用 (向量 0x80)：32 个 syscall 分发
- ISR 汇编存根保存/恢复所有通用寄存器 (R15-R15, RDI-RAX, INT_NO, ERR_CODE, RIP, CS, RFLAGS, RSP, SS)

### 可编程中断控制器 (PIC)

**文件**: `src/kernel/pic.c`, `src/kernel/pic.h`

- 8259 双片级联配置
- PIC1 基地址：IRQ32 (0x20 端口)
- PIC2 基地址：IRQ40 (0xA0 端口)
- 从片级联在 IRQ2
- 提供 EOI 发送、IRQ 屏蔽/取消屏蔽功能

### 可编程间隔定时器 (PIT)

**文件**: `src/kernel/pit.c`, `src/kernel/pit.h`

- 基准频率：1,193,182 Hz
- 配置为 100 Hz 方波 (通道 0)
- 用于系统时钟、进程调度和 GUI 动画计时

### 物理内存管理 (PMM)

**文件**: `src/kernel/pmm.c`, `src/kernel/pmm.h`

- **算法**：位图法，每个 bit 对应一个 4KB 物理页
- **页大小**：4096 字节
- **双模式初始化**：
  - UEFI 模式：解析 EFI_MEMORY_DESCRIPTOR 条目，自动转换内存类型
  - BIOS 模式：解析 Multiboot2 MMAP 条目，转换为 EFI 兼容格式
- **分配函数**：
  - `pmm_alloc_page()` — 分配单个物理页，使用 `__builtin_ctzll` 快速查找
  - `pmm_alloc_pages(n)` — 分配 n 个连续物理页
  - `pmm_free_page(addr)` — 释放物理页

### 虚拟内存管理 (VMM)

**文件**: `src/kernel/vmm.c`, `src/kernel/vmm.h`

- **四级页表**：PML4 -> PDPT -> PD -> PT -> Page (4 级分页)
- **页表项标志**：Present, Writable, User, Accessed, Dirty, Huge, Global, NX
- **地址掩码**：`0x000FFFFFFFFFF000`
- **核心功能**：
  - `vmm_map_page()` — 映射虚拟页到物理页，自动创建中间页表
  - `vmm_unmap_page()` — 取消映射并刷新 TLB (`invlpg`)
  - `vmm_get_physical()` — 虚拟地址到物理地址转换 (支持 2MB 大页)
  - `vmm_create_pml4()` — 创建新地址空间，复制内核映射 (高半区)
  - `vmm_destroy_pml4()` — 销毁地址空间，释放用户态页表

### 进程调度器

**文件**: `src/kernel/scheduler.c`, `src/kernel/scheduler.h`

- **算法**：轮转调度 (Round-Robin)
- **最大进程数**：64
- **时间片**：10 ticks (100ms @ 100Hz)
- **栈大小**：16KB (4 页)
- **进程状态**：UNUSED / READY / RUNNING / BLOCKED / ZOMBIE
- **上下文切换**：在 PIT 时钟中断中保存 RSP，恢复下一个进程的 RSP
- **TSS 更新**：每次切换更新 RSP0 以支持内核栈切换
- **CR3 切换**：进程地址空间不同时刷新页表

### 系统调用接口

**文件**: `src/kernel/syscall.c`, `src/kernel/syscall.h`

- **中断号**：`int 0x80`
- **最大调用数**：32
- **调用约定**：(syscall_number, arg0, arg1, arg2, arg3) -> int64_t 返回值

| 调用号 | 名称 | 功能 |
|--------|------|------|
| 0 | SYS_READ | 从文件描述符读取 |
| 1 | SYS_WRITE | 写入文件描述符 |
| 2 | SYS_OPEN | 打开文件 |
| 3 | SYS_CLOSE | 关闭文件描述符 |
| 4 | SYS_EXIT | 终止当前进程 |
| 5 | SYS_GETPID | 获取当前进程 PID |
| 6 | SYS_YIELD | 让出 CPU |
| 7 | SYS_SLEEP | 休眠 N 个 tick |
| 8 | SYS_PUTS | 输出字符串到控制台 |
| 9 | SYS_GETCHAR | 从控制台读取字符 |
| 10 | SYS_MMAP | 内存映射 |
| 11 | SYS_MUNMAP | 取消内存映射 |
| 12 | SYS_INFO | 获取系统信息 |

### PCI 总线枚举

**文件**: `src/kernel/pci.c`, `src/kernel/pci.h`

- 完整的 PCI 总线扫描 (bus 0-255, device 0-31, function 0-7)
- 6 个 BAR (基址寄存器) 解析，支持 I/O 和 MMIO 类型
- 设备启用 (I/O Space + Memory + Bus Master)
- 按 类/子类/编程接口查找设备
- 预定义常量：USB (0x0C), SATA/ATA (0x01), VGA (0x03), Network (0x02)
- 最大支持 128 个设备

### ATA 磁盘驱动

**文件**: `src/kernel/ata.c`, `src/kernel/ata.h`

- 支持 Primary/Secondary 通道的 Master/Slave 盘 (最多 4 台设备)
- LBA28 和 LBA48 寻址模式自动检测
- PIO 模式扇区读写 (28-bit 和 48-bit 命令)
- 设备识别 (IDENTIFY DEVICE 命令)
- 缓存刷新 (FLUSH CACHE)
- 数据结构包含：I/O 基地址、LBA48 支持、总扇区数、型号字符串

### xHCI / USB 驱动

**文件**: `src/kernel/xhci.c`, `src/kernel/xhci.h`, `src/kernel/usb.c`, `src/kernel/usb.h`

**xHCI 控制器驱动**:
- PCI 枚举发现并初始化 xHCI 控制器
- MMIO 寄存器映射和能力寄存器解析
- 命令环 / 事件环 / 传输环管理
- TRB (传输请求块) 类型：Normal/Setup/Status/Data/Link/No-op
- 设备插槽分配与管理 (最多 256 个插槽)
- 端点配置 (Control/Bulk/Interrupt)
- 中断处理 (事件环轮询)

**USB 核心协议栈**:
- 标准 USB 描述符解析 (Device/Config/Interface/Endpoint)
- 控制传输 (EP0)：GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION
- HID 设备类支持：键盘 (Boot Protocol) 和鼠标 (Boot Protocol)
- HID 报告轮询 (键盘状态 + 鼠标位移/按键)
- 最大支持 32 个 USB 设备、16 个 HID 设备

### SFS 文件系统

**文件**: `src/kernel/sfs.c`, `src/kernel/sfs.h`

- **格式名**：SFS1 (SpiritFoxOS File System v1)
- **超级块 Magic**：`"SFS1"`
- **布局策略**：超级块 (LBA 0) + 文件表 (LBA 1-2047) + 数据区 (LBA 2048+)
- **目录结构**：扁平文件表 (最多 64 个条目)，文件名最长 32 字节
- **分配方式**：连续扇区分配 + 位图空闲管理
- **API**：
  - `sfs_init()` / `sfs_format()` — 挂载/格式化
  - `sfs_create_file()` / `sfs_delete_file()` — 创建/删除
  - `sfs_read_file()` / `sfs_write_file()` — 读写
  - `sfs_list_files()` / `sfs_file_exists()` / `sfs_get_file_size()` — 查询
- **校验和**：每个文件目录项带 CRC 校验
- **删除机制**：逻辑删除 (flags 标记)，空间可复用

### 权限管理与加密

**文件**: `src/kernel/perm.c`, `src/kernel/perm.h`, `src/kernel/crypto.c`, `src/kernel/crypto.h`

**权限模型**:
- 12 种细粒度权限标志：文件读/写/执行、网络收发、设备访问、系统信息、进程/内存管理、GUI 渲染、输入读取、音频播放
- 应用安装时申请权限集，运行时按会话校验
- 系统内置应用自动全权 (terminal, filemanager, settings)
- 最大 64 个应用注册

**加密模块**:
- 非对称密钥对生成 (基于素域离散对数)
- 数字签名 (私钥签名，公钥验证)
- 哈希函数 (自定义 256 位输出，用于消息绑定)
- 随机数生成 (xorshift128 + RDTSC 熵源)

### 设备树与健康检查

**文件**: `src/kernel/devtree.c`, `src/kernel/devtree.h`

- **设备类型**：CPU / 内存 / 存储 / 输入 / 显示 / 网络 / USB / PCI
- **设备状态**：UNKNOWN / OK / FAILED / MISSING / DISABLED
- **关键性标记**：关键设备失败时触发安全模式降级
- **API**：
  - `devtree_register()` — 注册设备并记录厂商/设备 ID
  - `devtree_check_critical()` — 校验所有关键设备是否正常
  - `devtree_print_errors()` / `devtree_print_all()` — 信息输出
  - 最大 64 个设备条目

### 内核日志系统

**文件**: `src/kernel/log.c`, `src/kernel/log.h`

- **5 个级别**：DEBUG < INFO < WARN < ERROR < FATAL
- **三路输出**：环形缓冲区 + 串口 (COM1) + VGA (WARN 以上级别)
- **环形缓冲区**：8192 字节，按行存储
- **磁盘持久化**：通过 SFS 写入 `system.log` 文件
- **自动保存**：每 50 行日志自动刷盘 (可开关)
- **便捷宏**：`LOG_D()`, `LOG_I()`, `LOG_W()`, `LOG_E()`, `LOG_F()`
- **时间戳**：HH:MM:ss 格式 (基于 PIT tick 计数)
- **格式化**：支持 %d/%u/%x/%p/%c/%s/%%% 占位符

### 键盘驱动

**文件**: `src/kernel/keyboard.c`, `src/kernel/keyboard.h`

- **接口**：PS/2 键盘 (IRQ1, 端口 0x60)
- **扫描码集**：Set 1
- **特性**：
  - US 键盘布局 (普通 + Shift)
  - 修饰键支持 (Shift, Ctrl, Alt)
  - 扩展键支持 (方向键, Home/End, PgUp/PgDn)
  - Ctrl+C 特殊键码
  - Tab 键支持
  - 256 字符环形缓冲区
- **API**：
  - `keyboard_getchar()` — 阻塞获取可打印字符
  - `keyboard_getkey()` — 阻塞获取任意键码 (含特殊键)
  - `keyboard_try_getkey()` — 非阻塞版本 (GUI 使用)

### 鼠标驱动

**文件**: `src/kernel/mouse.c`, `src/kernel/mouse.h`

- **接口**：PS/2 鼠标 (IRQ12, 端口 0x60/0x64)
- **协议**：标准 3 字节 PS/2 鼠标协议
- **特性**：
  - 左/右/中键检测
  - X/Y 位移解析 (含符号扩展)
  - 屏幕边界限制
  - 同步检查 (byte0 bit3)
- **状态结构**：坐标 (x, y)、按钮状态、滚动、屏幕尺寸

### 串口通信

**文件**: `src/kernel/serial.c`, `src/kernel/serial.h`

- **端口**：COM1 (0x3F8), COM2 (0x2F8)
- **波特率**：115200
- **配置**：8N1 (8 数据位, 无校验, 1 停止位)
- **用途**：调试输出 (QEMU `-serial stdio` 模式)、GUI 终端串口输入

### VGA 文本模式

**文件**: `src/kernel/vga.c`, `src/kernel/vga.h`

- **分辨率**：80x25 字符
- **缓冲区地址**：`0xB8000`
- **颜色**：16 色 (0=黑 ~ 15=白)
- **功能**：字符/字符串输出、滚屏、硬件光标更新、颜色设置
- **简易 printf**：支持 %d, %u, %x, %p, %c, %s, %%

### 帧缓冲与图形模式

**文件**: `src/kernel/fb.c`, `src/kernel/fb.h`

- **分辨率**：由 Multiboot2/GOP 自适应 (通常 1024x768)
- **色深**：32 位 (0xRRGGBB 格式)
- **双缓冲**：分配后缓冲区，通过 `fb_swap_buffers()` 交换
- **绘图原语**：
  - `fb_draw_pixel()` — 像素绘制
  - `fb_fill_rect()` — 填充矩形
  - `fb_draw_rect()` — 矩形边框
  - `fb_draw_line()` — Bresenham 直线算法
  - `fb_clear()` — 清屏
  - `fb_get_pixel()` / `fb_get_draw_buffer()` — 像素读写
- **预定义颜色**：黑/白/红/绿/蓝/青/黄/品红/橙/灰 + SpiritFoxOS 主题色 (SF_ORANGE/SF_ACCENT)

### 字体渲染

**文件**: `src/kernel/font.c`, `src/kernel/font.h`

- **字体**：8x16 像素位图字体，128 个 ASCII 字符
- **渲染模式**：
  - `font_draw_char()` — 带前景/背景色渲染
  - `font_draw_string()` — 字符串渲染 (带背景)
  - `font_draw_string_transparent()` — 透明背景渲染

### 图形用户界面 (GUI)

**文件**: `src/kernel/gui.c`, `src/kernel/gui.h`

当检测到图形帧缓冲时自动进入 GUI 模式 (**SFGUI 2.0**)：

- **启动 Logo**：360x360 像素位图 Logo，显示 3 秒后进入桌面
- **桌面环境**：
  - 渐变背景 (深蓝色调)
  - 底部任务栏 (SpiritFoxOS 主题色)
  - 开始按钮 ("SpiritFox")
  - 系统时钟显示 (RTC 实时时钟)
  - 窗口按钮 (已打开窗口列表)
- **窗口系统**：
  - 最多 8 个窗口，Z-order 层级管理
  - 窗口阴影效果
  - 标题栏 (活动/非活动状态)
  - 关闭按钮 (红色 X) + 最小化按钮 (_)
  - 窗口拖拽 (标题栏区域)
  - **动画系统**：缓出立方曲线 (ease-out cubic) 动画
    - 打开动画 (从小到大，300ms)
    - 关闭动画 (缩小消失，200ms)
    - 最小化动画 (滑向任务栏，200ms)
    - 还原动画 (从任务栏展开，250ms)
- **桌面图标**：Terminal, System, About (点击打开对应窗口)
- **图形终端**：
  - 128x200 字符终端缓冲区
  - 绿色文字 + 深色背景 (类 Vim 配色)
  - 光标闪烁
  - 内置命令 (见下方 Shell 命令列表)
  - 自动演示模式 (启动后自动执行 help/uname/mem/uptime/neofetch/pcilist/disklist)
- **鼠标交互**：
  - 16x16 箭头光标 (带像素保存/恢复，无拖影)
  - 三态鼠标状态机 (IDLE/PRESSED/DRAGGING)
  - 点击图标打开窗口
  - 点击关闭/最小化按钮
  - 标题栏拖拽移动窗口
  - 任务栏点击聚焦/还原窗口
- **快捷键**：`x` 然后 `eee` (1 秒内) 强制退出 GUI
- **RTC 时钟**：从 CMOS 硬件读取实时时间，BCD 自动转换
- **USB HID 支持**：GUI 循环中自动轮询 USB 键盘/鼠标

**GUI 终端内置命令**：

| 命令 | 说明 |
|------|------|
| `help` | 显示帮助信息 |
| `uname` | 系统信息 |
| `uptime` | 系统运行时间 |
| `mem` | 内存统计 |
| `clear` | 清屏 |
| `neofetch` | 系统信息展示 (ASCII art) |
| `pcilist` | 列出 PCI 设备 |
| `usblist` | 列出 USB 设备 |
| `disklist` | 列出 ATA 磁盘 |
| `sfsformat` | 格式化文件系统 |
| `ls` | 列出文件 |
| `logsave` / `logload` | 日志持久化 |
| `logauto` | 切换自动保存 |
| `loglevel <0-4>` | 设置日志级别 |
| `permlist` | 列出应用权限 |
| `shutdown` | 关机 |
| `reboot` | 重启 |
| `exit` | 关闭终端窗口 |

### 命令行 Shell

**文件**: `src/kernel/shell.c`, `src/kernel/shell.h`

Shell 名称：`sfsh 1.0`，在 VGA 文本模式下运行。

**提示符**：`root@SpiritFoxOS:~#` (彩色)

**内置命令** (26 条)：

| 命令 | 说明 |
|------|------|
| `help` | 显示帮助信息 |
| `clear` | 清屏 |
| `echo <text>` | 输出文本 |
| `history` | 显示命令历史 |
| `color <fg> [bg]` | 更改文字颜色 (0-15) |
| `about` | 关于 SpiritFoxOS |
| `neofetch` | 系统信息展示 (ASCII 狐狸图标 + 调色板) |
| `cowsay <msg>` | ASCII 牛说消息 |
| `beep` | PC 扬声器蜂鸣 |
| `uname [-a]` | 系统信息 |
| `sysinfo` | 详细系统概览 |
| `cpuinfo` | CPU 信息 (CPUID 厂商和型号) |
| `date` | 系统时间 (相对启动时间) |
| `uptime` | 系统运行时间 |
| `whoami` | 当前用户 (root) |
| `hostname` | 主机名 (SpiritFoxOS) |
| `mem` | 内存统计 (含使用率进度条) |
| `free` | 内存详情 |
| `vmstat` | 虚拟内存统计 |
| `ps` | 进程列表 |
| `kill <pid>` | 终止进程 |
| `sleep <sec>` | 休眠 N 秒 (1-30) |
| `reboot` | 重启系统 (键盘控制器) |
| `halt` | 停止 CPU |
| `shutdown` | 关机 (ACPI 端口 0x604) |

**Shell 特性**：
- Tab 自动补全 (前缀匹配)
- 命令历史 (↑/↓ 方向键, 32 条记录)
- Ctrl+C 取消当前行
- 引号字符串参数解析
- 未知命令建议 (前缀匹配)

### Init 进程与安全模式

**文件**: `src/kernel/init.c`, `src/kernel/init.h`

**Init 进程** (Phase P/Q):
- 作为第一个"用户进程"在内核上下文中运行
- 挂载 SFS 文件系统
- 加载上次保存的日志
- 启动日志自动保存
- 初始化终端驱动并进入 Shell/GUI

**安全模式** (Phase M):
- 当关键设备自检失败时自动进入
- 提供降级命令环境 (help, reboot, halt, sysinfo)
- 在 VGA 文本模式下输出设备错误详情
- 保证系统基本可用性

---

## 公共头文件

| 文件 | 说明 |
|------|------|
| `io.h` | I/O 端口操作 (`inb`, `outb`, `inw`, `outw`, `io_wait`, `cli`, `sti`, `hlt`) |
| `multiboot2.h` | Multiboot2 协议数据结构和遍历宏 (`FOR_EACH_TAG`) |
| `efi.h` | UEFI 完整类型定义 (SystemTable, BootServices, GOP, GUID 等) |
| `bootinfo.h` | 统一启动信息结构 (framebuffer, mmap, ACPI, kernel bounds) |
| `stdint.h` | 标准整数类型定义 (int8_t ~ uint64_t, 极值宏) |
| `stddef.h` | 基本类型 (`size_t`, `ssize_t`, `NULL`) |
| `stdarg.h` | 可变参数支持 (基于 `__builtin_va_list`) |
| `string.h` | 字符串操作 + `snprintf` (memset, memcpy, strlen, strcmp, strncmp, snprintf) |

---

## 链接脚本

### BIOS 模式 (`linker.ld`)
```
入口: _start
加载地址: 1MB (0x100000)
段布局: .text (R+X) → .rodata (R) → .data (R+W) → .bss (R+W)
导出: _start, _end
```

### UEFI 模式 (`linker_efi.ld`)
```
入口: efi_main
起始地址: 0x1000 (PE/COFF 兼容)
段布局: .text → .rodata → .data → .bss → .reloc (必需!)
导出: _start (=0), _end
```

---

## 已实现功能清单

### 启动与引导
- [x] Multiboot2 引导协议 (BIOS)
- [x] UEFI 引导 (PE/COFF, 默认模式)
- [x] 32 位 → 64 位长模式切换
- [x] 4 级分页 (PML4 → PDPT → PD → PT)
- [x] 2MB 大页身份映射 (低 4GB)
- [x] 高半核映射支持

### 内核基础
- [x] GDT (64 位代码/数据段 + TSS)
- [x] IDT (CPU 异常 + 硬件中断 + 系统调用)
- [x] 8259 PIC 驱动 (双片级联)
- [x] PIT 定时器 (100Hz)
- [x] 物理内存管理 (位图法, 双模式初始化)
- [x] 虚拟内存管理 (4 级页表, 地址空间隔离)
- [x] 轮转进程调度器 (Round-Robin, 64 进程)
- [x] 系统调用接口 (int 0x80, 32 调用号)

### 硬件驱动
- [x] PCI 总线枚举 (128 设备, 6 BAR)
- [x] ATA/PATA 磁盘驱动 (LBA28/LBA48, 4 设备)
- [x] xHCI USB 3.0 主控驱动 (命令/事件/传输环)
- [x] USB HID 协议栈 (键盘/鼠标 Boot Protocol)
- [x] PS/2 键盘驱动 (US 布局 + 扩展键)
- [x] PS/2 鼠标驱动 (3 字节协议)
- [x] 串口通信 (COM1/COM2, 115200 baud)

### 显示与交互
- [x] VGA 文本模式 (80×25, 16 色)
- [x] 帧缓冲图形模式 (自适应分辨率, 32bpp)
- [x] 双缓冲渲染
- [x] 8×16 位图字体渲染
- [x] RTC 实时时钟 (CMOS 硬件读取)
- [x] 图形桌面环境 (渐变背景, 任务栏, 时钟)
- [x] 窗口系统 (阴影, 标题栏, 拖拽, Z-order)
- [x] 窗口动画 (ease-out cubic, 开/关/最小化/还原)
- [x] 图形终端 (128×200, 绿色主题, 光标闪烁)
- [x] 启动 Logo (360×360 位图, 3 秒展示)
- [x] 交互式 Shell (26 条内置命令, GUI + 文本双模式)
- [x] Tab 自动补全 + 命令历史 (32 条)

### 文件系统与存储
- [x] SFS1 文件系统 (扁平目录, 连续分配, 64 文件)
- [x] 文件 CRUD 操作 (创建/读取/写入/删除/列举)
- [x] 文件系统格式化与挂载

### 安全与基础设施
- [x] 设备树注册与健康检查 (安全模式降级)
- [x] 内核日志系统 (5 级, 三路输出, 磁盘持久化)
- [x] 权限管理器 (12 种权限, 应用沙箱, 会话令牌)
- [x] 加密模块 (非对称密钥, 数字签名, 哈希)
- [x] Init 进程 + 安全模式

### 构建与开发
- [x] Makefile 双模式构建 (BIOS + UEFI)
- [x] ELF → UEFI PE 转换工具链 (Python + mingw)
- [x] QEMU 运行/调试支持 (BIOS + UEFI)
