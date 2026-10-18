/* 有界树形 tmpfs；命名链接与打开引用分开，删除不破坏仍打开的文件。 */
#include "sfmod.h"

typedef struct node
{
    char name[SFM_FILE_NAME];
    uint8_t *data;
    uint64_t parent;
    uint32_t generation, type, length, references, linked, root;
} node_t;
static node_t nodes[SFM_FILE_NODES];
static uint64_t roots[SFM_FS_INSTANCES];
static const sfm_api_t *core;
static uint32_t owner_id;
static uint64_t endpoint;

static uint64_t handle(const node_t *node)
{ return ((uint64_t)node->generation << 32) | (uint32_t)(node - nodes + 1); }

static node_t *resolve(uint64_t id)
{
    uint32_t slot = (uint32_t)id;
    if (!slot || slot > SFM_FILE_NODES) return NULL;
    node_t *node = &nodes[slot - 1];
    return node->type && node->generation == (uint32_t)(id >> 32) ? node : NULL;
}

static void dispose(node_t *node)
{
    core->free(node->data);
    node->data = NULL;
    node->type = node->length = node->linked = node->references = 0;
}

static node_t *allocate(uint32_t type, uint64_t parent, const char *name)
{
    for (size_t i = 0; i < SFM_FILE_NODES; i++)
    {
        node_t *node = &nodes[i];
        if (node->type || node->generation == UINT32_MAX) continue;
        uint32_t generation = node->generation + 1;
        sfm_clear(node, sizeof(*node));
        node->generation = generation;
        node->type = type;
        node->parent = parent;
        node->linked = 1;
        for (size_t j = 0; name[j]; j++) node->name[j] = name[j];
        return node;
    }
    return NULL;
}

static node_t *child(uint64_t parent, const char *name)
{
    for (size_t i = 0; i < SFM_FILE_NODES; i++)
    {
        node_t *node = &nodes[i];
        if (!node->type || !node->linked || node->parent != parent) continue;
        size_t j = 0;
        while (j < SFM_FILE_NAME && node->name[j] == name[j])
            if (!name[j++]) return node;
    }
    return NULL;
}

static void describe(node_t *node, sfm_message_t *response)
{
    response->handle = handle(node);
    response->flags = node->type;
    response->result = node->length;
}

static int open_node(const sfm_message_t *req, sfm_message_t *resp)
{
    if (!req->size || req->size > SFM_FILE_PATH || req->data[req->size - 1] ||
        (req->flags & ~SFM_CREATE)) return -22;
    node_t *parent = resolve(req->handle);
    if (!parent) return -9;
    if (parent->type != SFM_NODE_DIR) return -20;
    if (!parent->linked) return -2;
    /* 先验证完整相对路径，不接受隐藏 NUL、空组件或越界组件。 */
    size_t component = 0;
    for (size_t i = 0; i + 1 < req->size; i++)
    {
        uint8_t c = req->data[i];
        if (!c || c < 32) return -22;
        if (c == '/') { if (!component) return -22; component = 0; }
        else if (++component >= SFM_FILE_NAME) return -36;
    }
    if (req->size > 1 && !component) return -22;
    if (req->size == 1)
    {
        if (req->type == SFM_FILE_MKDIR) return -17;
        describe(parent, resp);
        return 0;
    }
    size_t position = 0;
    for (;;)
    {
        char name[SFM_FILE_NAME] = {0};
        size_t length = 0;
        while (req->data[position] && req->data[position] != '/')
            name[length++] = (char)req->data[position++];
        if ((length == 1 && name[0] == '.') ||
            (length == 2 && name[0] == '.' && name[1] == '.')) return -22;
        int last = !req->data[position];
        node_t *node = child(handle(parent), name);
        if (!node && last && ((req->flags & SFM_CREATE) || req->type == SFM_FILE_MKDIR))
        {
            node = allocate(req->type == SFM_FILE_MKDIR ? SFM_NODE_DIR : SFM_NODE_FILE,
                            handle(parent), name);
            if (!node) return -28;
        }
        else if (node && last && req->type == SFM_FILE_MKDIR) return -17;
        if (!node) return -2;
        if (last) { describe(node, resp); return 0; }
        if (node->type != SFM_NODE_DIR) return -20;
        parent = node;
        position++;
    }
}

