#include "namespace_state.h"
static SfNamespace service_namespace;
SfNamespace*sf_ns_default(void){return &service_namespace;}
static int path_utf8(const uint8_t*p){while(*p){uint32_t c=*p++;if(c<32)return 0;if(c<128)continue;unsigned k;uint32_t min;if(c>=0xc2&&c<=0xdf){k=1;c&=31;min=128;}else if(c>=0xe0&&c<=0xef){k=2;c&=15;min=2048;}else if(c>=0xf0&&c<=0xf4){k=3;c&=7;min=65536;}else return 0;while(k--){if((*p&192)!=128)return 0;c=(c<<6)|(*p++&63);}if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return 0;}return 1;}
static int path_ok(const char*p){size_t len=strlen(p);if(*p!='/'||len>95||(len>1&&p[len-1]=='/')||!path_utf8((const uint8_t*)p))return 0;for(const char*s=p;*s;){if(*s++=='/'){const char*b=s;while(*s&&*s!='/')s++;size_t n=s-b;if((!n&&*s)||(n==1&&*b=='.')||(n==2&&b[0]=='.'&&b[1]=='.'))return 0;}}return 1;}
static int node_find(SfNamespace*n,const char*path){for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(n->nodes[i].type&&!strcmp(n->nodes[i].path,path))return i;return SF_NOT_FOUND;}
static unsigned default_mode(unsigned type){return type==NODE_DIR?0755:0644;}
static int parent_path(const char*path,char out[96]){
 if(strcmp(path,"/")==0)return SF_INVALID;size_t len=strlen(path),cut=len;
 while(cut>1&&path[cut-1]!='/')cut--;size_t plen=cut==1?1:cut-1;memcpy(out,path,plen);out[plen]=0;return 0;
}
/* Owner/group/other rwx bits. uid 0 is the trusted administrator and bypasses
 * all checks, mirroring the capability-root assertions used elsewhere. */
static int access_ok(const SfNode*x,uint32_t uid,uint32_t gid,unsigned need){
 if(!uid)return 1;unsigned shift=x->owner_uid==uid?6:x->owner_gid==gid?3:0;
 return ((x->mode>>shift)&need)==need;
}
/* Every directory between root (exclusive) and the target's parent (inclusive)
 * must grant execute, so a private home cannot be bypassed by a deeper path. */
