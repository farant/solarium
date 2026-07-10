/* sha256.h — SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104), PBKDF2-HMAC-SHA256
   (RFC 8018), and a constant-time compare. Strict C89, depends on the C
   library only. Serves the server arc: PKCE S256, token hashing at rest, the
   password KDF. Sibling in spirit to nid.c/json.c. */
#ifndef SHA256_H
#define SHA256_H

#include "sol_base.h"
#include <stddef.h>

typedef struct {
    sol_u32 state[8];
    sol_u32 count_lo, count_hi;   /* total input BYTES, 64-bit split across two u32 */
    unsigned char buf[64];
    unsigned buf_len;
} Sha256;

void sha256_init  (Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t len);
void sha256_final (Sha256 *s, unsigned char out[32]);
void sha256       (const void *data, size_t len, unsigned char out[32]);

void sha256_hmac  (const void *key, size_t key_len,
                   const void *msg, size_t msg_len, unsigned char out[32]);
void sha256_pbkdf2(const void *pw, size_t pw_len,
                   const void *salt, size_t salt_len,
                   sol_u32 iters, unsigned char *out, size_t out_len);

/* 1 if equal, 0 if not; runs in time independent of contents. */
int sha256_ct_equal(const void *a, const void *b, size_t len);

#endif /* SHA256_H */
