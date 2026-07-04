#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdint.h>
#include <stddef.h>

/* ========================================================================
 * Registry root keys
 * ======================================================================== */

#define REG_ROOT_SYSTEM    "HKEY_SYSTEM"    /* System global config + software install info */
#define REG_ROOT_USER      "HKEY_USER"      /* Current user config + preferences */
#define REG_ROOT_FILEASSOC "HKEY_FILEASSOC" /* File association + default apps */

/* ========================================================================
 * Registry value types
 * ======================================================================== */

#define REG_TYPE_NONE      0
#define REG_TYPE_STRING    1   /* Null-terminated string */
#define REG_TYPE_UINT32    2   /* 32-bit unsigned integer */
#define REG_TYPE_UINT64    3   /* 64-bit unsigned integer */
#define REG_TYPE_BINARY    4   /* Arbitrary binary data */
#define REG_TYPE_BOOL      5   /* Boolean (0 or 1) */
#define REG_TYPE_MULTI_SZ  6   /* Multiple null-terminated strings */

/* ========================================================================
 * Registry key flags
 * ======================================================================== */

#define REG_FLAG_PERSIST    0x01   /* Persist to disk */
#define REG_FLAG_READONLY   0x02   /* Read-only key */
#define REG_FLAG_ADMIN_ONLY 0x04   /* Admin write only */

/* ========================================================================
 * Limits
 * ======================================================================== */

#define REG_MAX_KEY_NAME    128
#define REG_MAX_VALUE_NAME  128
#define REG_MAX_VALUE_DATA  4096
#define REG_MAX_PATH_DEPTH  16
#define REG_MAX_KEYS        512
#define REG_MAX_VALUES      2048

/* ========================================================================
 * Registry value entry
 * ======================================================================== */

typedef struct reg_value {
    char     name[REG_MAX_VALUE_NAME];
    uint32_t type;
    uint32_t flags;
    uint32_t data_size;
    uint8_t  data[REG_MAX_VALUE_DATA];
} reg_value_t;

/* ========================================================================
 * Registry key
 * ======================================================================== */

typedef struct reg_key {
    char            name[REG_MAX_KEY_NAME];
    uint32_t        flags;
    uint32_t        nvalues;
    uint32_t        nsubkeys;
    struct reg_key *parent;
    struct reg_key *child;         /* First child key */
    struct reg_key *next;          /* Next sibling key */
    reg_value_t    *values;        /* Array of values */
    uint32_t        values_capacity;
} reg_key_t;

/* ========================================================================
 * Transaction support
 * ======================================================================== */

typedef struct {
    uint64_t  id;
    uint64_t  start_time;
    int       active;
    int       nops;                /* Number of pending operations */
} reg_transaction_t;

/* ========================================================================
 * Software installation record (stored in HKEY_SYSTEM/Software/<pkg>)
 * ======================================================================== */

typedef struct {
    char     name[64];
    char     version[32];
    char     vendor[64];
    char     install_path[256];
    char     install_date[32];
    char     pkg_format[8];        /* "deb" or "sfk" */
    char     pkg_id[64];           /* Package identifier */
    uint64_t install_size;
    uint64_t file_count;
    uint32_t sfk_perms;            /* SFK permissions bitmask (0 for deb) */
    char     uninstall_cmd[256];
    char     dependencies[512];
} reg_software_record_t;

/* ========================================================================
 * File association record (stored in HKEY_FILEASSOC)
 * ======================================================================== */

typedef struct {
    char     extension[16];        /* e.g. ".txt" */
    char     mime_type[64];        /* e.g. "text/plain" */
    char     app_path[256];        /* Default app path */
    char     app_name[64];         /* App display name */
    char     icon_path[256];       /* Icon path */
    char     pkg_id[64];           /* Owning package */
} reg_fileassoc_t;

/* ========================================================================
 * Service record (stored in HKEY_SYSTEM/Services/<name>)
 * ======================================================================== */

typedef struct {
    char     name[64];
    char     description[128];
    char     exec_path[256];
    char     pkg_id[64];
    uint32_t type;                 /* Service type */
    uint32_t start_type;           /* Auto/manual/disabled */
    uint32_t status;               /* Running/stopped */
    uint32_t pid;                  /* PID if running */
} reg_service_t;

/* ========================================================================
 * Registry API
 * ======================================================================== */

/* Initialize the registry system */
void registry_init(void);

/* Open/create a key at the given path (e.g. "HKEY_SYSTEM/Software/MyApp") */
reg_key_t *registry_open_key(const char *path, int create_if_missing);

/* Close a key (decrements reference) */
void registry_close_key(reg_key_t *key);

/* Delete a key and all subkeys */
int registry_delete_key(const char *path);

/* Check if a key exists */
int registry_key_exists(const char *path);

/* Read a value from a key */
int registry_read_value(const char *key_path, const char *value_name,
                        uint32_t *type, void *data, uint32_t *data_size);

/* Write a value to a key */
int registry_write_value(const char *key_path, const char *value_name,
                         uint32_t type, const void *data, uint32_t data_size);

/* Delete a value from a key */
int registry_delete_value(const char *key_path, const char *value_name);

/* List subkeys of a key */
int registry_list_keys(const char *key_path, char names[][REG_MAX_KEY_NAME],
                       int max_entries);

/* List values of a key */
int registry_list_values(const char *key_path, char names[][REG_MAX_VALUE_NAME],
                         int max_entries);

/* Transaction support */
uint64_t registry_transaction_begin(void);
int registry_transaction_commit(uint64_t tx_id);
int registry_transaction_abort(uint64_t tx_id);

/* Persistence */
int registry_save(const char *path);
int registry_load(const char *path);

/* Software record helpers */
int registry_register_software(const reg_software_record_t *record);
int registry_unregister_software(const char *pkg_id);
int registry_get_software(const char *pkg_id, reg_software_record_t *record);
int registry_list_software(char pkg_ids[][REG_MAX_KEY_NAME], int max_entries);

/* File association helpers */
int registry_register_fileassoc(const reg_fileassoc_t *assoc);
int registry_unregister_fileassoc(const char *extension);
int registry_get_fileassoc(const char *extension, reg_fileassoc_t *assoc);

/* Service record helpers */
int registry_register_service(const reg_service_t *service);
int registry_unregister_service(const char *name);
int registry_get_service(const char *name, reg_service_t *service);
int registry_update_service_status(const char *name, uint32_t status, uint32_t pid);

/* Debug/dump */
void registry_dump(void);

#endif /* REGISTRY_H */
