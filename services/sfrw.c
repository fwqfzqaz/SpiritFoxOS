/* SFRWFS1 bounded writable filesystem core. No libc or device dependencies. */
#include "sfrw.h"
#include "package.h"

static uint16_t g16(const uint8_t*p){uint16_t n;memcpy(&n,p,2);return n;}
static uint32_t g32(const uint8_t*p){uint32_t n;memcpy(&n,p,4);return n;}
static uint64_t g64(const uint8_t*p){uint64_t n;memcpy(&n,p,8);return n;}
static void p16(uint8_t*p,uint16_t n){memcpy(p,&n,2);}
static void p32(uint8_t*p,uint32_t n){memcpy(p,&n,4);}
static void p64(uint8_t*p,uint64_t n){memcpy(p,&n,8);}
static int zero(const uint8_t*p,size_t n){while(n--)if(*p++)return 0;return 1;}
static const uint8_t zeros[SF_RW_BLOCK_BYTES];

typedef struct {
 uint16_t mode,links;
 uint32_t uid,gid;
 uint64_t size;
 uint32_t flags;
 uint32_t direct[SF_RW_DIRECT_BLOCKS];
 uint32_t indirect;
} Inode;

static void inode_decode(const uint8_t*p,Inode*ip){
 ip->mode=g16(p);ip->links=g16(p+2);ip->uid=g32(p+4);ip->gid=g32(p+8);
 ip->size=g64(p+12);ip->flags=g32(p+20);
 for(unsigned i=0;i<SF_RW_DIRECT_BLOCKS;i++)ip->direct[i]=g32(p+24+4*i);
 ip->indirect=g32(p+72);
}
static void inode_encode(uint8_t*p,const Inode*ip){
 memset(p,0,SF_RW_INODE_BYTES);p16(p,ip->mode);p16(p+2,ip->links);p32(p+4,ip->uid);p32(p+8,ip->gid);
 p64(p+12,ip->size);p32(p+20,ip->flags);
 for(unsigned i=0;i<SF_RW_DIRECT_BLOCKS;i++)p32(p+24+4*i,ip->direct[i]);
 p32(p+72,ip->indirect);
}

static int super_load(Sfrw*s,const uint8_t*b){
 if(memcmp(b,"SFRWFS1\0",8))return SF_BAD_FORMAT;
 uint8_t h[32];sf_sha256(h,b,4064);if(memcmp(h,b+4064,32))return SF_BAD_FORMAT;
 uint32_t blocks=g32(b+16);
 if(g32(b+8)!=1||g32(b+12)!=SF_RW_BLOCK_BYTES||blocks<SF_RW_MIN_BLOCKS||blocks>SF_RW_MAX_BLOCKS)return SF_BAD_FORMAT;
 if(g32(b+20)!=SF_RW_MAX_INODES||g32(b+24)!=SF_RW_INODE_TABLE_BLOCK||g32(b+28)!=SF_RW_BITMAP_BLOCK||
    g32(b+32)!=SF_RW_JOURNAL_BLOCK||g32(b+36)!=SF_RW_JOURNAL_BLOCKS||g32(b+40)!=SF_RW_DATA_BLOCK||
    g32(b+52)!=SF_RW_ROOT||g32(b+76))return SF_BAD_FORMAT;
 uint32_t quota=g32(b+72);
 if(quota>SF_RW_QUOTA_MAX)return SF_BAD_FORMAT;
 if(g32(b+56)>blocks-SF_RW_DATA_BLOCK-1||g32(b+60)>SF_RW_MAX_INODES-2)return SF_BAD_FORMAT;
 s->block_count=blocks;
 s->generation=g64(b+44);
 s->free_blocks=g32(b+56);s->free_inodes=g32(b+60);
 s->default_quota_blocks=g32(b+64);s->default_quota_inodes=g32(b+68);
 s->quota_count=quota;
 for(uint32_t i=0;i<quota;i++){
  const uint8_t*q=b+80+i*16;
  if(g32(q+12))return SF_BAD_FORMAT;
  s->quota[i].uid=g32(q);s->quota[i].blocks=g32(q+4);s->quota[i].inodes=g32(q+8);s->quota[i].reserved=0;
  for(uint32_t j=0;j<i;j++)if(s->quota[j].uid==s->quota[i].uid)return SF_BAD_FORMAT;
 }
 return 0;
}
static void super_build(const Sfrw*s,uint8_t*b,uint64_t generation){
 memset(b,0,SF_RW_BLOCK_BYTES);
 memcpy(b,"SFRWFS1\0",8);
 p32(b+8,1);p32(b+12,SF_RW_BLOCK_BYTES);p32(b+16,s->block_count);p32(b+20,SF_RW_MAX_INODES);
 p32(b+24,SF_RW_INODE_TABLE_BLOCK);p32(b+28,SF_RW_BITMAP_BLOCK);p32(b+32,SF_RW_JOURNAL_BLOCK);
 p32(b+36,SF_RW_JOURNAL_BLOCKS);p32(b+40,SF_RW_DATA_BLOCK);p64(b+44,generation);p32(b+52,SF_RW_ROOT);
 p32(b+56,s->free_blocks);p32(b+60,s->free_inodes);p32(b+64,s->default_quota_blocks);
 p32(b+68,s->default_quota_inodes);p32(b+72,s->quota_count);
 for(uint32_t i=0;i<s->quota_count;i++){
  uint8_t*q=b+80+i*16;
  p32(q,s->quota[i].uid);p32(q+4,s->quota[i].blocks);p32(q+8,s->quota[i].inodes);
 }
 sf_sha256(b+4064,b,4064);
}

static int bit_get(const Sfrw*s,uint32_t b){return (s->bitmap[b>>3]>>(b&7))&1;}
static void bit_set(Sfrw*s,uint32_t b,int v){
 uint8_t*m=s->bitmap+(b>>3);uint8_t mask=(uint8_t)(1u<<(b&7));
 if(v)*m|=mask;else *m&=(uint8_t)~mask;
}
static uint32_t bitmap_free(const Sfrw*s){
 uint32_t n=0;for(uint32_t b=SF_RW_DATA_BLOCK;b<s->block_count;b++)if(!bit_get(s,b))n++;return n;
}

static int meta_read(Sfrw*s,uint32_t target,uint8_t*out){
 if(target>=s->block_count)return SF_INVALID;
 for(uint32_t i=0;i<s->journal_count;i++)
  if(s->journal_target[i]==target)return s->dev.read(s->dev.context,SF_RW_JOURNAL_BLOCK+1+i,out);
 return s->dev.read(s->dev.context,target,out);
}
static int stage_open(Sfrw*s){
 if(s->journal_clean)return 0;
 int r=s->dev.write(s->dev.context,SF_RW_JOURNAL_BLOCK,zeros);
 if(!r)r=s->dev.flush(s->dev.context);
 if(r)return r;
 s->journal_clean=1;return 0;
}
static int meta_write(Sfrw*s,uint32_t target,const uint8_t*data){
 if(target>=s->block_count||(target>=SF_RW_JOURNAL_BLOCK&&target<SF_RW_JOURNAL_BLOCK+SF_RW_JOURNAL_BLOCKS))return SF_INVALID;
 int r=stage_open(s);if(r)return r;
 uint32_t slot=s->journal_count;
 for(uint32_t i=0;i<s->journal_count;i++)if(s->journal_target[i]==target){slot=i;break;}
 if(slot>=SF_RW_JOURNAL_MAX)return SF_NO_SPACE;
 r=s->dev.write(s->dev.context,SF_RW_JOURNAL_BLOCK+1+slot,data);if(r)return r;
 sf_sha256(s->journal_hash[slot],data,SF_RW_BLOCK_BYTES);
 s->journal_target[slot]=target;s->journal_length[slot]=SF_RW_BLOCK_BYTES;
 if(slot==s->journal_count)s->journal_count++;
 s->dirty=1;return 0;
}

