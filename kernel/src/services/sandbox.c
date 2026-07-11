/*
 * sandbox.c - SpiritFoxOS 沙箱实现
 *
 * 为 SFK 应用提供基于权限的轻量级沙箱机制。
 * 每个沙箱进程都有一个策略，根据授予的 SFK 权限
 * 控制文件访问、网络、设备和信号。
 */

#include "sandbox.h"
#include "syscall.h"
#include "process.h"
#include "string.h"
#include "vga.h"

/* ========================================================================
 * 常量
 * ======================================================================== */

#define SANDBOX_MAX_ENTRIES 64

/* ========================================================================
 * Sandbox table entry
 * ======================================================================== */

typedef struct {
    int              in_use;
    int              pid;
    char             pkg_id[64];
    sandbox_policy_t policy;
} sandbox_entry_t;

/* ========================================================================
 * Static sandbox table
 * ======================================================================== */

static sandbox_entry_t sandbox_table[SANDBOX_MAX_ENTRIES];

/* ========================================================================
 * 辅助函数
 * ======================================================================== */

static int snprintf_local(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    char *out = buf;
    size_t remaining = size;
    const char *p = fmt;
    while (*p && remaining > 1) {
        if (*p == '%' && *(p+1)) {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                while (*s && remaining > 1) { *out++ = *s++; remaining--; }
            } else if (*p == 'u' || *p == 'd') {
                int v = __builtin_va_arg(ap, int);
                if (v < 0 && *p == 'd' && remaining > 1) { *out++ = '-'; remaining--; v = -v; }
                char tmp[20]; int pos = 0;
                if (v == 0) tmp[pos++] = '0';
                else while (v > 0) { tmp[pos++] = '0' + (v % 10); v /= 10; }
                for (int i = pos - 1; i >= 0 && remaining > 1; i--) { *out++ = tmp[i]; remaining--; }
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                const char *hex = "0123456789abcdef";
                char tmp[16]; int pos = 0;
                if (v == 0) tmp[pos++] = '0';
                else while (v > 0) { tmp[pos++] = hex[v & 0xf]; v >>= 4; }
                for (int i = pos - 1; i >= 0 && remaining > 1; i--) { *out++ = tmp[i]; remaining--; }
            } else { if (remaining > 1) { *out++ = '%'; remaining--; } if (remaining > 1) { *out++ = *p; remaining--; } }
            p++;
        } else {
            *out++ = *p++;
            remaining--;
        }
    }
    *out = '\0';
    __builtin_va_end(ap);
    return (int)(out - buf);
}

/* PID 到表索引的简单哈希 */
static unsigned int sandbox_hash(int pid)
{
    unsigned int h = (unsigned int)pid;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = ((h >> 16) ^ h) * 0x45d9f3b;
    h = (h >> 16) ^ h;
    return h % SANDBOX_MAX_ENTRIES;
}

/* Find a table entry by PID (linear probe) */
static sandbox_entry_t *sandbox_find_entry(int pid)
{
    unsigned int start = sandbox_hash(pid);
    unsigned int idx = start;

    do {
        if (sandbox_table[idx].in_use && sandbox_table[idx].pid == pid) {
            return &sandbox_table[idx];
        }
        if (!sandbox_table[idx].in_use) {
            return NULL;
        }
        idx = (idx + 1) % SANDBOX_MAX_ENTRIES;
    } while (idx != start);

    return NULL;
}

/* 在表中查找空闲槽位（线性探测） */
static sandbox_entry_t *sandbox_alloc_entry(int pid)
{
    unsigned int start = sandbox_hash(pid);
    unsigned int idx = start;

    do {
        if (!sandbox_table[idx].in_use) {
            return &sandbox_table[idx];
        }
        idx = (idx + 1) % SANDBOX_MAX_ENTRIES;
    } while (idx != start);

    return NULL;
}

/* 检查路径是否以给定前缀开头 */
static int path_starts_with(const char *path, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) {
        return 0;
    }
    /* 匹配精确或后跟 '/' */
    if (path[plen] == '/' || path[plen] == '\0') {
        return 1;
    }
    return 0;
}

