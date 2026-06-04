/* nid_test.c — standalone exercise for the nid generator. Checks length and
   alphabet, the time-sortability property, the same-second tie-break ordering,
   and uniqueness under a burst at one timestamp. Built by `build.sh nidtest`
   with ASan/UBSan. */

#include "nid.h"

#include <stdio.h>
#include <string.h>

static const char *ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static int valid_id(const char *id) {
    int i;
    for (i = 0; i < NID_LEN; i++) {
        if (strchr(ALPHABET, id[i]) == NULL) return 0;   /* char outside Crockford */
    }
    return id[NID_LEN] == '\0';                          /* exactly NID_LEN long */
}

int main(void) {
    char a[NID_LEN + 1], b[NID_LEN + 1], c[NID_LEN + 1];
    int  i, j;

    nid_seed(1234);                                      /* reproducible run */

    printf("--- sample nids ---\n");
    nid_generate_at(1000u, a); printf("t=1000  %s\n", a);
    nid_generate_at(2000u, b); printf("t=2000  %s\n", b);
    nid_generate_at(2000u, c); printf("t=2000  %s   (same second as above)\n", c);

    if (!valid_id(a) || !valid_id(b) || !valid_id(c)) {
        printf("FAIL: id has a bad length or a non-Crockford character\n");
        return 1;
    }
    printf("length == %d and all chars in Crockford alphabet: ok\n", NID_LEN);

    if (!(strcmp(a, b) < 0)) {                            /* earlier time -> smaller */
        printf("FAIL: t=1000 id should sort before t=2000 id\n");
        return 1;
    }
    if (!(strcmp(b, c) < 0)) {                            /* same second -> by counter */
        printf("FAIL: same-second ids should be strictly ordered by the counter\n");
        return 1;
    }
    printf("sort order tracks (timestamp, then counter): ok\n");

    {   /* a burst at one timestamp: every id must still be distinct */
        enum { N = 256 };
        char ids[N][NID_LEN + 1];
        for (i = 0; i < N; i++) nid_generate_at(5000u, ids[i]);
        for (i = 0; i < N; i++) {
            for (j = i + 1; j < N; j++) {
                if (strcmp(ids[i], ids[j]) == 0) {
                    printf("FAIL: duplicate id within one second (indices %d, %d)\n", i, j);
                    return 1;
                }
            }
        }
        printf("%d ids minted in one second, all unique: ok\n", N);
    }

    printf("nid_test: OK\n");
    return 0;
}
