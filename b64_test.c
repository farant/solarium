/* b64_test.c — base64url against the RFC 4648 §10 vectors (translated to the
   URL-safe alphabet, unpadded) + roundtrips incl. bytes that hit '-' and '_'.
   Built by `build.sh b64test` with ASan/UBSan. */

#include "b64.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void enc_check(const char *in, const char *want) {
    char out[64];
    size_t n = b64url_encode((const unsigned char *)in, strlen(in), out);
    if (n != strlen(want) || strcmp(out, want) != 0) {
        printf("FAIL enc(\"%s\"): got \"%s\" want \"%s\"\n", in, out, want);
        g_fail = 1;
    } else printf("ok   enc(\"%s\") = \"%s\"\n", in, want);
}

int main(void) {
    unsigned char raw[64], back[64];
    char enc[128];
    size_t n, m;
    int i;

    enc_check("", "");
    enc_check("f", "Zg");
    enc_check("fo", "Zm8");
    enc_check("foo", "Zm9v");
    enc_check("foob", "Zm9vYg");
    enc_check("fooba", "Zm9vYmE");
    enc_check("foobar", "Zm9vYmFy");

    /* bytes that exercise the URL-safe alphabet ('-' and '_') */
    raw[0] = 0xfb; raw[1] = 0xff; raw[2] = 0xbf;
    n = b64url_encode(raw, 3, enc);
    if (strcmp(enc, "-_-_") != 0) { printf("FAIL urlsafe: got \"%s\"\n", enc); g_fail = 1; }
    else printf("ok   urlsafe \"-_-_\"\n");

    /* roundtrip every length 0..48 with a byte ramp */
    for (i = 0; i < 48; i++) raw[i] = (unsigned char)(i * 7 + 3);
    for (i = 0; i <= 48; i++) {
        b64url_encode(raw, (size_t)i, enc);
        if (b64url_decode(enc, back, &m) != 0 || m != (size_t)i
            || memcmp(raw, back, m) != 0) {
            printf("FAIL roundtrip len %d\n", i);
            g_fail = 1;
        }
    }
    printf("ok   roundtrips 0..48\n");

    /* malformed inputs must be rejected */
    if (b64url_decode("A", back, &m) == 0)    { printf("FAIL: lone char accepted\n"); g_fail = 1; }
    if (b64url_decode("ab!c", back, &m) == 0) { printf("FAIL: '!' accepted\n"); g_fail = 1; }
    if (b64url_decode("ab=c", back, &m) == 0) { printf("FAIL: '=' accepted (unpadded format)\n"); g_fail = 1; }

    if (g_fail) { printf("b64_test: FAIL\n"); return 1; }
    printf("b64_test: all ok\n");
    return 0;
}
