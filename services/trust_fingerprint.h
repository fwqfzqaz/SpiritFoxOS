#ifndef SF_TRUST_FINGERPRINT_H
#define SF_TRUST_FINGERPRINT_H
#include "package.h"

/* Canonical digest of semantic trust: domain `SFTRUST-SNAP-V1\0`, LE32 key
 * count, sorted public-key/usage/revoked rows, then LE32 revoked count and
 * sorted runtime IDs. No structure padding, pointers, or unused tail bytes. */
int sf_trust_fingerprint(const SfRuntimeTrust*,uint8_t out[32]);
#endif
