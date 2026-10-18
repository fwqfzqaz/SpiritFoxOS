/* 有界 tmpfs 服务：平面命名空间、带代数的文件句柄、消息读写。 */
#include "sfmod.h"
#define FILES 16u
#define CAPACITY 65536u
typedef struct file
{
    char name[32];
    uint8_t *data;
    uint32_t generation;
    uint32_t length;
} file_t;
static file_t files[FILES];
static const sfm_api_t *core;
static uint32_t owner_id;
static uint64_t endpoint;

static int valid_name(const sfm_message_t *msg)
{
    if (msg->size < 2 || msg->size > 32 || msg->data[msg->size - 1]) return 0;
    for (size_t i = 0; i + 1 < msg->size; i++)
        if (!msg->data[i] || msg->data[i] == '/' || msg->data[i] < 32) return 0;
    return 1;
}

static int open_file(const sfm_message_t *req, sfm_message_t *resp)
{
    if (!valid_name(req)) return -22;
    file_t *file = NULL;
    for (size_t i = 0; i < FILES; i++)
        if (files[i].data && sfm_name_equal(files[i].name, (const char *)req->data))
        { file = &files[i]; break; }
    if (!file && (req->flags & SFM_CREATE))
    {
        for (size_t i = 0; i < FILES; i++)
        {
            if (files[i].data || files[i].generation == UINT32_MAX) continue;
            file = &files[i];
            file->data = core->alloc(CAPACITY);
            if (!file->data) return -12;
            sfm_clear(file->data, CAPACITY);
            sfm_clear(file->name, sizeof(file->name));
            sfm_copy(file->name, req->data, req->size);
            file->length = 0;
            file->generation++;
            break;
        }
        if (!file) return -28;
    }
    if (!file) return -2;
    resp->handle = ((uint64_t)file->generation << 32) | (uint32_t)(file - files + 1);
    return 0;
}

static int receive(const sfm_message_t *req, sfm_message_t *resp)
{
    if (req->type == SFM_FILE_OPEN) return open_file(req, resp);
    uint32_t slot = (uint32_t)req->handle;
    if (!slot || slot > FILES) return -9;
    file_t *file = &files[slot - 1];
    if (!file->data || file->generation != (uint32_t)(req->handle >> 32)) return -9;
    switch (req->type)
    {
        case SFM_FILE_WRITE:
            if (req->offset > CAPACITY || req->size > CAPACITY - req->offset) return -28;
            sfm_copy(file->data + req->offset, req->data, req->size);
            if (req->size && req->offset + req->size > file->length)
                file->length = (uint32_t)(req->offset + req->size);
            resp->result = req->size;
            return 0;
        case SFM_FILE_READ:
        {
            if (req->size != sizeof(uint32_t)) return -22;
            uint32_t length;
            sfm_copy(&length, req->data, sizeof(length));
            if (length > SFM_PAYLOAD) return -22;
            if (req->offset >= file->length) return 0;
            if (length > file->length - req->offset) length = file->length - req->offset;
            sfm_copy(resp->data, file->data + req->offset, length);
            resp->size = length;
            resp->result = length;
            return 0;
        }
        case SFM_FILE_STAT:
            resp->result = file->length;
            return 0;
        case SFM_FILE_TRUNCATE:
            if (req->offset > CAPACITY) return -28;
            /* 缩短后再扩展不能泄漏旧内容。 */
            if (req->offset < file->length)
                sfm_clear(file->data + req->offset, file->length - req->offset);
            file->length = (uint32_t)req->offset;
            return 0;
        case SFM_FILE_REMOVE:
            core->free(file->data);
            file->data = NULL;
            file->length = 0;
            return 0;
        default: return -38;
    }
}

static int start(const sfm_api_t *api, uint32_t owner)
{
    core = api;
    owner_id = owner;
    endpoint = core->endpoint_create(owner, "fs.tmpfs", receive);
    return endpoint ? 0 : -12;
}

static void stop(void)
{
    if (endpoint) core->endpoint_destroy(owner_id, endpoint);
    for (size_t i = 0; i < FILES; i++)
    {
        core->free(files[i].data);
        files[i].data = NULL;
    }
    endpoint = 0;
}
SFM_MODULE("fs.tmpfs", start, stop);
