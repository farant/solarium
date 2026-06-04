/* nid.c — ULID-style id generator. See nid.h. C library only, never GL. */

#include "nid.h"

#include <stdlib.h>   /* rand, srand */
#include <time.h>     /* time, NULL  */

/* Crockford Base32 (no I L O U). Listed in ascending value order, which also
   matches ASCII order — so strcmp() on two ids compares them numerically, and
   because the timestamp comes first, that means chronologically. Sized [] so
   it carries its NUL; only indices 0..31 are ever used. */
static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static int     g_seeded  = 0;   /* lazy, one-time srand()         */
static sol_u32 g_counter = 0;   /* per-run tie-breaker, monotonic */

/* Big-endian, zero-padded base32 of a 32-bit value into n_chars chars. Fixed
   width + zero pad + most-significant-char-first is exactly the sortability
   property. The guard avoids C's undefined behaviour of shifting a 32-bit value
   by >= 32 bits — for those high chars the value contributes nothing (padding). */
static void encode_field(sol_u32 value, char *out, int n_chars) {
    int i;
    for (i = 0; i < n_chars; i++) {
        int     shift = 5 * (n_chars - 1 - i);
        sol_u32 v     = (shift >= 32) ? 0u : (value >> shift);
        out[i] = CROCKFORD[v & 31u];
    }
}

void nid_seed(unsigned int seed) {
    srand(seed);
    g_seeded = 1;
}

void nid_generate_at(sol_u32 seconds, char *out) {
    int i;
    if (!g_seeded) nid_seed((unsigned int)time(NULL));

    encode_field(seconds,     out,      10);   /* [0..9]   timestamp  (big-endian) */
    encode_field(g_counter++, out + 10,  6);   /* [10..15] tie-breaker (monotonic) */
    for (i = 16; i < 26; i++) {                /* [16..25] randomness              */
        out[i] = CROCKFORD[(rand() >> 4) & 31u];   /* >>4 skips rand()'s weak low bit */
    }
    out[26] = '\0';
}

void nid_generate(char *out) {
    nid_generate_at((sol_u32)time(NULL), out);
}