/* ========================================================================
 * sandbox_init - Initialize the sandbox subsystem
 * ======================================================================== */

void sandbox_init(void)
{
    memset(sandbox_table, 0, sizeof(sandbox_table));
    printf("[sandbox] Sandbox subsystem initialized\n");
}

/* ========================================================================
 * sandbox_create_policy - Create a sandbox policy for an SFK app
 * ======================================================================== */

void sandbox_create_policy(const char *pkg_id, uint32_t granted_perms,
                           sandbox_policy_t *policy)
{
    memset(policy, 0, sizeof(*policy));

    policy->type = SANDBOX_LIGHT;
    policy->granted_perms = granted_perms;

    /* 根据授予的权限配置允许的路径 */
    if (granted_perms & SFK_PERM_FILE_READ) {
        strncpy(policy->allowed_paths[policy->n_allowed_paths],
                "/home", sizeof(policy->allowed_paths[0]) - 1);
        policy->n_allowed_paths++;
    }

    if (granted_perms & SFK_PERM_FILE_WRITE) {
        /* Avoid duplicating /home if both read and write are granted */
        if (!(granted_perms & SFK_PERM_FILE_READ)) {
            strncpy(policy->allowed_paths[policy->n_allowed_paths],
                    "/home", sizeof(policy->allowed_paths[0]) - 1);
            policy->n_allowed_paths++;
        }
    }

    /* App's own directory is always allowed */
    if (policy->n_allowed_paths < 8) {
        snprintf_local(policy->allowed_paths[policy->n_allowed_paths],
                       sizeof(policy->allowed_paths[0]),
                       "/opt/sfk/%s", pkg_id);
        policy->n_allowed_paths++;
    }

    /* 所有沙箱应用均可读取共享库 */
    if (policy->n_allowed_paths < 8) {
        strncpy(policy->allowed_paths[policy->n_allowed_paths],
                "/usr", sizeof(policy->allowed_paths[0]) - 1);
        policy->n_allowed_paths++;
    }
    if (policy->n_allowed_paths < 8) {
        strncpy(policy->allowed_paths[policy->n_allowed_paths],
                "/lib", sizeof(policy->allowed_paths[0]) - 1);
        policy->n_allowed_paths++;
    }

    /* Default: deny access to system directories */
    strncpy(policy->denied_paths[0], "/etc", sizeof(policy->denied_paths[0]) - 1);
    strncpy(policy->denied_paths[1], "/boot", sizeof(policy->denied_paths[0]) - 1);
    strncpy(policy->denied_paths[2], "/kernel", sizeof(policy->denied_paths[0]) - 1);
    policy->n_denied_paths = 3;

    /* Configure allowed devices based on permissions */
    if (granted_perms & SFK_PERM_AUDIO) {
        strncpy(policy->allowed_devices[policy->n_allowed_devices],
                "/dev/audio", sizeof(policy->allowed_devices[0]) - 1);
        policy->n_allowed_devices++;

        if (policy->n_allowed_devices < 8) {
            strncpy(policy->allowed_devices[policy->n_allowed_devices],
                    "/dev/dsp", sizeof(policy->allowed_devices[0]) - 1);
            policy->n_allowed_devices++;
        }
    }

    if (granted_perms & SFK_PERM_DEVICE_ACCESS) {
        /* Allow /dev devices broadly */
        strncpy(policy->allowed_devices[policy->n_allowed_devices],
                "/dev", sizeof(policy->allowed_devices[0]) - 1);
        policy->n_allowed_devices++;
    }

    /* 网络权限 */
    if (granted_perms & SFK_PERM_NETWORK) {
        /* 默认无特定主机限制 - 允许所有 */
    }
}

/* ========================================================================
 * sandbox_apply - Apply a sandbox policy to a process
 * ======================================================================== */

