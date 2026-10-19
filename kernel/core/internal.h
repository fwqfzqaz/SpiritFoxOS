/* 模块机制的内部接口；平台部分由内核或宿主测试提供。 */
#ifndef SFM_INTERNAL_H
#define SFM_INTERNAL_H
#include "sfmod.h"

#define SFM_EINVAL (-22)
#define SFM_ENOMEM (-12)
#define SFM_ENOENT (-2)
#define SFM_EBUSY (-16)
#define SFM_EAGAIN (-11)
#define SFM_ENOSPC (-28)
#define SFM_ENOEXEC (-8)
#define SFM_EEXIST (-17)
#define SFM_EPERM (-1)
#define SFM_ENOSYS (-38)
#define SFM_CORE_OWNER (SFM_MAX_MODULES + 1u)

void *sf_alloc(size_t);
void sf_free(void *);
void sf_log(const char *);
uint64_t sf_irq_save(void);
void sf_irq_restore(uint64_t);
int sf_pci_read(uint32_t, sfm_pci_record_t *);

typedef struct sfm_image
{
    void *allocation;
    uint8_t *base;
    size_t size;
    sfm_metadata_t meta;
    sfm_entry_t entry;
    char deps[SFM_MAX_DEPS][32];
    size_t dep_count;
} sfm_image_t;

int sfm_elf_load(const void *, size_t, sfm_image_t *);
void sfm_elf_free(sfm_image_t *);
int sfm_name_valid(const char *, size_t);
uint64_t sfm_endpoint_create(uint32_t, const char *, sfm_handler_t);
int sfm_endpoint_destroy(uint32_t, uint64_t);
uint64_t sfm_lookup(const char *);
int sfm_call(uint64_t, const sfm_message_t *, sfm_message_t *);
int sfm_send(uint64_t, const sfm_message_t *);
int sfm_recv(uint32_t, uint64_t, sfm_message_t *);
int sfm_owner_busy(uint32_t);
void sfm_owner_remove(uint32_t);
void sfm_owner_enable(uint32_t, int);
int sfm_package_load(const void *, size_t);
int sfm_module_unload(const char *);
void sfm_boot(void);
int sfm_selftest(void);
int sfm_device_selftest(void);

#endif
