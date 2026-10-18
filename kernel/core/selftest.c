/* 同一份测试在宿主和 QEMU 运行，实际调用重定位后的服务。 */
#include "internal.h"

#define CHECK(condition) do { if (!(condition)) goto fail; } while (0)
static void path_request(sfm_message_t *req, uint32_t operation, uint64_t parent, const char *path)
{
    sfm_clear(req, sizeof(*req));
    req->type = operation;
    req->handle = parent;
    while (path[req->size]) req->size++;
    req->size++;
    sfm_copy(req->data, path, req->size);
}

static int tree_test(uint64_t vfs, sfm_message_t *req, sfm_message_t *resp)
{
    path_request(req, SFM_FILE_OPEN, 0, "/tmp");
    CHECK(!sfm_call(vfs, req, resp) && resp->flags == SFM_NODE_DIR);
    uint64_t root = resp->handle;
    path_request(req, SFM_FILE_OPEN, 0, "/tmp-other/file");
    CHECK(sfm_call(vfs, req, resp) == -2);
    path_request(req, SFM_FILE_OPEN, root, "../escape");
    CHECK(sfm_call(vfs, req, resp) == -22);
    path_request(req, SFM_FILE_OPEN, root, "bad//name");
    CHECK(sfm_call(vfs, req, resp) == -22);
    path_request(req, SFM_FILE_MKDIR, 0, "/tmp/nested");
    CHECK(!sfm_call(vfs, req, resp));
    uint64_t directory = resp->handle;
    CHECK(sfm_call(vfs, req, resp) == -17);
    path_request(req, SFM_FILE_OPEN, directory, "data");
    req->flags = SFM_CREATE;
    CHECK(!sfm_call(vfs, req, resp));
    uint64_t original = resp->handle;
    req->type = SFM_FILE_REMOVE;
    req->handle = directory;
    req->size = 0;
    CHECK(sfm_call(vfs, req, resp) == -39);
    req->type = SFM_FILE_READDIR;
    CHECK(!sfm_call(vfs, req, resp) && resp->result == 1 &&
          resp->handle == original && resp->size == sizeof("data") && resp->data[0] == 'd');
    req->offset = 1;
    CHECK(!sfm_call(vfs, req, resp) && !resp->result);
    req->type = SFM_FILE_RETAIN;
    req->handle = original;
    CHECK(!sfm_call(vfs, req, resp));
    req->type = SFM_FILE_WRITE;
    req->offset = 0;
    req->size = 3;
    sfm_copy(req->data, "old", 3);
    CHECK(!sfm_call(vfs, req, resp) && resp->result == 3);
    req->type = SFM_FILE_REMOVE;
    req->size = 0;
    CHECK(!sfm_call(vfs, req, resp));
    path_request(req, SFM_FILE_OPEN, directory, "data");
    CHECK(sfm_call(vfs, req, resp) == -2);
    req->flags = SFM_CREATE;
    CHECK(!sfm_call(vfs, req, resp) && resp->handle != original);
    uint64_t replacement = resp->handle;
    path_request(req, SFM_FILE_OPEN, 0, "/run/data");
    req->flags = SFM_CREATE;
    CHECK(!sfm_call(vfs, req, resp) && resp->handle != replacement);
    uint64_t other_mount = resp->handle;
    path_request(req, SFM_FILE_OPEN, 0, "/tmp/data");
    CHECK(sfm_call(vfs, req, resp) == -2);
    sfm_clear(req, sizeof(*req));
    req->type = SFM_FILE_READ;
    req->handle = original;
    req->size = sizeof(uint32_t);
    uint32_t count = 8;
    sfm_copy(req->data, &count, sizeof(count));
    CHECK(!sfm_call(vfs, req, resp) && resp->result == 3 && resp->data[0] == 'o');
    req->type = SFM_FILE_RELEASE;
    req->size = 0;
    CHECK(!sfm_call(vfs, req, resp));
    req->type = SFM_FILE_STAT;
    CHECK(sfm_call(vfs, req, resp) == -9);
    req->type = SFM_FILE_RELEASE;
    req->handle = replacement;
    CHECK(sfm_call(vfs, req, resp) == -9);
    req->type = SFM_FILE_REMOVE;
    CHECK(!sfm_call(vfs, req, resp));
    req->handle = other_mount;
    CHECK(!sfm_call(vfs, req, resp));
    req->handle = directory;
    CHECK(!sfm_call(vfs, req, resp));
    req->handle = root;
    CHECK(sfm_call(vfs, req, resp) == -16);
    return 0;
fail:
    sf_log("[sfmod] tmpfs tree assertion failed\n");
    return -1;
}

