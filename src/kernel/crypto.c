/* SpiritFoxOS - 加密与随机数
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
#include "crypto.h"
#include "../include/string.h"

static volatile uint64_t crypto_entropy_pool = 0x5F3759DF;

static uint64_t xorshift64(volatile uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void mix_entropy(void) {
    uint64_t tsc;
    __asm__ volatile ("rdtsc" : "=A"(tsc));
    crypto_entropy_pool ^= tsc;
    crypto_entropy_pool ^= (crypto_entropy_pool >> 33) * 0xff51afd7ed558ccdULL;
    crypto_entropy_pool ^= (crypto_entropy_pool >> 33) * 0xc4ceb9fe1a85ec53ULL;
    crypto_entropy_pool ^= (crypto_entropy_pool >> 33);
}

void crypto_init(void) {
    mix_entropy();
    for (int i = 0; i < 16; i++) {
        xorshift64(&crypto_entropy_pool);
    }
}

static void random_bytes(uint8_t *buf, uint32_t len) {
    mix_entropy();
    for (uint32_t i = 0; i < len; i++) {
        if (i % 8 == 0) {
            xorshift64(&crypto_entropy_pool);
        }
        buf[i] = (uint8_t)(crypto_entropy_pool >> ((i % 8) * 8));
    }
}

static void mulmod256(const uint8_t *a, const uint8_t *b, const uint8_t *mod,
                      uint8_t *result) {
    uint8_t tmp[PERM_KEY_SIZE * 2];
    memset(tmp, 0, PERM_KEY_SIZE * 2);

    for (int i = PERM_KEY_SIZE - 1; i >= 0; i--) {
        for (int bit = 7; bit >= 0; bit--) {
            int carry = 0;
            for (int j = PERM_KEY_SIZE * 2 - 1; j > 0; j--) {
                int val = (tmp[j] << 1) | carry;
                carry = val >> 8;
                tmp[j] = (uint8_t)val;
            }
            tmp[0] = (uint8_t)((tmp[0] << 1) | carry);

            int needs_sub = 0;
            for (int j = 0; j < PERM_KEY_SIZE; j++) {
                if (tmp[j + PERM_KEY_SIZE] > mod[j]) { needs_sub = 1; break; }
                if (tmp[j + PERM_KEY_SIZE] < mod[j]) { needs_sub = 0; break; }
            }
            if (needs_sub) {
                int borrow = 0;
                for (int j = PERM_KEY_SIZE * 2 - 1; j >= PERM_KEY_SIZE; j--) {
                    int diff = tmp[j] - mod[j - PERM_KEY_SIZE] - borrow;
                    if (diff < 0) { diff += 256; borrow = 1; }
                    else borrow = 0;
                    tmp[j] = (uint8_t)diff;
                }
            }

            if (a[i] & (1 << bit)) {
                carry = 0;
                for (int j = PERM_KEY_SIZE * 2 - 1; j >= PERM_KEY_SIZE; j--) {
                    int sum = tmp[j] + b[j - PERM_KEY_SIZE] + carry;
                    carry = sum >> 8;
                    tmp[j] = (uint8_t)sum;
                }
                for (int j = PERM_KEY_SIZE - 1; j >= 0 && carry; j--) {
                    int sum = tmp[j] + carry;
                    carry = sum >> 8;
                    tmp[j] = (uint8_t)sum;
                }

                needs_sub = 0;
                for (int j = 0; j < PERM_KEY_SIZE; j++) {
                    if (tmp[j + PERM_KEY_SIZE] > mod[j]) { needs_sub = 1; break; }
                    if (tmp[j + PERM_KEY_SIZE] < mod[j]) { needs_sub = 0; break; }
                }
                if (needs_sub) {
                    int borrow2 = 0;
                    for (int j = PERM_KEY_SIZE * 2 - 1; j >= PERM_KEY_SIZE; j--) {
                        int diff = tmp[j] - mod[j - PERM_KEY_SIZE] - borrow2;
                        if (diff < 0) { diff += 256; borrow2 = 1; }
                        else borrow2 = 0;
                        tmp[j] = (uint8_t)diff;
                    }
                }
            }
        }
    }

    memcpy(result, tmp + PERM_KEY_SIZE, PERM_KEY_SIZE);
}

static const uint8_t prime_p[PERM_KEY_SIZE] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC5, 0x3B
};

static const uint8_t generator_g[PERM_KEY_SIZE] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03
};

void crypto_generate_keypair(perm_key_t *private_key, perm_key_t *public_key) {
    random_bytes(private_key->data, PERM_KEY_SIZE);
    private_key->data[0] = 0x00;
    private_key->data[1] = 0x00;
    private_key->data[2] &= 0x7F;

    mulmod256(generator_g, private_key->data, prime_p, public_key->data);
}

static void hash_data(const uint8_t *data, uint32_t len, uint8_t *out) {
    uint64_t h1 = 0x9E3779B97F4A7C15ULL;
    uint64_t h2 = 0x94D049BB133111EBULL;

    for (uint32_t i = 0; i < len; i++) {
        h1 ^= (uint64_t)data[i] * 0x100000001B3ULL;
        h1 = (h1 >> 31) | (h1 << 33);
        h2 ^= (uint64_t)data[i] * 0xC6A4A7935BD1E995ULL;
        h2 = (h2 >> 27) | (h2 << 37);
    }

    h1 ^= h2;
    h2 ^= h1;
    h1 = (h1 >> 33) * 0xff51afd7ed558ccdULL;
    h1 = (h1 >> 33) * 0xc4ceb9fe1a85ec53ULL;
    h1 ^= (h1 >> 33);
    h2 = (h2 >> 33) * 0xff51afd7ed558ccdULL;
    h2 = (h2 >> 33) * 0xc4ceb9fe1a85ec53ULL;
    h2 ^= (h2 >> 33);

    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(h1 >> (i * 8));
        out[i + 8] = (uint8_t)(h2 >> (i * 8));
    }

    uint8_t expanded[PERM_SIGNATURE_SIZE];
    for (int i = 0; i < 16; i++) {
        expanded[i] = out[i];
        expanded[i + 16] = (uint8_t)(out[i] ^ (out[i] >> 4) ^ (uint8_t)(h1 >> (i % 8)));
    }
    memcpy(out, expanded, PERM_SIGNATURE_SIZE);
}

void crypto_sign(const perm_key_t *private_key, uint64_t app_id,
                 perm_signature_t *out_sig) {
    uint8_t msg[PERM_KEY_SIZE + 8];
    memcpy(msg, private_key->data, PERM_KEY_SIZE);
    for (int i = 0; i < 8; i++) {
        msg[PERM_KEY_SIZE + i] = (uint8_t)(app_id >> (i * 8));
    }

    uint8_t h[PERM_SIGNATURE_SIZE];
    hash_data(msg, PERM_KEY_SIZE + 8, h);

    uint8_t s_inv[PERM_KEY_SIZE];
    random_bytes(s_inv, PERM_KEY_SIZE);
    s_inv[0] = 0x00;
    s_inv[1] = 0x00;
    s_inv[2] &= 0x7F;

    uint8_t r[PERM_KEY_SIZE];
    mulmod256(generator_g, s_inv, prime_p, r);

    uint8_t s[PERM_KEY_SIZE];
    mulmod256(private_key->data, r, prime_p, s);

    for (int i = 0; i < PERM_KEY_SIZE; i++) {
        out_sig->data[i] = r[i] ^ h[i];
        if (i + PERM_KEY_SIZE < PERM_SIGNATURE_SIZE) {
            out_sig->data[i + PERM_KEY_SIZE] = s[i] ^ h[(i + PERM_KEY_SIZE) % PERM_SIGNATURE_SIZE];
        }
    }
}

int crypto_verify(const perm_key_t *public_key, uint64_t app_id,
                  const perm_signature_t *sig) {
    uint8_t msg[PERM_KEY_SIZE + 8];
    memcpy(msg, public_key->data, PERM_KEY_SIZE);
    for (int i = 0; i < 8; i++) {
        msg[PERM_KEY_SIZE + i] = (uint8_t)(app_id >> (i * 8));
    }

    uint8_t h[PERM_SIGNATURE_SIZE];
    hash_data(msg, PERM_KEY_SIZE + 8, h);

    uint8_t r[PERM_KEY_SIZE];
    uint8_t s[PERM_KEY_SIZE];
    for (int i = 0; i < PERM_KEY_SIZE; i++) {
        r[i] = sig->data[i] ^ h[i];
        if (i + PERM_KEY_SIZE < PERM_SIGNATURE_SIZE) {
            s[i] = sig->data[i + PERM_KEY_SIZE] ^ h[(i + PERM_KEY_SIZE) % PERM_SIGNATURE_SIZE];
        }
    }

    uint8_t u1[PERM_KEY_SIZE];
    uint8_t u2[PERM_KEY_SIZE];
    mulmod256(generator_g, s, prime_p, u1);
    mulmod256(public_key->data, r, prime_p, u2);

    /* Compare all bytes of u1 and u2 for signature verification */
    for (int i = 0; i < PERM_KEY_SIZE; i++) {
        if (u1[i] != u2[i]) return 0;
    }
    return 1;
}
