/* sha256.c — see sha256.h. Verified against the FIPS 180-4 / RFC 4231 /
   RFC 7914 published vectors in sha256_test.c. */

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const sol_u32 K256[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static void sha256_block(Sha256 *s, const unsigned char *p) {
    sol_u32 w[64];
    sol_u32 a, b, c, d, e, f, g, h;
    sol_u32 s0, s1, t1, t2, ch, maj;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((sol_u32)p[i * 4] << 24) | ((sol_u32)p[i * 4 + 1] << 16)
             | ((sol_u32)p[i * 4 + 2] << 8) | (sol_u32)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->state[0]; b = s->state[1]; c = s->state[2]; d = s->state[3];
    e = s->state[4]; f = s->state[5]; g = s->state[6]; h = s->state[7];
    for (i = 0; i < 64; i++) {
        s1  = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        ch  = (e & f) ^ (~e & g);
        t1  = h + s1 + ch + K256[i] + w[i];
        s0  = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2  = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->state[0] += a; s->state[1] += b; s->state[2] += c; s->state[3] += d;
    s->state[4] += e; s->state[5] += f; s->state[6] += g; s->state[7] += h;
}

void sha256_init(Sha256 *s) {
    s->state[0] = 0x6a09e667UL; s->state[1] = 0xbb67ae85UL;
    s->state[2] = 0x3c6ef372UL; s->state[3] = 0xa54ff53aUL;
    s->state[4] = 0x510e527fUL; s->state[5] = 0x9b05688cUL;
    s->state[6] = 0x1f83d9abUL; s->state[7] = 0x5be0cd19UL;
    s->count_lo = 0; s->count_hi = 0;
    s->buf_len = 0;
}

void sha256_update(Sha256 *s, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    sol_u32 old_lo;
    unsigned space, take;

    old_lo = s->count_lo;
    s->count_lo = (sol_u32)(old_lo + (sol_u32)len);              /* bytes mod 2^32 */
    if (s->count_lo < old_lo) s->count_hi++;
    s->count_hi += (sol_u32)(((unsigned long)len >> 16) >> 16);  /* high half on LP64; 0 on ILP32 */

    while (len > 0) {
        space = 64u - s->buf_len;
        take = (len < (size_t)space) ? (unsigned)len : space;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take;
        p += take;
        len -= take;
        if (s->buf_len == 64u) { sha256_block(s, s->buf); s->buf_len = 0; }
    }
}

void sha256_final(Sha256 *s, unsigned char out[32]) {
    unsigned char tail[72];
    sol_u32 lo_bits, hi_bits;
    unsigned pad;
    int i;

    hi_bits = (s->count_hi << 3) | (s->count_lo >> 29);
    lo_bits = s->count_lo << 3;

    /* 0x80, zeros to 56 mod 64, then the 8-byte big-endian bit count */
    pad = (s->buf_len < 56u) ? (56u - s->buf_len) : (120u - s->buf_len);
    tail[0] = 0x80;
    for (i = 1; i < (int)pad; i++) tail[i] = 0;
    tail[pad + 0] = (unsigned char)(hi_bits >> 24);
    tail[pad + 1] = (unsigned char)(hi_bits >> 16);
    tail[pad + 2] = (unsigned char)(hi_bits >> 8);
    tail[pad + 3] = (unsigned char)(hi_bits);
    tail[pad + 4] = (unsigned char)(lo_bits >> 24);
    tail[pad + 5] = (unsigned char)(lo_bits >> 16);
    tail[pad + 6] = (unsigned char)(lo_bits >> 8);
    tail[pad + 7] = (unsigned char)(lo_bits);
    sha256_update(s, tail, (size_t)pad + 8);

    for (i = 0; i < 8; i++) {
        out[i * 4 + 0] = (unsigned char)(s->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s->state[i]);
    }
}

void sha256(const void *data, size_t len, unsigned char out[32]) {
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

void sha256_hmac(const void *key, size_t key_len,
                 const void *msg, size_t msg_len, unsigned char out[32]) {
    unsigned char k[64], pad[64], inner[32];
    Sha256 s;
    int i;

    memset(k, 0, 64);
    if (key_len > 64) sha256(key, key_len, k);
    else memcpy(k, key, key_len);

    for (i = 0; i < 64; i++) pad[i] = (unsigned char)(k[i] ^ 0x36);
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, msg, msg_len);
    sha256_final(&s, inner);

    for (i = 0; i < 64; i++) pad[i] = (unsigned char)(k[i] ^ 0x5c);
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, inner, 32);
    sha256_final(&s, out);
}

void sha256_pbkdf2(const void *pw, size_t pw_len,
                   const void *salt, size_t salt_len,
                   sol_u32 iters, unsigned char *out, size_t out_len) {
    unsigned char block[32], u[32];
    unsigned char *si;
    sol_u32 i, blkno;
    size_t off, take;
    int j;

    si = (unsigned char *)malloc(salt_len + 4);
    if (!si) abort();
    memcpy(si, salt, salt_len);

    blkno = 1;
    off = 0;
    while (off < out_len) {
        si[salt_len + 0] = (unsigned char)(blkno >> 24);
        si[salt_len + 1] = (unsigned char)(blkno >> 16);
        si[salt_len + 2] = (unsigned char)(blkno >> 8);
        si[salt_len + 3] = (unsigned char)(blkno);
        sha256_hmac(pw, pw_len, si, salt_len + 4, u);
        memcpy(block, u, 32);
        for (i = 1; i < iters; i++) {
            sha256_hmac(pw, pw_len, u, 32, u);
            for (j = 0; j < 32; j++) block[j] ^= u[j];
        }
        take = (out_len - off < 32) ? (out_len - off) : 32;
        memcpy(out + off, block, take);
        off += take;
        blkno++;
    }
    free(si);
}

int sha256_ct_equal(const void *a, const void *b, size_t len) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    unsigned char d = 0;
    size_t i;
    for (i = 0; i < len; i++) d = (unsigned char)(d | (x[i] ^ y[i]));
    return d == 0;
}