int sandbox_apply(process_t *proc, sandbox_policy_t *policy)
{
    sandbox_entry_t *entry;

    if (!proc || !policy) {
        return -1;
    }

    /* 检查此 PID 是否已有沙箱条目 */
    entry = sandbox_find_entry(proc->pid);
    if (entry) {
        /* Update existing entry */
        memcpy(&entry->policy, policy, sizeof(sandbox_policy_t));
    } else {
        /* 分配新条目 */
        entry = sandbox_alloc_entry(proc->pid);
        if (!entry) {
            printf("[sandbox] No free sandbox slots for PID %d\n", proc->pid);
            return -1;
        }
        entry->in_use = 1;
        entry->pid = proc->pid;
        strncpy(entry->pkg_id, proc->sfk_pkg_id, sizeof(entry->pkg_id) - 1);
        memcpy(&entry->policy, policy, sizeof(sandbox_policy_t));
    }

    /* 设置进程 SFK 标志和权限 */
    proc->sfk_perms = policy->granted_perms;
    proc->flags |= PROC_FLAG_SFK;

    return 0;
}

/* ========================================================================
 * sandbox_check - 检查进程是否被允许执行某操作
 * ======================================================================== */

int sandbox_check(int pid, int action, const void *target)
{
    sandbox_entry_t *entry;

    entry = sandbox_find_entry(pid);
    if (!entry) {
        /* No sandbox policy - allow everything */
        return 1;
    }

    if (entry->policy.type == SANDBOX_NONE) {
        return 1;
    }

    if (entry->policy.type == SANDBOX_LIGHT) {
        /* Dispatch to specific check based on action type */
        switch (action) {
        case SANDBOX_VIOLATION_FILE_READ:
            return sandbox_check_file_access(pid, (const char *)target, 0);
        case SANDBOX_VIOLATION_FILE_WRITE:
            return sandbox_check_file_access(pid, (const char *)target, 1);
        case SANDBOX_VIOLATION_NETWORK:
            return sandbox_check_network(pid, (const char *)target, 0);
        case SANDBOX_VIOLATION_DEVICE:
            return sandbox_check_device(pid, (const char *)target);
        case SANDBOX_VIOLATION_SIGNAL:
            return sandbox_check_signal(pid, 0, *(const int *)target);
        default:
            return 0;
        }
    }

    return 0;
}

/* ========================================================================
 * sandbox_check_file_access - 检查文件访问权限
 *
 * access_type: 0 = 读, 1 = 写
 * ======================================================================== */

int sandbox_check_file_access(int pid, const char *path, int access_type)
{
    sandbox_entry_t *entry;
    int i;

    entry = sandbox_find_entry(pid);
    if (!entry || entry->policy.type == SANDBOX_NONE) {
        return 1;
    }

    /* 始终拒绝沙箱应用访问系统目录 */
    for (i = 0; i < entry->policy.n_denied_paths; i++) {
        if (path_starts_with(path, entry->policy.denied_paths[i])) {
            sandbox_log_violation(pid, access_type == 0 ?
                                  SANDBOX_VIOLATION_FILE_READ :
                                  SANDBOX_VIOLATION_FILE_WRITE,
                                  path);
            return 0;
        }
    }

    /* 检查特定的 /etc、/boot、/kernel 拒绝规则（硬编码安全网） */
    if (path_starts_with(path, "/etc") ||
        path_starts_with(path, "/boot") ||
        path_starts_with(path, "/kernel")) {
        sandbox_log_violation(pid, access_type == 0 ?
                              SANDBOX_VIOLATION_FILE_READ :
                              SANDBOX_VIOLATION_FILE_WRITE,
                              path);
        return 0;
    }

    /* 允许访问应用自身目录：/opt/sfk/<pkg_id>/ */
    char own_dir[256];
    snprintf_local(own_dir, sizeof(own_dir), "/opt/sfk/%s", entry->pkg_id);
    if (path_starts_with(path, own_dir)) {
        return 1;
    }

    /* Allow read access to /usr and /lib (shared libraries) */
    if (access_type == 0) {
        if (path_starts_with(path, "/usr") || path_starts_with(path, "/lib")) {
            if (entry->policy.granted_perms & SFK_PERM_FILE_READ) {
                return 1;
            }
        }
    }

    /* 检查写入权限 */
    if (access_type == 1) {
        if (!(entry->policy.granted_perms & SFK_PERM_FILE_WRITE)) {
            sandbox_log_violation(pid, SANDBOX_VIOLATION_FILE_WRITE, path);
            return 0;
        }
    }

    /* 检查读取权限 */
    if (access_type == 0) {
        if (!(entry->policy.granted_perms & SFK_PERM_FILE_READ)) {
            sandbox_log_violation(pid, SANDBOX_VIOLATION_FILE_READ, path);
            return 0;
        }
    }

    /* 检查允许路径列表 */
    for (i = 0; i < entry->policy.n_allowed_paths; i++) {
        if (path_starts_with(path, entry->policy.allowed_paths[i])) {
            return 1;
        }
    }

    /* Default: deny */
    sandbox_log_violation(pid, access_type == 0 ?
                          SANDBOX_VIOLATION_FILE_READ :
                          SANDBOX_VIOLATION_FILE_WRITE,
                          path);
    return 0;
}

