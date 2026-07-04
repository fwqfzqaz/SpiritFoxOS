#ifndef INIT_H
#define INIT_H

#include "boot.h"

void init_core(BootInfo *boot_info);
void init_hardware(void);
void init_storage(void);
void init_filesystem(void);
void init_services(void);
void init_fs_hierarchy(void);

#endif /* INIT_H */
