# SpiritFoxOS libc Phase 1 实施计划 — 基础可用闭环

## Context

SpiritFoxOS 内核侧已实现 90+ 个对齐 Linux x86_64 ABI 的系统调用，但用户态 libc 功能覆盖度低、接口残缺，是制约用户态生态的核心瓶颈。本计划聚焦 Phase 1：**使 libc 能编译运行基础 CLI 工具（ls、cat、echo）**。

### 现有问题

1. **重复定义**：`unistd.c` 中的 `read/write/close` 与 `syscall.c` 完全重复且缺少 errno 处理
2. **无 FILE* 抽象**：stdio.c 仅提供 printf/snprintf，缺少 fopen/fread/fwrite 等文件流操作
3. **缺少大量头文件**：无 ctype.h、stdint.h、stdbool.h、signal.h、dirent.h、sys/stat.h、unistd.h、stdlib.h、string.h、stdio.h
4. **缺少关键函数**：stat/fstat/getdents64/opendir/readdir/strdup/strtol/atexit/sleep 等
5. **SYS_* 宏在各 .c 文件中重复定义**：syscall.c、unistd.c、stdio.c、malloc.c 各自硬编码
6. **Makefile 引用 musl-1.2.5 但未实际使用**

### 目标工具的 libc 依赖

| 工具 | 所需 syscall | 所需 libc 函数 |
|------|-------------|---------------|
| `echo` | write, exit | printf/puts, strlen |
| `cat`  | read, write, open, close, exit | fopen/fread/fwrite/fclose 或 read/write/open/close |
| `ls`   | open, close, getdents64, stat, write, exit, getcwd | opendir/readdir/closedir, stat, printf, strcmp, strlen |

---

## 实施步骤

### Step 1: 基础类型层（无依赖）

**1.1** 新建 `libc/include/stdint.h` — 精确宽度整数类型定义
- int8_t/uint8_t/int16_t/uint16_t/int32_t/uint32_t/int64_t/uint64_t
- intptr_t/uintptr_t/intmax_t/uintmax_t
- 限值宏 INT8_MIN~UINT64_MAX, SIZE_MAX

**1.2** 新建 `libc/include/stdbool.h` — bool/true/false 宏

**1.3** 修改 `libc/include/stddef.h` — 添加 `#include <stdint.h>` 确保类型可用

**1.4** 新建 `libc/src/internal.h` — 内部公共头文件
- 统一所有 `SYS_*` 宏定义（从 syscall.c 中提取）
- 统一 `SET_ERRNO` 宏
- `extern int sfk_errno` 声明
- 包含 `arch/x86_64/syscall.h`、`<sys/types.h>`、`<stddef.h>`、`<errno.h>`

### Step 2: 核心函数层（依赖 Step 1）

**2.1** 新建 `libc/include/string.h` — 声明所有字符串/内存函数

**2.2** 移动 `libc/string.c` → `libc/src/string.c`，扩展缺失函数
- 新增：strncmp, strncat, strrchr, strspn, strcspn, strpbrk, strtok, strdup, strstr
- 保留：memset, memcpy, memmove, memcmp, memchr, strlen, strcpy, strncpy, strcmp, strcat, strchr

**2.3** 新建 `libc/include/ctype.h` + `libc/src/ctype.c` — 字符分类
- 查表法实现：isalpha, isdigit, isalnum, isspace, isupper, islower, isprint, ispunct, isxdigit, isgraph, iscntrl, toupper, tolower

### Step 3: Syscall 扩展层（依赖 Step 1）

**3.1** 移动 `libc/syscall.c` → `libc/src/syscall.c`，重构并扩展
- 删除文件内 `#define SYS_*` 宏，改用 `#include "internal.h"`
- 将 `sfk_errno` 从 `static` 改为非 static
- 新增 wrapper：`fstat`, `stat`, `lstat`, `umask`, `access`, `fchmod`, `chown`, `ioctl`, `isatty`

**3.2** 新建 `libc/include/sys/stat.h` — struct stat 定义
- 字段布局必须与内核 `linux_stat_t`（kernel/include/syscall.h:209-228）严格一致
- 包含 S_IFMT/S_IFDIR/S_ISREG 等文件类型宏和 S_IS* 判断宏
- 包含 S_IRUSR/S_IWUSR 等权限位宏

**3.3** 新建 `libc/include/unistd.h` — POSIX 系统接口声明
- read/write/close/dup/dup2/lseek/pipe/fcntl
- chdir/getcwd/unlink/rmdir/mkdir/rename/chmod/access/umask
- fork/execve/getpid/getppid/getuid/getgid/setuid/setgid/wait4
- sleep/usleep/isatty

### Step 4: 标准库扩展层（依赖 Step 2-3）

**4.1** 移动 `libc/stdlib.c` → `libc/src/stdlib.c`，大幅扩展
- 新增 atexit 机制 + 修改 exit() 反序调用 atexit 函数
- 新增 strtol/strtoul
- 新增 qsort（插入排序）/ bsearch
- 新增 rand/srand（简单 LCG）
- 新增 getenv/setenv/unsetenv（Phase 1 返回 NULL/-1）

**4.2** 新建 `libc/include/stdlib.h` — stdlib 函数声明

