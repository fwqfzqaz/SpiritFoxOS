#ifndef NAMESPACE_H
#define NAMESPACE_H
#include "sf.h"
enum { NODE_DIR=1,NODE_FILE,NODE_PIPE,NODE_CONSOLE };
enum { OPEN_READ=1,OPEN_WRITE=2,OPEN_NONBLOCK=4 };
typedef struct {char path[96];uint8_t type,readonly;uint16_t writers;uint32_t size,head;uint8_t data[1024];} SfNode;
typedef struct {uint32_t node,offset,mode,refs;} SfDescription;
typedef struct {uint32_t owner,generation,description;} SfFileHandle;
typedef struct {SfNode nodes[32];SfDescription descriptions[32];SfFileHandle handles[64];long(*console)(const void*,size_t);} SfNamespace;
void sf_ns_init(SfNamespace*,long(*)(const void*,size_t));
int sf_ns_create(SfNamespace*,const char*,unsigned,int);
int sf_ns_open(SfNamespace*,uint32_t,const char*,const char*,unsigned);
int sf_ns_dup(SfNamespace*,uint32_t,uint32_t);
int sf_ns_close(SfNamespace*,uint32_t,uint32_t);
long sf_ns_read(SfNamespace*,uint32_t,uint32_t,void*,size_t);
long sf_ns_write(SfNamespace*,uint32_t,uint32_t,const void*,size_t);
long sf_ns_seek(SfNamespace*,uint32_t,uint32_t,uint64_t);
long sf_ns_poll(SfNamespace*,uint32_t,uint32_t);
long sf_ns_enumerate(SfNamespace*,uint32_t,uint32_t,char*,size_t);
void sf_ns_reap(SfNamespace*,uint32_t);
#endif
