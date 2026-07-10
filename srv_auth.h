/* srv_auth.h — users (PBKDF2-HMAC-SHA256), opaque bearer sessions (tokens
   stored SHA-256-hashed, spec §7), and the per-IP login throttle (spec §9).
   The OAuth grant machinery of sub-project 4 will grow here beside sessions.
   Timing rules: the KDF never runs under the write lock, and an unknown user
   still pays one KDF so name discovery can't ride on response time. */
#ifndef SRV_AUTH_H
#define SRV_AUTH_H

#include "srv_db.h"
#include "sol_base.h"

#define SRV_TOKEN_CHARS 43          /* 32 random bytes, base64url unpadded */
#define SRV_PBKDF2_ITERS 600000u    /* interim KDF strength; Argon2 is the
                                       documented upgrade path (spec §11) */

int  srv_auth_user_create(SrvDb *db, const char *name, const char *password);
int  srv_auth_user_create_iters(SrvDb *db, const char *name,
                                const char *password, sol_u32 iters);

long srv_auth_login(SrvDb *db, const char *name, const char *password,
                    char token_out[SRV_TOKEN_CHARS + 1]);
long srv_auth_session_user(SrvDb *db, const char *token);
void srv_auth_logout(SrvDb *db, const char *token);

int  srv_auth_throttle_ok   (SrvDb *db, const char *ip);
void srv_auth_throttle_fail (SrvDb *db, const char *ip);
void srv_auth_throttle_clear(SrvDb *db, const char *ip);

#endif /* SRV_AUTH_H */