**4.3** 新建 `libc/src/time.c` — time/sleep/usleep
- time() 封装 SYS_time(201)
- sleep() 基于 nanosleep 实现
- usleep() 基于 nanosleep 实现

**4.4** 扩展 `libc/include/time.h` — 添加 time() 声明

### Step 5: 文件 I/O 层（依赖 Step 3-4）

**5.1** 移动 `libc/stdio.c` → `libc/src/stdio.c`，重写为 FILE* 抽象
- FILE 结构体：fd, flags, err, eof（简化版，Phase 1 无缓冲）
- 预定义 stdin/stdout/stderr（静态 FILE 对象）
- fopen/fclose/fdopen — 文件打开/关闭
- fread/fwrite — 块读写
- fgetc/fputc/fgets/fputs — 字符/行操作
- fseek/ftell/rewind — 定位
- feof/ferror/clearerr/fflush — 状态
- perror — 错误信息输出
- 保留并改进 printf/snprintf/vsnprintf/puts/putchar
- 新增 fprintf/sprintf

**5.2** 新建 `libc/include/stdio.h` — FILE 抽象和所有 stdio 函数声明

**5.3** 新建 `libc/src/dirent.c` + `libc/include/dirent.h` — 目录操作
- DIR 结构体：fd + 内部 getdents64 缓冲区
- opendir：open + O_DIRECTORY + calloc DIR
- readdir：从缓冲区取 dirent，耗尽时调用 getdents64(217)
- closedir：close + free
- struct dirent：d_ino/d_off/d_reclen/d_type/d_name（对齐 Linux linux_dirent64）
- DT_UNKNOWN/DT_DIR/DT_REG 等 d_type 常量

**5.4** 新建 `libc/src/fcntl.c` — creat/openat
- 修改 `libc/include/fcntl.h` 添加函数声明

### Step 6: 信号与清理（依赖 Step 3）

**6.1** 新建 `libc/src/signal.c` + `libc/include/signal.h`
- kill/raise/signal/sigaction
- 信号编号：SIGHUP~SIGTSTP（与内核 process.h 一致）
- struct sigaction（与内核 linux_sigaction_t 对应）

**6.2** 移动 `libc/malloc.c` → `libc/src/malloc.c`，改用 `#include "internal.h"`

**6.3** 重写 `libc/src/unistd.c` — 仅保留 sleep/usleep，删除重复的 read/write/close

**6.4** 移动 `libc/start.c` → `libc/src/start.c`

**6.5** 更新 `libc/Makefile`
- 源文件路径改为 src/ 子目录
- 添加 -I./src 到 CFLAGS
- 移除 MUSL_SRCDIR 相关配置
- OBJS 列表更新

### Step 7: 主 Makefile 集成

- 在顶层 `Makefile` 中添加 libc 构建目标
- 用户程序编译模板：`crt1.o + crti.o + 用户.o + libc.a + crtn.o`

---

## 目录结构变更

```
libc/
  arch/x86_64/syscall.h       # 保持不变
  crt1.S                       # 保持不变
  crti.S                       # 保持不变
  crtn.S                       # 保持不变
  Makefile                      # 更新
  include/
    ctype.h                     # 新建
    dirent.h                    # 新建
    errno.h                     # 保持不变
    fcntl.h                     # 修改（添加函数声明）
    mman.h                      # 保持不变
    signal.h                    # 新建
    stdarg.h                    # 保持不变
    stdbool.h                   # 新建
    stddef.h                    # 修改（添加 #include <stdint.h>）
    stdint.h                    # 新建
    stdio.h                     # 新建
    stdlib.h                    # 新建
    string.h                    # 新建
    time.h                      # 修改（添加 time() 声明）
    unistd.h                    # 新建
    sys/
      stat.h                    # 新建
      types.h                   # 保持不变
  src/
    internal.h                  # 新建 — 核心内部头
    ctype.c                     # 新建
    dirent.c                    # 新建
    fcntl.c                     # 新建
    malloc.c                    # 移入 + 重构
    signal.c                    # 新建
    start.c                     # 移入
    stdio.c                     # 移入 + 重写
    stdlib.c                    # 移入 + 扩展
    string.c                    # 移入 + 扩展
    syscall.c                   # 移入 + 重构
    time.c                      # 新建
    unistd.c                    # 移入 + 重写
  dl/
    ldso.c                      # 保持不变
```

---

## 关键风险

1. **内核 getdents64 是桩函数**：当前 `sys_getdents64` 返回 0，`ls` 的 `readdir()` 将得到 NULL。libc 侧实现正确，但 ls 需要**内核侧修复 getdents** 才能工作。这是 ls 能否运行的**最大阻塞项**。
2. **struct stat 布局**：必须与内核 `linux_stat_t` 字段严格一致，否则 fstat 返回垃圾数据。
3. **无缓冲 I/O**：Phase 1 FILE* 无缓冲，fgetc/fputc 每次直接 syscall。cat 应使用 fread 大块读取。
4. **environ 空表**：Phase 1 getenv 始终返回 NULL。

---

## 验证方案

每步完成后用 `cd libc && make clean && make` 验证编译通过。

最终验证：编写 test_ls.c / test_cat.c / test_echo.c，用交叉编译器链接 libc.a 生成 ELF，在 QEMU 中运行。