/* ========================================================================
 * sandbox_check_network - 检查网络访问权限
 * ======================================================================== */

int sandbox_check_network(int pid, const char *host, uint16_t port)
{
    sandbox_entry_t *entry;
    int i;

    entry = sandbox_find_entry(pid);
    if (!entry || entry->policy.type == SANDBOX_NONE) {
        return 1;
    }

    if (!(entry->policy.granted_perms & SFK_PERM_NETWORK)) {
        char detail[128];
        snprintf_local(detail, sizeof(detail), "host=%s port=%u", host, port);
        sandbox_log_violation(pid, SANDBOX_VIOLATION_NETWORK, detail);
        return 0;
    }

    /* If allowed_net_hosts is configured, check against the list */
    if (entry->policy.n_allowed_net_hosts > 0) {
        for (i = 0; i < entry->policy.n_allowed_net_hosts; i++) {
            if (strcmp(host, entry->policy.allowed_net_hosts[i]) == 0) {
                return 1;
            }
        }
        char detail[128];
        snprintf_local(detail, sizeof(detail), "host=%s not in allowed list", host);
        sandbox_log_violation(pid, SANDBOX_VIOLATION_NETWORK, detail);
        return 0;
    }

    /* No host restrictions - allow all */
    return 1;
}

/* ========================================================================
 * sandbox_check_device - 检查设备访问权限
 * ======================================================================== */

int sandbox_check_device(int pid, const char *device)
{
    sandbox_entry_t *entry;
    int i;

    entry = sandbox_find_entry(pid);
    if (!entry || entry->policy.type == SANDBOX_NONE) {
        return 1;
    }

    /* 通过 SFK_PERM_AUDIO 检查音频设备 */
    if (strcmp(device, "/dev/audio") == 0 || strcmp(device, "/dev/dsp") == 0) {
        if (entry->policy.granted_perms & SFK_PERM_AUDIO) {
            return 1;
        }
        sandbox_log_violation(pid, SANDBOX_VIOLATION_DEVICE, device);
        return 0;
    }

    /* 通过 SFK_PERM_DEVICE_ACCESS 检查通用设备访问 */
    if (entry->policy.granted_perms & SFK_PERM_DEVICE_ACCESS) {
        /* 检查允许的设备列表 */
        if (entry->policy.n_allowed_devices > 0) {
            for (i = 0; i < entry->policy.n_allowed_devices; i++) {
                if (path_starts_with(device, entry->policy.allowed_devices[i])) {
                    return 1;
                }
            }
            sandbox_log_violation(pid, SANDBOX_VIOLATION_DEVICE, device);
            return 0;
        }
        return 1;
    }

    sandbox_log_violation(pid, SANDBOX_VIOLATION_DEVICE, device);
    return 0;
}

/* ========================================================================
 * sandbox_check_signal - 检查信号权限
 * ======================================================================== */

int sandbox_check_signal(int pid, int target_pid, int sig)
{
    sandbox_entry_t *entry;

    entry = sandbox_find_entry(pid);
    if (!entry || entry->policy.type == SANDBOX_NONE) {
        return 1;
    }

    /* SIGKILL 需要 SFK_PERM_PROCESS_KILL 权限 */
    if (sig == 9) { /* SIGKILL */
        if (!(entry->policy.granted_perms & SFK_PERM_PROCESS_KILL)) {
            char detail[64];
            snprintf_local(detail, sizeof(detail),
                           "target_pid=%d sig=%d", target_pid, sig);
            sandbox_log_violation(pid, SANDBOX_VIOLATION_SIGNAL, detail);
            return 0;
        }
    }

    return 1;
}

