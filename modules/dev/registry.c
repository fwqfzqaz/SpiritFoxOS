/* One immutable /dev namespace. Legacy hardware I/O stays in its VFS adapter. */
#include "sfmod.h"
static const sfm_api_t *core;
static uint32_t owner_id, count, sealed, references[SFM_DEV_MAX];
static uint64_t endpoint, special;
static sfm_device_record_t nodes[SFM_DEV_MAX];
/* One endpoint cannot reenter; release must not depend on allocation. */
static sfm_message_t forwarded;

static uint64_t handle(uint32_t slot)
{ return (endpoint & 0xffffffff00000000ull) | ((uint32_t)endpoint << 8) | (slot + 1u); }
static int resolve(uint64_t value)
{
    uint32_t slot = (uint8_t)value;
    if (!slot || slot > count || handle(slot - 1) != value) return -1;
    return (int)slot - 1;
}
static size_t length(const char *s, size_t limit)
{ size_t n = 0; while (n < limit && s[n]) n++; return n; }
static int equal(const char *a, const char *b)
{ for (size_t i = 0; i < 256; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; } return 0; }
static int path_valid(const char *path)
{
    size_t n = length(path, 256), component = 0, start = 5, depth = 0;
    if (n < 4 || n == 256 || path[0] != '/' || path[1] != 'd' ||
        path[2] != 'e' || path[3] != 'v') return 0;
    if (n == 4) return 1;
    if (path[4] != '/') return 0;
    for (size_t i = 5; i <= n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c && c != '/') {
            if (c < 33 || c == 127 || ++component > 63) return 0;
        } else {
            if (!component || (component == 1 && path[start] == '.') ||
                (component == 2 && path[start] == '.' && path[start + 1] == '.') || ++depth > 8) return 0;
            component = 0; start = i + 1;
        }
    }
    return 1;
}
static int find(const char *path)
{
    for (uint32_t i = 0; i < count; i++) if (equal(nodes[i].path, path)) return (int)i;
    return -1;
}
static void describe(uint32_t slot, sfm_message_t *resp)
{
    const sfm_device_record_t *n = &nodes[slot];
    resp->handle = n->handle; resp->flags = n->type;
    resp->offset = n->type == SFM_NODE_CHARDEV ? n->rdev : n->bytes;
    resp->result = (int64_t)n->bytes;
}
static int registration(const sfm_message_t *req, sfm_message_t *resp)
{
    if (sealed) return -30;
    if (req->size != sizeof(sfm_device_record_t)) return -22;
    sfm_device_record_t item;
    sfm_copy(&item, req->data, sizeof(item));
    if (item.version != SFM_DEV_VERSION || item.size != sizeof(item)) return -93;
    if (!path_valid(item.path) || item.handle || item.parent || item.reserved ||
        item.bytes > INT64_MAX || (item.flags & ~SFM_DEV_RDEV_KNOWN)) return -22;
    if (item.owner != SFM_DEV_BOOT && item.owner != SFM_DEV_SPECIAL) return -1;
    if (find(item.path) >= 0) return -17;
    uint32_t mode = item.type == SFM_NODE_DIR ? 0040000u :
        item.type == SFM_NODE_CHARDEV ? 0020000u : item.type == SFM_NODE_BLKDEV ? 0060000u : 0;
    if (!mode || (item.mode & ~07777u) != mode) return -22;
    if ((equal(item.path, "/dev/null") || equal(item.path, "/dev/zero")) &&
        (item.backend != SFM_DEV_TMPFS || item.owner != SFM_DEV_SPECIAL)) return -22;
    if (item.type != SFM_NODE_BLKDEV && item.bytes) return -22;
    if (item.backend == SFM_DEV_DIRECTORY) {
        if (item.type != SFM_NODE_DIR || item.owner != SFM_DEV_BOOT || item.backend_handle ||
            item.flags || item.rdev || item.bytes) return -22;
    } else if (item.backend == SFM_DEV_TMPFS) {
        if (item.owner != SFM_DEV_SPECIAL || item.type != SFM_NODE_CHARDEV || item.bytes ||
            item.flags != SFM_DEV_RDEV_KNOWN ||
            (!sfm_name_equal(item.path, "/dev/null") && !sfm_name_equal(item.path, "/dev/zero"))) return -22;
        sfm_message_t *check = core->alloc(sizeof(*check)), *answer = core->alloc(sizeof(*answer));
        if (!check || !answer) { core->free(check); core->free(answer); return -12; }
        sfm_clear(check, sizeof(*check));
        check->type = SFM_FILE_STAT; check->handle = item.backend_handle;
        int result = core->call(special, check, answer);
        if (!result && (answer->flags != SFM_NODE_CHARDEV || answer->offset != item.rdev ||
            item.rdev != (sfm_name_equal(item.path, "/dev/null") ? 0x103u : 0x105u))) result = -22;
        core->free(check); core->free(answer);
        if (result) return result;
    } else if (item.owner != SFM_DEV_BOOT || item.flags || item.rdev ||
        (item.backend == SFM_DEV_CHAR ? item.type != SFM_NODE_CHARDEV || item.backend_handle >= 256 :
         item.backend == SFM_DEV_BLOCK ? item.type != SFM_NODE_BLKDEV || item.backend_handle >= 16 :
         item.backend == SFM_DEV_CONSOLE ? item.type != SFM_NODE_CHARDEV || item.backend_handle != 1 : 1)) return -22;
    size_t split = length(item.path, sizeof(item.path));
    while (split && item.path[split - 1] != '/') split--;
    char parent[256];
    sfm_copy(parent, item.path, split - 1); parent[split - 1] = 0;
    int p = find(parent);
    if (p < 0) return -2;
    if (nodes[p].type != SFM_NODE_DIR) return -20;
    if (count == SFM_DEV_MAX) return -28;
    item.handle = handle(count); item.parent = nodes[p].handle;
    /* Canonical zero tail makes records deterministic and avoids padding leaks. */
    size_t n = length(item.path, sizeof(item.path));
    sfm_clear(item.path + n, sizeof(item.path) - n);
    sfm_copy(&nodes[count], &item, sizeof(item));
    describe(count++, resp);
    return 0;
}
static int open_node(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->flags) return -30;
    int parent = resolve(req->handle);
    if (parent < 0) return -9;
    if (nodes[parent].type != SFM_NODE_DIR) return -20;
    if (!req->size || req->size > 256 || req->data[req->size - 1]) return -22;
    for (uint32_t i = 0; i + 1 < req->size; i++) if (!req->data[i]) return -22;
    char path[256];
    size_t n = length(nodes[parent].path, sizeof(path));
    if (req->size == 1) { describe((uint32_t)parent, resp); return 0; }
    if (n + 1 + req->size > sizeof(path)) return -36;
    sfm_copy(path, nodes[parent].path, n); path[n++] = '/';
    sfm_copy(path + n, req->data, req->size);
    if (!path_valid(path)) return -22;
    int slot = find(path);
    if (slot < 0) return -2;
    describe((uint32_t)slot, resp);
    return 0;
}
static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->type >= SFM_DEV_REGISTER && req->type <= SFM_DEV_UNREGISTER) {
        if (req->flags != SFM_DEV_VERSION) return -93;
        if (req->route || req->handle || (req->offset && req->type != SFM_DEV_ENUM)) return -22;
        if (req->type == SFM_DEV_REGISTER) return registration(req, resp);
        if (req->type == SFM_DEV_UNREGISTER) return -38;
        if (req->type == SFM_DEV_SEAL) {
            if (req->size) return -22;
            if (sealed) return -16;
            if (find("/dev/null") < 0 || find("/dev/zero") < 0) return -22;
            sealed = 1; return 0;
        }
        if (!sealed) return -11;
        int slot;
        if (req->type == SFM_DEV_LOOKUP) {
            if (!req->size || req->size > 256 || req->data[req->size - 1]) return -22;
            for (uint32_t i = 0; i + 1 < req->size; i++) if (!req->data[i]) return -22;
            if (!path_valid((const char *)req->data)) return -22;
            slot = find((const char *)req->data);
            if (slot < 0) return -2;
        } else {
            if (req->size || req->offset > count) return -22;
            if (req->offset == count) return 0;
            slot = (int)req->offset;
        }
        sfm_copy(resp->data, &nodes[slot], sizeof(nodes[slot]));
        resp->size = sizeof(nodes[slot]); resp->result = 1; resp->handle = nodes[slot].handle;
        return 0;
    }
    if (req->type == SFM_FS_ROOT) {
        if (req->size || req->offset) return -22;
        describe(0, resp); return 0;
    }
    if (!sealed) return -11;
    if (req->type == SFM_FILE_OPEN) return open_node(req, resp);
    int slot = resolve(req->handle);
    if (slot < 0) return -9;
    if (req->type == SFM_FILE_MKDIR || req->type == SFM_FILE_REMOVE || req->type == SFM_FILE_SYMLINK) return -30;
    if (req->type == SFM_FILE_STAT) { describe((uint32_t)slot, resp); return 0; }
    if (req->type == SFM_FILE_READDIR) {
        if (nodes[slot].type != SFM_NODE_DIR) return -20;
        uint64_t seen = 0;
        for (uint32_t i = 1; i < count; i++) if (nodes[i].parent == req->handle && seen++ == req->offset) {
            describe(i, resp);
            size_t n = length(nodes[i].path, 256);
            while (n && nodes[i].path[n - 1] != '/') n--;
            resp->size = (uint32_t)length(nodes[i].path + n, 64) + 1;
            sfm_copy(resp->data, nodes[i].path + n, resp->size); resp->result = 1;
            return 0;
        }
        return 0;
    }
    if (nodes[slot].backend == SFM_DEV_TMPFS) {
        sfm_copy(&forwarded, req, sizeof(forwarded)); forwarded.handle = nodes[slot].backend_handle;
        int result = core->call(special, &forwarded, resp);
        resp->handle = req->handle; return result;
    }
    if (req->type == SFM_FILE_RETAIN) {
        if (references[slot] == UINT32_MAX) return -75;
        references[slot]++; return 0;
    }
    if (req->type == SFM_FILE_RELEASE) {
        if (!references[slot]) return -9;
        references[slot]--; return 0;
    }
    return -95; /* Legacy I/O is supported only through its existing VFS adapter. */
}
static int start(const sfm_api_t *api, uint32_t owner)
{
    core = api; owner_id = owner; special = core->lookup("dev.tmpfs");
    if (!special) return -2;
    endpoint = core->endpoint_create(owner, "dev.registry", receive);
    if (!endpoint) return -12;
    if ((uint32_t)endpoint > 0xffffffu) return -75;
    sfm_clear(nodes, sizeof(nodes)); sfm_clear(references, sizeof(references));
    count = 1; sealed = 0;
    nodes[0].version = SFM_DEV_VERSION; nodes[0].size = sizeof(nodes[0]);
    nodes[0].handle = nodes[0].parent = handle(0);
    nodes[0].owner = SFM_DEV_BOOT; nodes[0].type = SFM_NODE_DIR; nodes[0].mode = 0040755;
    sfm_copy(nodes[0].path, "/dev", 5);
    return 0;
}
static void stop(void)
{ if (endpoint) core->endpoint_destroy(owner_id, endpoint); endpoint = 0; count = sealed = 0; }
SFM_DEPENDS("dev.tmpfs");
SFM_MODULE("dev.registry", start, stop);
