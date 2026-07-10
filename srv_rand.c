/* srv_rand.c — see srv_rand.h. */

#include "srv_rand.h"

#ifdef __APPLE__

#include <stdlib.h>

void srv_rand_bytes(void *buf, size_t n) {
    arc4random_buf(buf, n);
}

#else /* Linux */

#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>

void srv_rand_bytes(void *buf, size_t n) {
    unsigned char *p = (unsigned char *)buf;
    while (n > 0) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) { perror("srv_rand: getrandom"); abort(); }
        p += r;
        n -= (size_t)r;
    }
}

#endif
