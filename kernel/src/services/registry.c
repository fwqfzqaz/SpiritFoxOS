/*
 * SpiritFoxOS System Registry
 *
 * Tree-structured key-value store with three root keys:
 *   HKEY_SYSTEM   - System global config, software installation records
 *   HKEY_FILEASSOC - File associations
 *   HKEY_USER     - User preferences
 */

#include "registry.h"
#include "kmalloc.h"
#include "vfs.h"
#include "vga.h"
#include "string.h"
#include "memory.h"

/* ========================================================================
 * Internal state
 * ======================================================================== */

static reg_key_t *reg_roots[3];     /* HKEY_SYSTEM, HKEY_FILEASSOC, HKEY_USER */
static int        reg_initialized = 0;

/* Simple key cache for fast lookup */
#define REG_CACHE_SIZE 64
typedef struct {
    char      path[256];
    reg_key_t *key;
} reg_cache_entry_t;

static reg_cache_entry_t reg_cache[REG_CACHE_SIZE];
static uint32_t reg_cache_count = 0;

/* Transaction state */
static uint64_t reg_next_tx_id = 1;

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

/* Create a new key node */
static reg_key_t *create_key(const char *name, reg_key_t *parent)
{
    reg_key_t *key = (reg_key_t *)kcalloc(1, sizeof(reg_key_t));
    if (!key) return NULL;

    strncpy(key->name, name, REG_MAX_KEY_NAME - 1);
    key->name[REG_MAX_KEY_NAME - 1] = '\0';
    key->flags = 0;
    key->nvalues = 0;
    key->nsubkeys = 0;
    key->parent = parent;
    key->child = NULL;
    key->next = NULL;
    key->values = NULL;
    key->values_capacity = 0;

    /* Link into parent's child list */
    if (parent) {
        key->next = parent->child;
        parent->child = key;
        parent->nsubkeys++;
    }

    return key;
}

/* Find a child key by name in parent's child list */
static reg_key_t *find_child(reg_key_t *parent, const char *name)
{
    if (!parent || !name) return NULL;
    reg_key_t *cur = parent->child;
    while (cur) {
        if (strcmp(cur->name, name) == 0) return cur;
        cur = cur->next;
    }
    return NULL;
}

/* Parse the first component of a path separated by '/'.
 * Returns pointer to the rest of the path (after the '/'), or NULL if done.
 * Writes the component into `out`. */
static const char *path_next_component(const char *path, char *out, int out_size)
{
    if (!path || *path == '\0') return NULL;

    int i = 0;
    while (*path && *path != '/' && i < out_size - 1) {
        out[i++] = *path++;
    }
    out[i] = '\0';

    if (*path == '/') path++;  /* skip the slash */
    return (*path != '\0') ? path : NULL;
}

/* Find a root key by name (HKEY_SYSTEM, HKEY_FILEASSOC, HKEY_USER) */
static reg_key_t *find_root(const char *name)
{
    for (int i = 0; i < 3; i++) {
        if (reg_roots[i] && strcmp(reg_roots[i]->name, name) == 0)
            return reg_roots[i];
    }
    return NULL;
}

/* Walk a full path and return the key. If create_if_missing, create intermediates. */
static reg_key_t *walk_path(const char *path, int create_if_missing)
{
    if (!path || *path == '\0') return NULL;

    char comp[REG_MAX_KEY_NAME];
    const char *p = path;

    /* First component must be a root key */
    const char *next = path_next_component(p, comp, sizeof(comp));
    if (!next && comp[0] == '\0') return NULL;

    reg_key_t *cur = find_root(comp);
    if (!cur) return NULL;

    /* Walk remaining components */
    p = next;
    while (p) {
        next = path_next_component(p, comp, sizeof(comp));
        reg_key_t *child = find_child(cur, comp);
        if (!child) {
            if (!create_if_missing) return NULL;
            child = create_key(comp, cur);
            if (!child) return NULL;
        }
        cur = child;
        p = next;
    }

    return cur;
}

