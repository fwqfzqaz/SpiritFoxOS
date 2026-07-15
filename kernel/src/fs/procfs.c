/*
 * SpiritFoxOS /proc 文件系统
 *
 * 提供最小化的 /proc 文件系统，包含进程信息、
 * CPU 信息、内存信息、运行时间和内核版本。
 * 使用 memfs 支持的文件，由 procfs_update() 刷新。
 */

#include "procfs.h"
#include "vfs.h"
#include "process.h"
#include "memory.h"
#include "timer.h"
#include "hal.h"
#include "string.h"
#include "vga.h"

/* ========================================================================
 * 常量
 * ======================================================================== */

#define MAX_PROCS_INTERNAL 256
#define PROCFS_BUF_SIZE    1024
#define SPIRITFOX_VERSION  "SpiritFoxOS 0.1.0 (x86_64)"

/* ========================================================================
 * 简单的整数转字符串辅助函数（内核中没有 snprintf）
 * ======================================================================== */

static int itoa_local(char *buf, int val)
{
    char tmp[12];
    int pos = 0;
    int neg = 0;

    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { tmp[pos++] = '0'; }
    else {
        while (val > 0) { tmp[pos++] = '0' + (val % 10); val /= 10; }
    }

    int out = 0;
    if (neg) buf[out++] = '-';
    for (int i = pos - 1; i >= 0; i--)
        buf[out++] = tmp[i];
    buf[out] = '\0';
    return out;
}

static int uitoa_local(char *buf, uint64_t val)
{
    char tmp[20];
    int pos = 0;

    if (val == 0) { tmp[pos++] = '0'; }
    else {
        while (val > 0) { tmp[pos++] = '0' + (val % 10); val /= 10; }
    }

    int out = 0;
    for (int i = pos - 1; i >= 0; i--)
        buf[out++] = tmp[i];
    buf[out] = '\0';
    return out;
}

/* 简单的字符串追加到缓冲区，返回新偏移 */
static int buf_append(char *buf, int offset, const char *str)
{
    while (*str && offset < PROCFS_BUF_SIZE - 1)
        buf[offset++] = *str++;
    buf[offset] = '\0';
    return offset;
}

/* 追加一个整数 */
static int buf_append_int(char *buf, int offset, int val)
{
    char tmp[12];
    itoa_local(tmp, val);
    return buf_append(buf, offset, tmp);
}

/* 追加一个无符号整数 */
static int buf_append_uint(char *buf, int offset, uint64_t val)
{
    char tmp[20];
    uitoa_local(tmp, val);
    return buf_append(buf, offset, tmp);
}

/* ========================================================================
 * 辅助函数：将内容写入 /proc 文件路径
 * ======================================================================== */

static void procfs_write_file(const char *path, const char *content)
{
    int fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0);
    if (fd < 0)
        return;
    vfs_write(fd, content, strlen(content));
    vfs_close(fd);
}

/* ========================================================================
 * 内容生成器
 * ======================================================================== */

static void procfs_gen_version(void)
{
    procfs_write_file("/proc/version", SPIRITFOX_VERSION "\n");
}

static void procfs_gen_uptime(void)
{
    char buf[128];
    int off = 0;
    uint64_t ms = timer_get_ms();
    uint64_t sec = ms / 1000;
    off = buf_append_uint(buf, off, sec);
    off = buf_append(buf, off, ".");
    /* 小数部分（毫秒的 3 位数字） */
    uint64_t frac = ms % 1000;
    if (frac < 100) off = buf_append(buf, off, "0");
    if (frac < 10)  off = buf_append(buf, off, "0");
    off = buf_append_uint(buf, off, frac);
    off = buf_append(buf, off, "\n");
    procfs_write_file("/proc/uptime", buf);
}

static void procfs_gen_meminfo(void)
{
    char buf[PROCFS_BUF_SIZE];
    int off = 0;

    uint64_t total = pmm_total_pages();
    uint64_t used  = pmm_used_pages();
    uint64_t free  = total - used;
    uint64_t total_kb = total * (PAGE_SIZE / 1024);
    uint64_t used_kb  = used  * (PAGE_SIZE / 1024);
    uint64_t free_kb  = free  * (PAGE_SIZE / 1024);

    off = buf_append(buf, off, "MemTotal:       ");
    off = buf_append_uint(buf, off, total_kb);
    off = buf_append(buf, off, " kB\n");

    off = buf_append(buf, off, "MemFree:        ");
    off = buf_append_uint(buf, off, free_kb);
    off = buf_append(buf, off, " kB\n");

    off = buf_append(buf, off, "MemUsed:        ");
    off = buf_append_uint(buf, off, used_kb);
    off = buf_append(buf, off, " kB\n");

    off = buf_append(buf, off, "PagesTotal:     ");
    off = buf_append_uint(buf, off, total);
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "PagesFree:      ");
    off = buf_append_uint(buf, off, free);
    off = buf_append(buf, off, "\n");

    procfs_write_file("/proc/meminfo", buf);
}

