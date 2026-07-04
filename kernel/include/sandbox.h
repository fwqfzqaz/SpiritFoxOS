#ifndef SANDBOX_H
#define SANDBOX_H

#include <stdint.h>
#include "sfk_perms.h"
#include "process.h"

/* ========================================================================
 * Sandbox types
 * ======================================================================== */

#define SANDBOX_NONE      0   /* No sandbox (DEB/native apps) */
#define SANDBOX_LIGHT     1   /* Lightweight permission-based sandbox (SFK apps) */
#define SANDBOX_STRICT    2   /* Strict isolation (future) */

/* ========================================================================
 * Sandbox violation types
 * ======================================================================== */

#define SANDBOX_VIOLATION_NETWORK      1
#define SANDBOX_VIOLATION_FILE_READ    2
#define SANDBOX_VIOLATION_FILE_WRITE   3
#define SANDBOX_VIOLATION_DEVICE       4
#define SANDBOX_VIOLATION_SIGNAL       5
#define SANDBOX_VIOLATION_PROCESS      6
#define SANDBOX_VIOLATION_ADMIN        7

/* ========================================================================
 * Sandbox policy
 * ======================================================================== */

typedef struct {
    uint32_t type;                 /* SANDBOX_* type */
    uint32_t granted_perms;        /* SFK_PERM_* bitmask */
    char     allowed_paths[8][256]; /* Allowed file paths (for file access) */
    int      n_allowed_paths;
    char     denied_paths[8][256];  /* Explicitly denied paths */
    int      n_denied_paths;
    char     allowed_devices[8][64];/* Allowed device paths */
    int      n_allowed_devices;
    char     allowed_net_hosts[8][64];/* Allowed network hosts */
    int      n_allowed_net_hosts;
    uint32_t max_file_size;         /* Max single file size */
    uint32_t max_total_size;        /* Max total file usage */
} sandbox_policy_t;

/* ========================================================================
 * Sandbox API
 * ======================================================================== */

/* Initialize the sandbox subsystem */
void sandbox_init(void);

/* Create a sandbox policy for an SFK app based on its granted permissions */
void sandbox_create_policy(const char *pkg_id, uint32_t granted_perms,
                           sandbox_policy_t *policy);

/* Apply a sandbox policy to a process */
int sandbox_apply(process_t *proc, sandbox_policy_t *policy);

/* Check if a process is allowed to perform an action */
int sandbox_check(int pid, int action, const void *target);

/* Check file access permission */
int sandbox_check_file_access(int pid, const char *path, int access_type);

/* Check network access permission */
int sandbox_check_network(int pid, const char *host, uint16_t port);

/* Check device access permission */
int sandbox_check_device(int pid, const char *device);

/* Check signal permission */
int sandbox_check_signal(int pid, int target_pid, int sig);

/* Log a sandbox violation */
void sandbox_log_violation(int pid, int violation_type, const char *detail);

/* Get sandbox policy for a process */
sandbox_policy_t *sandbox_get_policy(int pid);

/* Remove sandbox for a process (on exit) */
void sandbox_remove(int pid);

/* Dump sandbox status for debugging */
void sandbox_dump(void);

#endif /* SANDBOX_H */
