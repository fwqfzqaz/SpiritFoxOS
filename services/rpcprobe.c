#include "libsf.h"
#include "namespace.h"
#include "arguments.h"
static uint64_t sequence;
static int rpc(const SfBootArgs*a,SfMessage*m){m->h.request_id=++sequence;int r=sf_send(a->manager,m);if(r)return r;long from=sf_recv(a->service,m,sf_deadline(3000));return from<0?from:m->h.request_id!=sequence?SF_INVALID:(int)m->status;}
static int oversized(const SfBootArgs*a,unsigned op,uint32_t h){
 const uint32_t lengths[]={257,512,0x80000000u,UINT32_MAX};
 for(unsigned i=0;i<4;i++){SfMessage m;sf_message(&m,op,0);m.handle=h;m.length=lengths[i];memset(m.data,'^',sizeof(m.data));if(sf_send(a->manager,&m)!=SF_INVALID||m.length!=lengths[i])return 1;}return 0;
}
long sf_main(const SfBootArgs*a,size_t bytes){
 if(sf_boot_arguments_validate(a,bytes))return 1;
 SfMessage m;sf_message(&m,OP_ROOT,0);if(rpc(a,&m))return 2;uint32_t root=m.handle;
 if(oversized(a,OP_ENUMERATE,root))return 3;
 const char*path=a->argc>1?sf_argument(a,bytes,1):"readme.txt";size_t n=strlen(path)+1;if(n>SF_MSG_BYTES)return 4;
 sf_message(&m,OP_OPENAT,0);m.handle=root;m.mode=OPEN_READ;m.length=n;memcpy(m.data,path,n);if(rpc(a,&m))return 4;uint32_t file=m.handle;
 if(oversized(a,OP_READ,file))return 5;
 sf_message(&m,OP_READ,0);m.handle=file;m.length=SF_MSG_BYTES;int got=rpc(a,&m);if(got<=0||got>SF_MSG_BYTES||m.length!=(unsigned)got)return 6;
 sf_message(&m,OP_TRUNCATE,0);m.handle=file;m.offset=0;if(rpc(a,&m)!=SF_DENIED)return 7;
 sf_message(&m,OP_WRITE,0);m.handle=file;m.length=1;if(rpc(a,&m)!=SF_DENIED)return 8;
 sf_message(&m,OP_OPEN,0);m.mode=OPEN_WRITE;m.length=13;memcpy(m.data,"/dev/console",13);if(rpc(a,&m))return 9;uint32_t output=m.handle;
 if(oversized(a,OP_WRITE,output))return 10;
 sf_message(&m,OP_WRITE,0);m.handle=output;const char*text="FILE RPC APP PASSED\n";m.length=strlen(text);memcpy(m.data,text,m.length);if(rpc(a,&m)<0)return 11;
 uint32_t handles[]={root,file,output};for(unsigned i=0;i<3;i++){sf_message(&m,OP_CLOSE,0);m.handle=handles[i];if(rpc(a,&m))return 12;}
 return 0;
}
