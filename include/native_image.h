#ifndef SF_NATIVE_IMAGE_H
#define SF_NATIVE_IMAGE_H
#include "package.h"
/* Input is the same authenticated immutable package snapshot used for planning.
 * Only segment and entry/base fields are replaced; grants/arguments stay owned
 * by the process manager. Output image pages are later privately copied. */
int sf_native_image(const SfPackage *,uint64_t base,void *staging,size_t capacity,SfLoadPlan *);
#endif
