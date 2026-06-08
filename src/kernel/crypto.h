#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include "perm.h"

void crypto_init(void);

void crypto_generate_keypair(perm_key_t *private_key, perm_key_t *public_key);

void crypto_sign(const perm_key_t *private_key, uint64_t app_id,
                 perm_signature_t *out_sig);

int crypto_verify(const perm_key_t *public_key, uint64_t app_id,
                  const perm_signature_t *sig);

#endif