/* Build the full path string for a key (for caching / display) */
static void key_build_path(reg_key_t *key, char *buf, int buf_size)
{
    if (!key || !buf || buf_size <= 0) return;

    /* Collect names from root to this key */
    char *stack[REG_MAX_PATH_DEPTH];
    int depth = 0;

    reg_key_t *cur = key;
    while (cur) {
        stack[depth++] = cur->name;
        cur = cur->parent;
    }

    buf[0] = '\0';
    int pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        int len = strlen(stack[i]);
        if (pos + len + 1 >= buf_size) break;
        memcpy(buf + pos, stack[i], len);
        pos += len;
        if (i > 0) {
            buf[pos++] = '/';
        }
    }
    buf[pos] = '\0';
}

/* Cache lookup */
static reg_key_t *cache_lookup(const char *path)
{
    for (uint32_t i = 0; i < reg_cache_count; i++) {
        if (strcmp(reg_cache[i].path, path) == 0)
            return reg_cache[i].key;
    }
    return NULL;
}

/* Cache insert */
static void cache_insert(const char *path, reg_key_t *key)
{
    if (reg_cache_count >= REG_CACHE_SIZE) return;
    strncpy(reg_cache[reg_cache_count].path, path, sizeof(reg_cache[reg_cache_count].path) - 1);
    reg_cache[reg_cache_count].path[sizeof(reg_cache[reg_cache_count].path) - 1] = '\0';
    reg_cache[reg_cache_count].key = key;
    reg_cache_count++;
}

/* Cache invalidate - remove entries containing a prefix */
static void cache_invalidate_prefix(const char *prefix)
{
    int prefix_len = strlen(prefix);
    uint32_t write = 0;
    for (uint32_t i = 0; i < reg_cache_count; i++) {
        if (strncmp(reg_cache[i].path, prefix, prefix_len) != 0) {
            if (write != i) {
                reg_cache[write] = reg_cache[i];
            }
            write++;
        }
    }
    reg_cache_count = write;
}

/* Invalidate entire cache */
static void cache_invalidate_all(void)
{
    reg_cache_count = 0;
}

/* Unlink a key from its parent's child list (does not free) */
static void unlink_from_parent(reg_key_t *key)
{
    if (!key || !key->parent) return;

    reg_key_t *parent = key->parent;
    reg_key_t **pp = &parent->child;
    while (*pp) {
        if (*pp == key) {
            *pp = key->next;
            parent->nsubkeys--;
            break;
        }
        pp = &(*pp)->next;
    }
    key->parent = NULL;
    key->next = NULL;
}

/* Recursively free a key and all its subkeys and values */
static void free_key_recursive(reg_key_t *key)
{
    if (!key) return;

    /* Free all children first */
    while (key->child) {
        reg_key_t *child = key->child;
        key->child = child->next;
        free_key_recursive(child);
    }

    /* Free values array */
    if (key->values) {
        kfree(key->values);
        key->values = NULL;
    }

    kfree(key);
}

/* Find a value by name in a key */
static reg_value_t *find_value(reg_key_t *key, const char *name)
{
    if (!key || !name) return NULL;
    for (uint32_t i = 0; i < key->nvalues; i++) {
        if (strcmp(key->values[i].name, name) == 0)
            return &key->values[i];
    }
    return NULL;
}

/* Add or get a value slot in a key */
static reg_value_t *alloc_value(reg_key_t *key, const char *name)
{
    if (!key || !name) return NULL;

    /* Check if value already exists */
    reg_value_t *val = find_value(key, name);
    if (val) return val;

    /* Need to add a new value */
    if (key->nvalues >= key->values_capacity) {
        uint32_t new_cap = key->values_capacity == 0 ? 4 : key->values_capacity * 2;
        if (new_cap > REG_MAX_VALUES) new_cap = REG_MAX_VALUES;
        reg_value_t *new_arr = (reg_value_t *)kcalloc(new_cap, sizeof(reg_value_t));
        if (!new_arr) return NULL;
        if (key->values) {
            memcpy(new_arr, key->values, key->nvalues * sizeof(reg_value_t));
            kfree(key->values);
        }
        key->values = new_arr;
        key->values_capacity = new_cap;
    }

    val = &key->values[key->nvalues];
    memset(val, 0, sizeof(reg_value_t));
    strncpy(val->name, name, REG_MAX_VALUE_NAME - 1);
    key->nvalues++;

    return val;
}

