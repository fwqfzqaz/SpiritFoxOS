/* SpiritFoxOS - 加密接口
 * Copyright (C) 2025 SpiritFoxOS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
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
