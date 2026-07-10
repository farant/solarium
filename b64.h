/* b64.h — base64url (RFC 4648 §5), unpadded, as OAuth/PKCE and our opaque
   tokens use it. Strict C89, no deps. */
#ifndef B64_H
#define B64_H

#include <stddef.h>

/* Buffer size needed to encode n bytes (worst case incl. the NUL). */
#define B64URL_ENC_MAX(n) (((n) + 2) / 3 * 4 + 1)

/* Encode in[0..in_len) into out as unpadded base64url; NUL-terminates and
   returns the string length. out must hold B64URL_ENC_MAX(in_len). */
size_t b64url_encode(const unsigned char *in, size_t in_len, char *out);

/* Decode a NUL-terminated unpadded base64url string. Returns 0 and sets
   *out_len on success, -1 on any malformed input ('=' padding included).
   out must hold strlen(in) / 4 * 3 + 2 bytes. */
int b64url_decode(const char *in, unsigned char *out, size_t *out_len);

#endif /* B64_H */
