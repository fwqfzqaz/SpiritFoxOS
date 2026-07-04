#ifndef KMALLOC_H
#define KMALLOC_H

#include <stdint.h>
#include <stddef.h>

/* Initialize the kernel heap allocator */
void kmalloc_init(void);

/* Allocate kernel heap memory */
void *kmalloc(size_t size);

/* Allocate and zero-fill kernel heap memory */
void *kcalloc(size_t num, size_t size);

/* Reallocate kernel heap memory */
void *krealloc(void *ptr, size_t size);

/* Free kernel heap memory */
void kfree(void *ptr);

/* Get heap statistics */
size_t kmalloc_used(void);
size_t kmalloc_total(void);

#endif /* KMALLOC_H */