static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->type == SFM_FILE_SYMLINK)
    {
        node_t *parent = resolve(req->handle);
        if (!parent) return -9;
        if (parent->type != SFM_NODE_DIR) return -20;
        if (!parent->linked) return -2;
        size_t length = 0;
        while (length < req->size && req->data[length])
        {
            if (req->data[length] < 32 || req->data[length] == '/') return -22;
            length++;
        }
        if (!length || length >= SFM_FILE_NAME || length + 2 >= req->size ||
            req->size - length - 1 > SFM_FILE_PATH || req->data[req->size - 1]) return -22;
        if ((length == 1 && req->data[0] == '.') ||
            (length == 2 && req->data[0] == '.' && req->data[1] == '.')) return -22;
        for (size_t i = length + 1; i + 1 < req->size; i++)
            if (!req->data[i]) return -22;
        if (child(req->handle, (const char *)req->data)) return -17;
        uint8_t *target = core->alloc(req->size - length - 1);
        if (!target) return -12;
        node_t *node = allocate(SFM_NODE_SYMLINK, req->handle, (const char *)req->data);
        if (!node) { core->free(target); return -28; }
        node->data = target;
        node->length = req->size - (uint32_t)length - 2;
        sfm_copy(target, req->data + length + 1, node->length + 1);
        describe(node, resp);
        return 0;
    }
    if (req->type == SFM_FS_ROOT)
    {
        if (req->size || req->offset >= SFM_FS_INSTANCES) return -22;
        describe(resolve(roots[req->offset]), resp);
        return 0;
    }
    if (req->type == SFM_FILE_OPEN || req->type == SFM_FILE_MKDIR)
        return open_node(req, resp);
    node_t *node = resolve(req->handle);
    if (!node) return -9;
    switch (req->type)
    {
        case SFM_FILE_RETAIN:
            if (node->references == UINT32_MAX) return -75;
            node->references++;
            return 0;
        case SFM_FILE_RELEASE:
            if (!node->references) return -9;
            if (!--node->references && !node->linked) dispose(node);
            return 0;
        case SFM_FILE_STAT:
            describe(node, resp);
            return 0;
        case SFM_FILE_REMOVE:
            if (node->root) return -16;
            if (!node->linked) return -2;
            if (node->type == SFM_NODE_DIR)
                for (size_t i = 0; i < SFM_FILE_NODES; i++)
                    if (nodes[i].type && nodes[i].linked && nodes[i].parent == req->handle)
                        return -39;
            node->linked = 0;
            node->parent = 0;
            if (!node->references) dispose(node);
            return 0;
        case SFM_FILE_READDIR:
        {
            if (node->type != SFM_NODE_DIR) return -20;
            uint64_t index = 0;
            for (size_t i = 0; i < SFM_FILE_NODES; i++)
                if (nodes[i].type && nodes[i].linked && nodes[i].parent == req->handle)
                {
                    if (index++ != req->offset) continue;
                    describe(&nodes[i], resp);
                    size_t length = 0;
                    while (nodes[i].name[length]) length++;
                    resp->size = (uint32_t)length + 1;
                    sfm_copy(resp->data, nodes[i].name, resp->size);
                    resp->offset = nodes[i].length;
                    resp->result = 1;
                    return 0;
                }
            return 0;
        }
        case SFM_FILE_WRITE:
        case SFM_FILE_TRUNCATE:
        {
            if (node->type != SFM_NODE_FILE) return node->type == SFM_NODE_DIR ? -21 : -22;
            uint64_t length = req->type == SFM_FILE_WRITE ? req->size : 0;
            if (req->offset > SFM_FILE_CAPACITY || length > SFM_FILE_CAPACITY - req->offset)
                return -28;
            if (req->type == SFM_FILE_WRITE && !length) return 0;
            uint32_t end = (uint32_t)(req->offset + length);
            if (end && !node->data)
            {
                node->data = core->alloc(SFM_FILE_CAPACITY);
                if (!node->data) return -12;
                sfm_clear(node->data, SFM_FILE_CAPACITY);
            }
            if (req->type == SFM_FILE_TRUNCATE)
            {
                if (end < node->length)
                    sfm_clear(node->data + end, node->length - end);
                node->length = end;
            }
            else
            {
                sfm_copy(node->data + req->offset, req->data, req->size);
                if (end > node->length) node->length = end;
                resp->result = req->size;
            }
            return 0;
        }
        case SFM_FILE_READ:
        {
            if (node->type == SFM_NODE_DIR) return -21;
            if (req->size != sizeof(uint32_t)) return -22;
            uint32_t length;
            sfm_copy(&length, req->data, sizeof(length));
            if (length > SFM_PAYLOAD) return -22;
            if (req->offset >= node->length || !length) return 0;
            if (length > node->length - req->offset) length = node->length - req->offset;
            sfm_copy(resp->data, node->data + req->offset, length);
            resp->size = length;
            resp->result = length;
            return 0;
        }
        default: return -38;
    }
}

static int start(const sfm_api_t *api, uint32_t owner)
{
    core = api;
    owner_id = owner;
    for (size_t i = 0; i < SFM_FS_INSTANCES; i++)
    {
        node_t *root = allocate(SFM_NODE_DIR, 0, "");
        if (!root) return -28;
        root->root = 1;
        roots[i] = handle(root);
    }
    endpoint = core->endpoint_create(owner, "fs.tmpfs", receive);
    return endpoint ? 0 : -12;
}

static void stop(void)
{
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
    for (size_t i = 0; i < SFM_FILE_NODES; i++) dispose(&nodes[i]);
    endpoint = 0;
}
SFM_MODULE("fs.tmpfs", start, stop);