static int commit(Sfrw*s){
 if(!s->mounted)return SF_DEVICE_GONE;
 if(!s->dirty&&!s->journal_count)return 0;
 /* 目标集溢出时不得继续以未提交内存状态运行：失败关闭，重挂载得到旧状态。 */
 if(s->journal_count+4>SF_RW_JOURNAL_MAX){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 uint64_t generation=s->generation+1;
 super_build(s,s->aux,generation);
 int r=meta_write(s,SF_RW_BITMAP_BLOCK,s->bitmap);
 if(!r)r=meta_write(s,SF_RW_BITMAP_BLOCK+1,s->bitmap+SF_RW_BLOCK_BYTES);
 if(!r)r=meta_write(s,0,s->aux);
 if(!r)r=meta_write(s,1,s->aux);
 if(r)goto uncertain;
 if(s->dev.flush(s->dev.context))goto uncertain;
 uint8_t*h=s->aux;memset(h,0,SF_RW_BLOCK_BYTES);
 memcpy(h,"SFRWJ1\0\0",8);p32(h+8,1);p32(h+12,1);p64(h+16,generation);p32(h+24,s->journal_count);
 for(uint32_t i=0;i<s->journal_count;i++){
  p32(h+32+4*i,s->journal_target[i]);p32(h+96+4*i,s->journal_length[i]);
  memcpy(h+160+32*i,s->journal_hash[i],32);
 }
 sf_sha256(h+4064,h,4064);
 if(s->dev.write(s->dev.context,SF_RW_JOURNAL_BLOCK,h)||s->dev.flush(s->dev.context))goto uncertain;
 for(uint32_t i=0;i<s->journal_count;i++){
  r=s->dev.read(s->dev.context,SF_RW_JOURNAL_BLOCK+1+i,s->block);
  if(!r)r=s->dev.write(s->dev.context,s->journal_target[i],s->block);
  if(r)goto uncertain;
 }
 if(s->dev.flush(s->dev.context))goto uncertain;
 if(s->dev.write(s->dev.context,SF_RW_JOURNAL_BLOCK,zeros)||s->dev.flush(s->dev.context))goto uncertain;
 s->generation=generation;s->journal_count=0;s->journal_clean=1;s->dirty=0;return 0;
uncertain:
 s->mounted=0;return SF_COMMIT_UNCERTAIN;
}

static int journal_replay(Sfrw*s){
 uint8_t*h=s->aux;
 int r=s->dev.read(s->dev.context,SF_RW_JOURNAL_BLOCK,h);if(r)return r;
 if(memcmp(h,"SFRWJ1\0\0",8)){s->journal_clean=zero(h,SF_RW_BLOCK_BYTES);return 0;}
 uint8_t digest[32];sf_sha256(digest,h,4064);
 if(memcmp(digest,h+4064,32)){s->journal_clean=0;return 0;}
 if(g32(h+8)!=1)return SF_BAD_FORMAT;
 if(g32(h+12)!=1){s->journal_clean=0;return 0;}
 if(g32(h+28))return SF_BAD_FORMAT;
 uint32_t count=g32(h+24);if(count>SF_RW_JOURNAL_MAX)return SF_BAD_FORMAT;
 for(uint32_t i=0;i<count;i++){
  uint32_t target=g32(h+32+4*i),length=g32(h+96+4*i);
  if(length!=SF_RW_BLOCK_BYTES||target>=s->block_count||
     (target>=SF_RW_JOURNAL_BLOCK&&target<SF_RW_JOURNAL_BLOCK+SF_RW_JOURNAL_BLOCKS))return SF_BAD_FORMAT;
  for(uint32_t j=0;j<i;j++)if(g32(h+32+4*j)==target)return SF_BAD_FORMAT;
  r=s->dev.read(s->dev.context,SF_RW_JOURNAL_BLOCK+1+i,s->block);if(r)return r;
  sf_sha256(digest,s->block,SF_RW_BLOCK_BYTES);
  if(memcmp(digest,h+160+32*i,32))return SF_BAD_FORMAT;
 }
 for(uint32_t i=0;i<count;i++){
  r=s->dev.read(s->dev.context,SF_RW_JOURNAL_BLOCK+1+i,s->block);if(r)return r;
  r=s->dev.write(s->dev.context,g32(h+32+4*i),s->block);if(r)return r;
 }
 if(s->dev.flush(s->dev.context))return SF_COMMIT_UNCERTAIN;
 if(s->dev.write(s->dev.context,SF_RW_JOURNAL_BLOCK,zeros)||s->dev.flush(s->dev.context))return SF_COMMIT_UNCERTAIN;
 s->journal_clean=1;s->journal_count=0;return 0;
}

static int inode_read(Sfrw*s,uint32_t ino,Inode*ip){
 if(ino<1||ino>=SF_RW_MAX_INODES)return SF_INVALID;
 uint32_t blk=SF_RW_INODE_TABLE_BLOCK+ino/SF_RW_INODES_PER_BLOCK;
 int r=meta_read(s,blk,s->aux);if(r)return r;
 inode_decode(s->aux+(ino%SF_RW_INODES_PER_BLOCK)*SF_RW_INODE_BYTES,ip);return 0;
}
static int inode_write(Sfrw*s,uint32_t ino,const Inode*ip){
 if(ino<1||ino>=SF_RW_MAX_INODES)return SF_INVALID;
 uint32_t blk=SF_RW_INODE_TABLE_BLOCK+ino/SF_RW_INODES_PER_BLOCK;
 int r=meta_read(s,blk,s->aux);if(r)return r;
 inode_encode(s->aux+(ino%SF_RW_INODES_PER_BLOCK)*SF_RW_INODE_BYTES,ip);
 return meta_write(s,blk,s->aux);
}
static int block_map(Sfrw*s,const Inode*ip,uint32_t lb,uint32_t*out){
 if(lb>=SF_RW_FILE_BLOCKS)return SF_INVALID;
 if(lb<SF_RW_DIRECT_BLOCKS){*out=ip->direct[lb];return 0;}
 if(!ip->indirect){*out=0;return 0;}
 int r=meta_read(s,ip->indirect,s->indirect);if(r)return r;
 *out=g32(s->indirect+(lb-SF_RW_DIRECT_BLOCKS)*4);return 0;
}
static int inode_block_count(Sfrw*s,const Inode*ip,uint32_t*out){
 uint32_t n=0;
 for(unsigned i=0;i<SF_RW_DIRECT_BLOCKS;i++)if(ip->direct[i])n++;
 if(ip->indirect){
  n++;int r=meta_read(s,ip->indirect,s->indirect);if(r)return r;
  for(unsigned i=0;i<SF_RW_INDIRECT_ENTRIES;i++)if(g32(s->indirect+4*i))n++;
 }
 *out=n;return 0;
}

static int allowed(const Inode*ip,uint32_t uid,uint32_t mask){
 if(uid==0)return 1;
 uint32_t bits=ip->mode&SF_RW_PERM_MASK;
 bits=ip->uid==uid?(bits>>6):(bits&7);
 return (bits&mask)==mask;
}
static int name_ok(const char*n,size_t len){
 if(!len||len>SF_RW_NAME_MAX)return 0;
 if((len==1&&n[0]=='.')||(len==2&&n[0]=='.'&&n[1]=='.'))return 0;
 for(size_t i=0;i<len;i++){unsigned c=(uint8_t)n[i];if(c<33||c>126||c=='/'||c=='\\')return 0;}
 return 1;
}
static int path_ok(const char*p){
 size_t n=0;while(n<=SF_RW_PATH_MAX&&p[n])n++;
 if(n<1||n>SF_RW_PATH_MAX||p[0]!='/')return 0;
 if(n>1&&p[n-1]=='/')return 0;
 for(size_t i=0;i<n;i++){unsigned c=(uint8_t)p[i];if(c<33||c>126||c=='\\')return 0;}
 for(size_t i=1;i<n;){
  size_t j=i;while(j<n&&p[j]!='/')j++;
  if(!name_ok(p+i,j-i))return 0;
  i=j+1;
 }
 return 1;
}
static int path_parent(const char*p,char*parent,char*name){
 size_t n=strlen(p);if(!n||p[0]!='/')return 0;
 size_t slash=0;for(size_t i=0;i<n;i++)if(p[i]=='/')slash=i;
 size_t len=n-slash-1;
 if(!name_ok(p+slash+1,len))return 0;
 if(slash==0){parent[0]='/';parent[1]=0;}else{memcpy(parent,p,slash);parent[slash]=0;}
 memcpy(name,p+slash+1,len+1);return 1;
}

static SfrwUsage* usage_slot(Sfrw*s,uint32_t uid,int create){
 for(uint32_t i=0;i<s->usage_count;i++)if(s->usage[i].uid==uid)return &s->usage[i];
 if(!create||s->usage_count>=SF_RW_UID_TRACK)return 0;
 SfrwUsage*u=&s->usage[s->usage_count++];memset(u,0,sizeof(*u));u->uid=uid;return u;
}
static void quota_limits(const Sfrw*s,uint32_t uid,uint32_t*blocks,uint32_t*inodes){
 *blocks=s->default_quota_blocks;*inodes=s->default_quota_inodes;
 for(uint32_t i=0;i<s->quota_count;i++)if(s->quota[i].uid==uid){*blocks=s->quota[i].blocks;*inodes=s->quota[i].inodes;}
}
static int usage_check(Sfrw*s,uint32_t uid,uint32_t blocks,uint32_t inodes){
 if(uid==0)return 0;
 SfrwUsage*u=usage_slot(s,uid,0);
 uint32_t used_blocks=u?u->blocks:0,used_inodes=u?u->inodes:0;
 uint32_t limit_blocks,limit_inodes;quota_limits(s,uid,&limit_blocks,&limit_inodes);
 if(limit_blocks&&(uint64_t)used_blocks+blocks>limit_blocks)return SF_NO_SPACE;
 if(limit_inodes&&(uint64_t)used_inodes+inodes>limit_inodes)return SF_NO_SPACE;
 return 0;
}
static int usage_reserve(Sfrw*s,uint32_t uid,uint32_t blocks,uint32_t inodes){
 int r=usage_check(s,uid,blocks,inodes);if(r)return r;
 SfrwUsage*u=usage_slot(s,uid,1);if(!u)return SF_NO_SPACE;
 u->blocks+=blocks;u->inodes+=inodes;return 0;
}
static void usage_release(Sfrw*s,uint32_t uid,uint32_t blocks,uint32_t inodes){
 SfrwUsage*u=usage_slot(s,uid,0);if(!u)return;
 u->blocks=u->blocks>blocks?u->blocks-blocks:0;
 u->inodes=u->inodes>inodes?u->inodes-inodes:0;
}
static uint32_t block_alloc(Sfrw*s){
 for(uint32_t b=SF_RW_DATA_BLOCK;b<s->block_count;b++)
  if(!bit_get(s,b)){bit_set(s,b,1);s->free_blocks--;return b;}
 return 0;
}
static void block_release(Sfrw*s,uint32_t b){
 if(b>=SF_RW_DATA_BLOCK&&b<s->block_count&&bit_get(s,b)){bit_set(s,b,0);s->free_blocks++;}
}
static int free_inode_blocks(Sfrw*s,const Inode*ip){
 if(!ip->indirect)goto direct_only;
 {
  int r=meta_read(s,ip->indirect,s->indirect);if(r)return r;
  for(unsigned i=0;i<SF_RW_INDIRECT_ENTRIES;i++){
   uint32_t b=g32(s->indirect+4*i);if(!b)continue;
   block_release(s,b);usage_release(s,ip->uid,1,0);
  }
  block_release(s,ip->indirect);usage_release(s,ip->uid,1,0);
 }
direct_only:
 for(unsigned i=0;i<SF_RW_DIRECT_BLOCKS;i++){
  if(!ip->direct[i])continue;
  block_release(s,ip->direct[i]);usage_release(s,ip->uid,1,0);
 }
 return 0;
}

static int dir_find(Sfrw*s,uint32_t dir,const char*name,uint32_t*out,uint8_t*type,uint32_t*slot_block,uint32_t*slot_index){
 Inode ip;int r=inode_read(s,dir,&ip);if(r)return r;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)return SF_INVALID;
 uint32_t blocks=(uint32_t)(ip.size/SF_RW_BLOCK_BYTES);size_t len=strlen(name);
 for(uint32_t lb=0;lb<blocks&&lb<SF_RW_FILE_BLOCKS;lb++){
  uint32_t blk;r=block_map(s,&ip,lb,&blk);if(r)return r;if(!blk)return SF_BAD_FORMAT;
  r=meta_read(s,blk,s->block);if(r)return r;
  for(uint32_t i=0;i<SF_RW_BLOCK_BYTES/128;i++){
   const uint8_t*e=s->block+i*128;uint32_t ino=g32(e);if(!ino)continue;
   uint32_t n=e[4];if(n!=len||n>SF_RW_NAME_MAX)continue;
   if(!memcmp(e+8,name,len)){
    if(out)*out=ino;if(type)*type=e[5];
    if(slot_block)*slot_block=blk;if(slot_index)*slot_index=i;
    return 0;
   }
  }
 }
 return SF_NOT_FOUND;
}
static int dir_remove(Sfrw*s,uint32_t dir,const char*name){
 uint32_t blk,index;int r=dir_find(s,dir,name,0,0,&blk,&index);if(r)return r;
 r=meta_read(s,blk,s->block);if(r)return r;
 memset(s->block+index*128,0,128);
 return meta_write(s,blk,s->block);
}
static int dir_add(Sfrw*s,uint32_t dir,const char*name,uint32_t child,uint8_t type){
 Inode ip;int r=inode_read(s,dir,&ip);if(r)return r;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)return SF_INVALID;
 uint32_t blocks=(uint32_t)(ip.size/SF_RW_BLOCK_BYTES);size_t len=strlen(name);
 for(uint32_t lb=0;lb<blocks&&lb<SF_RW_FILE_BLOCKS;lb++){
  uint32_t blk;r=block_map(s,&ip,lb,&blk);if(r)return r;if(!blk)return SF_BAD_FORMAT;
  r=meta_read(s,blk,s->block);if(r)return r;
  for(uint32_t i=0;i<SF_RW_BLOCK_BYTES/128;i++){
   uint8_t*e=s->block+i*128;
   if(g32(e))continue;
   memset(e,0,128);p32(e,child);e[4]=(uint8_t)len;e[5]=type;memcpy(e+8,name,len);
   return meta_write(s,blk,s->block);
  }
 }
 if(blocks>=SF_RW_FILE_BLOCKS)return SF_NO_SPACE;
 uint32_t need=1+((blocks>=SF_RW_DIRECT_BLOCKS&&!ip.indirect)?1:0);
 if(s->free_blocks<need)return SF_NO_SPACE;
 r=usage_reserve(s,ip.uid,need,0);if(r)return r;
 uint32_t data=block_alloc(s);
 if(!data){usage_release(s,ip.uid,need,0);return SF_NO_SPACE;}
 memset(s->block,0,SF_RW_BLOCK_BYTES);
 p32(s->block,child);s->block[4]=(uint8_t)len;s->block[5]=type;memcpy(s->block+8,name,len);
 if(blocks>=SF_RW_DIRECT_BLOCKS&&!ip.indirect){
  uint32_t ind=block_alloc(s);
  if(!ind){block_release(s,data);usage_release(s,ip.uid,need,0);return SF_NO_SPACE;}
  memset(s->indirect,0,SF_RW_BLOCK_BYTES);
  r=meta_write(s,ind,s->indirect);if(r)return r;
  ip.indirect=ind;
 }
 if(blocks<SF_RW_DIRECT_BLOCKS)ip.direct[blocks]=data;
 else{
  r=meta_read(s,ip.indirect,s->indirect);if(r)return r;
  p32(s->indirect+(blocks-SF_RW_DIRECT_BLOCKS)*4,data);
  r=meta_write(s,ip.indirect,s->indirect);if(r)return r;
 }
 ip.size=(uint64_t)(blocks+1)*SF_RW_BLOCK_BYTES;
 r=meta_write(s,data,s->block);if(r)return r;
 return inode_write(s,dir,&ip);
}
static int dir_can_add(Sfrw*s,uint32_t dir,uint32_t*need_out){
 Inode ip;int r=inode_read(s,dir,&ip);if(r)return r;
 uint32_t blocks=(uint32_t)(ip.size/SF_RW_BLOCK_BYTES);
 *need_out=0;
 for(uint32_t lb=0;lb<blocks&&lb<SF_RW_FILE_BLOCKS;lb++){
  uint32_t blk;r=block_map(s,&ip,lb,&blk);if(r)return r;if(!blk)return SF_BAD_FORMAT;
  r=meta_read(s,blk,s->block);if(r)return r;
  for(uint32_t i=0;i<SF_RW_BLOCK_BYTES/128;i++)if(!g32(s->block+i*128))return 0;
 }
 if(blocks>=SF_RW_FILE_BLOCKS)return SF_NO_SPACE;
 uint32_t need=1+((blocks>=SF_RW_DIRECT_BLOCKS&&!ip.indirect)?1:0);
 if(s->free_blocks<need)return SF_NO_SPACE;
 r=usage_check(s,ip.uid,need,0);if(r)return r;
 *need_out=need;return 0;
}
static int resolve_dir(Sfrw*s,const char*path,uint32_t uid,uint32_t forbidden,uint32_t*out){
 if(!path_ok(path))return SF_INVALID;
 uint32_t ino=SF_RW_ROOT;size_t n=strlen(path),i=1;
 if(ino==forbidden)return SF_INVALID;
 while(i<n){
  size_t j=i;while(j<n&&path[j]!='/')j++;
  char name[SF_RW_NAME_MAX+1];size_t len=j-i;memcpy(name,path+i,len);name[len]=0;
  Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
  if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)return SF_NOT_FOUND;
  if(!allowed(&ip,uid,1))return SF_DENIED;
  r=dir_find(s,ino,name,&ino,0,0,0);if(r)return r;
  if(ino==forbidden)return SF_INVALID;
  i=j+1;
 }
 Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)return SF_NOT_FOUND;
 *out=ino;return 0;
}
static int resolve_parent(Sfrw*s,const char*path,uint32_t uid,uint32_t*pino,char*name){
 char parent[SF_RW_PATH_MAX+1];
 if(!path_parent(path,parent,name))return SF_INVALID;
 return resolve_dir(s,parent,uid,0,pino);
}
static int inode_alloc(Sfrw*s,uint32_t*out){
 for(uint32_t ino=2;ino<SF_RW_MAX_INODES;ino++){
  Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
  if(!ip.mode){*out=ino;return 0;}
 }
 return SF_NO_SPACE;
}
static int usage_add(Sfrw*s,uint32_t uid,uint32_t blocks,uint32_t inodes){
 SfrwUsage*u=usage_slot(s,uid,1);if(!u)return SF_NO_SPACE;
 u->blocks+=blocks;u->inodes+=inodes;return 0;
}
static int validate_inode(Sfrw*s,uint32_t ino,const Inode*ip){
 uint32_t type=ip->mode&SF_RW_TYPE_MASK;
 if(type!=SF_RW_TYPE_DIR&&type!=SF_RW_TYPE_FILE)return SF_BAD_FORMAT;
 if(!ip->links||ip->size>SF_RW_FILE_MAX)return SF_BAD_FORMAT;
 uint32_t need=ip->size?(uint32_t)((ip->size+SF_RW_BLOCK_BYTES-1)/SF_RW_BLOCK_BYTES):0;
 if(type==SF_RW_TYPE_DIR&&(ip->size<SF_RW_BLOCK_BYTES||ip->size%SF_RW_BLOCK_BYTES))return SF_BAD_FORMAT;
 for(uint32_t i=0;i<SF_RW_DIRECT_BLOCKS;i++){
  uint32_t b=ip->direct[i];if(!b)continue;
  if(i>=need||b<SF_RW_DATA_BLOCK||b>=s->block_count||!bit_get(s,b))return SF_BAD_FORMAT;
 }
 if(ip->indirect){
  uint32_t b=ip->indirect;
  if(b<SF_RW_DATA_BLOCK||b>=s->block_count||!bit_get(s,b))return SF_BAD_FORMAT;
  int r=meta_read(s,b,s->indirect);if(r)return r;
  for(uint32_t i=0;i<SF_RW_INDIRECT_ENTRIES;i++){
   uint32_t e=g32(s->indirect+4*i);if(!e)continue;
   if(SF_RW_DIRECT_BLOCKS+i>=need||e<SF_RW_DATA_BLOCK||e>=s->block_count||!bit_get(s,e))return SF_BAD_FORMAT;
  }
 }
 if(type==SF_RW_TYPE_DIR){
  for(uint32_t lb=0;lb<need;lb++){
   uint32_t b;int r=block_map(s,ip,lb,&b);if(r)return r;if(!b)return SF_BAD_FORMAT;
  }
 }
 (void)ino;return 0;
}

