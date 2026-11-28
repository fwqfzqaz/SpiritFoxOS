/* Namespace-core mutation tests with a recording backend. These cover the
 * contract the SFRWFS1 runtime relies on without needing a formatted volume:
 * hook ordering, error propagation, read-only mounts, permissions, rename
 * loops, descriptor busy checks and slot exhaustion. */
#include <stdio.h>
#include "namespace_state.h"
static SfNamespace ns;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){printf("NS MUTATE FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct {
 unsigned calls,creates,removes,renames;
 int fail;
 char from[96],to[96];
 int directory;
 uint32_t uid,inode;
 unsigned mode;
} Fake;
static int fake_create(void*context,const char*path,int directory,uint32_t uid,unsigned mode,uint32_t*inode){
 Fake*f=context;f->calls++;
 if(f->fail)return f->fail;
 f->creates++;f->directory=directory;f->uid=uid;f->mode=mode;
 snprintf(f->from,sizeof(f->from),"%s",path);
 *inode=++f->inode;return 0;
}
static int fake_remove(void*context,const char*path,int directory,uint32_t uid){
 Fake*f=context;f->calls++;
 if(f->fail)return f->fail;
 f->removes++;f->directory=directory;f->uid=uid;
 snprintf(f->from,sizeof(f->from),"%s",path);return 0;
}
static int fake_rename(void*context,const char*from,const char*to,uint32_t uid){
 Fake*f=context;f->calls++;
 if(f->fail)return f->fail;
 f->renames++;f->uid=uid;
 snprintf(f->from,sizeof(f->from),"%s",from);
 snprintf(f->to,sizeof(f->to),"%s",to);return 0;
}
static int open_dir(const char*path,uint32_t uid,int write){
 return sf_ns_open_as(&ns,1,"/",path,OPEN_READ|(write?OPEN_WRITE:0)|OPEN_DIRECTORY,uid,uid);
}
static int open_file(const char*path,uint32_t uid,int write){
 return sf_ns_open_as(&ns,1,"/",path,OPEN_READ|(write?OPEN_WRITE:0),uid,uid);
}
static int exists(const char*path){int h=open_file(path,0,0);if(h<=0)return 0;sf_ns_close(&ns,1,(uint32_t)h);return 1;}
static const SfMountEntry disk_entries[]={
 {"/disk",NODE_DIR,0,SF_NS_RW_BIT|1u,0},
 {"/disk/dir",NODE_DIR,0,SF_NS_RW_BIT|2u,0},
 {"/disk/file",NODE_DISK,0,SF_NS_RW_BIT|3u,4},
 {"/disk/empty",NODE_DIR,0,SF_NS_RW_BIT|4u,0},
};
static const SfMountEntry ro_entries[]={
 {"/ro",NODE_DIR,1,0,0},
 {"/ro/keep",NODE_DIR,1,0,0},
};
int main(void){
 Fake fake;memset(&fake,0,sizeof(fake));fake.inode=100;
 SfNsDirBackend backend={fake_create,fake_remove,fake_rename,&fake};
 sf_ns_init(&ns,0);
 CHECK(!sf_ns_mount_publish(&ns,disk_entries,4,1));
 CHECK(!sf_ns_mount_publish(&ns,ro_entries,2,2));
 sf_ns_dir_backend(&ns,&backend);
 /* Directories and files created through the namespace become volume nodes. */
 CHECK(!sf_ns_create_owned(&ns,"/disk/new",NODE_DIR,0,0,0,0750));
 CHECK(fake.creates==1&&fake.directory==1&&fake.uid==0&&fake.mode==0750);
 CHECK(!strcmp(fake.from,"/disk/new"));
 int h=open_dir("/disk/new",0,0);CHECK(h>0);
 SfFileStat st;CHECK(!sf_ns_stat(&ns,1,(uint32_t)h,&st)&&st.type==NODE_DIR);
 CHECK(!sf_ns_close(&ns,1,(uint32_t)h));
 CHECK(!sf_ns_create_owned(&ns,"/disk/newfile",NODE_FILE,0,0,0,0640));
 CHECK(fake.creates==2&&fake.directory==0&&fake.mode==0640);
 h=open_file("/disk/newfile",0,0);
 CHECK(h>0&&!sf_ns_stat(&ns,1,(uint32_t)h,&st)&&st.type==NODE_DISK&&st.size==0);
 CHECK(!sf_ns_close(&ns,1,(uint32_t)h));
 /* Backend failures propagate and never leave a visible node behind. */
 fake.fail=SF_NO_SPACE;
 CHECK(sf_ns_create_owned(&ns,"/disk/rejected",NODE_DIR,0,0,0,0755)==SF_NO_SPACE);
 fake.fail=0;
 CHECK(!exists("/disk/rejected"));
 int dirh=open_dir("/disk",0,1);CHECK(dirh>0);
 CHECK(!sf_ns_create_owned(&ns,"/disk/dir/sub",NODE_DIR,0,0,0,0755));
 /* Non-empty removal is rejected before the backend sees the request. */
 unsigned before=fake.calls;
 CHECK(sf_ns_rmdir_as(&ns,1,(uint32_t)dirh,"dir",0,0)==SF_BUSY);
 CHECK(fake.calls==before);
 CHECK(!sf_ns_create_owned(&ns,"/disk/victim",NODE_DIR,0,0,0,0755));
 CHECK(!sf_ns_rmdir_as(&ns,1,(uint32_t)dirh,"victim",0,0));
 CHECK(fake.removes==1&&fake.directory==1&&!strcmp(fake.from,"/disk/victim"));
 CHECK(!exists("/disk/victim"));
 /* A refused backend removal keeps the namespace node intact. */
 fake.fail=SF_DEVICE_GONE;
 CHECK(sf_ns_unlink_as(&ns,1,(uint32_t)dirh,"file",0,0)==SF_DEVICE_GONE);
 fake.fail=0;
 CHECK(exists("/disk/file"));
 CHECK(!sf_ns_unlink_as(&ns,1,(uint32_t)dirh,"file",0,0));
 CHECK(fake.removes==2&&fake.directory==0&&!exists("/disk/file"));
 CHECK(!sf_ns_close(&ns,1,(uint32_t)dirh));
 /* Rename requires the runtime hook and a writable formatted node. */
 sf_ns_dir_backend(&ns,0);
 CHECK(sf_ns_rename(&ns,1,"/disk/new","/disk/other")==SF_READ_ONLY);
 sf_ns_dir_backend(&ns,&backend);
 CHECK(sf_ns_rename(&ns,1,"/ro/keep","/ro/gone")==SF_READ_ONLY);
 CHECK(sf_ns_rename(&ns,1,"/disk/missing","/disk/x")==SF_NOT_FOUND);
 CHECK(sf_ns_rename(&ns,1,"/disk/dir","/disk/dir/sub/deep")==SF_INVALID);
 CHECK(sf_ns_rename(&ns,1,"/disk/new","/")==SF_INVALID);
 CHECK(fake.renames==0);
 /* An open destination description blocks replacement instead of aliasing. */
 CHECK(!sf_ns_create_owned(&ns,"/disk/target",NODE_FILE,0,0,0,0644));
 int busy=open_file("/disk/target",0,1);CHECK(busy>0);
 CHECK(sf_ns_rename(&ns,1,"/disk/newfile","/disk/target")==SF_BUSY);
 CHECK(!sf_ns_close(&ns,1,(uint32_t)busy));
 /* Rename failures are transactional for the namespace tree. */
 fake.fail=SF_COMMIT_UNCERTAIN;
 CHECK(sf_ns_rename(&ns,1,"/disk/newfile","/disk/renamed")==SF_COMMIT_UNCERTAIN);
 fake.fail=0;
 CHECK(exists("/disk/newfile")&&!exists("/disk/renamed"));
 CHECK(!sf_ns_rename(&ns,1,"/disk/newfile","/disk/renamed"));
 CHECK(fake.renames==1&&!strcmp(fake.from,"/disk/newfile")&&!strcmp(fake.to,"/disk/renamed"));
 CHECK(exists("/disk/renamed")&&!exists("/disk/newfile"));
 /* Replacing an existing file removes its node after the disk commit. */
 CHECK(!sf_ns_create_owned(&ns,"/disk/old",NODE_FILE,0,0,0,0644));
 CHECK(!sf_ns_rename(&ns,1,"/disk/renamed","/disk/old"));
 CHECK(exists("/disk/old")&&!exists("/disk/renamed"));
 /* A directory rename moves the whole published subtree. */
 CHECK(!sf_ns_create_owned(&ns,"/disk/dir/sub/f",NODE_FILE,0,0,0,0644));
 CHECK(!sf_ns_rename(&ns,1,"/disk/dir","/disk/moved"));
 CHECK(!exists("/disk/dir")&&exists("/disk/moved/sub/f"));
 int moved=open_dir("/disk/moved",0,0);CHECK(moved>0&&!sf_ns_close(&ns,1,(uint32_t)moved));
 /* Mounts cannot be crossed even when both use the same namespace shape. */
 CHECK(sf_ns_rename(&ns,1,"/disk/moved/sub/f","/ro/f")==SF_READ_ONLY);
 /* A directory can replace an empty directory but not a file. */
 CHECK(sf_ns_rename(&ns,1,"/disk/moved","/disk/old")==SF_INVALID);
 CHECK(!sf_ns_rename(&ns,1,"/disk/moved","/disk/empty"));
 CHECK(!exists("/disk/moved")&&exists("/disk/empty/sub/f"));
 /* Permissions: owner 5 with 0700 accepts uid 5 and rejects uid 6. */
 CHECK(!sf_ns_create_owned(&ns,"/disk/private",NODE_DIR,0,0,0,0755));
 CHECK(!sf_ns_assign(&ns,"/disk/private",5,5,0700));
 CHECK(sf_ns_create_owned(&ns,"/disk/private/no",NODE_DIR,0,6,6,0755)==SF_DENIED);
 CHECK(!sf_ns_create_owned(&ns,"/disk/private/yes",NODE_DIR,0,5,5,0755));
 int ph=open_dir("/disk/private",5,1);CHECK(ph>0);
 CHECK(sf_ns_rmdir_as(&ns,1,(uint32_t)ph,"yes",6,6)==SF_DENIED);
 CHECK(!sf_ns_rmdir_as(&ns,1,(uint32_t)ph,"yes",5,5));
 CHECK(!sf_ns_close(&ns,1,(uint32_t)ph));
 CHECK(sf_ns_rename_as(&ns,1,"/disk/private","/disk/p2",6,6)==SF_DENIED);
 /* Slot exhaustion is reported before asking the backend to create. */
 unsigned created=0,before_full=0;
 for(unsigned i=0;i<SF_NS_NODE_MAX+8;i++){
  char path[96];snprintf(path,sizeof(path),"/disk/n%u",i);
  before_full=fake.calls;int r=sf_ns_create_owned(&ns,path,NODE_DIR,0,0,0,0755);
  if(r==SF_NO_SPACE){CHECK(fake.calls==before_full);break;}
  CHECK(!r);created++;
 }
 CHECK(created>16);
 /* Unmount removes the subtree and detaches the backend; new nodes are plain
  * namespace objects and must not touch the volume. */
 CHECK(!sf_ns_unmount(&ns,1));
 CHECK(!exists("/disk/n0"));
 unsigned detached=fake.calls;
 CHECK(!sf_ns_create_owned(&ns,"/disk/after",NODE_DIR,0,0,0,0755));
 CHECK(fake.calls==detached);
 /* The read-only volume still resolves its published children. */
 CHECK(exists("/ro/keep"));
 CHECK(sf_ns_create_owned(&ns,"/ro/new",NODE_DIR,0,0,0,0755)==SF_READ_ONLY);
 printf("Namespace mutation tests passed: %u assertions\n",checks);return 0;
}