/* Remove a value from a key by name */
static int remove_value(reg_key_t *key, const char *name)
{
    if (!key || !name) return -1;

    for (uint32_t i = 0; i < key->nvalues; i++) {
        if (strcmp(key->values[i].name, name) == 0) {
            /* Shift remaining values down */
            for (uint32_t j = i; j < key->nvalues - 1; j++) {
                key->values[j] = key->values[j + 1];
            }
            key->nvalues--;
            return 0;
        }
    }
    return -1;
}

/* Convert a byte to hex characters */
static void byte_to_hex(uint8_t b, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    out[0] = hex[(b >> 4) & 0x0F];
    out[1] = hex[b & 0x0F];
}

/* Convert hex character pair to byte. Returns -1 on error. */
static int hex_to_byte(const char *s, uint8_t *out)
{
    static const char hex[] = "0123456789ABCDEFabcdef";
    uint8_t hi, lo;

    /* High nibble */
    char *p = strchr(hex, s[0]);
    if (!p) return -1;
    hi = (uint8_t)(p - hex);
    if (hi >= 16) hi -= 6; /* handle lowercase */

    /* Low nibble */
    p = strchr(hex, s[1]);
    if (!p) return -1;
    lo = (uint8_t)(p - hex);
    if (lo >= 16) lo -= 6;

    *out = (hi << 4) | lo;
    return 0;
}

/* ========================================================================
 * Debug dump helpers
 * ======================================================================== */

static void dump_key(reg_key_t *key, int indent)
{
    if (!key) return;

    /* Print indentation + key name */
    for (int i = 0; i < indent; i++) printf("  ");
    printf("[%s] (%u values, %u subkeys)\n", key->name, key->nvalues, key->nsubkeys);

    /* Print values */
    for (uint32_t v = 0; v < key->nvalues; v++) {
        for (int i = 0; i < indent + 1; i++) printf("  ");
        reg_value_t *val = &key->values[v];
        printf("  \"%s\" type=%u size=%u =", val->name, val->type, val->data_size);
        uint32_t show = val->data_size > 32 ? 32 : val->data_size;
        for (uint32_t b = 0; b < show; b++) {
            printf(" %02X", val->data[b]);
        }
        if (val->data_size > 32) printf(" ...");
        printf("\n");
    }

    /* Recurse into children */
    reg_key_t *child = key->child;
    while (child) {
        dump_key(child, indent + 1);
        child = child->next;
    }
}

/* ========================================================================
 * Save/load helpers
 * ======================================================================== */

/* Simple string formatting for registry persistence.
 * Only supports %s, %u, %d, %x - enough for our needs. */