static int dirs_ok(SfNamespace*n,const char*root,const char*path,uint32_t uid,uint32_t gid){
 if(!uid)return 0;int base=node_find(n,root);if(base<0)return base;if(n->nodes[base].type!=NODE_DIR)return SF_INVALID;
 if(!access_ok(&n->nodes[base],uid,gid,1))return SF_DENIED;
 size_t start=strlen(root)==1?1:strlen(root)+1,len=strlen(path);
 for(size_t cut=start;cut<len;cut++){if(path[cut]!='/')continue;char prefix[96];memcpy(prefix,path,cut);prefix[cut]=0;
  int i=node_find(n,prefix);if(i<0||n->nodes[i].type!=NODE_DIR)return SF_NOT_FOUND;if(!access_ok(&n->nodes[i],uid,gid,1))return SF_DENIED;}
 return 0;
}
static int parent_write_ok(SfNamespace*n,const char*path,uint32_t uid,uint32_t gid){
 if(!uid)return 0;char parent[96];int r=parent_path(path,parent);if(r)return r;
 int i=node_find(n,parent);if(i<0)return i;if(n->nodes[i].type!=NODE_DIR)return SF_NOT_FOUND;
 return access_ok(&n->nodes[i],uid,gid,3)?0:SF_DENIED;
}
void sf_ns_init(SfNamespace*n,long(*console)(const void*,size_t)){memset(n,0,sizeof(*n));n->console=console;sf_ns_create(n,"/",NODE_DIR,0);}
SfNsRead sf_ns_external(SfNamespace*n,SfNsRead read,long(*write)(unsigned,uint64_t,const void*,size_t),int(*truncate)(unsigned,uint64_t)){
 SfNsRead previous=n->external_read;n->external_read=read;n->external_write=write;n->external_truncate=truncate;return previous;
}
void sf_ns_mount_hooks(SfNamespace*n,int(*open)(unsigned),void(*close)(unsigned)){n->mount_open=open;n->mount_close=close;}
void sf_ns_projection_backend(SfNamespace*n,SfNsRead read){n->projection_read=read;}
static int beneath(const char*root,const char*path){size_t len=strlen(root);return strlen(path)>=len&&!memcmp(root,path,len)&&(!path[len]||path[len]=='/');}
int sf_ns_mount_check(SfNamespace*n,const char*root){
 if(!root||!path_ok(root)||!strcmp(root,"/"))return SF_INVALID;
 int base=node_find(n,root);if(base>=0&&(n->nodes[base].type!=NODE_DIR||n->nodes[base].mount_tag))return SF_BUSY;
 for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(n->nodes[i].type&&beneath(root,n->nodes[i].path)&&strcmp(root,n->nodes[i].path))return SF_BUSY;
 for(unsigned i=0;i<SF_NS_DESCRIPTION_MAX;i++)if(n->descriptions[i].refs&&beneath(root,n->nodes[n->descriptions[i].node].path))return SF_BUSY;
 if(base<0){char parent[96];parent_path(root,parent);int i=node_find(n,parent);if(i<0||n->nodes[i].type!=NODE_DIR)return SF_NOT_FOUND;if(n->nodes[i].readonly)return SF_READ_ONLY;}
 return 0;
}
int sf_ns_mount_publish(SfNamespace*n,const SfMountEntry*entries,unsigned count,unsigned tag){
 if(!entries||!count||count>SF_NS_NODE_MAX||!tag)return SF_INVALID;
 /* Validate fixed arrays before using any C-string helper. No callbacks or
  * mutations occur until every path, parent and destination slot is known. */
 for(unsigned i=0;i<count;i++){
  unsigned end=0;while(end<sizeof(entries[i].path)&&entries[i].path[end])end++;
  if(end==sizeof(entries[i].path)||!path_ok(entries[i].path)||entries[i].readonly>1||
     (entries[i].type!=NODE_DIR&&entries[i].type!=NODE_DISK))return SF_INVALID;
 }
 if(entries[0].type!=NODE_DIR)return SF_INVALID;
 const char*root=entries[0].path;int r=sf_ns_mount_check(n,root);if(r)return r;
 unsigned slots[SF_NS_NODE_MAX],free_slots[SF_NS_NODE_MAX],free_count=0,used=0;
 int base=node_find(n,root);
 for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(!n->nodes[i].type)free_slots[free_count++]=i;
 if(count-(base>=0?1u:0u)>free_count)return SF_NO_SPACE;
 for(unsigned i=0;i<count;i++){
  const SfMountEntry*e=&entries[i];
  if(!beneath(root,e->path))return SF_DENIED;
  for(unsigned j=0;j<i;j++)if(!strcmp(entries[j].path,e->path))return SF_BUSY;
  if(i){char parent[96];parent_path(e->path,parent);unsigned j=0;
   for(;j<i;j++)if(!strcmp(entries[j].path,parent))break;
   if(j==i||entries[j].type!=NODE_DIR)return SF_NOT_FOUND;
  }
  slots[i]=!i&&base>=0?(unsigned)base:free_slots[used++];
 }
 for(unsigned i=0;i<count;i++){
  const SfMountEntry*e=&entries[i];SfNode*x=&n->nodes[slots[i]];
  /* Preserve an existing empty root's ownership and permission bits. */
  if(i||base<0){memset(x,0,sizeof(*x));memcpy(x->path,e->path,strlen(e->path)+1);x->type=(uint8_t)e->type;x->mode=(uint16_t)default_mode(e->type);}
  x->readonly=(uint8_t)e->readonly;x->external=e->external;x->size=e->size;x->mount_tag=tag;
 }
 return 0;
}
int sf_ns_create(SfNamespace*n,const char*p,unsigned type,int ro){return sf_ns_create_owned(n,p,type,ro,0,0,default_mode(type));}
int sf_ns_create_owned(SfNamespace*n,const char*p,unsigned type,int ro,uint32_t uid,uint32_t gid,unsigned mode){
 if(!path_ok(p)||type<1||type>NODE_PROJECTION||mode>0777)return SF_INVALID;if(node_find(n,p)>=0)return SF_BUSY;
 if(strcmp(p,"/")){char parent[96];int r=parent_path(p,parent);if(r)return r;
  int i=node_find(n,parent);if(i<0||n->nodes[i].type!=NODE_DIR)return SF_NOT_FOUND;if(n->nodes[i].readonly)return SF_READ_ONLY;
  r=dirs_ok(n,"/",p,uid,gid);if(r)return r;r=parent_write_ok(n,p,uid,gid);if(r)return r;}
 for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(!n->nodes[i].type){SfNode*x=&n->nodes[i];memset(x,0,sizeof(*x));memcpy(x->path,p,strlen(p)+1);x->type=type;x->readonly=ro;x->mode=(uint16_t)(mode&0777);x->owner_uid=uid;x->owner_gid=gid;return 0;}return SF_NO_SPACE;
}
int sf_ns_assign(SfNamespace*n,const char*path,uint32_t uid,uint32_t gid,unsigned mode){
 if(!path_ok(path)||mode>0777)return SF_INVALID;int i=node_find(n,path);if(i<0)return i;
 n->nodes[i].owner_uid=uid;n->nodes[i].owner_gid=gid;n->nodes[i].mode=(uint16_t)(mode&0777);return 0;
}
/* A projection is a read-only file whose bytes are produced on every read by
 * the owner service. It has no cached payload and cannot be written or removed. */