static int device_test(uint64_t vfs, sfm_message_t *req, sfm_message_t *resp)
{
    path_request(req, SFM_FILE_OPEN, 0, "/dev");
    CHECK(!sfm_call(vfs, req, resp) && resp->route == SFM_ROUTE_DEVFS &&
          resp->flags == SFM_NODE_DIR);
    uint64_t root = resp->handle;
    sfm_clear(req, sizeof(*req));
    req->type = SFM_FILE_READDIR;
    req->route = SFM_ROUTE_DEVFS;
    req->handle = root;
    CHECK(!sfm_call(vfs, req, resp) && resp->result == 1 &&
          resp->flags == SFM_NODE_CHARDEV && resp->data[0] == 'n');
    req->offset = 1;
    CHECK(!sfm_call(vfs, req, resp) && resp->result == 1 && resp->data[0] == 'z');
    req->offset = 2;
    CHECK(!sfm_call(vfs, req, resp) && !resp->result);
    path_request(req, SFM_FILE_OPEN, 0, "/dev/null");
    CHECK(!sfm_call(vfs, req, resp) && resp->route == SFM_ROUTE_DEVFS &&
          resp->flags == SFM_NODE_CHARDEV && resp->offset == 0x103);
    uint64_t null = resp->handle;
    req->handle = null;
    req->route = SFM_ROUTE_DEVFS;
    req->type = SFM_FILE_READ;
    uint32_t count = 32;
    req->size = sizeof(count);
    sfm_copy(req->data, &count, sizeof(count));
    CHECK(!sfm_call(vfs, req, resp) && !resp->result && !resp->size);
    req->type = SFM_FILE_WRITE;
    req->size = 3;
    req->offset = UINT64_MAX;
    CHECK(!sfm_call(vfs, req, resp) && resp->result == 3);
    req->type = SFM_FILE_STAT;
    CHECK(!sfm_call(vfs, req, resp) && !resp->result && resp->offset == 0x103);
    req->type = SFM_FILE_REMOVE;
    CHECK(sfm_call(vfs, req, resp) == -30);
    req->type = SFM_FILE_RELEASE;
    CHECK(sfm_call(vfs, req, resp) == -9);
    path_request(req, SFM_FILE_OPEN, 0, "/dev/zero");
    CHECK(!sfm_call(vfs, req, resp) && resp->route == SFM_ROUTE_DEVFS && resp->offset == 0x105);
    req->route = SFM_ROUTE_DEVFS;
    req->handle = resp->handle;
    req->type = SFM_FILE_RETAIN;
    req->size = 0;
    CHECK(!sfm_call(vfs, req, resp));
    req->type = SFM_FILE_READ;
    count = SFM_PAYLOAD;
    req->size = sizeof(count);
    sfm_copy(req->data, &count, sizeof(count));
    CHECK(!sfm_call(vfs, req, resp) && resp->result == SFM_PAYLOAD && resp->size == SFM_PAYLOAD);
    for (size_t i = 0; i < SFM_PAYLOAD; i++) CHECK(!resp->data[i]);
    count++;
    sfm_copy(req->data, &count, sizeof(count));
    CHECK(sfm_call(vfs, req, resp) == -22);
    req->type = SFM_FILE_SEEK;
    req->flags = 2;
    CHECK(!sfm_call(vfs, req, resp) && !resp->result);
    req->flags = 3;
    CHECK(sfm_call(vfs, req, resp) == -22);
    req->type = SFM_FILE_STAT;
    req->route = SFM_ROUTE_COUNT;
    CHECK(sfm_call(vfs, req, resp) == -22);
    req->route = SFM_ROUTE_TMPFS;
    CHECK(sfm_call(vfs, req, resp) == -9);
    req->route = SFM_ROUTE_DEVFS;
    req->type = SFM_FILE_RELEASE;
    CHECK(!sfm_call(vfs, req, resp));
    path_request(req, SFM_FILE_OPEN, 0, "/dev/null");
    req->flags = SFM_CREATE;
    CHECK(sfm_call(vfs, req, resp) == -30);
    path_request(req, SFM_FILE_OPEN, 0, "/dev-other/null");
    CHECK(sfm_call(vfs, req, resp) == -2);
    CHECK(sfm_module_unload("dev.tmpfs") == SFM_EBUSY);
    sf_log("[sfmod] device service self-test passed\n");
    return 0;
fail:
    sf_log("[sfmod] device service self-test failed\n");
    return -1;
}

