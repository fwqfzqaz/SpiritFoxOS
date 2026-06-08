#include "perm.h"
#include "crypto.h"
#include "../include/string.h"

static perm_app_entry_t app_registry[PERM_MAX_APPS];
static uint64_t next_app_id = 1;

static const char *perm_names[] = {
    "FILE_READ",
    "FILE_WRITE",
    "FILE_EXEC",
    "NET_SEND",
    "NET_RECV",
    "DEV_ACCESS",
    "SYS_INFO",
    "PROC_MGMT",
    "MEM_MGMT",
    "GUI_RENDER",
    "INPUT_READ",
    "AUDIO_PLAY"
};

#define PERM_NAME_COUNT (sizeof(perm_names) / sizeof(perm_names[0]))

void perm_init(void) {
    for (int i = 0; i < PERM_MAX_APPS; i++) {
        memset(&app_registry[i], 0, sizeof(perm_app_entry_t));
        app_registry[i].app_id = 0;
        app_registry[i].installed = 0;
        app_registry[i].active = 0;
    }
    next_app_id = 1;
    crypto_init();
}

static perm_app_entry_t *find_free_slot(void) {
    for (int i = 0; i < PERM_MAX_APPS; i++) {
        if (!app_registry[i].installed) return &app_registry[i];
    }
    return NULL;
}

uint64_t perm_register_system_app(const char *name, uint32_t perms) {
    perm_app_entry_t *slot = find_free_slot();
    if (!slot) return 0;

    slot->app_id = next_app_id++;
    strncpy(slot->name, name, PERM_NAME_LEN - 1);
    slot->name[PERM_NAME_LEN - 1] = '\0';
    slot->type = APP_SYSTEM;
    slot->requested_perms = perms;
    slot->granted_perms = perms;
    memset(&slot->public_key, 0, sizeof(perm_key_t));
    memset(&slot->private_key, 0, sizeof(perm_key_t));
    slot->active = 0;
    slot->installed = 1;

    return slot->app_id;
}

uint64_t perm_install_app(const char *name, uint32_t requested_perms,
                          perm_key_t *out_public_key) {
    perm_app_entry_t *slot = find_free_slot();
    if (!slot) return 0;

    slot->app_id = next_app_id++;
    strncpy(slot->name, name, PERM_NAME_LEN - 1);
    slot->name[PERM_NAME_LEN - 1] = '\0';
    slot->type = APP_INSTALLED;
    slot->requested_perms = requested_perms;
    slot->granted_perms = 0;

    crypto_generate_keypair(&slot->private_key, &slot->public_key);

    if (out_public_key) {
        memcpy(out_public_key, &slot->public_key, sizeof(perm_key_t));
    }

    slot->active = 0;
    slot->installed = 1;

    return slot->app_id;
}

int perm_uninstall_app(uint64_t app_id) {
    perm_app_entry_t *app = perm_find_app(app_id);
    if (!app || !app->installed) return -1;
    if (app->active) return -2;

    memset(app, 0, sizeof(perm_app_entry_t));
    return 0;
}

int perm_app_start(uint64_t app_id, const perm_signature_t *signature,
                   perm_session_t *out_session) {
    perm_app_entry_t *app = perm_find_app(app_id);
    if (!app || !app->installed) return -1;
    if (app->active) return -2;

    if (app->type == APP_INSTALLED) {
        if (!signature) return -3;

        perm_signature_t expected;
        crypto_sign(&app->private_key, app_id, &expected);

        if (memcmp(signature->data, expected.data, PERM_SIGNATURE_SIZE) != 0) {
            return -4;
        }

        app->granted_perms = app->requested_perms;
    }

    app->active = 1;

    if (out_session) {
        out_session->app_id = app_id;
        out_session->granted_perms = app->granted_perms;
        crypto_sign(&app->private_key, app_id, &out_session->auth_token);
    }

    return 0;
}

int perm_app_stop(uint64_t app_id) {
    perm_app_entry_t *app = perm_find_app(app_id);
    if (!app || !app->installed) return -1;
    if (!app->active) return -2;

    if (app->type == APP_INSTALLED) {
        app->granted_perms = 0;
    }

    app->active = 0;
    return 0;
}

int perm_check(uint64_t app_id, perm_flag_t perm) {
    perm_app_entry_t *app = perm_find_app(app_id);
    if (!app || !app->installed || !app->active) return 0;
    return (app->granted_perms & (uint32_t)perm) != 0;
}

int perm_check_session(const perm_session_t *session, perm_flag_t perm) {
    if (!session) return 0;
    perm_app_entry_t *app = perm_find_app(session->app_id);
    if (!app || !app->active) return 0;

    perm_signature_t expected;
    crypto_sign(&app->private_key, session->app_id, &expected);
    if (memcmp(session->auth_token.data, expected.data, PERM_SIGNATURE_SIZE) != 0) {
        return 0;
    }

    return (session->granted_perms & (uint32_t)perm) != 0;
}

perm_app_entry_t *perm_find_app(uint64_t app_id) {
    for (int i = 0; i < PERM_MAX_APPS; i++) {
        if (app_registry[i].installed && app_registry[i].app_id == app_id) {
            return &app_registry[i];
        }
    }
    return NULL;
}

perm_app_entry_t *perm_find_app_by_name(const char *name) {
    for (int i = 0; i < PERM_MAX_APPS; i++) {
        if (app_registry[i].installed && strcmp(app_registry[i].name, name) == 0) {
            return &app_registry[i];
        }
    }
    return NULL;
}

uint32_t perm_get_granted(uint64_t app_id) {
    perm_app_entry_t *app = perm_find_app(app_id);
    if (!app) return 0;
    return app->granted_perms;
}

const char *perm_name(perm_flag_t perm) {
    for (uint32_t i = 0; i < PERM_NAME_COUNT; i++) {
        if ((uint32_t)perm == (1u << i)) return perm_names[i];
    }
    return "UNKNOWN";
}

void perm_list_apps(void) {
    (void)perm_names;
    for (int i = 0; i < PERM_MAX_APPS; i++) {
        if (!app_registry[i].installed) continue;
        perm_app_entry_t *a = &app_registry[i];
        (void)a;
    }
}
