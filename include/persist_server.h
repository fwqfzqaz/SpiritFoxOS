#ifndef PERSIST_SERVER_H
#define PERSIST_SERVER_H
#include <stddef.h>
#include <stdint.h>
/* Volume-side API used only by the sf-persist process. The volume, transaction
 * banks and update state live in that address space; sf-init talks to them
 * exclusively through include/persistence.h. */
int persist_mount(uint64_t*notes_bytes);
int persist_volume_ready(void);
long persist_notes_read(uint64_t offset,void*out,size_t bytes);
long persist_notes_write(uint64_t offset,const void*data,size_t bytes);
int persist_notes_truncate(uint64_t bytes);
int persist_stage(uint64_t offset,const void*data,size_t bytes);
size_t persist_staged_bytes(void);
int persist_install(void);
int persist_begin(void);
int persist_finish(int success);
int persist_revoke(void);
long persist_export(int preview);
const uint8_t* persist_export_data(void);
long persist_revoked_list(void*out,size_t capacity);
void persist_status(void);
#endif