int sf_rw_format(Sfrw*s,SfrwDev dev,const SfrwFormatOptions*options){
 if(!s||!dev.read||!dev.write||!dev.flush||!options)return SF_INVALID;
 uint32_t blocks=options->block_count;
 if(blocks<SF_RW_MIN_BLOCKS||blocks>SF_RW_MAX_BLOCKS)return SF_INVALID;
 memset(s,0,sizeof(*s));s->dev=dev;s->block_count=blocks;s->generation=1;
 s->default_quota_blocks=options->default_quota_blocks;
 s->default_quota_inodes=options->default_quota_inodes;
 memset(s->bitmap,0,SF_RW_BITMAP_BYTES);
 for(uint32_t b=0;b<=SF_RW_DATA_BLOCK;b++)bit_set(s,b,1);
 s->free_blocks=blocks-SF_RW_DATA_BLOCK-1;
 s->free_inodes=SF_RW_MAX_INODES-2;
 int r=0;
 for(uint32_t b=SF_RW_INODE_TABLE_BLOCK;b<SF_RW_INODE_TABLE_BLOCK+SF_RW_INODE_TABLE_BLOCKS;b++)
  if((r=dev.write(dev.context,b,zeros)))return r;
 if((r=dev.write(dev.context,SF_RW_BITMAP_BLOCK,s->bitmap)))return r;
 if((r=dev.write(dev.context,SF_RW_BITMAP_BLOCK+1,s->bitmap+SF_RW_BLOCK_BYTES)))return r;
 if((r=dev.write(dev.context,SF_RW_JOURNAL_BLOCK,zeros)))return r;
 memset(s->block,0,SF_RW_BLOCK_BYTES);
 p32(s->block,SF_RW_ROOT);s->block[4]=1;s->block[5]=SF_RW_ENTRY_DIR;s->block[8]='.';
 p32(s->block+128,SF_RW_ROOT);s->block[132]=2;s->block[133]=SF_RW_ENTRY_DIR;s->block[136]='.';s->block[137]='.';
 if((r=dev.write(dev.context,SF_RW_DATA_BLOCK,s->block)))return r;
 memset(s->aux,0,SF_RW_BLOCK_BYTES);
 {
  Inode root;memset(&root,0,sizeof(root));
  root.mode=SF_RW_TYPE_DIR|0755;root.links=2;root.size=SF_RW_BLOCK_BYTES;root.direct[0]=SF_RW_DATA_BLOCK;
  inode_encode(s->aux+SF_RW_INODE_BYTES,&root);
 }
 if((r=dev.write(dev.context,SF_RW_INODE_TABLE_BLOCK,s->aux)))return r;
 super_build(s,s->block,1);
 if((r=dev.write(dev.context,0,s->block)))return r;
 if((r=dev.write(dev.context,1,s->block)))return r;
 if((r=dev.flush(dev.context)))return r;
 s->journal_clean=1;s->journal_count=0;s->dirty=0;s->mounted=0;return 0;
}

