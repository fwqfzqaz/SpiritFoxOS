# SpiritFoxOS

**版本**: 1.0.0  
**架构**: x86_64 (Long Mode)  
**引导协议**: Multiboot2  
**许可证**: 开源爱好项目

SpiritFoxOS 是一个从零开始编写的 x86_64 操作系统，具备完整的启动流程、硬件抽象、内存管理、进程调度、图形界面和交互式 Shell。

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
  - [键盘驱动](#键盘驱动)
  - [鼠标驱动](#鼠标驱动)
  - [串口通信](#串口通信)
  - [VGA 文本模式](#vga-文本模式)
  - [帧缓冲与图形模式](#帧缓冲与图形模式)
  - [字体渲染](#字体渲染)
  - [图形用户界面 (GUI)](#图形用户界面-gui)
  - [命令行 Shell](#命令行-shell)
- [公共头文件](#公共头文件)
- [链接脚本](#链接脚本)
- [已实现功能清单](#已实现功能清单)

---

## 项目概述

SpiritFoxOS 是一个 64 位爱好操作系统，运行在 x86_64 架构上。系统通过 GRUB 引导加载程序以 Multiboot2 协议启动，从 32 位保护模式切换到 64 位长模式，然后进入内核主函数。

系统支持两种显示模式：
- **VGA 文本模式**：80×25 字符终端，提供功能完整的命令行 Shell
- **图形帧缓冲模式**：1024×768 分辨率，32 位色深，带有桌面环境、窗口系统和图形终端

---

## 项目结构

```
SpiritFoxOS/
├── Makefile                  # 构建系统
├── linker.ld                 # 链接脚本
├── iso/
│   └── boot/
│       ├── grub/
│       │   └── grub.cfg      # GRUB 引导配置
│       └── kernel.elf        # 内核 ELF 文件 (构建产物)
├── build/                    # 构建输出目录
│   ├── kernel.elf
│   ├── spiritfoxos.iso
│   ├── boot/
│   │   └── boot.o
│   └── kernel/
│       ├── fb.o
│       ├── font.o
│       ├── gdt.o
│       ├── gui.o
│       ├── idt.o
│       ├── isr.o
│       ├── kernel.o
│       ├── keyboard.o
│       ├── mouse.o
│       ├── pic.o
│       ├── pit.o
│       ├── pmm.o
│       ├── scheduler.o
│       ├── serial.o
│       ├── shell.o
│       ├── vga.o
│       └── vmm.o
└── src/
    ├── boot/
    │   └── boot.asm          # Multiboot2 头 + 32位入口 + 长模式切换
    ├── include/
    │   ├── io.h              # I/O 端口操作内联函数
    │   ├── multiboot2.h      # Multiboot2 数据结构定义
    │   ├── stdarg.h          # 可变参数支持
    │   ├── stddef.h          # 基本类型定义 (size_t, NULL)
    │   ├── stdint.h          # 标准整数类型
    │   └── string.h          # 字符串操作函数
    └── kernel/
        ├── fb.c / fb.h       # 帧缓冲驱动
        ├── font.c / font.h   # 8×16 位图字体
        ├── gdt.c / gdt.h     # 全局描述符表
        ├── gui.c / gui.h     # 图形用户界面
        ├── idt.c / idt.h     # 中断描述符表
        ├── isr.asm           # 中断服务例程 (汇编)
        ├── kernel.c          # 内核主入口
        ├── keyboard.c / keyboard.h  # PS/2 键盘驱动
        ├── mouse.c / mouse.h        # PS/2 鼠标驱动
        ├── pic.c / pic.h            # 8259 PIC 驱动
        ├── pit.c / pit.h            # PIT 定时器驱动
        ├── pmm.c / pmm.h            # 物理内存管理器
        ├── scheduler.c / scheduler.h # 轮转调度器
        ├── serial.c / serial.h      # 串口通信驱动
        ├── shell.c / shell.h        # 交互式命令行 Shell
        ├── vga.c / vga.h            # VGA 文本模式驱动
        └── vmm.c / vmm.h            # 虚拟内存管理器
```

---

## 构建与运行

### 依赖工具

| 工具 | 用途 |
|------|------|
| `nasm` | 汇编器 (输出 ELF64 格式) |
| `gcc` / `x86_64-elf-gcc` | C 编译器 (freestanding 模式) |
| `ld` / `x86_64-elf-ld` | 链接器 |
| `grub-mkrescue` | 创建可引导 ISO |
| `xorriso`, `mtools` | ISO 创建依赖 |
| `qemu-system-x86_64` | 模拟器运行与调试 |

### 编译命令

```bash
make            # 编译内核 ELF
make iso        # 创建可引导 ISO 镜像
make run        # 编译 + 创建 ISO + QEMU 运行
make debug      # QEMU 运行 + 串口调试输出
make qemu-direct # 直接引导内核 (无需 ISO)
make clean      # 清理构建产物
```

### 编译器标志

```
-ffreestanding -fno-pie -fno-stack-protector -fno-pic
-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-sse3
-mcmodel=small -m64
-Wall -Wextra -Werror
-nostdlib -nostdinc -fno-builtin -nodefaultlibs
```

---

## 启动流程

```
GRUB 引导加载程序
    │
    ├─ 读取 Multiboot2 头
    ├─ 请求 1024×768×32bpp 图形帧缓冲
    ├─ 加载 kernel.elf 至 1MB 地址
    └─ 切换到 32 位保护模式，跳转 _start
         │
         ▼
boot.asm: _start (32位)
    ├─ 保存 Multiboot2 magic 和 info 指针
    ├─ 设置 32 位栈
    ├─ 检查 CPUID 支持
    ├─ 检查长模式支持
    ├─ 设置页表 (PML4→PDPT→PD, 2MB 大页)
    │   ├─ 身份映射低 4GB
    │   └─ 高半核映射 0xFFFFFFFF80000000
    ├─ 启用 PAE (CR4 bit 5)
    ├─ 启用长模式 (IA32_EFER.LME)
    ├─ 启用分页 (CR0 bit 31)
    ├─ 加载 64 位 GDT
    └─ 远跳转至 long_mode_entry
         │
         ▼
boot.asm: long_mode_entry (64位)
    ├─ 加载 64 位数据段选择子
    ├─ 设置 64 位栈
    ├─ 传递 Multiboot2 参数 (edi, esi)
    └─ call kernel_main
         │
         ▼
kernel.c: kernel_main()
    ├─ 初始化串口 (COM1, 115200 baud)
    ├─ 解析 Multiboot2 信息 (mmap, framebuffer)
    ├─ 检测图形/文本模式
    ├─ 初始化 VGA (文本模式时)
    ├─ 初始化 GDT
    ├─ 初始化 IDT
    ├─ 初始化 PIC
    ├─ 初始化 PIT (100Hz)
    ├─ 初始化 PMM
    ├─ 初始化 VMM
    ├─ 初始化键盘驱动
    ├─ 初始化调度器
    ├─ 启用中断 (sti)
    └─ 进入 GUI 或 Shell 主循环
```

---

## 内核架构

### 引导与长模式切换

**文件**: `src/boot/boot.asm`

引导汇编代码负责从 GRUB 交接的 32 位保护模式切换到 64 位长模式：

- **Multiboot2 头**：魔数 `0xE85250D6`，架构 i386，请求 1024×768×32bpp 帧缓冲
- **页表设置**：5 个 4KB 页表 (PML4, PDPT, PD0-PD3)，使用 2MB 大页映射前 4GB 物理内存
  - `PML4[0]` → 低 4GB 身份映射
  - `PML4[256]` → 高半核映射 (0xFFFFFFFF80000000)
- **GDT64**：Null + 64位代码段 + 数据段，使用 `lgdt` 加载后远跳转

### 全局描述符表 (GDT)

**文件**: `src/kernel/gdt.c`, `src/kernel/gdt.h`

| 索引 | 选择子 | 描述 |
|------|--------|------|
| 0 | 0x00 | 空描述符 |
| 1 | 0x08 | 64 位内核代码段 (Execute/Read, Present, Long mode) |
| 2 | 0x10 | 内核数据段 (Read/Write, Present) |
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
- ISR 汇编存根保存/恢复所有通用寄存器，调用 C 函数 `interrupt_handler()`
- 支持动态注册中断处理函数

**中断栈帧结构**：
```
R15, R14, R13, R12, R11, R10, R9, R8    ← 保存的通用寄存器
RDI, RSI, RBP, RDX, RCX, RBX, RAX
INT_NO, ERR_CODE                          ← 中断号和错误码
RIP, CS, RFLAGS, RSP, SS                  ← CPU 压入的上下文
```

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
- 用于系统时钟和进程调度

### 物理内存管理 (PMM)

**文件**: `src/kernel/pmm.c`, `src/kernel/pmm.h`

- **算法**：位图法，每个 bit 对应一个 4KB 物理页
- **页大小**：4096 字节
- **位图存储**：在可用内存区域中自动定位，页对齐
- **初始化流程**：
  1. 扫描 Multiboot2 内存映射，找到最高可用地址
  2. 计算位图大小并放置位图
  3. 初始标记所有页为已使用
  4. 将可用内存区域标记为空闲
  5. 标记内核和位图自身为已使用
- **分配函数**：
  - `pmm_alloc_page()` — 分配单个物理页，使用 `__builtin_ctzll` 快速查找
  - `pmm_alloc_pages(n)` — 分配 n 个连续物理页
  - `pmm_free_page(addr)` — 释放物理页

### 虚拟内存管理 (VMM)

**文件**: `src/kernel/vmm.c`, `src/kernel/vmm.h`

- **四级页表**：PML4 → PDPT → PD → PT → Page (4 级分页)
- **页表项标志**：Present, Writable, User, Accessed, Dirty, Huge, Global, NX
- **地址掩码**：`0x000FFFFFFFFFF000`
- **身份映射**：前 4GB 物理地址 = 虚拟地址 (由引导代码设置)
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
- **进程控制块**：
  ```c
  typedef struct process {
      uint64_t pid;
      proc_state_t state;
      uint64_t rsp;           // 保存的内核栈指针
      uint64_t cr3;           // 页表物理地址
      uint64_t stack_top;     // 栈顶 (用于释放)
      uint64_t remaining_ticks;
      struct process *next;   // 队列链接
  } process_t;
  ```
- **上下文切换**：在 PIT 时钟中断中保存 RSP，恢复下一个进程的 RSP
- **TSS 更新**：每次切换更新 RSP0 以支持内核栈切换
- **CR3 切换**：进程地址空间不同时刷新页表

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

### 鼠标驱动

**文件**: `src/kernel/mouse.c`, `src/kernel/mouse.h`

- **接口**：PS/2 鼠标 (IRQ12, 端口 0x60/0x64)
- **协议**：标准 3 字节 PS/2 鼠标协议
- **特性**：
  - 左/右/中键检测
  - X/Y 位移解析 (含符号扩展)
  - 屏幕边界限制
  - 同步检查 (byte0 bit3)
- **状态结构**：
  ```c
  typedef struct {
      int32_t x, y;
      uint8_t buttons;    // bit0=左, bit1=右, bit2=中
      int8_t scroll;
      uint32_t screen_width, screen_height;
  } mouse_state_t;
  ```

### 串口通信

**文件**: `src/kernel/serial.c`, `src/kernel/serial.h`

- **端口**：COM1 (0x3F8), COM2 (0x2F8)
- **波特率**：115200
- **配置**：8N1 (8 数据位, 无校验, 1 停止位)
- **用途**：调试输出 (QEMU `-serial stdio` 模式)

### VGA 文本模式

**文件**: `src/kernel/vga.c`, `src/kernel/vga.h`

- **分辨率**：80×25 字符
- **缓冲区地址**：`0xB8000`
- **颜色**：16 色 (0=黑 ~ 15=白)
- **功能**：
  - 字符/字符串输出
  - 滚屏
  - 硬件光标更新
  - 颜色设置
  - 简易 `printf` 实现 (支持 %d, %u, %x, %p, %c, %s, %%)

### 帧缓冲与图形模式

**文件**: `src/kernel/fb.c`, `src/kernel/fb.h`

- **分辨率**：1024×768 (由 Multiboot2 请求)
- **色深**：32 位 (0xRRGGBB 格式)
- **双缓冲**：分配后缓冲区，通过 `fb_swap_buffers()` 交换
- **绘图原语**：
  - `fb_draw_pixel()` — 像素绘制
  - `fb_fill_rect()` — 填充矩形
  - `fb_draw_rect()` — 矩形边框
  - `fb_draw_line()` — Bresenham 直线算法
  - `fb_clear()` — 清屏
- **预定义颜色**：黑、白、红、绿、蓝、青、黄、品红、橙、灰 + SpiritFoxOS 主题色

### 字体渲染

**文件**: `src/kernel/font.c`, `src/kernel/font.h`

- **字体**：8×16 像素位图字体，128 个 ASCII 字符
- **渲染模式**：
  - `font_draw_char()` — 带前景/背景色渲染
  - `font_draw_string()` — 字符串渲染 (带背景)
  - `font_draw_string_transparent()` — 透明背景渲染

### 图形用户界面 (GUI)

**文件**: `src/kernel/gui.c`, `src/kernel/gui.h`

当检测到图形帧缓冲时自动进入 GUI 模式：

- **桌面环境**：
  - 渐变背景 (深蓝色调)
  - 底部任务栏 (SpiritFoxOS 主题色)
  - 开始按钮 ("SpiritFox")
  - 系统时钟显示
  - 桌面图标 (Terminal, System, About)
- **窗口系统**：
  - 窗口阴影效果
  - 标题栏 (活动/非活动状态)
  - 关闭按钮 (红色 X)
  - 窗口边框
- **终端窗口**：
  - 128×48 字符终端缓冲区
  - 绿色文字 + 深色背景
  - 光标闪烁
  - 内置命令：help, uname, uptime, mem, clear, neofetch, exit
- **鼠标交互**：
  - 16×16 箭头光标
  - 点击桌面图标打开窗口
  - 点击关闭按钮关闭窗口
  - Enter 键打开终端

### 命令行 Shell

**文件**: `src/kernel/shell.c`, `src/kernel/shell.h`

Shell 名称：`sfsh 1.0`，在 VGA 文本模式下运行。

**提示符**：`root@SpiritFoxOS:~#` (彩色)

**内置命令**：

| 命令 | 说明 |
|------|------|
| `help` | 显示帮助信息 |
| `clear` | 清屏 |
| `echo <text>` | 输出文本 |
| `history` | 显示命令历史 |
| `color <fg> [bg]` | 更改文字颜色 (0-15) |
| `about` | 关于 SpiritFoxOS |
| `neofetch` | 系统信息展示 (含 ASCII 狐狸图标和调色板) |
| `cowsay <msg>` | ASCII 牛说消息 |
| `beep` | PC 扬声器蜂鸣 |
| `uname [-a]` | 系统信息 |
| `sysinfo` | 详细系统概览 |
| `cpuinfo` | CPU 信息 (通过 CPUID 获取厂商和型号) |
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
- Tab 自动补全
- 命令历史 (↑/↓ 方向键, 32 条记录)
- Ctrl+C 取消当前行
- 引号字符串参数解析
- 未知命令建议 (前缀匹配)

---

## 公共头文件

| 文件 | 说明 |
|------|------|
| `io.h` | I/O 端口操作 (`inb`, `outb`, `inw`, `outw`, `io_wait`, `cli`, `sti`, `hlt`) |
| `multiboot2.h` | Multiboot2 协议数据结构和遍历宏 (`FOR_EACH_TAG`) |
| `stdint.h` | 标准整数类型定义 (int8_t ~ uint64_t, 极值宏) |
| `stddef.h` | 基本类型 (`size_t`, `ssize_t`, `NULL`) |
| `stdarg.h` | 可变参数支持 (基于 `__builtin_va_list`) |
| `string.h` | 字符串操作 (`memset`, `memcpy`, `memcmp`, `strlen`, `strcpy`, `strncpy`, `strcmp`, `strncmp`, `skip_spaces`) |

---

## 链接脚本

**文件**: `linker.ld`

```
入口: _start
加载地址: 1MB (0x100000)

段布局:
  .text   (R+X) ← Multiboot2 头 + 代码
  .rodata (R)   ← 只读数据 (GDT64 等)
  .data   (R+W) ← 已初始化数据
  .bss    (R+W) ← 未初始化数据

导出符号:
  _start → 内核起始地址
  _end   → 内核结束地址
```

---

## 已实现功能清单

- [x] Multiboot2 引导协议
- [x] 32 位 → 64 位长模式切换
- [x] 4 级分页 (PML4→PDPT→PD→PT)
- [x] 2MB 大页身份映射 (低 4GB)
- [x] 高半核映射支持
- [x] GDT (64 位代码/数据段 + TSS)
- [x] IDT (CPU 异常 + 硬件中断)
- [x] 8259 PIC 驱动
- [x] PIT 定时器 (100Hz)
- [x] 物理内存管理 (位图法)
- [x] 虚拟内存管理 (4 级页表)
- [x] 轮转进程调度器
- [x] PS/2 键盘驱动 (US 布局 + 扩展键)
- [x] PS/2 鼠标驱动
- [x] 串口通信 (COM1, 115200 baud)
- [x] VGA 文本模式 (80×25, 16 色)
- [x] 帧缓冲图形模式 (1024×768×32bpp)
- [x] 双缓冲
- [x] 位图字体渲染 (8×16)
- [x] 图形桌面环境
- [x] 窗口系统
- [x] 图形终端
- [x] 交互式 Shell (26 条内置命令)
- [x] Tab 自动补全
- [x] 命令历史
- [x] CPUID 信息读取
- [x] QEMU 运行/调试支持