static void procfs_gen_cpuinfo(void)
{
    char buf[PROCFS_BUF_SIZE];
    int off = 0;

    hal_cpuid_t cpuid_data;
    hal_cpuid(0, 0, &cpuid_data);

    /* 厂商字符串（来自 CPUID 叶 0 的 EBX、EDX、ECX） */
    char vendor[13];
    uint32_t *vb = (uint32_t *)vendor;
    vb[0] = cpuid_data.ebx;
    vb[1] = cpuid_data.edx;
    vb[2] = cpuid_data.ecx;
    vendor[12] = '\0';

    off = buf_append(buf, off, "processor\t: 0\n");
    off = buf_append(buf, off, "vendor_id\t: ");
    off = buf_append(buf, off, vendor);
    off = buf_append(buf, off, "\n");

    hal_cpuid(1, 0, &cpuid_data);
    off = buf_append(buf, off, "cpu family\t: ");
    off = buf_append_int(buf, off, (int)((cpuid_data.eax >> 8) & 0xF));
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "model\t\t: ");
    off = buf_append_int(buf, off, (int)((cpuid_data.eax >> 4) & 0xF));
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "stepping\t: ");
    off = buf_append_int(buf, off, (int)(cpuid_data.eax & 0xF));
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic\n");

    procfs_write_file("/proc/cpuinfo", buf);
}

static const char *state_name(int state)
{
    switch (state) {
    case PROC_UNUSED:   return "Unused";
    case PROC_READY:    return "Ready";
    case PROC_RUNNING:  return "Running";
    case PROC_BLOCKED:  return "Sleeping";
    case PROC_ZOMBIE:   return "Zombie";
    default:            return "Unknown";
    }
}

static void procfs_gen_pid_status(int pid)
{
    process_t *p = process_get(pid);
    if (!p)
        return;

    char path[64];
    char buf[PROCFS_BUF_SIZE];
    int off = 0;

    /* 写入 /proc/<pid>/status */
    off = buf_append(buf, off, "Name:\tProcess");
    off = buf_append_int(buf, off, pid);
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "State:\t");
    off = buf_append(buf, off, state_name(p->state));
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "Pid:\t");
    off = buf_append_int(buf, off, p->pid);
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "PPid:\t");
    off = buf_append_int(buf, off, p->ppid);
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "Uid:\t");
    off = buf_append_uint(buf, off, p->uid);
    off = buf_append(buf, off, "\n");

    off = buf_append(buf, off, "Gid:\t");
    off = buf_append_uint(buf, off, p->gid);
    off = buf_append(buf, off, "\n");

    strcpy(path, "/proc/");
    int plen = 6;
    plen += itoa_local(path + plen, pid);
    strcpy(path + plen, "/status");

    procfs_write_file(path, buf);
}

static void procfs_gen_pid_cmdline(int pid)
{
    process_t *p = process_get(pid);
    if (!p)
        return;

    char path[64];
    char buf[64];

    /* 简单的 cmdline：仅进程名占位符 */
    int off = 0;
    off = buf_append(buf, off, "proc:");
    off = buf_append_int(buf, off, pid);
    off = buf_append(buf, off, "\0");  /* cmdline 以 NUL 分隔 */

    strcpy(path, "/proc/");
    int plen = 6;
    plen += itoa_local(path + plen, pid);
    strcpy(path + plen, "/cmdline");

    procfs_write_file(path, buf);
}

static void procfs_gen_pid_maps(int pid)
{
    process_t *p = process_get(pid);
    if (!p)
        return;

    char path[64];
    char buf[PROCFS_BUF_SIZE];
    int off = 0;

    /* 代码段：entry_point 到 brk */
    off = buf_append(buf, off, "0x");
    /* 入口点十六进制 - 使用简单十六进制转换 */
    {
        char hex[17];
        for (int i = 15; i >= 0; i--) {
            hex[15 - i] = "0123456789abcdef"[(p->entry_point >> (i * 4)) & 0xF];
        }
        hex[16] = '\0';
        off = buf_append(buf, off, hex);
    }
    off = buf_append(buf, off, "-");

    {
        char hex[17];
        for (int i = 15; i >= 0; i--) {
            hex[15 - i] = "0123456789abcdef"[(p->brk >> (i * 4)) & 0xF];
        }
        hex[16] = '\0';
        off = buf_append(buf, off, hex);
    }
    off = buf_append(buf, off, " rwxp 00000000 00:00 0  [text]\n");

    /* 栈区域 */
    off = buf_append(buf, off, "0x");
    {
        uint64_t stack_base = p->stack_top - PROC_STACK_SIZE;
        char hex[17];
        for (int i = 15; i >= 0; i--) {
            hex[15 - i] = "0123456789abcdef"[(stack_base >> (i * 4)) & 0xF];
        }
        hex[16] = '\0';
        off = buf_append(buf, off, hex);
    }
    off = buf_append(buf, off, "-");
    {
        char hex[17];
        for (int i = 15; i >= 0; i--) {
            hex[15 - i] = "0123456789abcdef"[(p->stack_top >> (i * 4)) & 0xF];
        }
        hex[16] = '\0';
        off = buf_append(buf, off, hex);
    }
    off = buf_append(buf, off, " rwxp 00000000 00:00 0  [stack]\n");

    strcpy(path, "/proc/");
    int plen = 6;
    plen += itoa_local(path + plen, pid);
    strcpy(path + plen, "/maps");

    procfs_write_file(path, buf);
}

