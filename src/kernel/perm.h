#ifndef PERM_H
#define PERM_H

#include <stdint.h>

#define PERM_MAX_APPS        64
#define PERM_NAME_LEN        32
#define PERM_KEY_SIZE        32
#define PERM_SIGNATURE_SIZE  32

typedef enum {
    PERM_FILE_READ    = (1 << 0),
    PERM_FILE_WRITE   = (1 << 1),
    PERM_FILE_EXEC    = (1 << 2),
    PERM_NET_SEND     = (1 << 3),
    PERM_NET_RECV     = (1 << 4),
    PERM_DEV_ACCESS   = (1 << 5),
    PERM_SYS_INFO     = (1 << 6),
    PERM_PROCESS_MGMT = (1 << 7),
    PERM_MEMORY_MGMT  = (1 << 8),
    PERM_GUI_RENDER   = (1 << 9),
    PERM_INPUT_READ   = (1 << 10),
    PERM_AUDIO_PLAY   = (1 << 11),
    PERM_ALL          = 0xFFFFFFFF
} perm_flag_t;

typedef enum {
    APP_SYSTEM = 0,
    APP_INSTALLED
} app_type_t;

typedef struct {
    uint8_t data[PERM_KEY_SIZE];
} perm_key_t;

typedef struct {
    uint8_t data[PERM_SIGNATURE_SIZE];
} perm_signature_t;

typedef struct {
    uint64_t app_id;
    char name[PERM_NAME_LEN];
    app_type_t type;
    uint32_t requested_perms;
    uint32_t granted_perms;
    perm_key_t public_key;
    perm_key_t private_key;
    int active;
    int installed;
} perm_app_entry_t;

typedef struct {
    uint64_t app_id;
    uint32_t granted_perms;
    perm_signature_t auth_token;
} perm_session_t;

void perm_init(void);

uint64_t perm_install_app(const char *name, uint32_t requested_perms,
                          perm_key_t *out_public_key);

int perm_uninstall_app(uint64_t app_id);

int perm_app_start(uint64_t app_id, const perm_signature_t *signature,
                   perm_session_t *out_session);

int perm_app_stop(uint64_t app_id);

int perm_check(uint64_t app_id, perm_flag_t perm);

int perm_check_session(const perm_session_t *session, perm_flag_t perm);

perm_app_entry_t *perm_find_app(uint64_t app_id);

perm_app_entry_t *perm_find_app_by_name(const char *name);

uint32_t perm_get_granted(uint64_t app_id);

const char *perm_name(perm_flag_t perm);

void perm_list_apps(void);

uint64_t perm_register_system_app(const char *name, uint32_t perms);

#endif
