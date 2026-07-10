/* b64.c — see b64.h. */

#include "b64.h"

#include <string.h>

static const char T[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t b64url_encode(const unsigned char *in, size_t in_len, char *out) {
    size_t i = 0, o = 0;
    unsigned long v;

    while (in_len - i >= 3) {
        v = ((unsigned long)in[i] << 16) | ((unsigned long)in[i + 1] << 8) | in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
        i += 3;
    }
    if (in_len - i == 1) {
        v = (unsigned long)in[i] << 16;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
    } else if (in_len - i == 2) {
        v = ((unsigned long)in[i] << 16) | ((unsigned long)in[i + 1] << 8);
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
    }
    out[o] = 0;
    return o;
}

int b64url_decode(const char *in, unsigned char *out, size_t *out_len) {
    unsigned char rev[256];
    unsigned char c0, c1, c2, c3;
    size_t n, i, o, rem;
    unsigned long v;
    int j;

    for (j = 0; j < 256; j++) rev[j] = 0xFF;
    for (j = 0; j < 64; j++) rev[(unsigned char)T[j]] = (unsigned char)j;

    n = strlen(in);
    rem = n % 4;
    if (rem == 1) return -1;

    i = 0; o = 0;
    while (n - i >= 4) {
        c0 = rev[(unsigned char)in[i]];
        c1 = rev[(unsigned char)in[i + 1]];
        c2 = rev[(unsigned char)in[i + 2]];
        c3 = rev[(unsigned char)in[i + 3]];
        if (c0 == 0xFF || c1 == 0xFF || c2 == 0xFF || c3 == 0xFF) return -1;
        v = ((unsigned long)c0 << 18) | ((unsigned long)c1 << 12)
          | ((unsigned long)c2 << 6) | c3;
        out[o++] = (unsigned char)(v >> 16);
        out[o++] = (unsigned char)(v >> 8);
        out[o++] = (unsigned char)(v);
        i += 4;
    }
    if (rem == 2) {
        c0 = rev[(unsigned char)in[i]];
        c1 = rev[(unsigned char)in[i + 1]];
        if (c0 == 0xFF || c1 == 0xFF) return -1;
        v = ((unsigned long)c0 << 18) | ((unsigned long)c1 << 12);
        out[o++] = (unsigned char)(v >> 16);
    } else if (rem == 3) {
        c0 = rev[(unsigned char)in[i]];
        c1 = rev[(unsigned char)in[i + 1]];
        c2 = rev[(unsigned char)in[i + 2]];
        if (c0 == 0xFF || c1 == 0xFF || c2 == 0xFF) return -1;
        v = ((unsigned long)c0 << 18) | ((unsigned long)c1 << 12)
          | ((unsigned long)c2 << 6);
        out[o++] = (unsigned char)(v >> 16);
        out[o++] = (unsigned char)(v >> 8);
    }
    *out_len = o;
    return 0;
}
