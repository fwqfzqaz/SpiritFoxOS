#include "namespace.h"
static int path_utf8(const uint8_t*p){while(*p){uint32_t c=*p++;if(c<32)return 0;if(c<128)continue;unsigned k;uint32_t min;if(c>=0xc2&&c<=0xdf){k=1;c&=31;min=128;}else if(c>=0xe0&&c<=0xef){k=2;c&=15;min=2048;}else if(c>=0xf0&&c<=0xf4){k=3;c&=7;min=65536;}else return 0;while(k--){if((*p&192)!=128)return 0;c=(c<<6)|(*p++&63);}if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return 0;}return 1;}
static int path_ok(const char*p){size_t len=strlen(p);if(*p!='/'||len>95||(len>1&&p[len-1]=='/')||!path_utf8((const uint8_t*)p))return 0;for(const char*s=p;*s;){if(*s++=='/'){const char*b=s;while(*s&&*s!='/')s++;size_t n=s-b;if((!n&&*s)||(n==1&&*b=='.')||(n==2&&b[0]=='.'&&b[1]=='.'))return 0;}}return 1;}
static int node_find(SfNamespace*n,const char*path){for(unsigned i=0;i<32;i++)if(n->nodes[i].type&&!strcmp(n->nodes[i].path,path))return i;return SF_NOT_FOUND;}
void sf_ns_init(SfNamespace*n,long(*console)(const void*,size_t)){memset(n,0,sizeof(*n));n->console=console;sf_ns_create(n,"/",NODE_DIR,0);}
int sf_ns_create(SfNamespace*n,const char*p,unsigned type,int ro){if(!path_ok(p)||type<1||type>5)return SF_INVALID;if(node_find(n,p)>=0)return SF_BUSY;
 if(strcmp(p,"/")){char parent[96];size_t len=strlen(p),cut=len;while(cut>1&&p[cut-1]!='/')cut--;size_t plen=cut==1?1:cut-1;memcpy(parent,p,plen);parent[plen]=0;int i=node_find(n,parent);if(i<0||n->nodes[i].type!=NODE_DIR)return SF_NOT_FOUND;if(n->nodes[i].readonly)return SF_READ_ONLY;}
 for(unsigned i=0;i<32;i++)if(!n->nodes[i].type){SfNode*x=&n->nodes[i];memset(x,0,sizeof(*x));memcpy(x->path,p,strlen(p)+1);x->type=type;x->readonly=ro;return 0;}return SF_NO_SPACE;
}
static SfDescription*description(SfNamespace*n,uint32_t owner,uint32_t h){unsigned i=h&255;if(!i||i>64)return 0;SfFileHandle*x=&n->handles[i-1];if(x->owner!=owner||x->generation!=(h>>8)||!x->description)return 0;return&n->descriptions[x->description-1];}
static int handle_new(SfNamespace*n,uint32_t owner,unsigned d){for(unsigned i=0;i<64;i++)if(!n->handles[i].description){SfFileHandle*h=&n->handles[i];if(h->generation>=0x7fffff)continue;h->generation++;h->owner=owner;h->description=d+1;n->descriptions[d].refs++;return(h->generation<<8)|(i+1);}return SF_NO_SPACE;}
int sf_ns_open(SfNamespace*n,uint32_t owner,const char*root,const char*path,unsigned mode){
 if(!owner||!path_ok(root)||!path_ok(path)||!(mode&3)||(mode&~15))return SF_INVALID;
 size_t len=strlen(root);if(strcmp(root,"/")&&(strlen(path)<len||memcmp(path,root,len)|| (path[len]&&path[len]!='/')))return SF_DENIED;
 int base=node_find(n,root);if(base<0)return base;if(n->nodes[base].type!=NODE_DIR)return SF_INVALID;
 int i=node_find(n,path);if(i<0)return i;SfNode*x=&n->nodes[i];if((mode&OPEN_DIRECTORY)&&x->type!=NODE_DIR)return SF_INVALID;if(x->readonly&&(mode&OPEN_WRITE))return SF_READ_ONLY;
 for(unsigned d=0;d<32;d++)if(!n->descriptions[d].refs){if(x->mount_tag&&n->mount_open){int r=n->mount_open(x->mount_tag);if(r)return r;}n->descriptions[d]=(SfDescription){i,0,mode,0};int h=handle_new(n,owner,d);if(h<0){if(x->mount_tag&&n->mount_close)n->mount_close(x->mount_tag);return h;}if(x->type==NODE_PIPE&&(mode&OPEN_WRITE))x->writers++;return h;}return SF_NO_SPACE;
}
static int relative_path(const SfNode*base,const char*relative,char path[96]){
 if(!*relative||*relative=='/')return SF_INVALID;size_t a=strlen(base->path),b=strlen(relative);unsigned separator=a!=1;
 if(b>95||a+separator+b>95)return SF_INVALID;memcpy(path,base->path,a);if(separator)path[a++]='/';memcpy(path+a,relative,b+1);
 /* There are no symlinks or aliases: only canonical components beneath the
  * validated directory object may contribute to a relative request. */
 return path_ok(path)?0:SF_INVALID;
}
int sf_ns_openat(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative,unsigned mode){
 SfDescription*d=description(n,owner,directory);if(!d)return SF_STALE_HANDLE;
 SfNode*base=&n->nodes[d->node];if(base->type!=NODE_DIR)return SF_INVALID;
 if(!(mode&3)||(mode&~15))return SF_INVALID;if((mode&3)&~d->mode)return SF_DENIED;
 char path[96];int r=relative_path(base,relative,path);return r?r:sf_ns_open(n,owner,base->path,path,mode);
}
int sf_ns_unlink(SfNamespace*n,uint32_t owner,uint32_t directory,const char*relative){
 SfDescription*d=description(n,owner,directory);if(!d)return SF_STALE_HANDLE;
 SfNode*base=&n->nodes[d->node];if(base->type!=NODE_DIR)return SF_INVALID;
 if(!(d->mode&OPEN_WRITE))return SF_DENIED;
 char path[96];int r=relative_path(base,relative,path);if(r)return r;
 if(base->readonly||base->mount_tag)return SF_READ_ONLY;
 /* Recheck every current parent, including intermediate directories reached
  * from a wider capability. A mount or read-only subtree cannot be bypassed. */
 for(size_t i=strlen(base->path);path[i];i++)if(path[i]=='/'){
  path[i]=0;int parent=node_find(n,path);path[i]='/';
  if(parent<0||n->nodes[parent].type!=NODE_DIR)return SF_NOT_FOUND;
  if(n->nodes[parent].readonly||n->nodes[parent].mount_tag)return SF_READ_ONLY;
 }
 int target=node_find(n,path);if(target<0)return target;SfNode*x=&n->nodes[target];
 if(x->readonly||x->mount_tag)return SF_READ_ONLY;if(x->type!=NODE_FILE)return SF_INVALID;
 /* This service serializes mutation and opening. Keeping open files busy
  * prevents an old description from ever aliasing a reused node slot. */
 for(unsigned i=0;i<32;i++)if(n->descriptions[i].refs&&n->descriptions[i].node==(unsigned)target)return SF_BUSY;
 memset(x,0,sizeof(*x));return 0;
}
int sf_ns_dup(SfNamespace*n,uint32_t owner,uint32_t h){SfDescription*d=description(n,owner,h);return d?handle_new(n,owner,d-n->descriptions):SF_STALE_HANDLE;}
int sf_ns_close(SfNamespace*n,uint32_t owner,uint32_t h){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!--d->refs){SfNode*x=&n->nodes[d->node];if(x->type==NODE_PIPE&&(d->mode&OPEN_WRITE))x->writers--;if(x->mount_tag&&n->mount_close)n->mount_close(x->mount_tag);}n->handles[(h&255)-1].description=0;return 0;}
long sf_ns_read(SfNamespace*n,uint32_t owner,uint32_t h,void*out,size_t bytes){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!(d->mode&OPEN_READ))return SF_DENIED;SfNode*x=&n->nodes[d->node];if(x->type==NODE_DIR)return SF_INVALID;if(!bytes)return 0;if(x->type==NODE_CONSOLE)return SF_WOULD_BLOCK;
 if(x->type==NODE_DISK){if(!n->external_read)return SF_DEVICE_GONE;long r=n->external_read(x->external,d->offset,out,bytes);if(r>=0){if((uint64_t)r>bytes||(uint64_t)r>UINT32_MAX-d->offset)return SF_BAD_FORMAT;d->offset+=r;}return r;}
 if(x->type==NODE_PIPE){if(!x->size)return x->writers?SF_WOULD_BLOCK:0;if(bytes>x->size)bytes=x->size;for(size_t i=0;i<bytes;i++)((uint8_t*)out)[i]=x->data[(x->head+i)%sizeof(x->data)];x->head=(x->head+bytes)%sizeof(x->data);x->size-=bytes;return bytes;}
 if(d->offset>=x->size)return 0;if(bytes>x->size-d->offset)bytes=x->size-d->offset;memcpy(out,x->data+d->offset,bytes);d->offset+=bytes;return bytes;
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
long sf_ns_seek(SfNamespace*n,uint32_t owner,uint32_t h,uint64_t off){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;SfNode*x=&n->nodes[d->node];if(x->type==NODE_PIPE||x->type==NODE_CONSOLE)return SF_NOT_SEEKABLE;if((x->type!=NODE_FILE&&x->type!=NODE_DISK)||off>(x->type==NODE_DISK?x->size:sizeof(x->data)))return SF_INVALID;d->offset=off;return off;}
long sf_ns_poll(SfNamespace*n,uint32_t owner,uint32_t h){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;SfNode*x=&n->nodes[d->node];unsigned mask=0;if(x->type==NODE_PIPE){if((d->mode&1)&&(x->size||!x->writers))mask|=1;if((d->mode&2)&&x->size<sizeof(x->data))mask|=2;if(!x->writers)mask|=4;}else{if(d->mode&1)mask|=1;if((d->mode&2)&&!x->readonly)mask|=2;}return mask;}
long sf_ns_enumerate(SfNamespace*n,uint32_t owner,uint32_t h,char*out,size_t size){SfDescription*d=description(n,owner,h);if(!d)return SF_STALE_HANDLE;if(!(d->mode&1))return SF_DENIED;const char*p=n->nodes[d->node].path;if(n->nodes[d->node].type!=NODE_DIR)return SF_INVALID;size_t used=0,len=strlen(p);for(unsigned i=0;i<32;i++){const char*s=n->nodes[i].path;if(!n->nodes[i].type||!strcmp(p,s)||strlen(s)<=len)continue;const char*child;if(!strcmp(p,"/"))child=s+1;else{if(memcmp(p,s,len)||s[len]!='/')continue;child=s+len+1;}int nested=0;for(const char*c=child;*c;c++)if(*c=='/')nested=1;if(nested)continue;size_t k=strlen(child);if(k+1>size-used)return used? (long)used:SF_NO_SPACE;memcpy(out+used,child,k);out[used+k]='\n';used+=k+1;}return used;}
void sf_ns_reap(SfNamespace*n,uint32_t owner){for(unsigned i=0;i<64;i++)if(n->handles[i].description&&n->handles[i].owner==owner)sf_ns_close(n,owner,(n->handles[i].generation<<8)|(i+1));}
int sf_ns_unmount(SfNamespace*n,unsigned tag){if(!tag)return SF_INVALID;for(unsigned i=0;i<32;i++)if(n->descriptions[i].refs&&n->nodes[n->descriptions[i].node].mount_tag==tag)return SF_BUSY;for(unsigned i=0;i<32;i++)if(n->nodes[i].mount_tag==tag){if(!strcmp(n->nodes[i].path,"/disk")){n->nodes[i].mount_tag=0;n->nodes[i].readonly=0;}else memset(&n->nodes[i],0,sizeof(n->nodes[i]));}return 0;}
