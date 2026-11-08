#ifndef SF_NATIVE_IMAGE_H
#define SF_NATIVE_IMAGE_H
#include "package.h"
/* Input is the same authenticated immutable package snapshot used for planning.
 * Only segment and entry/base fields are replaced; grants/arguments stay owned
 * by the process manager. Output image pages are later privately copied. */
int sf_native_image(const SfPackage *,uint64_t base,void *staging,size_t capacity,SfLoadPlan *);
/* Bounded load-time linking: the authenticated shared library image is
 * appended to the application address space and every declared import slot
 * is bound by (name, ABI) to an exported code symbol. No other process
 * address is ever written into the slot. */
int sf_native_link(const SfPackage *app,const SfPackage *library,uint64_t base,void *staging,size_t capacity,SfLoadPlan *);
#endif