static int reg_sprintf(char *buf, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    char *out = buf;
    const char *p = fmt;
    while (*p) {
        if (*p == '%' && *(p+1)) {
            p++;
            if (*p == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                while (*s) *out++ = *s++;
            } else if (*p == 'u' || *p == 'd') {
                int v = __builtin_va_arg(ap, int);
                if (v < 0 && *p == 'd') { *out++ = '-'; v = -v; }
                char tmp[20]; int pos = 0;
                if (v == 0) tmp[pos++] = '0';
                else while (v > 0) { tmp[pos++] = '0' + (v % 10); v /= 10; }
                for (int i = pos - 1; i >= 0; i--) *out++ = tmp[i];
            } else if (*p == 'x') {
                unsigned int v = __builtin_va_arg(ap, unsigned int);
                const char *hex = "0123456789abcdef";
                char tmp[16]; int pos = 0;
                if (v == 0) tmp[pos++] = '0';
                else while (v > 0) { tmp[pos++] = hex[v & 0xf]; v >>= 4; }
                for (int i = pos - 1; i >= 0; i--) *out++ = tmp[i];
            } else { *out++ = '%'; *out++ = *p; }
            p++;
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    __builtin_va_end(ap);
    return (int)(out - buf);
}

static void save_key_recursive(reg_key_t *key, const char *parent_path, int fd)
{
    if (!key) return;

    /* Build current key path */
    char cur_path[256];
    if (parent_path[0] == '\0') {
        strncpy(cur_path, key->name, sizeof(cur_path) - 1);
    } else {
        int len = strlen(parent_path);
        memcpy(cur_path, parent_path, len);
        cur_path[len] = '/';
        strncpy(cur_path + len + 1, key->name, sizeof(cur_path) - len - 2);
        cur_path[sizeof(cur_path) - 1] = '\0';
    }

    /* Write KEY line */
    char line[512];
    int n = reg_sprintf(line, "KEY %s\n", cur_path);
    vfs_write(fd, line, n);

    /* Write VALUE lines */
    for (uint32_t v = 0; v < key->nvalues; v++) {
        reg_value_t *val = &key->values[v];
        n = reg_sprintf(line, "VALUE %s %s %u ", val->name, cur_path, val->type);
        /* Append hex data */
        for (uint32_t b = 0; b < val->data_size; b++) {
            byte_to_hex(val->data[b], line + n);
            n += 2;
        }
        line[n++] = '\n';
        line[n] = '\0';
        vfs_write(fd, line, n);
    }

    /* Recurse into children */
    reg_key_t *child = key->child;
    while (child) {
        save_key_recursive(child, cur_path, fd);
        child = child->next;
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void registry_init(void)
{
    if (reg_initialized) return;

    memset(reg_cache, 0, sizeof(reg_cache));
    reg_cache_count = 0;

    /* Create root keys (no parent - they are the roots) */
    reg_roots[0] = create_key(REG_ROOT_SYSTEM, NULL);
    reg_roots[1] = create_key(REG_ROOT_FILEASSOC, NULL);
    reg_roots[2] = create_key(REG_ROOT_USER, NULL);

    /* Create default subkey structure for HKEY_SYSTEM */
    create_key("Software", reg_roots[0]);
    create_key("Services", reg_roots[0]);
    create_key("Config", reg_roots[0]);

    /* HKEY_FILEASSOC is flat - extensions become subkeys, no defaults */

    /* Create default subkey structure for HKEY_USER */
    create_key("Preferences", reg_roots[2]);

    reg_initialized = 1;
}

reg_key_t *registry_open_key(const char *path, int create_if_missing)
{
    if (!path) return NULL;

    /* Check cache first */
    reg_key_t *cached = cache_lookup(path);
    if (cached) return cached;

    reg_key_t *key = walk_path(path, create_if_missing);
    if (key) {
        cache_insert(path, key);
    }
    return key;
}

void registry_close_key(reg_key_t *key)
{
    /* No-op for now - no reference counting */
    (void)key;
}

int registry_delete_key(const char *path)
{
    if (!path) return -1;

    reg_key_t *key = walk_path(path, 0);
    if (!key) return -1;

    /* Cannot delete root keys */
    if (!key->parent) return -1;

    /* Invalidate cache entries with this prefix */
    char prefix[256];
    strncpy(prefix, path, sizeof(prefix) - 1);
    prefix[sizeof(prefix) - 1] = '\0';
    int plen = strlen(prefix);
    if (plen + 1 < (int)sizeof(prefix)) {
        prefix[plen] = '/';
        prefix[plen + 1] = '\0';
    }
    cache_invalidate_prefix(prefix);
    cache_invalidate_prefix(path);

    /* Unlink from parent */
    unlink_from_parent(key);

    /* Recursively free */
    free_key_recursive(key);

    return 0;
}

int registry_key_exists(const char *path)
{
    if (!path) return 0;
    return walk_path(path, 0) ? 1 : 0;
}

int registry_read_value(const char *key_path, const char *value_name,
                        uint32_t *type, void *data, uint32_t *data_size)
{
    if (!key_path || !value_name) return -1;

    reg_key_t *key = walk_path(key_path, 0);
    if (!key) return -1;

    reg_value_t *val = find_value(key, value_name);
    if (!val) return -1;

    if (type) *type = val->type;

    if (data && data_size) {
        uint32_t copy_size = val->data_size;
        if (*data_size < copy_size) copy_size = *data_size;
        memcpy(data, val->data, copy_size);
        *data_size = val->data_size;
    } else if (data_size) {
        *data_size = val->data_size;
    }

    return 0;
}

int registry_write_value(const char *key_path, const char *value_name,
                         uint32_t type, const void *data, uint32_t data_size)
{
    if (!key_path || !value_name) return -1;

    reg_key_t *key = walk_path(key_path, 1);
    if (!key) return -1;

    reg_value_t *val = alloc_value(key, value_name);
    if (!val) return -1;

    val->type = type;
    if (data && data_size > 0) {
        uint32_t copy_size = data_size;
        if (copy_size > REG_MAX_VALUE_DATA) copy_size = REG_MAX_VALUE_DATA;
        memcpy(val->data, data, copy_size);
        val->data_size = copy_size;
    } else {
        val->data_size = data_size;
    }

    return 0;
}

int registry_delete_value(const char *key_path, const char *value_name)
{
    if (!key_path || !value_name) return -1;

    reg_key_t *key = walk_path(key_path, 0);
    if (!key) return -1;

    return remove_value(key, value_name);
}

int registry_list_keys(const char *key_path, char names[][REG_MAX_KEY_NAME],
                       int max_entries)
{
    if (!key_path || !names || max_entries <= 0) return -1;

    reg_key_t *key = walk_path(key_path, 0);
    if (!key) return -1;

    int count = 0;
    reg_key_t *child = key->child;
    while (child && count < max_entries) {
        strncpy(names[count], child->name, REG_MAX_KEY_NAME - 1);
        names[count][REG_MAX_KEY_NAME - 1] = '\0';
        count++;
        child = child->next;
    }
    return count;
}

int registry_list_values(const char *key_path, char names[][REG_MAX_VALUE_NAME],
                         int max_entries)
{
    if (!key_path || !names || max_entries <= 0) return -1;

    reg_key_t *key = walk_path(key_path, 0);
    if (!key) return -1;

    int count = 0;
    uint32_t n = key->nvalues;
    if (n > (uint32_t)max_entries) n = max_entries;
    for (uint32_t i = 0; i < n; i++) {
        strncpy(names[count], key->values[i].name, REG_MAX_VALUE_NAME - 1);
        names[count][REG_MAX_VALUE_NAME - 1] = '\0';
        count++;
    }
    return count;
}

/* ========================================================================
 * Transaction support (simplified)
 * ======================================================================== */

uint64_t registry_transaction_begin(void)
{
    return reg_next_tx_id++;
}

int registry_transaction_commit(uint64_t tx_id)
{
    /* All writes are immediate - no-op */
    (void)tx_id;
    return 0;
}

int registry_transaction_abort(uint64_t tx_id)
{
    /* No-op since all writes are immediate */
    (void)tx_id;
    return 0;
}

/* ========================================================================
 * Persistence
 * ======================================================================== */

int registry_save(const char *path)
{
    if (!path) return -1;

    int fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
    if (fd < 0) return -1;

    /* Write each root key and its subtree */
    for (int i = 0; i < 3; i++) {
        save_key_recursive(reg_roots[i], "", fd);
    }

    vfs_close(fd);
    return 0;
}

int registry_load(const char *path)
{
    if (!path) return -1;

    int fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd < 0) return -1;

    /* Read entire file into a buffer */
    char *buf = (char *)kmalloc(65536);
    if (!buf) {
        vfs_close(fd);
        return -1;
    }
    memset(buf, 0, 65536);

    int total = 0;
    int n;
    while ((n = vfs_read(fd, buf + total, 4096)) > 0) {
        total += n;
        if (total >= 65536 - 1) break;
    }
    buf[total] = '\0';
    vfs_close(fd);

    /* Re-initialize the registry (clear existing data) */
    /* First, free all existing keys */
    for (int i = 0; i < 3; i++) {
        if (reg_roots[i]) {
            /* Free children */
            while (reg_roots[i]->child) {
                reg_key_t *child = reg_roots[i]->child;
                reg_roots[i]->child = child->next;
                free_key_recursive(child);
            }
            /* Free values */
            if (reg_roots[i]->values) {
                kfree(reg_roots[i]->values);
                reg_roots[i]->values = NULL;
            }
            reg_roots[i]->nvalues = 0;
            reg_roots[i]->nsubkeys = 0;
            reg_roots[i]->values_capacity = 0;
        }
    }

    cache_invalidate_all();

    /* Parse the file line by line */
    char *line = buf;
    while (line && *line) {
        /* Find end of line */
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        int line_len = (int)(eol - line);
        char saved = *eol;
        *eol = '\0';

        if (strncmp(line, "KEY ", 4) == 0) {
            /* KEY <path> - ensure the key exists */
            char *key_path = line + 4;
            /* Trim trailing whitespace */
            char *end = key_path + strlen(key_path) - 1;
            while (end > key_path && (*end == ' ' || *end == '\r')) *end-- = '\0';

            walk_path(key_path, 1);
        } else if (strncmp(line, "VALUE ", 6) == 0) {
            /* VALUE <name> <key_path> <type> <hex_data> */
            char *p = line + 6;
            char vname[REG_MAX_VALUE_NAME];
            char kpath[256];
            uint32_t vtype;

            /* Parse value name */
            int i = 0;
            while (*p && *p != ' ' && i < REG_MAX_VALUE_NAME - 1)
                vname[i++] = *p++;
            vname[i] = '\0';
            if (*p == ' ') p++;

            /* Parse key path */
            i = 0;
            while (*p && *p != ' ' && i < 255)
                kpath[i++] = *p++;
            kpath[i] = '\0';
            if (*p == ' ') p++;

            /* Parse type */
            vtype = 0;
            while (*p && *p != ' ') {
                vtype = vtype * 10 + (*p - '0');
                p++;
            }
            if (*p == ' ') p++;

            /* Open the key */
            reg_key_t *key = walk_path(kpath, 1);
            if (key) {
                reg_value_t *val = alloc_value(key, vname);
                if (val) {
                    val->type = vtype;
                    /* Parse hex data */
                    int dpos = 0;
                    while (*p && *p != '\r' && *p != '\n' && dpos < (int)REG_MAX_VALUE_DATA) {
                        if (p[0] && p[1]) {
                            uint8_t b;
                            if (hex_to_byte(p, &b) == 0) {
                                val->data[dpos++] = b;
                            }
                            p += 2;
                            if (*p == ' ') p++;
                        } else {
                            break;
                        }
                    }
                    val->data_size = dpos;
                }
            }
        }

        /* Move to next line */
        *eol = saved;
        line = (*eol) ? eol + 1 : NULL;
    }

    kfree(buf);
    return 0;
}

/* ========================================================================
 * Software record helpers
 * ======================================================================== */

int registry_register_software(const reg_software_record_t *record)
{
    if (!record) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/Software/%s", REG_ROOT_SYSTEM, record->pkg_id);

    /* Create the key */
    reg_key_t *key = registry_open_key(key_path, 1);
    if (!key) return -1;

    /* Write all fields as individual values */
    registry_write_value(key_path, "name", REG_TYPE_STRING,
                         record->name, strlen(record->name) + 1);
    registry_write_value(key_path, "version", REG_TYPE_STRING,
                         record->version, strlen(record->version) + 1);
    registry_write_value(key_path, "vendor", REG_TYPE_STRING,
                         record->vendor, strlen(record->vendor) + 1);
    registry_write_value(key_path, "install_path", REG_TYPE_STRING,
                         record->install_path, strlen(record->install_path) + 1);
    registry_write_value(key_path, "install_date", REG_TYPE_STRING,
                         record->install_date, strlen(record->install_date) + 1);
    registry_write_value(key_path, "pkg_format", REG_TYPE_STRING,
                         record->pkg_format, strlen(record->pkg_format) + 1);
    registry_write_value(key_path, "pkg_id", REG_TYPE_STRING,
                         record->pkg_id, strlen(record->pkg_id) + 1);
    registry_write_value(key_path, "install_size", REG_TYPE_UINT64,
                         &record->install_size, sizeof(uint64_t));
    registry_write_value(key_path, "file_count", REG_TYPE_UINT64,
                         &record->file_count, sizeof(uint64_t));
    registry_write_value(key_path, "sfk_perms", REG_TYPE_UINT32,
                         &record->sfk_perms, sizeof(uint32_t));
    registry_write_value(key_path, "uninstall_cmd", REG_TYPE_STRING,
                         record->uninstall_cmd, strlen(record->uninstall_cmd) + 1);
    registry_write_value(key_path, "dependencies", REG_TYPE_STRING,
                         record->dependencies, strlen(record->dependencies) + 1);

    return 0;
}

int registry_unregister_software(const char *pkg_id)
{
    if (!pkg_id) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/Software/%s", REG_ROOT_SYSTEM, pkg_id);

    return registry_delete_key(key_path);
}

int registry_get_software(const char *pkg_id, reg_software_record_t *record)
{
    if (!pkg_id || !record) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/Software/%s", REG_ROOT_SYSTEM, pkg_id);

    if (!registry_key_exists(key_path)) return -1;

    memset(record, 0, sizeof(reg_software_record_t));

    uint32_t type;
    uint32_t dsize;

    /* Read string fields */
    dsize = sizeof(record->name);
    registry_read_value(key_path, "name", &type, record->name, &dsize);

    dsize = sizeof(record->version);
    registry_read_value(key_path, "version", &type, record->version, &dsize);

    dsize = sizeof(record->vendor);
    registry_read_value(key_path, "vendor", &type, record->vendor, &dsize);

    dsize = sizeof(record->install_path);
    registry_read_value(key_path, "install_path", &type, record->install_path, &dsize);

    dsize = sizeof(record->install_date);
    registry_read_value(key_path, "install_date", &type, record->install_date, &dsize);

    dsize = sizeof(record->pkg_format);
    registry_read_value(key_path, "pkg_format", &type, record->pkg_format, &dsize);

    dsize = sizeof(record->pkg_id);
    registry_read_value(key_path, "pkg_id", &type, record->pkg_id, &dsize);

    dsize = sizeof(record->install_size);
    registry_read_value(key_path, "install_size", &type, &record->install_size, &dsize);

    dsize = sizeof(record->file_count);
    registry_read_value(key_path, "file_count", &type, &record->file_count, &dsize);

    dsize = sizeof(record->sfk_perms);
    registry_read_value(key_path, "sfk_perms", &type, &record->sfk_perms, &dsize);

    dsize = sizeof(record->uninstall_cmd);
    registry_read_value(key_path, "uninstall_cmd", &type, record->uninstall_cmd, &dsize);

    dsize = sizeof(record->dependencies);
    registry_read_value(key_path, "dependencies", &type, record->dependencies, &dsize);

    return 0;
}

int registry_list_software(char pkg_ids[][REG_MAX_KEY_NAME], int max_entries)
{
    char key_path[128];
    reg_sprintf(key_path, "%s/Software", REG_ROOT_SYSTEM);
    return registry_list_keys(key_path, pkg_ids, max_entries);
}

/* ========================================================================
 * File association helpers
 * ======================================================================== */

int registry_register_fileassoc(const reg_fileassoc_t *assoc)
{
    if (!assoc) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/%s", REG_ROOT_FILEASSOC, assoc->extension);

    reg_key_t *key = registry_open_key(key_path, 1);
    if (!key) return -1;

    registry_write_value(key_path, "extension", REG_TYPE_STRING,
                         assoc->extension, strlen(assoc->extension) + 1);
    registry_write_value(key_path, "mime_type", REG_TYPE_STRING,
                         assoc->mime_type, strlen(assoc->mime_type) + 1);
    registry_write_value(key_path, "app_path", REG_TYPE_STRING,
                         assoc->app_path, strlen(assoc->app_path) + 1);
    registry_write_value(key_path, "app_name", REG_TYPE_STRING,
                         assoc->app_name, strlen(assoc->app_name) + 1);
    registry_write_value(key_path, "icon_path", REG_TYPE_STRING,
                         assoc->icon_path, strlen(assoc->icon_path) + 1);
    registry_write_value(key_path, "pkg_id", REG_TYPE_STRING,
                         assoc->pkg_id, strlen(assoc->pkg_id) + 1);

    return 0;
}

int registry_unregister_fileassoc(const char *extension)
{
    if (!extension) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/%s", REG_ROOT_FILEASSOC, extension);

    return registry_delete_key(key_path);
}

int registry_get_fileassoc(const char *extension, reg_fileassoc_t *assoc)
{
    if (!extension || !assoc) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/%s", REG_ROOT_FILEASSOC, extension);

    if (!registry_key_exists(key_path)) return -1;

    memset(assoc, 0, sizeof(reg_fileassoc_t));

    uint32_t type;
    uint32_t dsize;

    dsize = sizeof(assoc->extension);
    registry_read_value(key_path, "extension", &type, assoc->extension, &dsize);

    dsize = sizeof(assoc->mime_type);
    registry_read_value(key_path, "mime_type", &type, assoc->mime_type, &dsize);

    dsize = sizeof(assoc->app_path);
    registry_read_value(key_path, "app_path", &type, assoc->app_path, &dsize);

    dsize = sizeof(assoc->app_name);
    registry_read_value(key_path, "app_name", &type, assoc->app_name, &dsize);

    dsize = sizeof(assoc->icon_path);
    registry_read_value(key_path, "icon_path", &type, assoc->icon_path, &dsize);

    dsize = sizeof(assoc->pkg_id);
    registry_read_value(key_path, "pkg_id", &type, assoc->pkg_id, &dsize);

    return 0;
}

/* ========================================================================
 * Service record helpers
 * ======================================================================== */

int registry_register_service(const reg_service_t *service)
{
    if (!service) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/Services/%s", REG_ROOT_SYSTEM, service->name);

    reg_key_t *key = registry_open_key(key_path, 1);
    if (!key) return -1;

    registry_write_value(key_path, "name", REG_TYPE_STRING,
                         service->name, strlen(service->name) + 1);
    registry_write_value(key_path, "description", REG_TYPE_STRING,
                         service->description, strlen(service->description) + 1);
    registry_write_value(key_path, "exec_path", REG_TYPE_STRING,
                         service->exec_path, strlen(service->exec_path) + 1);
    registry_write_value(key_path, "pkg_id", REG_TYPE_STRING,
                         service->pkg_id, strlen(service->pkg_id) + 1);
    registry_write_value(key_path, "type", REG_TYPE_UINT32,
                         &service->type, sizeof(uint32_t));
    registry_write_value(key_path, "start_type", REG_TYPE_UINT32,
                         &service->start_type, sizeof(uint32_t));
    registry_write_value(key_path, "status", REG_TYPE_UINT32,
                         &service->status, sizeof(uint32_t));
    registry_write_value(key_path, "pid", REG_TYPE_UINT32,
                         &service->pid, sizeof(uint32_t));

    return 0;
}

int registry_unregister_service(const char *name)
{
    if (!name) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/Services/%s", REG_ROOT_SYSTEM, name);

    return registry_delete_key(key_path);
}

int registry_get_service(const char *name, reg_service_t *service)
{
    if (!name || !service) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/Services/%s", REG_ROOT_SYSTEM, name);

    if (!registry_key_exists(key_path)) return -1;

    memset(service, 0, sizeof(reg_service_t));

    uint32_t type;
    uint32_t dsize;

    dsize = sizeof(service->name);
    registry_read_value(key_path, "name", &type, service->name, &dsize);

    dsize = sizeof(service->description);
    registry_read_value(key_path, "description", &type, service->description, &dsize);

    dsize = sizeof(service->exec_path);
    registry_read_value(key_path, "exec_path", &type, service->exec_path, &dsize);

    dsize = sizeof(service->pkg_id);
    registry_read_value(key_path, "pkg_id", &type, service->pkg_id, &dsize);

    dsize = sizeof(service->type);
    registry_read_value(key_path, "type", &type, &service->type, &dsize);

    dsize = sizeof(service->start_type);
    registry_read_value(key_path, "start_type", &type, &service->start_type, &dsize);

    dsize = sizeof(service->status);
    registry_read_value(key_path, "status", &type, &service->status, &dsize);

    dsize = sizeof(service->pid);
    registry_read_value(key_path, "pid", &type, &service->pid, &dsize);

    return 0;
}

int registry_update_service_status(const char *name, uint32_t status, uint32_t pid)
{
    if (!name) return -1;

    char key_path[256];
    reg_sprintf(key_path, "%s/Services/%s", REG_ROOT_SYSTEM, name);

    if (!registry_key_exists(key_path)) return -1;

    int ret = 0;
    if (registry_write_value(key_path, "status", REG_TYPE_UINT32,
                             &status, sizeof(uint32_t)) != 0)
        ret = -1;
    if (registry_write_value(key_path, "pid", REG_TYPE_UINT32,
                             &pid, sizeof(uint32_t)) != 0)
        ret = -1;

    return ret;
}

/* ========================================================================
 * Debug dump
 * ======================================================================== */

void registry_dump(void)
{
    printf("=== Registry Dump ===\n");
    for (int i = 0; i < 3; i++) {
        dump_key(reg_roots[i], 0);
    }
    printf("=== End Registry Dump ===\n");
}