/* ========================================================================
 * sandbox_log_violation - 记录沙箱违规
 * ======================================================================== */

void sandbox_log_violation(int pid, int violation_type, const char *detail)
{
    const char *type_str;

    switch (violation_type) {
    case SANDBOX_VIOLATION_NETWORK:
        type_str = "NETWORK";
        break;
    case SANDBOX_VIOLATION_FILE_READ:
        type_str = "FILE_READ";
        break;
    case SANDBOX_VIOLATION_FILE_WRITE:
        type_str = "FILE_WRITE";
        break;
    case SANDBOX_VIOLATION_DEVICE:
        type_str = "DEVICE";
        break;
    case SANDBOX_VIOLATION_SIGNAL:
        type_str = "SIGNAL";
        break;
    case SANDBOX_VIOLATION_PROCESS:
        type_str = "PROCESS";
        break;
    case SANDBOX_VIOLATION_ADMIN:
        type_str = "ADMIN";
        break;
    default:
        type_str = "UNKNOWN";
        break;
    }

    printf("[sandbox] VIOLATION: pid=%d type=%s detail='%s'\n",
           pid, type_str, detail ? detail : "");
}

/* ========================================================================
 * sandbox_get_policy - 获取进程的沙箱策略
 * ======================================================================== */

sandbox_policy_t *sandbox_get_policy(int pid)
{
    sandbox_entry_t *entry;

    entry = sandbox_find_entry(pid);
    if (!entry) {
        return NULL;
    }

    return &entry->policy;
}

/* ========================================================================
 * sandbox_remove - 移除进程的沙箱
 * ======================================================================== */

void sandbox_remove(int pid)
{
    sandbox_entry_t *entry;

    entry = sandbox_find_entry(pid);
    if (entry) {
        entry->in_use = 0;
        entry->pid = 0;
        memset(&entry->policy, 0, sizeof(sandbox_policy_t));
        entry->pkg_id[0] = '\0';
    }
}

/* ========================================================================
 * sandbox_dump - 转储所有活跃沙箱
 * ======================================================================== */

void sandbox_dump(void)
{
    int i;
    int active = 0;

    printf("[sandbox] === Sandbox Table Dump ===\n");

    for (i = 0; i < SANDBOX_MAX_ENTRIES; i++) {
        if (!sandbox_table[i].in_use) {
            continue;
        }

        active++;
        sandbox_entry_t *e = &sandbox_table[i];
        printf("  [%d] pid=%d pkg_id=%s type=%s perms=0x%x\n",
               i, e->pid, e->pkg_id,
               e->policy.type == SANDBOX_NONE ? "NONE" :
               e->policy.type == SANDBOX_LIGHT ? "LIGHT" :
               e->policy.type == SANDBOX_STRICT ? "STRICT" : "???",
               e->policy.granted_perms);

        printf("    allowed_paths(%d):", e->policy.n_allowed_paths);
        for (int j = 0; j < e->policy.n_allowed_paths; j++) {
            printf(" %s", e->policy.allowed_paths[j]);
        }
        printf("\n");

        printf("    denied_paths(%d):", e->policy.n_denied_paths);
        for (int j = 0; j < e->policy.n_denied_paths; j++) {
            printf(" %s", e->policy.denied_paths[j]);
        }
        printf("\n");

        printf("    allowed_devices(%d):", e->policy.n_allowed_devices);
        for (int j = 0; j < e->policy.n_allowed_devices; j++) {
            printf(" %s", e->policy.allowed_devices[j]);
        }
        printf("\n");

        printf("    allowed_net_hosts(%d):", e->policy.n_allowed_net_hosts);
        for (int j = 0; j < e->policy.n_allowed_net_hosts; j++) {
            printf(" %s", e->policy.allowed_net_hosts[j]);
        }
        printf("\n");
    }

    printf("[sandbox] Active sandboxes: %d / %d\n", active, SANDBOX_MAX_ENTRIES);
}