int sfm_selftest(void)
{
    sfm_message_t *req = sf_alloc(sizeof(*req));
    sfm_message_t *resp = sf_alloc(sizeof(*resp));
    if (!req || !resp) goto fail;
    sfm_clear(req, sizeof(*req));
    uint64_t hello = sfm_lookup("demo.hello"), vfs = sfm_lookup("fs.vfs");
    CHECK(hello && vfs);
    req->type = SFM_PING;
    req->sequence = 123;
    req->size = 4;
    sfm_copy(req->data, "ping", 4);
    CHECK(!sfm_call(hello, req, resp) && resp->size == 4 &&
          resp->data[0] == 'p' && resp->sequence == 123 && resp->src == hello);
    /* 队列满不丢消息，未授权 owner 不能取走消息。 */
    sfm_owner_enable(SFM_CORE_OWNER, 1);
    uint64_t queue = sfm_endpoint_create(SFM_CORE_OWNER, "test.queue", NULL);
    CHECK(queue);
    for (size_t i = 0; i < SFM_QUEUE_DEPTH; i++)
    {
        req->sequence = (uint32_t)i;
        CHECK(!sfm_send(queue, req));
    }
    CHECK(sfm_send(queue, req) == SFM_EAGAIN);
    CHECK(sfm_recv(0, queue, resp) == SFM_EPERM);
    for (size_t i = 0; i < SFM_QUEUE_DEPTH; i++)
        CHECK(!sfm_recv(SFM_CORE_OWNER, queue, resp) && resp->sequence == i);
    CHECK(sfm_recv(SFM_CORE_OWNER, queue, resp) == SFM_EAGAIN);
    CHECK(!sfm_endpoint_destroy(SFM_CORE_OWNER, queue));
    CHECK(sfm_send(queue, req) == SFM_ENOENT);
    sfm_owner_remove(SFM_CORE_OWNER);

    sfm_clear(req, sizeof(*req));
    req->type = SFM_FILE_OPEN;
    req->flags = SFM_CREATE;
    req->size = sizeof("/modules/hello.txt");
    sfm_copy(req->data, "/modules/hello.txt", req->size);
    CHECK(!sfm_call(vfs, req, resp) && resp->handle);
    uint64_t file = resp->handle;
    req->type = SFM_FILE_WRITE;
    req->handle = file;
    req->offset = 3;
    req->size = 5;
    sfm_copy(req->data, "hello", 5);
    CHECK(!sfm_call(vfs, req, resp) && resp->result == 5);
    req->type = SFM_FILE_READ;
    req->offset = 0;
    uint32_t length = 32;
    req->size = sizeof(length);
    sfm_copy(req->data, &length, sizeof(length));
    CHECK(!sfm_call(vfs, req, resp) && resp->result == 8 &&
          !resp->data[0] && !resp->data[1] && !resp->data[2] &&
          resp->data[3] == 'h' && resp->data[7] == 'o');
    req->type = SFM_FILE_WRITE;
    req->offset = UINT64_MAX;
    CHECK(sfm_call(vfs, req, resp) == SFM_ENOSPC);
    req->type = SFM_FILE_REMOVE;
    CHECK(!sfm_call(vfs, req, resp));
    req->type = SFM_FILE_STAT;
    CHECK(sfm_call(vfs, req, resp) == -9);
    CHECK(!tree_test(vfs, req, resp));
    CHECK(!device_test(vfs, req, resp));
    CHECK(sfm_module_unload("fs.tmpfs") == SFM_EBUSY);
    sf_free(req);
    sf_free(resp);
    return 0;
fail:
    sf_log("[sfmod] self-test assertion failed\n");
    sfm_owner_remove(SFM_CORE_OWNER);
    sf_free(req);
    sf_free(resp);
    return -1;
}