int sf_ns_project(SfNamespace*n,const char*p,unsigned id){if(!id)return SF_INVALID;
 for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(n->nodes[i].type==NODE_PROJECTION&&n->nodes[i].external==id)return SF_BUSY;
 int r=sf_ns_create(n,p,NODE_PROJECTION,1);if(r)return r;int i=node_find(n,p);if(i<0)return SF_INVALID;n->nodes[i].external=id;return 0;
}
int sf_ns_protect(SfNamespace*n,const char*p){int i=node_find(n,p);if(i<0)return i;if(n->nodes[i].type!=NODE_DIR)return SF_INVALID;n->nodes[i].readonly=1;return 0;}
static SfDescription*description(SfNamespace*n,uint32_t owner,uint32_t h){unsigned i=h&255;if(!i||i>SF_NS_HANDLE_MAX)return 0;SfFileHandle*x=&n->handles[i-1];if(x->owner!=owner||x->generation!=(h>>8)||!x->description)return 0;return&n->descriptions[x->description-1];}
int sf_ns_handoff(SfNamespace*n,uint32_t owner,uint32_t h,uint32_t next){
 if(!owner||!next)return SF_INVALID;if(!description(n,owner,h))return SF_STALE_HANDLE;
 n->handles[(h&255)-1].owner=next;return 0;
}
int sf_ns_stat(SfNamespace*n,uint32_t owner,uint32_t h,SfFileStat*out){
 if(!out)return SF_INVALID;SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;SfNode*x=&n->nodes[d->node];
 /* Cached object metadata only: no backend I/O or new access authority. */
 SfFileStat value={sizeof(SfFileStat),x->type,d->mode,(x->readonly?SF_FILE_READ_ONLY:0)|(x->mount_tag?SF_FILE_MOUNTED:0)|((x->type==NODE_FILE||x->type==NODE_DISK)?SF_FILE_SEEKABLE:0),x->type==NODE_DIR||x->type==NODE_CONSOLE?0:x->size,d->offset};
 memcpy(out,&value,sizeof(value));return 0;
}
static int handle_new(SfNamespace*n,uint32_t owner,unsigned d){for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++)if(!n->handles[i].description){SfFileHandle*h=&n->handles[i];if(h->generation>=0x7fffff)continue;h->generation++;h->owner=owner;h->description=d+1;n->descriptions[d].refs++;return(h->generation<<8)|(i+1);}return SF_NO_SPACE;}
int sf_ns_open(SfNamespace*n,uint32_t owner,const char*root,const char*path,unsigned mode){return sf_ns_open_as(n,owner,root,path,mode,0,0);}
int sf_ns_open_as(SfNamespace*n,uint32_t owner,const char*root,const char*path,unsigned mode,uint32_t uid,uint32_t gid){
 if(!owner||!path_ok(root)||!path_ok(path)||!(mode&3)||(mode&~15))return SF_INVALID;
 size_t len=strlen(root);if(strcmp(root,"/")&&(strlen(path)<len||memcmp(path,root,len)|| (path[len]&&path[len]!='/')))return SF_DENIED;
 int base=node_find(n,root);if(base<0)return base;if(n->nodes[base].type!=NODE_DIR)return SF_INVALID;
 int r=dirs_ok(n,root,path,uid,gid);if(r)return r;
 int i=node_find(n,path);if(i<0)return i;SfNode*x=&n->nodes[i];if((mode&OPEN_DIRECTORY)&&x->type!=NODE_DIR)return SF_INVALID;if(x->readonly&&(mode&OPEN_WRITE))return SF_READ_ONLY;
 unsigned need=(mode&OPEN_READ?4:0)|(mode&OPEN_WRITE?2:0);if(x->type==NODE_DIR)need|=1;
 if(!access_ok(x,uid,gid,need))return SF_DENIED;
 for(unsigned d=0;d<SF_NS_DESCRIPTION_MAX;d++)if(!n->descriptions[d].refs){if(x->mount_tag&&n->mount_open){int r=n->mount_open(x->mount_tag);if(r)return r;}n->descriptions[d]=(SfDescription){i,0,mode,0};int h=handle_new(n,owner,d);if(h<0){if(x->mount_tag&&n->mount_close)n->mount_close(x->mount_tag);return h;}if(x->type==NODE_PIPE&&(mode&OPEN_WRITE))x->writers++;return h;}return SF_NO_SPACE;
}
static int relative_path(const SfNode*base,const char*relative,char path[96]){
 if(!*relative||*relative=='/')return SF_INVALID;size_t a=strlen(base->path),b=strlen(relative);unsigned separator=a!=1;
 if(b>95||a+separator+b>95)return SF_INVALID;memcpy(path,base->path,a);if(separator)path[a++]='/';memcpy(path+a,relative,b+1);
 /* There are no symlinks or aliases: only canonical components beneath the
  * validated directory object may contribute to a relative request. */
 return path_ok(path)?0:SF_INVALID;
}
int sf_ns_openat(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative,unsigned mode){return sf_ns_openat_as(n,owner,directory,relative,mode,0,0);}
int sf_ns_openat_as(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative,unsigned mode,uint32_t uid,uint32_t gid){
 SfDescription*d=description(n,owner,directory);if(!d)return SF_STALE_HANDLE;
 SfNode*base=&n->nodes[d->node];if(base->type!=NODE_DIR)return SF_INVALID;
 if(!(mode&3)||(mode&~15))return SF_INVALID;if((mode&3)&~d->mode)return SF_DENIED;
 if(!access_ok(base,uid,gid,1))return SF_DENIED;
 char path[96];int r=relative_path(base,relative,path);return r?r:sf_ns_open_as(n,owner,base->path,path,mode,uid,gid);
}
static int remove_node_as(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative,unsigned kind,uint32_t uid,uint32_t gid){
 SfDescription*d=description(n,owner,directory);if(!d)return SF_STALE_HANDLE;
 SfNode*base=&n->nodes[d->node];if(base->type!=NODE_DIR)return SF_INVALID;
 if(!(d->mode&OPEN_WRITE))return SF_DENIED;
 char path[96];int r=relative_path(base,relative,path);if(r)return r;
 if(base->readonly||base->mount_tag)return SF_READ_ONLY;
 if(!access_ok(base,uid,gid,3))return SF_DENIED;
 /* Recheck every current parent, including intermediate directories reached
  * from a wider capability. A mount or read-only subtree cannot be bypassed. */
 for(size_t i=strlen(base->path);path[i];i++)if(path[i]=='/'){
  path[i]=0;int parent=node_find(n,path);path[i]='/';
  if(parent<0||n->nodes[parent].type!=NODE_DIR)return SF_NOT_FOUND;
  if(n->nodes[parent].readonly||n->nodes[parent].mount_tag)return SF_READ_ONLY;
  if(!access_ok(&n->nodes[parent],uid,gid,1))return SF_DENIED;
 }
 if(uid){char parent[96];int r=parent_path(path,parent);if(r)return r;int p=node_find(n,parent);
  if(p<0||n->nodes[p].type!=NODE_DIR)return SF_NOT_FOUND;if(!access_ok(&n->nodes[p],uid,gid,3))return SF_DENIED;}
 int target=node_find(n,path);if(target<0)return target;SfNode*x=&n->nodes[target];
 if(x->readonly||x->mount_tag)return SF_READ_ONLY;if(x->type!=kind)return SF_INVALID;
 /* This service serializes mutation and opening. Keeping open files busy
  * prevents an old description from ever aliasing a reused node slot. */
 for(unsigned i=0;i<SF_NS_DESCRIPTION_MAX;i++)if(n->descriptions[i].refs&&n->descriptions[i].node==(unsigned)target)return SF_BUSY;
 if(kind==NODE_DIR){size_t length=strlen(path);
  for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(n->nodes[i].type&&strlen(n->nodes[i].path)>length&&!memcmp(n->nodes[i].path,path,length)&&n->nodes[i].path[length]=='/')return SF_BUSY;
 }
 memset(x,0,sizeof(*x));return 0;
}
int sf_ns_unlink(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative){return remove_node_as(n,owner,directory,relative,NODE_FILE,0,0);}
int sf_ns_unlink_as(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative,uint32_t uid,uint32_t gid){return remove_node_as(n,owner,directory,relative,NODE_FILE,uid,gid);}
int sf_ns_rmdir(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative){return remove_node_as(n,owner,directory,relative,NODE_DIR,0,0);}
int sf_ns_rmdir_as(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative,uint32_t uid,uint32_t gid){return remove_node_as(n,owner,directory,relative,NODE_DIR,uid,gid);}
int sf_ns_dup(SfNamespace*n,uint32_t owner,uint32_t h){SfDescription*d=description(n,owner,h);return d?handle_new(n,owner,d-n->descriptions):SF_STALE_HANDLE;}
int sf_ns_close(SfNamespace*n,uint32_t owner,uint32_t h){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!--d->refs){SfNode*x=&n->nodes[d->node];if(x->type==NODE_PIPE&&(d->mode&OPEN_WRITE))x->writers--;if(x->mount_tag&&n->mount_close)n->mount_close(x->mount_tag);}n->handles[(h&255)-1].description=0;return 0;}
long sf_ns_read(SfNamespace*n,uint32_t owner,uint32_t h,void*out,size_t bytes){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!(d->mode&OPEN_READ))return SF_DENIED;SfNode*x=&n->nodes[d->node];if(x->type==NODE_DIR)return SF_INVALID;if(!bytes)return 0;if(x->type==NODE_CONSOLE)return SF_WOULD_BLOCK;
 if(x->type==NODE_PROJECTION){if(!n->projection_read)return SF_DEVICE_GONE;long r=n->projection_read(x->external,d->offset,out,bytes);if(r>=0){if((uint64_t)r>bytes||(uint64_t)r>UINT32_MAX-d->offset)return SF_BAD_FORMAT;d->offset+=r;}return r;}
 if(x->type==NODE_DISK){if(!n->external_read)return SF_DEVICE_GONE;long r=n->external_read(x->external,d->offset,out,bytes);if(r>=0){if((uint64_t)r>bytes||(uint64_t)r>UINT32_MAX-d->offset)return SF_BAD_FORMAT;d->offset+=r;}return r;}
 if(x->type==NODE_PIPE){if(!x->size)return x->writers?SF_WOULD_BLOCK:0;if(bytes>x->size)bytes=x->size;for(size_t i=0;i<bytes;i++)((uint8_t*)out)[i]=x->data[(x->head+i)%sizeof(x->data)];x->head=(x->head+bytes)%sizeof(x->data);x->size-=bytes;return bytes;}
 if(d->offset>=x->size)return 0;if(bytes>x->size-d->offset)bytes=x->size-d->offset;memcpy(out,x->data+d->offset,bytes);d->offset+=bytes;return bytes;
}
long sf_ns_pread(SfNamespace*n,uint32_t owner,uint32_t h,void*out,size_t bytes,uint64_t offset){
 SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!(d->mode&OPEN_READ))return SF_DENIED;SfNode*x=&n->nodes[d->node];
 if(x->type==NODE_DIR)return SF_INVALID;if(x->type==NODE_PIPE||x->type==NODE_CONSOLE)return SF_NOT_SEEKABLE;
 if(bytes&&!out)return SF_INVALID;if(!bytes)return 0;
 /* Positional reads never borrow or modify the shared description cursor.
  * The backend still checks device health, including nonempty EOF requests. */
 if(x->type==NODE_PROJECTION){if(!n->projection_read)return SF_DEVICE_GONE;long r=n->projection_read(x->external,offset,out,bytes);return r>=0&&(uint64_t)r>bytes?SF_BAD_FORMAT:r;}
 if(x->type==NODE_DISK){if(!n->external_read)return SF_DEVICE_GONE;long r=n->external_read(x->external,offset,out,bytes);
  return r>=0&&((uint64_t)r>bytes||(uint64_t)r>UINT64_MAX-offset)?SF_BAD_FORMAT:r;
 }
 if(offset>=x->size)return 0;if(bytes>x->size-offset)bytes=x->size-offset;memcpy(out,x->data+offset,bytes);return bytes;
}
long sf_ns_write(SfNamespace*n,uint32_t owner,uint32_t h,const void*data,size_t bytes){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!(d->mode&OPEN_WRITE))return SF_DENIED;SfNode*x=&n->nodes[d->node];if(x->readonly)return SF_READ_ONLY;if(x->type==NODE_DIR)return SF_INVALID;if(!bytes)return 0;if(x->type==NODE_CONSOLE)return n->console?n->console(data,bytes):SF_DEVICE_GONE;
 if(x->type==NODE_DISK){if(!n->external_write)return SF_READ_ONLY;long r=n->external_write(x->external,d->offset,data,bytes);if(r>=0){if((uint64_t)r>bytes||(uint64_t)r>UINT32_MAX-d->offset)return SF_BAD_FORMAT;d->offset+=r;if(d->offset>x->size)x->size=d->offset;}return r;}
 if(d->offset>sizeof(x->data)||x->size>sizeof(x->data))return SF_INVALID;
 if(x->type==NODE_PIPE){if(bytes>sizeof(x->data)-x->size)bytes=sizeof(x->data)-x->size;if(!bytes)return SF_WOULD_BLOCK;for(size_t i=0;i<bytes;i++)x->data[(x->head+x->size+i)%sizeof(x->data)]=((const uint8_t*)data)[i];x->size+=bytes;return bytes;}
 if(bytes>sizeof(x->data)-d->offset)bytes=sizeof(x->data)-d->offset;if(!bytes)return SF_NO_SPACE;if(d->offset>x->size)memset(x->data+x->size,0,d->offset-x->size);memcpy(x->data+d->offset,data,bytes);d->offset+=bytes;if(d->offset>x->size)x->size=d->offset;return bytes;
}
int sf_ns_truncate(SfNamespace*n,uint32_t owner,uint32_t h,uint64_t bytes){
 SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!(d->mode&OPEN_WRITE))return SF_DENIED;SfNode*x=&n->nodes[d->node];if(x->readonly)return SF_READ_ONLY;
 if(x->type!=NODE_FILE&&x->type!=NODE_DISK)return SF_INVALID;if(bytes>UINT32_MAX)return SF_NO_SPACE;
 if(x->type==NODE_DISK){if(!n->external_truncate)return SF_READ_ONLY;int r=n->external_truncate(x->external,bytes);if(r)return r>0?SF_BAD_FORMAT:r;}
 else{if(bytes>sizeof(x->data))return SF_NO_SPACE;size_t start=bytes<x->size?bytes:x->size;memset(x->data+start,0,sizeof(x->data)-start);}
 x->size=bytes;return 0;
}
int64_t sf_ns_seek(SfNamespace*n,uint32_t owner,uint32_t h,uint64_t off){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;SfNode*x=&n->nodes[d->node];if(x->type==NODE_PIPE||x->type==NODE_CONSOLE)return SF_NOT_SEEKABLE;if(x->type==NODE_PROJECTION){if(off>UINT32_MAX)return SF_INVALID;d->offset=off;return off;}if((x->type!=NODE_FILE&&x->type!=NODE_DISK)||off>(x->type==NODE_DISK?x->size:sizeof(x->data)))return SF_INVALID;d->offset=off;return off;}
long sf_ns_poll(SfNamespace*n,uint32_t owner,uint32_t h){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;SfNode*x=&n->nodes[d->node];unsigned mask=0;if(x->type==NODE_PIPE){if((d->mode&1)&&(x->size||!x->writers))mask|=1;if((d->mode&2)&&x->size<sizeof(x->data))mask|=2;if(!x->writers)mask|=4;}else{if(d->mode&1)mask|=1;if((d->mode&2)&&!x->readonly)mask|=2;}return mask;}
long sf_ns_enumerate(SfNamespace*n,uint32_t owner,uint32_t h,char*out,size_t size){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!(d->mode&1))return SF_DENIED;const char*p=n->nodes[d->node].path;if(n->nodes[d->node].type!=NODE_DIR)return SF_INVALID;size_t used=0,len=strlen(p);for(unsigned i=0;i<SF_NS_NODE_MAX;i++){const char*s=n->nodes[i].path;if(!n->nodes[i].type||!strcmp(p,s)||strlen(s)<=len)continue;const char*child;if(!strcmp(p,"/"))child=s+1;else{if(memcmp(p,s,len)||s[len]!='/')continue;child=s+len+1;}int nested=0;for(const char*c=child;*c;c++)if(*c=='/')nested=1;if(nested)continue;size_t k=strlen(child);if(k+1>size-used)return used? (long)used:SF_NO_SPACE;memcpy(out+used,child,k);out[used+k]='\n';used+=k+1;}return used;}
/* Stateless directory cursor: callers pass the zero-based child index and get
 * one NUL-terminated name (or 0 at the end). No server state is retained. */
