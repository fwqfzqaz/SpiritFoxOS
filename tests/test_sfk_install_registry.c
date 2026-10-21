/* Real installer + borrowed value views, synthetic VFS/registry backends. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pkgmgr.h"
#include "vfs.h"

enum { H=1084, P=164, R=392, SECOND=H+P+R+7, SIZE=SECOND+R+3+16 };
static uint8_t image[SIZE];
static unsigned allocations, opens, side_effects, keys, key_calls, writes;
static int budget=-1, fail_key=-1, fail_write=-1;
static reg_key_t fake_key;
void *kmalloc(size_t size)
{
    if (!budget) return NULL;
    if (budget>0) --budget;
    void *p=malloc(size); if (p) ++allocations; return p;
}
void *kcalloc(size_t n,size_t size)
{
    if (!budget) return NULL;
    if (budget>0) --budget;
    void *p=calloc(n,size); if (p) ++allocations; return p;
}
void kfree(void *p) { if(p) { assert(allocations); --allocations; free(p); } }
int vfs_open(const char *path,uint32_t flags,uint32_t mode)
{ (void)path;(void)flags;(void)mode; ++opens; return 7; }
int vfs_close(int fd) { assert(fd==7 && opens); --opens; return 0; }
int vfs_fstat(int fd,vfs_inode_t *stat)
{ assert(fd==7); memset(stat,0,sizeof(*stat)); stat->size=SIZE; return 0; }
int vfs_read(int fd,void *data,size_t size)
{ assert(fd==7 && size==SIZE); memcpy(data,image,size); return (int)size; }
int vfs_mkdir(const char *path,uint32_t mode)
{ (void)path;(void)mode; ++side_effects; return 0; }
int registry_register_software(const reg_software_record_t *record)
{ (void)record; ++side_effects; return 0; }
reg_key_t *registry_open_key(const char *path,int create)
{
    assert(strcmp(path,"HKEY_USER/Test")==0 && create);
    ++side_effects;
    if ((int)key_calls++==fail_key) return NULL;
    ++keys; return &fake_key;
}
void registry_close_key(reg_key_t *key)
{ assert(key==&fake_key && keys); --keys; }
int registry_write_value(const char *path,const char *name,uint32_t type,
                         const void *data,uint32_t size)
{
    assert(strcmp(path,"HKEY_USER/Test")==0 && keys);
    unsigned second=strcmp(name,"second")==0;
    assert(strcmp(name,second ? "second" : "first")==0 && type==REG_TYPE_BINARY);
    assert(size==(second ? 3U : 7U));
    const uint8_t expected[]={0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x80,0x81,0x82};
    assert(memcmp(data,expected+(second ? 7 : 0),size)==0);
    ++side_effects;
    return (int)writes++==fail_write ? -1 : 0;
}
int registry_register_fileassoc(const reg_fileassoc_t *assoc)
{ (void)assoc; ++side_effects; return 0; }
int registry_register_service(const reg_service_t *service)
{ (void)service; ++side_effects; return 0; }
static void put32(unsigned offset,uint32_t v)
{ for(unsigned i=0;i<4;++i) image[offset+i]=(uint8_t)(v>>(8*i)); }
static void put64(unsigned offset,uint64_t v)
{ for(unsigned i=0;i<8;++i) image[offset+i]=(uint8_t)(v>>(8*i)); }
static void fixture(unsigned count)
{
    assert(!allocations && !opens && !keys);
    memset(image,0,sizeof(image));
    put32(0,SFK_MAGIC); put32(4,1);
    memcpy(image+8,"org.spiritfox.registry",sizeof("org.spiritfox.registry"));
    put32(776,1); put32(780,count); put64(812,H);
    put64(820,P+R+7+(count==2 ? R+3 : 0));
    put64(796,SIZE-16); put64(804,16);
    memcpy(image+H+P,"HKEY_USER/Test",15);
    memcpy(image+H+P+256,"first",6);
    put32(H+P+384,REG_TYPE_BINARY); put32(H+P+388,7);
    const uint8_t first[]={0x11,0x12,0x13,0x14,0x15,0x16,0x17};
    memcpy(image+H+P+R,first,7);
    memcpy(image+SECOND,"HKEY_USER/Test",15);
    memcpy(image+SECOND+256,"second",7);
    put32(SECOND+384,REG_TYPE_BINARY); put32(SECOND+388,3);
    image[SECOND+R]=0x80; image[SECOND+R+1]=0x81; image[SECOND+R+2]=0x82;
    side_effects=key_calls=writes=0;
    budget=fail_key=fail_write=-1;
}
static void clean(void) { assert(!allocations && !opens && !keys); }
int main(void)
{
    unsigned cases=0;
    /* Original installer reads seven bytes beyond its one-header allocation. */
    fixture(1);
    assert(pkgmgr_install_sfk("fixture.sfk",0,NULL)==0 && writes==1);
    clean(); ++cases;
    for(unsigned grant=0;grant<2;++grant) for(unsigned dry=0;dry<2;++dry) {
        fixture(2); uint32_t granted=1;
        assert(pkgmgr_install_sfk("fixture.sfk",dry ? PKG_INSTALL_DRYRUN : 0,
                                  grant ? &granted : NULL)==0);
        assert(dry ? side_effects==0 : writes==2); clean(); ++cases;
        for(unsigned bad=0;bad<5;++bad) {
            fixture(2);
            if(bad==0) put64(820,P+2*R+9);
            if(bad==1) memset(image+SECOND,'x',256);
            if(bad==2) memset(image+SECOND+256,'x',128);
            if(bad==3) put32(SECOND+388,UINT32_MAX);
            if(bad==4) put32(780,65);
            assert(pkgmgr_install_sfk("fixture.sfk",dry ? PKG_INSTALL_DRYRUN : 0,
                                      grant ? &granted : NULL)<0);
            assert(!side_effects); clean(); ++cases;
        }
    }
    for(int fail=0;fail<=1;++fail) {
        fixture(2); budget=fail;
        assert(pkgmgr_install_sfk("fixture.sfk",0,NULL)<0);
        assert(!side_effects); clean(); ++cases;
        fixture(2); fail_key=fail;
        assert(pkgmgr_install_sfk("fixture.sfk",0,NULL)<0); clean(); ++cases;
        fixture(2); fail_write=fail;
        assert(pkgmgr_install_sfk("fixture.sfk",0,NULL)<0); clean(); ++cases;
    }
    printf("SFK installer registry tests passed: %u cases, exact values, early rejection, cleanup\n",cases);
    return 0;
}
