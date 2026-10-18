/* 同一份测试在宿主和 QEMU 运行，实际调用重定位后的服务。 */
#include "internal.h"

#define CHECK(condition) do { if (!(condition)) goto fail; } while (0)
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