long sf_ns_enumerate_at(SfNamespace*n,uint32_t owner,uint32_t h,uint32_t index,char*out,size_t size){
 SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!(d->mode&1))return SF_DENIED;
 SfNode*self=&n->nodes[d->node];if(self->type!=NODE_DIR)return SF_INVALID;if(!out||!size)return SF_INVALID;
 size_t len=strlen(self->path);unsigned seen=0;
 for(unsigned i=0;i<SF_NS_NODE_MAX;i++){
  const char*s=n->nodes[i].path;if(!n->nodes[i].type||!strcmp(self->path,s)||strlen(s)<=len)continue;
  const char*child;if(!strcmp(self->path,"/"))child=s+1;else{if(memcmp(self->path,s,len)||s[len]!='/')continue;child=s+len+1;}
  int nested=0;for(const char*c=child;*c;c++)if(*c=='/')nested=1;if(nested)continue;
  if(seen++!=index)continue;
  size_t k=strlen(child);if(k+1>size)return SF_NO_SPACE;
  memcpy(out,child,k);out[k]=0;return(long)(k+1);
 }
 return 0;
}
void sf_ns_reap(SfNamespace*n,uint32_t owner){for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++)if(n->handles[i].description&&n->handles[i].owner==owner)sf_ns_close(n,owner,(n->handles[i].generation<<8)|(i+1));}
int sf_ns_unmount(SfNamespace*n,unsigned tag){if(!tag)return SF_INVALID;for(unsigned i=0;i<SF_NS_DESCRIPTION_MAX;i++)if(n->descriptions[i].refs&&n->nodes[n->descriptions[i].node].mount_tag==tag)return SF_BUSY;for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(n->nodes[i].mount_tag==tag){if(!strcmp(n->nodes[i].path,"/disk")){n->nodes[i].mount_tag=0;n->nodes[i].readonly=0;}else memset(&n->nodes[i],0,sizeof(n->nodes[i]));}return 0;}
