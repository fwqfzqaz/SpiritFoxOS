/* 有界消息队列与内核内 RPC。不会持锁调用模块，也不在 IRQ 上下文阻塞。 */
#include "../internal.h"

#define ENDPOINTS 32u
typedef struct endpoint
{
    uint32_t owner;
    uint32_t generation;
    uint32_t active;
    uint32_t head;
    uint32_t count;
    char name[32];
    sfm_handler_t handler;
    sfm_message_t *queue;
} endpoint_t;

static endpoint_t endpoints[ENDPOINTS];
static uint8_t owners[SFM_CORE_OWNER + 1];
static unsigned char endpoint_lock;

static uint64_t lock(void)
{
    uint64_t flags = sf_irq_save();
    while (__atomic_test_and_set(&endpoint_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return flags;
}

static void unlock(uint64_t flags)
{
    __atomic_clear(&endpoint_lock, __ATOMIC_RELEASE);
    sf_irq_restore(flags);
}

int sfm_name_valid(const char *name, size_t capacity)
{
    if (!name || !capacity || !name[0]) return 0;
    for (size_t i = 0; i < capacity; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (!c) return 1;
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') &&
            c != '.' && c != '_' && c != '-') return 0;
    }
    return 0;
}

static uint64_t handle(endpoint_t *ep)
{
    return ((uint64_t)ep->generation << 32) | (uint32_t)(ep - endpoints + 1);
}

static endpoint_t *resolve(uint64_t id)
{
    uint32_t slot = (uint32_t)id;
    if (!slot || slot > ENDPOINTS) return NULL;
    endpoint_t *ep = &endpoints[slot - 1];
    return ep->owner && ep->generation == (uint32_t)(id >> 32) ? ep : NULL;
}

void sfm_owner_enable(uint32_t owner, int enable)
{
    uint64_t flags = lock();
    if (owner && owner <= SFM_CORE_OWNER) owners[owner] = !!enable;
    unlock(flags);
}

uint64_t sfm_lookup(const char *name)
{
    if (!sfm_name_valid(name, 32)) return 0;
    uint64_t flags = lock(), id = 0;
    for (size_t i = 0; i < ENDPOINTS; i++)
        if (endpoints[i].owner && sfm_name_equal(name, endpoints[i].name))
        {
            id = handle(&endpoints[i]);
            break;
        }
    unlock(flags);
    return id;
}

uint64_t sfm_endpoint_create(uint32_t owner, const char *name, sfm_handler_t fn)
{
    if (!owner || owner > SFM_CORE_OWNER || !sfm_name_valid(name, 32)) return 0;
    sfm_message_t *queue = sf_alloc(SFM_QUEUE_DEPTH * sizeof(*queue));
    if (!queue) return 0;
    uint64_t flags = lock(), id = 0;
    if (!owners[owner]) goto done;
    for (size_t i = 0; i < ENDPOINTS; i++)
        if (endpoints[i].owner && sfm_name_equal(name, endpoints[i].name)) goto done;
    for (size_t i = 0; i < ENDPOINTS; i++)
    {
        endpoint_t *ep = &endpoints[i];
        if (ep->owner) continue;
        /* 代数溢出时永久停用该槽，避免旧句柄再次有效。 */
        if (ep->generation == UINT32_MAX) continue;
        ep->generation++;
        ep->owner = owner;
        ep->active = ep->head = ep->count = 0;
        sfm_clear(ep->name, sizeof(ep->name));
        for (size_t j = 0; name[j]; j++) ep->name[j] = name[j];
        ep->handler = fn;
        ep->queue = queue;
        id = handle(ep);
        queue = NULL;
        break;
    }
done:
    unlock(flags);
    sf_free(queue);
    return id;
}

int sfm_endpoint_destroy(uint32_t owner, uint64_t id)
{
    uint64_t flags = lock();
    endpoint_t *ep = resolve(id);
    int result = SFM_ENOENT;
    sfm_message_t *queue = NULL;
    if (ep)
    {
        result = ep->owner != owner ? SFM_EPERM : ep->active ? SFM_EBUSY : 0;
        if (!result)
        {
            queue = ep->queue;
            ep->owner = 0;
            ep->queue = NULL;
            ep->handler = NULL;
        }
    }
    unlock(flags);
    sf_free(queue);
    return result;
}

int sfm_call(uint64_t id, const sfm_message_t *request, sfm_message_t *response)
{
    if (!request || !response || request == response || request->size > SFM_PAYLOAD)
        return SFM_EINVAL;
    uint64_t flags = lock();
    endpoint_t *ep = resolve(id);
    if (!ep || !ep->handler)
    {
        unlock(flags);
        return ep ? SFM_ENOSYS : SFM_ENOENT;
    }
    /* 禁止同一服务重入，避免环形调用导致栈耗尽或破坏服务状态。 */
    if (ep->active)
    {
        unlock(flags);
        return SFM_EBUSY;
    }
    ep->active++;
    sfm_handler_t fn = ep->handler;
    unlock(flags);
    sfm_clear(response, sizeof(*response));
    int result = fn(request, response);
    response->src = id;
    response->dst = request->src;
    response->sequence = request->sequence;
    response->type = request->type;
    if (response->size > SFM_PAYLOAD) result = SFM_EINVAL;
    if (result) response->result = result;
    flags = lock();
    ep->active--;
    unlock(flags);
    return result;
}

int sfm_send(uint64_t id, const sfm_message_t *message)
{
    if (!message || message->size > SFM_PAYLOAD) return SFM_EINVAL;
    uint64_t flags = lock();
    endpoint_t *ep = resolve(id);
    int result = SFM_ENOENT;
    if (ep)
    {
        result = ep->count == SFM_QUEUE_DEPTH ? SFM_EAGAIN : 0;
        if (!result)
        {
            sfm_message_t *slot = &ep->queue[(ep->head + ep->count) % SFM_QUEUE_DEPTH];
            sfm_clear(slot, sizeof(*slot));
            sfm_copy(slot, message, offsetof(sfm_message_t, data) + message->size);
            slot->dst = id;
            ep->count++;
        }
    }
    unlock(flags);
    return result;
}

int sfm_recv(uint32_t owner, uint64_t id, sfm_message_t *message)
{
    if (!message) return SFM_EINVAL;
    uint64_t flags = lock();
    endpoint_t *ep = resolve(id);
    int result = SFM_ENOENT;
    if (ep)
    {
        result = ep->owner != owner ? SFM_EPERM : !ep->count ? SFM_EAGAIN : 0;
        if (!result)
        {
            sfm_copy(message, &ep->queue[ep->head], sizeof(*message));
            ep->head = (ep->head + 1) % SFM_QUEUE_DEPTH;
            ep->count--;
        }
    }
    unlock(flags);
    return result;
}

int sfm_owner_busy(uint32_t owner)
{
    uint64_t flags = lock();
    int busy = 0;
    for (size_t i = 0; i < ENDPOINTS; i++)
        if (endpoints[i].owner == owner && endpoints[i].active) busy = 1;
    unlock(flags);
    return busy;
}

void sfm_owner_remove(uint32_t owner)
{
    sfm_owner_enable(owner, 0);
    for (size_t i = 0; i < ENDPOINTS; i++)
        if (endpoints[i].owner == owner) sfm_endpoint_destroy(owner, handle(&endpoints[i]));
}
