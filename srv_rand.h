/* srv_rand.h — CSPRNG bytes for the server: session/OAuth tokens, salts.
   Platform glue (arc4random_buf on macOS/BSD, getrandom(2) on Linux), so this
   TU is exempt from c89check the way platform_*.c are. */
#ifndef SRV_RAND_H
#define SRV_RAND_H

#include <stddef.h>

/* Fill buf with n cryptographically-random bytes. Aborts on failure — a
   server that cannot mint unpredictable tokens must not keep serving. */
void srv_rand_bytes(void *buf, size_t n);

#endif /* SRV_RAND_H */
