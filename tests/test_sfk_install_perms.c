/* Real install entry, synthetic read-only package and observable side effects. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkgmgr.h"
#include "vfs.h"

enum { HEADER = 1084, RECORD = 164, PACKAGE = HEADER + RECORD + 16 };
static uint8_t image[PACKAGE];
static unsigned side_effects, opens, allocations;
static uint32_t registered_perms;

void *kmalloc(size_t size) { void *p = malloc(size); if (p) ++allocations; return p; }
void *kcalloc(size_t count, size_t size) { void *p = calloc(count, size); if (p) ++allocations; return p; }
void kfree(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
int vfs_open(const char *path, uint32_t flags, uint32_t mode)
{ (void)path; (void)flags; (void)mode; ++opens; return 7; }
int vfs_close(int fd) { assert(fd == 7 && opens); --opens; return 0; }
int vfs_fstat(int fd, vfs_inode_t *info)
{ assert(fd == 7); memset(info, 0, sizeof(*info)); info->size = PACKAGE; return 0; }
int vfs_read(int fd, void *out, size_t count)
{ assert(fd == 7 && count == PACKAGE); memcpy(out, image, count); return (int)count; }
int vfs_mkdir(const char *path, uint32_t mode)
{ (void)path; (void)mode; ++side_effects; return 0; }
int registry_register_software(const reg_software_record_t *record)
{ ++side_effects; registered_perms = record->sfk_perms; return 0; }
reg_key_t *registry_open_key(const char *path, int create)
{ (void)path; (void)create; ++side_effects; return NULL; }
void registry_close_key(reg_key_t *key) { (void)key; }
int registry_write_value(const char *path, const char *name, uint32_t type,
                         const void *data, uint32_t size)
{ (void)path; (void)name; (void)type; (void)data; (void)size; ++side_effects; return 0; }
int registry_register_fileassoc(const reg_fileassoc_t *assoc)
{ (void)assoc; ++side_effects; return 0; }
int registry_register_service(const reg_service_t *service)
{ (void)service; ++side_effects; return 0; }

static void put32(unsigned offset, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) image[offset + i] = (uint8_t)(value >> (8 * i)); }
static void put64(unsigned offset, uint64_t value)
{ for (unsigned i = 0; i < 8; ++i) image[offset + i] = (uint8_t)(value >> (8 * i)); }
static void fixture(void)
{
    memset(image, 0, sizeof(image));
    put32(0, SFK_MAGIC); put32(4, 1);
    memcpy(image + 8, "org.spiritfox.perms", sizeof("org.spiritfox.perms"));
    put32(776, 1); put64(812, HEADER); put64(820, RECORD);
    put64(796, HEADER + RECORD); put64(804, 16);
    put32(HEADER, 1);
    side_effects = 0; registered_perms = 0;
}
int main(void)
{
    unsigned cases = 0;
    for (unsigned malformed = 0; malformed < 3; ++malformed) {
        for (unsigned explicit = 0; explicit < 2; ++explicit) {
            for (unsigned dry = 0; dry < 2; ++dry) {
                fixture();
                if (malformed == 0) put64(820, RECORD - 1);
                if (malformed == 1) memset(image + HEADER + 4, 'x', 32);
                if (malformed == 2) put32(776, 33);
                uint32_t granted = 0x1234;
                assert(pkgmgr_install_sfk("fixture.sfk", dry ? PKG_INSTALL_DRYRUN : 0,
                                          explicit ? &granted : NULL) < 0);
                assert(!side_effects && !opens && !allocations);
                ++cases;
            }
        }
    }
    for (unsigned explicit = 0; explicit < 2; ++explicit) {
        fixture(); uint32_t granted = 0x1234;
        assert(pkgmgr_install_sfk("fixture.sfk", 0, explicit ? &granted : NULL) == 0);
        assert(side_effects == 3 && !opens && !allocations);
        /* Legacy authorization policy is unchanged by record validation. */
        assert(registered_perms == (explicit ? 0x1234U : 1U));
        ++cases;
    }
    printf("SFK installer permission guard passed: %u cases, reject before side effects, cleanup\n", cases);
    return 0;
}