int sf_rw_mount(Sfrw*s,SfrwDev dev){
 if(!s||!dev.read||!dev.write||!dev.flush)return SF_INVALID;
 memset(s,0,sizeof(*s));s->dev=dev;s->mounted=0;
 uint8_t b[SF_RW_BLOCK_BYTES];
 int r=dev.read(dev.context,0,b);if(!r)r=super_load(s,b);
 if(r){r=dev.read(dev.context,1,b);if(r)return r;r=super_load(s,b);}
 if(r)return SF_BAD_FORMAT;
 r=journal_replay(s);if(r)return r;
 r=dev.read(dev.context,0,b);if(!r)r=super_load(s,b);
 if(r){r=dev.read(dev.context,1,b);if(r)return r;r=super_load(s,b);}
 if(r)return SF_BAD_FORMAT;
 r=dev.read(dev.context,SF_RW_BITMAP_BLOCK,s->bitmap);
 if(!r)r=dev.read(dev.context,SF_RW_BITMAP_BLOCK+1,s->bitmap+SF_RW_BLOCK_BYTES);
 if(r)return r;
 for(uint32_t i=0;i<SF_RW_DATA_BLOCK;i++)if(!bit_get(s,i))return SF_BAD_FORMAT;
 for(uint32_t i=s->block_count;i<SF_RW_MAX_BLOCKS;i++)if(bit_get(s,i))return SF_BAD_FORMAT;
 if(bitmap_free(s)!=s->free_blocks)return SF_BAD_FORMAT;
 uint32_t free_inodes=0;
 /* One block read per 32 inodes: identical verification to inode_read() in a
  * loop, but avoids re-reading the same table block for every inode. */
 for(uint32_t blk=0;blk<SF_RW_INODE_TABLE_BLOCKS;blk++){
  r=meta_read(s,SF_RW_INODE_TABLE_BLOCK+blk,s->aux);if(r)return r;
  for(uint32_t slot=0;slot<SF_RW_INODES_PER_BLOCK;slot++){
   uint32_t ino=blk*SF_RW_INODES_PER_BLOCK+slot;
   if(!ino)continue;
   if(ino>=SF_RW_MAX_INODES)break;
   Inode ip;inode_decode(s->aux+slot*SF_RW_INODE_BYTES,&ip);
   if(!ip.mode){free_inodes++;continue;}
   r=validate_inode(s,ino,&ip);if(r)return r;
   uint32_t n;r=inode_block_count(s,&ip,&n);if(r)return r;
   r=usage_add(s,ip.uid,n,1);if(r)return r;
  }
 }
 if(free_inodes!=s->free_inodes)return SF_BAD_FORMAT;
 Inode root;r=inode_read(s,SF_RW_ROOT,&root);if(r)return r;
 if((root.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)return SF_BAD_FORMAT;
 s->mounted=1;s->dirty=0;s->journal_count=0;return 0;
}

int sf_rw_fsync(Sfrw*s){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(s->dev.flush(s->dev.context)){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 return 0;
}
static int fail_after(Sfrw*s,int r){
 if(s->dirty){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 return r;
}
int sf_rw_open(Sfrw*s,const char*path,uint32_t uid,uint32_t flags,uint32_t*out){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!path||!out||!flags||(flags&~(SF_RW_READ|SF_RW_WRITE)))return SF_INVALID;
 if(!path_ok(path))return SF_INVALID;
 if(!strcmp(path,"/")){
  if(flags&SF_RW_WRITE)return SF_INVALID;
  Inode ip;int r=inode_read(s,SF_RW_ROOT,&ip);if(r)return r;
  if(!allowed(&ip,uid,4))return SF_DENIED;
  *out=SF_RW_ROOT;return 0;
 }
 char name[SF_RW_NAME_MAX+1];
 uint32_t pino;int r=resolve_parent(s,path,uid,&pino,name);if(r)return r;
 Inode pip;r=inode_read(s,pino,&pip);if(r)return r;
 if(!allowed(&pip,uid,1))return SF_DENIED;
 uint32_t ino;r=dir_find(s,pino,name,&ino,0,0,0);if(r)return r;
 Inode ip;r=inode_read(s,ino,&ip);if(r)return r;
 if(!ip.mode)return SF_NOT_FOUND;
 if(flags&SF_RW_READ)if(!allowed(&ip,uid,4))return SF_DENIED;
 if(flags&SF_RW_WRITE){
  if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_FILE)return SF_INVALID;
  if(!allowed(&ip,uid,2))return SF_DENIED;
 }
 *out=ino;return 0;
}
int sf_rw_create(Sfrw*s,const char*path,uint32_t uid,uint16_t mode){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!path||!path_ok(path))return SF_INVALID;
 char name[SF_RW_NAME_MAX+1];
 uint32_t pino;int r=resolve_parent(s,path,uid,&pino,name);if(r)return r;
 Inode pip;r=inode_read(s,pino,&pip);if(r)return r;
 if(!allowed(&pip,uid,3))return SF_DENIED;
 r=dir_find(s,pino,name,0,0,0,0);
 if(r==0)return SF_INVALID;
 if(r!=SF_NOT_FOUND)return r;
 uint32_t grow;r=dir_can_add(s,pino,&grow);if(r)return r;
 if(s->free_blocks<grow)return SF_NO_SPACE;
 uint32_t ino;r=inode_alloc(s,&ino);if(r)return r;
 r=usage_reserve(s,uid,0,1);if(r)return r;
 r=dir_add(s,pino,name,ino,SF_RW_ENTRY_FILE);
 if(r){usage_release(s,uid,0,1);return fail_after(s,r);}
 Inode ip;memset(&ip,0,sizeof(ip));
 ip.mode=(uint16_t)(SF_RW_TYPE_FILE|(mode&SF_RW_PERM_MASK));ip.links=1;ip.uid=uid;ip.gid=uid;
 r=inode_write(s,ino,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 s->free_inodes--;
 return commit(s);
}
int sf_rw_mkdir(Sfrw*s,const char*path,uint32_t uid,uint16_t mode){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!path||!path_ok(path))return SF_INVALID;
 char name[SF_RW_NAME_MAX+1];
 uint32_t pino;int r=resolve_parent(s,path,uid,&pino,name);if(r)return r;
 Inode pip;r=inode_read(s,pino,&pip);if(r)return r;
 if(!allowed(&pip,uid,3))return SF_DENIED;
 r=dir_find(s,pino,name,0,0,0,0);
 if(r==0)return SF_INVALID;
 if(r!=SF_NOT_FOUND)return r;
 uint32_t ino;r=inode_alloc(s,&ino);if(r)return r;
 uint32_t grow;r=dir_can_add(s,pino,&grow);if(r)return r;
 if(s->free_blocks<grow+1)return SF_NO_SPACE;
 r=usage_reserve(s,uid,1,1);if(r)return r;
 uint32_t blk=block_alloc(s);
 if(!blk){usage_release(s,uid,1,1);return SF_NO_SPACE;}
 r=dir_add(s,pino,name,ino,SF_RW_ENTRY_DIR);
 if(r){block_release(s,blk);usage_release(s,uid,1,1);return fail_after(s,r);}
 memset(s->block,0,SF_RW_BLOCK_BYTES);
 p32(s->block,ino);s->block[4]=1;s->block[5]=SF_RW_ENTRY_DIR;s->block[8]='.';
 p32(s->block+128,pino);s->block[132]=2;s->block[133]=SF_RW_ENTRY_DIR;s->block[136]='.';s->block[137]='.';
 r=meta_write(s,blk,s->block);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 Inode ip;memset(&ip,0,sizeof(ip));
 ip.mode=(uint16_t)(SF_RW_TYPE_DIR|(mode&SF_RW_PERM_MASK));ip.links=2;ip.uid=uid;ip.gid=uid;
 ip.size=SF_RW_BLOCK_BYTES;ip.direct[0]=blk;
 r=inode_write(s,ino,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 Inode pip2;r=inode_read(s,pino,&pip2);if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 pip2.links++;
 r=inode_write(s,pino,&pip2);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 s->free_inodes--;
 return commit(s);
}
int sf_rw_stat(Sfrw*s,uint32_t ino,SfrwStat*st){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!st)return SF_INVALID;
 Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
 if(!ip.mode)return SF_NOT_FOUND;
 uint32_t n;r=inode_block_count(s,&ip,&n);if(r)return r;
 st->inode=ino;st->mode=ip.mode;st->uid=ip.uid;st->gid=ip.gid;st->links=ip.links;
 st->blocks=n;st->size=ip.size;return 0;
}
int sf_rw_readdir(Sfrw*s,uint32_t dir,uint32_t uid,uint32_t index,char name[SF_RW_NAME_MAX+1],uint32_t*out,uint8_t*type){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!name)return SF_INVALID;
 Inode ip;int r=inode_read(s,dir,&ip);if(r)return r;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)return SF_INVALID;
 if(!allowed(&ip,uid,4))return SF_DENIED;
 uint32_t blocks=(uint32_t)(ip.size/SF_RW_BLOCK_BYTES),seen=0;
 for(uint32_t lb=0;lb<blocks&&lb<SF_RW_FILE_BLOCKS;lb++){
  uint32_t blk;r=block_map(s,&ip,lb,&blk);if(r)return r;if(!blk)return SF_BAD_FORMAT;
  r=meta_read(s,blk,s->block);if(r)return r;
  for(uint32_t i=0;i<SF_RW_BLOCK_BYTES/128;i++){
   const uint8_t*e=s->block+i*128;uint32_t ino=g32(e);if(!ino)continue;
   if(seen++!=index)continue;
   uint32_t n=e[4];if(n>SF_RW_NAME_MAX)n=0;
   memcpy(name,e+8,n);name[n]=0;
   if(out)*out=ino;if(type)*type=e[5];
   return 0;
  }
 }
 return SF_NOT_FOUND;
}
long sf_rw_read(Sfrw*s,uint32_t ino,uint32_t uid,uint64_t offset,void*out,size_t len){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!out&&len)return SF_INVALID;
 Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
 if(!ip.mode)return SF_NOT_FOUND;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_FILE)return SF_INVALID;
 if(!allowed(&ip,uid,4))return SF_DENIED;
 if(offset>=ip.size)return 0;
 uint64_t available=ip.size-offset;
 if((uint64_t)len>available)len=(size_t)available;
 uint8_t*o=out;size_t done=0;
 while(done<len){
  uint64_t pos=offset+done;uint32_t lb=(uint32_t)(pos/SF_RW_BLOCK_BYTES);
  uint32_t bo=(uint32_t)(pos%SF_RW_BLOCK_BYTES);size_t n=SF_RW_BLOCK_BYTES-bo;
  if(n>len-done)n=len-done;
  uint32_t blk;r=block_map(s,&ip,lb,&blk);if(r)return r;
  if(blk){
   r=meta_read(s,blk,s->block);if(r)return r;
   memcpy(o+done,s->block+bo,n);
  }else memset(o+done,0,n);
  done+=n;
 }
 return (long)done;
}
long sf_rw_write(Sfrw*s,uint32_t ino,uint32_t uid,uint64_t offset,const void*data,size_t len){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!data&&len)return SF_INVALID;
 Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
 if(!ip.mode)return SF_NOT_FOUND;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_FILE)return SF_INVALID;
 if(!allowed(&ip,uid,2))return SF_DENIED;
 if(!len)return 0;
 if(offset>SF_RW_FILE_MAX||(uint64_t)len>SF_RW_FILE_MAX-offset)return SF_NO_SPACE;
 uint32_t first=(uint32_t)(offset/SF_RW_BLOCK_BYTES);
 uint32_t last=(uint32_t)((offset+(uint64_t)len-1)/SF_RW_BLOCK_BYTES);
 uint32_t count=last-first+1,add=0,new_indirect=0;
 for(uint32_t i=0;i<count;i++){
  r=block_map(s,&ip,first+i,&s->old_block[i]);if(r)return r;
  if(!s->old_block[i])add++;
 }
 if(first+count>SF_RW_DIRECT_BLOCKS&&!ip.indirect)new_indirect=1;
 if(s->free_blocks<count+new_indirect)return SF_NO_SPACE;
 r=usage_check(s,ip.uid,add+new_indirect,0);if(r)return r;
 r=usage_reserve(s,ip.uid,add+new_indirect,0);if(r)return r;
 uint32_t allocated=0,ind=ip.indirect;
 for(uint32_t i=0;i<count;i++){
  uint32_t b=block_alloc(s);
  if(!b){
   for(uint32_t j=0;j<allocated;j++)block_release(s,s->new_block[j]);
   usage_release(s,ip.uid,add+new_indirect,0);return SF_NO_SPACE;
  }
  s->new_block[i]=b;allocated++;
 }
 if(new_indirect){
  ind=block_alloc(s);
  if(!ind){
   for(uint32_t j=0;j<allocated;j++)block_release(s,s->new_block[j]);
   usage_release(s,ip.uid,add+new_indirect,0);return SF_NO_SPACE;
  }
 }
 for(uint32_t i=0;i<count;i++){
  uint64_t start=(uint64_t)(first+i)*SF_RW_BLOCK_BYTES;
  uint64_t begin=offset>start?offset:start;
  uint64_t end=(offset+(uint64_t)len)<(start+SF_RW_BLOCK_BYTES)?(offset+(uint64_t)len):(start+SF_RW_BLOCK_BYTES);
  size_t inoff=(size_t)(begin-offset),bo=(size_t)(begin-start),n=(size_t)(end-begin);
  if(!bo&&n==SF_RW_BLOCK_BYTES)memcpy(s->block,(const uint8_t*)data+inoff,SF_RW_BLOCK_BYTES);
  else{
   if(s->old_block[i]){
    r=s->dev.read(s->dev.context,s->old_block[i],s->block);
    if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
   }else memset(s->block,0,SF_RW_BLOCK_BYTES);
   memcpy(s->block+bo,(const uint8_t*)data+inoff,n);
  }
  if(s->dev.write(s->dev.context,s->new_block[i],s->block)){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 }
 if(s->dev.flush(s->dev.context)){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 if(first+count>SF_RW_DIRECT_BLOCKS){
  if(new_indirect){ip.indirect=ind;memset(s->indirect,0,SF_RW_BLOCK_BYTES);}
  else{
   r=meta_read(s,ip.indirect,s->indirect);
   if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
  }
  for(uint32_t i=0;i<count;i++){
   uint32_t lb=first+i;
   if(lb<SF_RW_DIRECT_BLOCKS)ip.direct[lb]=s->new_block[i];
   else p32(s->indirect+(lb-SF_RW_DIRECT_BLOCKS)*4,s->new_block[i]);
  }
  r=meta_write(s,ip.indirect,s->indirect);
  if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 }else{
  for(uint32_t i=0;i<count;i++)ip.direct[first+i]=s->new_block[i];
 }
 for(uint32_t i=0;i<count;i++)if(s->old_block[i])block_release(s,s->old_block[i]);
 if(offset+(uint64_t)len>ip.size)ip.size=offset+(uint64_t)len;
 r=inode_write(s,ino,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 r=commit(s);
 return r?r:(long)len;
}
int sf_rw_truncate(Sfrw*s,uint32_t ino,uint32_t uid,uint64_t bytes){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(bytes>SF_RW_FILE_MAX)return SF_NO_SPACE;
 Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
 if(!ip.mode)return SF_NOT_FOUND;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_FILE)return SF_INVALID;
 if(!allowed(&ip,uid,2))return SF_DENIED;
 if(bytes==ip.size)return 0;
 uint32_t old_need=ip.size?(uint32_t)((ip.size+SF_RW_BLOCK_BYTES-1)/SF_RW_BLOCK_BYTES):0;
 uint32_t new_need=bytes?(uint32_t)((bytes+SF_RW_BLOCK_BYTES-1)/SF_RW_BLOCK_BYTES):0;
 if(bytes<ip.size){
  if(ip.indirect){
   r=meta_read(s,ip.indirect,s->indirect);
   if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
  }
  for(uint32_t lb=new_need;lb<old_need;lb++){
   uint32_t b=lb<SF_RW_DIRECT_BLOCKS?ip.direct[lb]:(ip.indirect?g32(s->indirect+(lb-SF_RW_DIRECT_BLOCKS)*4):0);
   if(!b)continue;
   if(lb<SF_RW_DIRECT_BLOCKS)ip.direct[lb]=0;
   else p32(s->indirect+(lb-SF_RW_DIRECT_BLOCKS)*4,0);
   block_release(s,b);usage_release(s,ip.uid,1,0);
  }
  if(new_need<=SF_RW_DIRECT_BLOCKS&&ip.indirect){
   block_release(s,ip.indirect);usage_release(s,ip.uid,1,0);ip.indirect=0;
  }else if(ip.indirect){
   r=meta_write(s,ip.indirect,s->indirect);
   if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
  }
 }
 ip.size=bytes;
 r=inode_write(s,ino,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 return commit(s);
}
int sf_rw_unlink(Sfrw*s,const char*path,uint32_t uid){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!path||!path_ok(path))return SF_INVALID;
 char name[SF_RW_NAME_MAX+1];
 uint32_t pino;int r=resolve_parent(s,path,uid,&pino,name);if(r)return r;
 Inode pip;r=inode_read(s,pino,&pip);if(r)return r;
 if(!allowed(&pip,uid,3))return SF_DENIED;
 uint32_t ino;uint8_t type;
 r=dir_find(s,pino,name,&ino,&type,0,0);if(r)return r;
 if(type==SF_RW_ENTRY_DIR)return SF_INVALID;
 Inode ip;r=inode_read(s,ino,&ip);if(r)return r;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_FILE)return SF_INVALID;
 /* 硬链接：仅最后一个链接释放数据块与 inode。 */
 if(ip.links>1){
  ip.links--;
  r=inode_write(s,ino,&ip);
  if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
  r=dir_remove(s,pino,name);
  if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
  return commit(s);
 }
 uint32_t owner=ip.uid;
 r=free_inode_blocks(s,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 Inode empty;memset(&empty,0,sizeof(empty));
 r=inode_write(s,ino,&empty);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 s->free_inodes++;
 r=dir_remove(s,pino,name);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 usage_release(s,owner,0,1);
 return commit(s);
}
static int dir_empty(Sfrw*s,uint32_t dir){
 Inode ip;int r=inode_read(s,dir,&ip);if(r)return r;
 uint32_t blocks=(uint32_t)(ip.size/SF_RW_BLOCK_BYTES);
 for(uint32_t lb=0;lb<blocks&&lb<SF_RW_FILE_BLOCKS;lb++){
  uint32_t blk;r=block_map(s,&ip,lb,&blk);if(r)return r;if(!blk)return SF_BAD_FORMAT;
  r=meta_read(s,blk,s->block);if(r)return r;
  for(uint32_t i=0;i<SF_RW_BLOCK_BYTES/128;i++){
   const uint8_t*e=s->block+i*128;if(!g32(e))continue;
   uint32_t n=e[4];
   if(n==1&&e[8]=='.')continue;
   if(n==2&&e[8]=='.'&&e[9]=='.')continue;
   return 0;
  }
 }
 return 1;
}
int sf_rw_rmdir(Sfrw*s,const char*path,uint32_t uid){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!path||!path_ok(path))return SF_INVALID;
 char name[SF_RW_NAME_MAX+1];
 uint32_t pino;int r=resolve_parent(s,path,uid,&pino,name);if(r)return r;
 Inode pip;r=inode_read(s,pino,&pip);if(r)return r;
 if(!allowed(&pip,uid,3))return SF_DENIED;
 uint32_t ino;uint8_t type;
 r=dir_find(s,pino,name,&ino,&type,0,0);if(r)return r;
 if(ino==SF_RW_ROOT||type!=SF_RW_ENTRY_DIR)return SF_INVALID;
 Inode ip;r=inode_read(s,ino,&ip);if(r)return r;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)return SF_INVALID;
 r=dir_empty(s,ino);if(r<0)return r;if(!r)return SF_INVALID;
 uint32_t owner=ip.uid;
 r=free_inode_blocks(s,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 Inode empty;memset(&empty,0,sizeof(empty));
 r=inode_write(s,ino,&empty);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 s->free_inodes++;
 r=dir_remove(s,pino,name);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 if(pip.links)pip.links--;
 r=inode_write(s,pino,&pip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 usage_release(s,owner,0,1);
 return commit(s);
}
int sf_rw_rename(Sfrw*s,const char*from,const char*to,uint32_t uid){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!from||!to||!path_ok(from)||!path_ok(to))return SF_INVALID;
 char fn[SF_RW_NAME_MAX+1],tn[SF_RW_NAME_MAX+1],tp[SF_RW_PATH_MAX+1];
 uint32_t fpino,tpino;
 if(!path_parent(to,tp,tn))return SF_INVALID;
 int r=resolve_parent(s,from,uid,&fpino,fn);if(r)return r;
 r=resolve_parent(s,to,uid,&tpino,tn);if(r)return r;
 Inode fdp;r=inode_read(s,fpino,&fdp);if(r)return r;
 if(!allowed(&fdp,uid,3))return SF_DENIED;
 if(tpino!=fpino){
  Inode tdp;r=inode_read(s,tpino,&tdp);if(r)return r;
  if(!allowed(&tdp,uid,3))return SF_DENIED;
 }
 if(!strcmp(from,to))return 0;
 uint32_t src;uint8_t stype;
 r=dir_find(s,fpino,fn,&src,&stype,0,0);if(r)return r;
 Inode sip;r=inode_read(s,src,&sip);if(r)return r;
 int src_dir=(sip.mode&SF_RW_TYPE_MASK)==SF_RW_TYPE_DIR;
 if(src_dir&&src==SF_RW_ROOT)return SF_INVALID;
 uint32_t dst=0;uint8_t dtype=0;int dst_exists=0,dst_dir=0;
 int dr=dir_find(s,tpino,tn,&dst,&dtype,0,0);
 if(dr!=0&&dr!=SF_NOT_FOUND)return dr;
 if(dr==0){
  dst_exists=1;
  if(dst==src)return 0;
  Inode dip;r=inode_read(s,dst,&dip);if(r)return r;
  dst_dir=(dip.mode&SF_RW_TYPE_MASK)==SF_RW_TYPE_DIR;
  if(src_dir){
   if(!dst_dir)return SF_INVALID;
   r=dir_empty(s,dst);if(r<0)return r;if(!r)return SF_INVALID;
  }else if(dst_dir)return SF_INVALID;
 }
 if(src_dir){
  r=resolve_dir(s,tp,uid,src,&tpino);if(r)return r;
 }
 int same_dir=fpino==tpino;
 uint32_t grow=0;
 if(!same_dir&&!dst_exists){
  r=dir_can_add(s,tpino,&grow);if(r)return r;
  if(s->free_blocks<grow)return SF_NO_SPACE;
 }
 r=dir_remove(s,fpino,fn);if(r)return r;
 if(dst_exists){
  r=dir_remove(s,tpino,tn);if(r)return r;
  Inode dip;r=inode_read(s,dst,&dip);if(r)goto fatal;
  if(!dst_dir&&dip.links>1){
   /* 被替换文件仍有其他硬链接：只递减链接计数。 */
   dip.links--;
   r=inode_write(s,dst,&dip);if(r)goto fatal;
  }else{
   uint32_t downer=dip.uid;
   r=free_inode_blocks(s,&dip);if(r)goto fatal;
   Inode empty;memset(&empty,0,sizeof(empty));
   r=inode_write(s,dst,&empty);if(r)goto fatal;
   s->free_inodes++;
   usage_release(s,downer,0,1);
  }
 }
 r=dir_add(s,tpino,tn,src,src_dir?SF_RW_ENTRY_DIR:SF_RW_ENTRY_FILE);
 if(r)goto fatal;
 if(src_dir){
  Inode current;r=inode_read(s,src,&current);if(r)goto fatal;
  uint32_t blk;r=block_map(s,&current,0,&blk);
  if(r||!blk)goto fatal;
  r=meta_read(s,blk,s->block);if(r)goto fatal;
  int updated=0;
  for(uint32_t i=0;i<SF_RW_BLOCK_BYTES/128;i++){
   uint8_t*e=s->block+i*128;
   if(e[4]==2&&e[8]=='.'&&e[9]=='.'){
    p32(e,tpino);e[5]=SF_RW_ENTRY_DIR;updated=1;break;
   }
  }
  if(!updated)goto fatal;
  r=meta_write(s,blk,s->block);if(r)goto fatal;
  if(!same_dir){
   Inode f;r=inode_read(s,fpino,&f);if(r)goto fatal;
   if(f.links)f.links--;
   r=inode_write(s,fpino,&f);if(r)goto fatal;
   if(!(dst_exists&&dst_dir)){
    Inode t;r=inode_read(s,tpino,&t);if(r)goto fatal;
    t.links++;
    r=inode_write(s,tpino,&t);if(r)goto fatal;
   }
  }
 }
 return commit(s);
fatal:
 s->mounted=0;return SF_COMMIT_UNCERTAIN;
}
int sf_rw_link(Sfrw*s,const char*existing,const char*newpath,uint32_t uid){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(!existing||!newpath||!path_ok(existing)||!path_ok(newpath))return SF_INVALID;
 char xn[SF_RW_NAME_MAX+1],nn[SF_RW_NAME_MAX+1];
 uint32_t xpino,npino;
 int r=resolve_parent(s,existing,uid,&xpino,xn);if(r)return r;
 r=resolve_parent(s,newpath,uid,&npino,nn);if(r)return r;
 Inode ndp;r=inode_read(s,npino,&ndp);if(r)return r;
 if(!allowed(&ndp,uid,3))return SF_DENIED;
 uint32_t src;uint8_t stype;
 r=dir_find(s,xpino,xn,&src,&stype,0,0);if(r)return r;
 if(stype!=SF_RW_ENTRY_FILE)return SF_INVALID;
 Inode ip;r=inode_read(s,src,&ip);if(r)return r;
 if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_FILE)return SF_INVALID;
 r=dir_find(s,npino,nn,0,0,0,0);
 if(r==0)return SF_INVALID;
 if(r!=SF_NOT_FOUND)return r;
 uint32_t grow;r=dir_can_add(s,npino,&grow);if(r)return r;
 if(s->free_blocks<grow)return SF_NO_SPACE;
 r=dir_add(s,npino,nn,src,SF_RW_ENTRY_FILE);
 if(r)return fail_after(s,r);
 ip.links++;
 r=inode_write(s,src,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 return commit(s);
}
int sf_rw_chmod(Sfrw*s,uint32_t ino,uint32_t uid,uint16_t mode){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(mode&~SF_RW_PERM_MASK)return SF_INVALID;
 Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
 if(!ip.mode)return SF_NOT_FOUND;
 if(uid!=0&&ip.uid!=uid)return SF_DENIED;
 if((ip.mode&SF_RW_PERM_MASK)==mode)return 0;
 ip.mode=(uint16_t)((ip.mode&SF_RW_TYPE_MASK)|mode);
 r=inode_write(s,ino,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 return commit(s);
}
int sf_rw_chown(Sfrw*s,uint32_t ino,uint32_t uid,uint32_t new_uid,uint32_t new_gid){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(uid!=0)return SF_DENIED;
 Inode ip;int r=inode_read(s,ino,&ip);if(r)return r;
 if(!ip.mode)return SF_NOT_FOUND;
 if(ip.uid==new_uid&&ip.gid==new_gid)return 0;
 if(new_uid!=ip.uid){
  uint32_t n;r=inode_block_count(s,&ip,&n);if(r)return r;
  /* 先按目标 uid 校验配额并确认可跟踪，再迁移占用。 */
  r=usage_check(s,new_uid,n,1);if(r)return r;
  SfrwUsage*nu=usage_slot(s,new_uid,1);if(!nu)return SF_NO_SPACE;
  usage_release(s,ip.uid,n,1);
  nu->blocks+=n;nu->inodes+=1;
  ip.uid=new_uid;
 }
 ip.gid=new_gid;
 r=inode_write(s,ino,&ip);
 if(r){s->mounted=0;return SF_COMMIT_UNCERTAIN;}
 return commit(s);
}
int sf_rw_set_quota(Sfrw*s,uint32_t uid,uint32_t target,uint32_t blocks,uint32_t inodes){
 if(!s||!s->mounted)return SF_DEVICE_GONE;
 if(uid!=0)return SF_DENIED;
 if(blocks>s->block_count)return SF_INVALID;
 for(uint32_t i=0;i<s->quota_count;i++)if(s->quota[i].uid==target){
  s->quota[i].blocks=blocks;s->quota[i].inodes=inodes;
  s->dirty=1;return commit(s);
 }
 if(s->quota_count>=SF_RW_QUOTA_MAX)return SF_NO_SPACE;
 s->quota[s->quota_count].uid=target;
 s->quota[s->quota_count].blocks=blocks;
 s->quota[s->quota_count].inodes=inodes;
 s->quota[s->quota_count].reserved=0;
 s->quota_count++;
 s->dirty=1;
 return commit(s);
}
static int ref_get(const Sfrw*s,uint32_t b){return (s->ref[b>>3]>>(b&7))&1;}
static void ref_set(Sfrw*s,uint32_t b){s->ref[b>>3]|=(uint8_t)(1u<<(b&7));}
static int reach_get(const Sfrw*s,uint32_t ino){return (s->reach[ino>>3]>>(ino&7))&1;}
static void reach_set(Sfrw*s,uint32_t ino){s->reach[ino>>3]|=(uint8_t)(1u<<(ino&7));}
static int fsck_meta(Sfrw*s,uint32_t target,const uint8_t*data){
 if(s->journal_count>=SF_RW_JOURNAL_MAX-4){int r=commit(s);if(r)return r;}
 return meta_write(s,target,data);
}
static int fsck_load(Sfrw*s,SfrwDev dev){
 memset(s,0,sizeof(*s));s->dev=dev;
 uint8_t b[SF_RW_BLOCK_BYTES];
 int r=dev.read(dev.context,0,b);if(!r)r=super_load(s,b);
 if(r){r=dev.read(dev.context,1,b);if(r)return r;r=super_load(s,b);}
 if(r)return SF_BAD_FORMAT;
 r=journal_replay(s);if(r)return r;
 r=dev.read(dev.context,0,b);if(!r)r=super_load(s,b);
 if(r){r=dev.read(dev.context,1,b);if(r)return r;r=super_load(s,b);}
 if(r)return SF_BAD_FORMAT;
 r=dev.read(dev.context,SF_RW_BITMAP_BLOCK,s->bitmap);
 if(!r)r=dev.read(dev.context,SF_RW_BITMAP_BLOCK+1,s->bitmap+SF_RW_BLOCK_BYTES);
 if(r)return r;
 s->mounted=1;s->dirty=0;s->journal_count=0;s->usage_count=0;
 return 0;
}

int sf_rw_fsck(Sfrw*s,SfrwDev dev,SfckReport*report){
 if(!s||!dev.read||!dev.write||!dev.flush||!report)return SF_INVALID;
 memset(report,0,sizeof(*report));
 int r=fsck_load(s,dev);if(r)return r;
 uint32_t old_used=0;
 for(uint32_t b=SF_RW_DATA_BLOCK;b<s->block_count;b++)if(bit_get(s,b))old_used++;
 memset(s->ref,0,SF_RW_BITMAP_BYTES);
 for(uint32_t b=0;b<SF_RW_DATA_BLOCK;b++)ref_set(s,b);
 memset(s->reach,0,SF_RW_MAX_INODES/8);
 memset(s->refs,0,sizeof(s->refs));
 memset(s->subdirs,0,sizeof(s->subdirs));
 Inode root;
 r=inode_read(s,SF_RW_ROOT,&root);
 if(r||(root.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)goto failed;
 reach_set(s,SF_RW_ROOT);
 uint32_t head=0,tail=1;
 s->queue[0]=SF_RW_ROOT;
 while(head<tail){
  uint32_t dir=s->queue[head++];
  Inode ip;
  r=inode_read(s,dir,&ip);if(r)goto failed;
  if((ip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR)continue;
  uint32_t blocks=(uint32_t)(ip.size/SF_RW_BLOCK_BYTES);
  for(uint32_t lb=0;lb<blocks&&lb<SF_RW_FILE_BLOCKS;lb++){
   uint32_t blk;
   r=block_map(s,&ip,lb,&blk);
   if(r||!blk)continue;
   /* 目录块指针必须落在数据区，坏指针只报告不读写元数据区。 */
   if(blk<SF_RW_DATA_BLOCK||blk>=s->block_count){
    /* 坏目录块：绝不读写元数据区；直接块可用新数据块替换后写回 inode。 */
    report->bad_entries++;
    if(lb<SF_RW_DIRECT_BLOCKS){
     uint32_t fresh=block_alloc(s);
     if(fresh){
      memset(s->block,0,SF_RW_BLOCK_BYTES);
      p32(s->block,dir);s->block[4]=1;s->block[5]=SF_RW_ENTRY_DIR;s->block[8]='.';
      p32(s->block+128,SF_RW_ROOT);s->block[132]=2;s->block[133]=SF_RW_ENTRY_DIR;s->block[136]='.';s->block[137]='.';
      r=fsck_meta(s,fresh,s->block);if(r)goto failed;
      ip.direct[lb]=fresh;
      r=inode_write(s,dir,&ip);if(r)goto failed;
      report->repaired++;
     }
    }
    continue;
   }
   r=meta_read(s,blk,s->block);if(r)goto failed;
   int modified=0;
   for(uint32_t i=0;i<SF_RW_BLOCK_BYTES/128;i++){
    uint8_t*e=s->block+i*128;
    uint32_t ino=g32(e);
    if(!ino)continue;
    uint32_t len=e[4];
    if(len>SF_RW_NAME_MAX||ino>=SF_RW_MAX_INODES){
     memset(e,0,128);modified=1;report->bad_entries++;continue;
    }
    Inode cip;
    r=inode_read(s,ino,&cip);
    if(r||!cip.mode){memset(e,0,128);modified=1;report->bad_entries++;continue;}
    int dot=(len==1&&e[8]=='.')||(len==2&&e[8]=='.'&&e[9]=='.');
    if(dot){
     if((cip.mode&SF_RW_TYPE_MASK)!=SF_RW_TYPE_DIR){
      memset(e,0,128);modified=1;report->bad_entries++;continue;
     }
     reach_set(s,ino);
     if(len==1&&ino!=dir){p32(e,dir);e[5]=SF_RW_ENTRY_DIR;modified=1;report->repaired++;}
     continue;
    }
    if(!name_ok((char*)e+8,len)){
     memset(e,0,128);modified=1;report->bad_entries++;continue;
    }
    int child_dir=(cip.mode&SF_RW_TYPE_MASK)==SF_RW_TYPE_DIR;
    if((child_dir&&e[5]!=SF_RW_ENTRY_DIR)||(!child_dir&&e[5]!=SF_RW_ENTRY_FILE)){
     memset(e,0,128);modified=1;report->bad_entries++;continue;
    }
    if(child_dir){
     if(reach_get(s,ino)||tail>=SF_RW_MAX_INODES){
      memset(e,0,128);modified=1;report->bad_entries++;continue;
     }
     reach_set(s,ino);s->queue[tail++]=ino;
     if(dir<SF_RW_MAX_INODES)s->subdirs[dir]++;
    }else{
     reach_set(s,ino);
     if(ino<SF_RW_MAX_INODES)s->refs[ino]++;
    }
   }
   if(modified){r=fsck_meta(s,blk,s->block);if(r)goto failed;}
  }
 }
 for(uint32_t ino=1;ino<SF_RW_MAX_INODES;ino++){
  Inode ip;
  r=inode_read(s,ino,&ip);if(r)goto failed;
  if(!ip.mode||reach_get(s,ino))continue;
  Inode empty;memset(&empty,0,sizeof(empty));
  r=inode_write(s,ino,&empty);if(r)goto failed;
  report->lost_inodes++;report->repaired++;
 }
 memset(s->ref,0,SF_RW_BITMAP_BYTES);
 for(uint32_t b=0;b<SF_RW_DATA_BLOCK;b++)ref_set(s,b);
 s->usage_count=0;
 for(uint32_t ino=1;ino<SF_RW_MAX_INODES;ino++){
  if(!reach_get(s,ino))continue;
  Inode ip;
  r=inode_read(s,ino,&ip);if(r)goto failed;
  if(!ip.mode)continue;
  report->used_inodes++;
  uint32_t need=ip.size?(uint32_t)((ip.size+SF_RW_BLOCK_BYTES-1)/SF_RW_BLOCK_BYTES):0;
  uint32_t used=0;int changed=0;
  int file_inode=(ip.mode&SF_RW_TYPE_MASK)==SF_RW_TYPE_FILE;
  for(uint32_t i=0;i<SF_RW_DIRECT_BLOCKS;i++){
   uint32_t b=ip.direct[i];if(!b)continue;
   /* size 之外（或目录块数之外）的指针清零，不加入引用位图，随位图重建回收。 */
   if(i>=need){ip.direct[i]=0;changed=1;report->repaired++;continue;}
   if(b<SF_RW_DATA_BLOCK||b>=s->block_count){ip.direct[i]=0;changed=1;report->repaired++;continue;}
   if(ref_get(s,b)){
    report->duplicate_blocks++;
    if(file_inode){ip.direct[i]=0;changed=1;report->repaired++;}
    continue;
   }
   ref_set(s,b);used++;
  }
  if(ip.indirect){
   if(need<=SF_RW_DIRECT_BLOCKS){
    /* 不需要间接块：间接块本体与表项都不再引用。 */
    ip.indirect=0;changed=1;report->repaired++;
   }else{
    uint32_t b=ip.indirect;
    if(b<SF_RW_DATA_BLOCK||b>=s->block_count){
     ip.indirect=0;changed=1;report->repaired++;
    }else{
     if(ref_get(s,b))report->duplicate_blocks++;else{ref_set(s,b);used++;}
     r=meta_read(s,b,s->indirect);if(r)goto failed;
     int im=0;
     for(uint32_t i=0;i<SF_RW_INDIRECT_ENTRIES;i++){
      uint32_t e=g32(s->indirect+4*i);if(!e)continue;
      if(SF_RW_DIRECT_BLOCKS+i>=need){p32(s->indirect+4*i,0);im=1;report->repaired++;continue;}
      if(e<SF_RW_DATA_BLOCK||e>=s->block_count){p32(s->indirect+4*i,0);im=1;report->repaired++;continue;}
      if(ref_get(s,e)){
       report->duplicate_blocks++;
       if(file_inode){p32(s->indirect+4*i,0);im=1;report->repaired++;}
       continue;
      }
      ref_set(s,e);used++;
     }
     if(im){r=fsck_meta(s,b,s->indirect);if(r)goto failed;}
    }
   }
  }
  uint32_t expected=(ip.mode&SF_RW_TYPE_MASK)==SF_RW_TYPE_DIR?2u+s->subdirs[ino]:s->refs[ino];
  if(!expected)expected=1;
  if(ip.links!=expected){ip.links=(uint16_t)expected;changed=1;report->fixed_links++;report->repaired++;}
  if(changed){r=inode_write(s,ino,&ip);if(r)goto failed;}
  (void)usage_add(s,ip.uid,used,1);
 }
 memcpy(s->bitmap,s->ref,SF_RW_BITMAP_BYTES);
 uint32_t used=0;
 for(uint32_t b=SF_RW_DATA_BLOCK;b<s->block_count;b++)if(bit_get(s,b))used++;
 s->free_blocks=s->block_count-SF_RW_DATA_BLOCK-used;
 uint32_t free_inodes=0;
 for(uint32_t ino=1;ino<SF_RW_MAX_INODES;ino++){
  Inode ip;
  r=inode_read(s,ino,&ip);if(r)goto failed;
  if(!ip.mode)free_inodes++;
 }
 s->free_inodes=free_inodes;
 report->used_blocks=used;
 report->free_blocks=s->free_blocks;
 report->free_inodes=free_inodes;
 report->checked_inodes=SF_RW_MAX_INODES-1;
 report->unreachable_blocks=old_used>used?old_used-used:0;
 s->dirty=1;
 return commit(s);
failed:
 s->mounted=0;
 return r?r:SF_COMMIT_UNCERTAIN;
}