/* ========================================================================
 * 为指定 pid 创建 /proc/<pid> 目录及其文件
 * ======================================================================== */

static void procfs_create_pid_dir(int pid)
{
    char path[64];

    /* /proc/<pid>/ 目录 */
    strcpy(path, "/proc/");
    int plen = 6;
    plen += itoa_local(path + plen, pid);
    path[plen] = '\0';
    vfs_mkdir(path, VFS_S_IRUSR | VFS_S_IXUSR);

    /* /proc/<pid>/status 文件 */
    strcpy(path + plen, "/status");
    {
        int fd = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR);
        if (fd >= 0) vfs_close(fd);
    }

    /* /proc/<pid>/cmdline 文件 */
    strcpy(path + plen, "/cmdline");
    {
        int fd = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR);
        if (fd >= 0) vfs_close(fd);
    }

    /* /proc/<pid>/maps 文件 */
    strcpy(path + plen, "/maps");
    {
        int fd = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR);
        if (fd >= 0) vfs_close(fd);
    }
}

/* ========================================================================
 * 公共 API
 * ======================================================================== */

void procfs_init(void)
{
    /* 创建 /proc 目录 */
    vfs_mkdir("/proc", VFS_S_IRUSR | VFS_S_IXUSR);

    /* /proc/version - 简单静态内容 */
    {
        int fd = vfs_open("/proc/version", VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR);
        if (fd >= 0) {
            vfs_write(fd, "SpiritFoxOS 0.4.0\n", 19);
            vfs_close(fd);
        }
    }

    /* /proc/uptime 文件 */
    {
        int fd = vfs_open("/proc/uptime", VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR);
        if (fd >= 0) {
            vfs_write(fd, "0.0\n", 4);
            vfs_close(fd);
        }
    }

    /* /proc/meminfo 文件 */
    {
        int fd = vfs_open("/proc/meminfo", VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR);
        if (fd >= 0) {
            vfs_write(fd, "MemTotal: 0 kB\n", 15);
            vfs_close(fd);
        }
    }

    /* /proc/self 目录 */
    vfs_mkdir("/proc/self", VFS_S_IRUSR | VFS_S_IXUSR);
    {
        int fd = vfs_open("/proc/self/status", VFS_O_CREAT | VFS_O_WRONLY, VFS_S_IRUSR);
        if (fd >= 0) {
            vfs_write(fd, "Name:\tkernel\nPid:\t0\n", 20);
            vfs_close(fd);
        }
    }
}

void procfs_update(void)
{
    /* 更新静态文件 */
    procfs_gen_version();
    procfs_gen_uptime();
    procfs_gen_meminfo();
    procfs_gen_cpuinfo();

    /* 更新 /proc/self（当前进程信息） */
    {
        process_t *cur = process_current();
        if (cur) {
            /* 写入 self/status，包含当前进程信息 */
            char buf[PROCFS_BUF_SIZE];
            int off = 0;

            off = buf_append(buf, off, "Name:\tProcess");
            off = buf_append_int(buf, off, cur->pid);
            off = buf_append(buf, off, "\n");

            off = buf_append(buf, off, "State:\t");
            off = buf_append(buf, off, state_name(cur->state));
            off = buf_append(buf, off, "\n");

            off = buf_append(buf, off, "Pid:\t");
            off = buf_append_int(buf, off, cur->pid);
            off = buf_append(buf, off, "\n");

            off = buf_append(buf, off, "PPid:\t");
            off = buf_append_int(buf, off, cur->ppid);
            off = buf_append(buf, off, "\n");

            off = buf_append(buf, off, "Uid:\t");
            off = buf_append_uint(buf, off, cur->uid);
            off = buf_append(buf, off, "\n");

            off = buf_append(buf, off, "Gid:\t");
            off = buf_append_uint(buf, off, cur->gid);
            off = buf_append(buf, off, "\n");

            procfs_write_file("/proc/self/status", buf);

            /* self/cmdline 文件 */
            {
                char cbuf[64];
                int coff = 0;
                coff = buf_append(cbuf, coff, "proc:");
                coff = buf_append_int(cbuf, coff, cur->pid);
                coff = buf_append(cbuf, coff, "\0");
                procfs_write_file("/proc/self/cmdline", cbuf);
            }
        }
    }

    /* 更新各进程文件 */
    for (int i = 0; i < MAX_PROCS_INTERNAL; i++) {
        process_t *p = process_get(i);
        if (p) {
            procfs_gen_pid_status(i);
            procfs_gen_pid_cmdline(i);
            procfs_gen_pid_maps(i);
        }
    }
}
