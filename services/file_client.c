#include "sf_file.h"
#ifndef SF_FILE_SYSCALL
#include "libsf.h"
#define SF_FILE_SYSCALL sf_sys
#else
/* Host models replace only INT80 transport, not the client codec. */
extern int64_t SF_FILE_SYSCALL(uint64_t,uint64_t,uint64_t,uint64_t);
#endif
static uint64_t sf_file_sequence;
static int64_t bad_reply(SfFileClient*c){c->error=SF_BAD_FORMAT;return c->error;}
int sf_file_client_init(SfFileClient*c,uint64_t manager,uint64_t reply,uint32_t timeout_ms){
 if(!c||!manager||manager>UINT32_MAX||!reply||reply>UINT32_MAX||!timeout_ms||timeout_ms>60000)return SF_INVALID;
 *c=(SfFileClient){manager,reply,timeout_ms,0};return 0;
}
static int64_t exchange(SfFileClient*c,SfMessage*m,unsigned op,int handle){
 if(!c||!c->manager||!c->reply||!c->timeout_ms||c->timeout_ms>60000)return SF_INVALID;
 if(c->error)return c->error;
 if(handle&&(!m->handle||m->handle>UINT32_MAX))return SF_STALE_HANDLE;
 if(sf_file_sequence==UINT64_MAX)return SF_NO_SPACE;
 int64_t ticks=SF_FILE_SYSCALL(SYS_TICKS,0,0,0);uint64_t delta=(uint64_t)c->timeout_ms*1000000;
 if(ticks<0||(uint64_t)ticks>(UINT64_MAX-delta)/10000000)return SF_INVALID;
 uint64_t deadline=(uint64_t)ticks*10000000+delta,id=++sf_file_sequence;
 m->h=(SfIpcHeader){40,1,0,op,0,id,sizeof(*m)-40,0,deadline};
 int64_t r=SF_FILE_SYSCALL(SYS_SEND,c->manager,(uintptr_t)m,0);if(r<0)return r;if(r)return bad_reply(c);
 for(unsigned stale=0;;){
  r=SF_FILE_SYSCALL(SYS_RECV,c->reply,(uintptr_t)m,deadline);
  if(r<0){c->error=r;return r;}
  if(r!=1||m->h.flags!=1||m->h.size!=40||m->h.protocol_major!=1||m->h.protocol_minor||m->h.payload_bytes!=sizeof(*m)-40||m->h.cap_count||m->length>sizeof(m->data))return bad_reply(c);
  if(m->h.request_id!=id){if(m->h.request_id&&m->h.request_id<id&&stale++<8)continue;return bad_reply(c);}
  if(m->h.opcode!=op)return bad_reply(c);return m->status;
 }
}
static int path_message(SfMessage*m,const char*path){
 if(!path)return SF_INVALID;unsigned n=0;while(n<sizeof(m->data)&&path[n])n++;
 if(!n||n==sizeof(m->data))return SF_INVALID;memcpy(m->data,path,n+1);m->length=n+1;return 0;
}
static int64_t opened(SfFileClient*c,SfMessage*m,int64_t r){
 if(r<0)return r;if(r||!m->handle||m->handle>UINT32_MAX)return bad_reply(c);return m->handle;
}
int64_t sf_file_root(SfFileClient*c){SfMessage m={0};int64_t r=exchange(c,&m,OP_ROOT,0);return opened(c,&m,r);}
int64_t sf_file_console(SfFileClient*c){SfMessage m={0};path_message(&m,"/dev/console");m.mode=SF_OPEN_WRITE;int64_t r=exchange(c,&m,OP_OPEN,0);return opened(c,&m,r);}
int64_t sf_file_openat(SfFileClient*c,uint64_t h,const char*path,uint32_t mode){
 SfMessage m={0};if(path_message(&m,path))return SF_INVALID;m.handle=h;m.mode=mode;int64_t r=exchange(c,&m,OP_OPENAT,1);return opened(c,&m,r);
}
static int64_t read_data(SfFileClient*c,uint64_t h,void*out,size_t bytes,unsigned op){
 SfMessage m={0};if(bytes&&!out)return SF_INVALID;size_t n=bytes>sizeof(m.data)?sizeof(m.data):bytes;m.handle=h;m.length=n;
 int64_t r=exchange(c,&m,op,1);if(r<0)return r;if((uint64_t)r>n||m.length!=(uint64_t)r)return bad_reply(c);if(r)memcpy(out,m.data,r);return r;
}
int64_t sf_file_read(SfFileClient*c,uint64_t h,void*out,size_t bytes){return read_data(c,h,out,bytes,OP_READ);}
int64_t sf_file_enumerate(SfFileClient*c,uint64_t h,char*out,size_t bytes){return read_data(c,h,out,bytes,OP_ENUMERATE);}
int64_t sf_file_write(SfFileClient*c,uint64_t h,const void*data,size_t bytes){
 SfMessage m={0};if(bytes&&!data)return SF_INVALID;size_t n=bytes>sizeof(m.data)?sizeof(m.data):bytes;m.handle=h;m.length=n;if(n)memcpy(m.data,data,n);
 int64_t r=exchange(c,&m,OP_WRITE,1);if(r>=0&&(uint64_t)r>n)return bad_reply(c);return r;
}
int64_t sf_file_stat(SfFileClient*c,uint64_t h,SfFileStat*out){
 if(!out)return SF_INVALID;SfMessage m={0};m.handle=h;int64_t r=exchange(c,&m,OP_STAT,1);if(r<0)return r;if(r||m.length!=sizeof(*out))return bad_reply(c);
 SfFileStat value;memcpy(&value,m.data,sizeof(value));if(value.bytes!=sizeof(value)||value.type<SF_TYPE_DIRECTORY||value.type>SF_TYPE_DISK||!(value.mode&3)||(value.mode&~15u)||(value.flags&~7u))return bad_reply(c);
 memcpy(out,&value,sizeof(value));return 0;
}
int64_t sf_file_seek(SfFileClient*c,uint64_t h,uint64_t offset){
 if(offset>INT64_MAX)return SF_INVALID;SfMessage m={0};m.handle=h;m.offset=offset;int64_t r=exchange(c,&m,OP_SEEK,1);if(r>=0&&(uint64_t)r!=offset)return bad_reply(c);return r;
}
int64_t sf_file_poll(SfFileClient*c,uint64_t h){SfMessage m={0};m.handle=h;int64_t r=exchange(c,&m,OP_POLL,1);return r>7?bad_reply(c):r;}
static int64_t mutation(SfFileClient*c,uint64_t h,unsigned op,uint64_t offset,const char*path){
 SfMessage m={0};m.handle=h;m.offset=offset;if(path&&path_message(&m,path))return SF_INVALID;
 if((op==OP_UNLINK||op==OP_RMDIR)&&!path)return SF_INVALID;
 int64_t r=exchange(c,&m,op,1);return r>0?bad_reply(c):r;
}
int64_t sf_file_close(SfFileClient*c,uint64_t h){return mutation(c,h,OP_CLOSE,0,0);}
int64_t sf_file_truncate(SfFileClient*c,uint64_t h,uint64_t bytes){return mutation(c,h,OP_TRUNCATE,bytes,0);}
int64_t sf_file_unlink(SfFileClient*c,uint64_t h,const char*path){return mutation(c,h,OP_UNLINK,0,path);}
int64_t sf_file_rmdir(SfFileClient*c,uint64_t h,const char*path){return mutation(c,h,OP_RMDIR,0,path);}
