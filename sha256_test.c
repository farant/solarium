/* sha256_test.c — SHA-256 against the FIPS 180-4 vectors (incl. the
   one-million-'a' streaming case), HMAC against RFC 4231, PBKDF2 against the
   RFC 7914 §11 published vectors. Built by `build.sh sha256test` with
   ASan/UBSan. */

#include "sha256.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void hex(const unsigned char *in, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) { out[i*2] = H[in[i] >> 4]; out[i*2+1] = H[in[i] & 15]; }
    out[n*2] = 0;
}

static void check(const char *name, const unsigned char *got, size_t n, const char *want) {
    char buf[129];
    hex(got, n, buf);
    if (strcmp(buf, want) != 0) { printf("FAIL %s\n  got  %s\n  want %s\n", name, buf, want); g_fail = 1; }
    else printf("ok   %s\n", name);
}

int main(void) {
    unsigned char d[64];
    Sha256 s;
    int i;

    sha256("", 0, d);
    check("sha256(\"\")", d, 32,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    sha256("abc", 3, d);
    check("sha256(\"abc\")", d, 32,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    check("sha256(two-block)", d, 32,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* streaming: one million 'a' in 1000-byte chunks */
    {
        char chunk[1000];
        memset(chunk, 'a', sizeof chunk);
        sha256_init(&s);
        for (i = 0; i < 1000; i++) sha256_update(&s, chunk, sizeof chunk);
        sha256_final(&s, d);
        check("sha256(1M x 'a', streamed)", d, 32,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /* HMAC-SHA256, RFC 4231 test cases 1 and 2 */
    {
        unsigned char key[20];
        memset(key, 0x0b, sizeof key);
        sha256_hmac(key, 20, "Hi There", 8, d);
        check("hmac rfc4231 tc1", d, 32,
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
        sha256_hmac("Jefe", 4, "what do ya want for nothing?", 28, d);
        check("hmac rfc4231 tc2", d, 32,
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }

    /* PBKDF2-HMAC-SHA256, RFC 7914 §11 vectors */
    sha256_pbkdf2("password", 8, "salt", 4, 1, d, 32);
    check("pbkdf2 c=1", d, 32,
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    sha256_pbkdf2("password", 8, "salt", 4, 2, d, 32);
    check("pbkdf2 c=2", d, 32,
        "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    sha256_pbkdf2("password", 8, "salt", 4, 4096, d, 32);
    check("pbkdf2 c=4096", d, 32,
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    /* constant-time compare */
    if (!sha256_ct_equal("aaaa", "aaaa", 4)) { printf("FAIL ct_equal same\n"); g_fail = 1; }
    if (sha256_ct_equal("aaaa", "aaab", 4))  { printf("FAIL ct_equal diff\n"); g_fail = 1; }

    if (g_fail) { printf("sha256_test: FAIL\n"); return 1; }
    printf("sha256_test: all ok\n");
    return 0;
}
