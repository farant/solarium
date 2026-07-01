# Server Foundation (solsrv) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Each task is built by a fresh implementer, spec-reviewed and quality-reviewed ("don't trust the report"), then committed.

**Goal:** Sub-project 1 of the server arc — a deployable C server (`solsrv`) with CivetWeb + SQLite embedded, an event-sourced data core, users/sessions with login throttling, a minimal authenticated `/boards` HTML page, and the VPS deploy kit (systemd + Caddy).

**Architecture:** One binary, sources flat in the repo as `srv_*.c`, sharing the engine's pure modules (`json.c`, `nid.c`, `sol_base.h`) and never linking GL/Metal/GLFW. An append-only `events` table is the source of truth; projection tables update in the same transaction. CivetWeb is thread-per-request; SQLite runs WAL with one mutex-guarded write connection and short-lived read-only connections. TLS never appears in C — Caddy terminates HTTPS in production; `solsrv` binds loopback only.

**Tech Stack:** C89 sources (server TUs + new pure modules `sha256.c`, `b64.c`); vendored CivetWeb v1.16 (MIT) + SQLite amalgamation (public domain) as the only new deps; hand-written `build.sh` gains `server` + test modes; integration via `srv_test.sh` (curl).

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-01-server-sync-web-mcp-architecture-design.md` is the source of truth for scope. Sub-project 1 ONLY — no sync endpoints, no OAuth, no MCP, no JSON writer (those are sub-projects 2–4).
- **Branch:** this plan assumes the spec/plan have reached `main` (they ride the `map-pins` ff-merge). Start with `git checkout -b server-foundation main`.
- **C89** (`-std=c89 -pedantic-errors -Werror -Wall -Wextra`) for ALL new `.c`/`.h` except `srv_rand.c` (platform glue, exempt like `platform_*.c`) and `vendor/` (not ours, compiled `-w`, headers via `-isystem vendor`). Consequences you must respect in server code: declarations at the TOP of every block; `/* */` comments only; **no `long long`** (use `long` — LP64 on both macOS and Linux targets, so 64-bit); **no `snprintf`** (C99 — use `sprintf` only into provably-large-enough buffers); no `strdup` (POSIX — write a local `xstrdup`).
- **`*_test.c` files build under `-std=c11`** with ASan+UBSan via `build.sh` modes, following the existing pattern (see `build.sh nidtest`).
- **No TLS, no port 443, no cert code.** `solsrv` binds `127.0.0.1` by default. **Never bind `0.0.0.0` in any default or example.**
- **Prepared statements only** — never assemble SQL from strings. **`sb_put_html` for every dynamic value** that reaches HTML.
- **Running things:** subagents MAY run `./solsrv`, the `*_test` binaries, and `srv_test.sh` (all headless). Do NOT run `./solarium` (human live-verify only).
- **Gauntlet per task:** `./build.sh c89check` + the task's own test build+run + `./build.sh server`. Task 1 additionally runs `./build.sh` (it edits build.sh, so prove the engine build still works). Task 9 runs `./srv_test.sh`.
- **Commit discipline:** `git add` ONLY the named files; NEVER `git add -A`/`git add .`; NEVER stage `NOTES.stml` or `paper-picture.png`. Commit subject `Server foundation Task N: <what>`. Body ends EXACTLY with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

## Context the implementer needs

**Repo facts (verified against the current tree):**
- `nid.h`: `#define NID_LEN 26`; `void nid_generate(char *out)` writes NID_LEN chars + NUL using `time()`. Nids are Crockford-Base32, strcmp-sortable. Use them as entity ids.
- `sol_base.h`: `sol_u32` = `unsigned int` (32-bit assumed on all targets), `sol_bool`/`SOL_TRUE`/`SOL_FALSE`. No 64-bit typedef exists — that's why server APIs use `long` (LP64).
- `json.h`: parse-only DOM — `json_parse`, `json_free`, `json_member`, `json_string` (returns NULL on absence/mismatch — including when the value you pass IS null). Strict C89, no deps. The server links it for projection payloads. There is NO writer; do not add one in this sub-project.
- `build.sh` style: a linear chain of `if [ "$MODE" = ... ]` blocks, one per target; test modes compile with `-std=c11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Wextra` and print a "built ./<name> — run it" line. The `c89check` mode syntax-checks a fixed list of sources.
- `.gitignore` uses leading-slash absolute entries for build artifacts.

**Vendored-code ground rules:** `vendor/` files are third-party — never edit them, never reformat them, compile them `-w`, and include their headers with `-isystem vendor` so `-pedantic-errors` doesn't fire inside them. Our code may use `sqlite3_int64` only via casts at bind/column sites; API surfaces use `long`.

**SQLite concurrency model (spec §4):** one write connection lives in `SrvDb`, guarded by `srv_db_wlock`/`srv_db_wunlock` — EVERY touch of `db->w` holds the lock, including multi-statement transactions. Reads open a short-lived `SQLITE_OPEN_READONLY` connection per request (`srv_db_ropen`/`srv_db_rclose`); WAL means readers never block the writer. Never run the PBKDF2 KDF while holding the write lock (Task 6 shows where this bites).

**CivetWeb API you'll use (all stable since v1.12):** `mg_init_library(0)` / `mg_exit_library()`; `mg_start(&callbacks, user_data, options)` / `mg_stop(ctx)`; `mg_set_request_handler(ctx, "/", handler, cbdata)` where the handler is `int h(struct mg_connection *c, void *cbdata)` returning the status code it handled; `mg_get_request_info(c)` → `->request_method`, `->local_uri`, `->remote_addr`; `mg_read(c, buf, len)`; `mg_write` / `mg_printf`; `mg_get_header(c, "Cookie")`; `mg_get_cookie(hdr, "sid", buf, sz)` (negative = absent); `mg_get_var(body, len, "user", dst, sz)` (negative = absent). We register ONE catch-all handler at `"/"` and dispatch through our own route table (spec §4) — this sidesteps CivetWeb's pattern-precedence rules entirely.

## File Structure

```
vendor/sqlite3.c/.h        (Task 1)  SQLite amalgamation, public domain
vendor/civetweb.c/.h,*.inl (Task 1)  CivetWeb v1.16, MIT (LICENSE.md alongside)
srv_main.c                 (Tasks 1,8,9)  flags, boot, route table, handlers, CLI
sha256.c/.h                (Task 2)  SHA-256 + HMAC + PBKDF2 + ct-compare (pure)
sha256_test.c              (Task 2)
b64.c/.h                   (Task 3)  base64url encode/decode (pure)
b64_test.c                 (Task 3)
srv_rand.c/.h              (Task 3)  CSPRNG bytes (platform glue, c89check-exempt)
srv_db.c/.h                (Task 4)  open/migrate/WAL, write lock, read conns
srv_db_test.c              (Task 4)
srv_events.c/.h            (Task 5)  event append + projections + query-after
srv_events_test.c          (Task 5)
srv_auth.c/.h              (Task 6)  users (PBKDF2), sessions, login throttle
srv_auth_test.c            (Task 6)
srv_web.c/.h               (Task 7)  Sb string builder, html escape, pages (pure)
srv_web_test.c             (Task 7)
srv_test.sh                (Tasks 1,8,9)  integration gauntlet (curl)
deploy/Caddyfile           (Task 10)
deploy/solsrv.service      (Task 10)
deploy/backup.sh           (Task 10)
deploy/DEPLOY.md           (Task 10)
build.sh                   (Tasks 1–8)  `server` mode + test modes + c89check adds
.gitignore                 (Task 1)   all new artifacts at once
```

---

## Task 1 — vendor the chassis; `solsrv` answers `/health`

**Files:**
- Create: `vendor/sqlite3.c`, `vendor/sqlite3.h`, `vendor/civetweb.c`, `vendor/civetweb.h`, `vendor/*.inl`, `vendor/CIVETWEB-LICENSE.md`, `srv_main.c`, `srv_test.sh`
- Modify: `build.sh`, `.gitignore`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: `./build.sh server` → `./solsrv [-p host:port] [-d dbfile] [-s]`; `vendor_objs` shell function in build.sh; `GET /health` → `200 ok`. Later tasks add their TUs to the `server` compile line and the c89check list.

- [ ] **Step 1.1 — fetch SQLite amalgamation**

```bash
cd /tmp && rm -rf sqlite-fetch && mkdir sqlite-fetch && cd sqlite-fetch
ZIP=$(curl -s https://www.sqlite.org/download.html | grep -o "20[0-9][0-9]/sqlite-amalgamation-[0-9]*\.zip" | head -1)
curl -sO "https://www.sqlite.org/$ZIP"
unzip -q sqlite-amalgamation-*.zip
cp sqlite-amalgamation-*/sqlite3.c sqlite-amalgamation-*/sqlite3.h \
   ~/Documents/projects/solarium/vendor/
```

Expected: `vendor/sqlite3.c` (~9 MB) and `vendor/sqlite3.h` exist. (SQLite is public domain — no license file needed.)

- [ ] **Step 1.2 — fetch CivetWeb v1.16**

```bash
cd /tmp && rm -rf civetweb-fetch
git clone --depth 1 --branch v1.16 https://github.com/civetweb/civetweb civetweb-fetch
cp civetweb-fetch/src/civetweb.c civetweb-fetch/src/*.inl \
   civetweb-fetch/include/civetweb.h \
   ~/Documents/projects/solarium/vendor/
cp civetweb-fetch/LICENSE.md ~/Documents/projects/solarium/vendor/CIVETWEB-LICENSE.md
```

Expected: `vendor/civetweb.c`, `vendor/civetweb.h`, several `vendor/*.inl` (they are `#include`d by civetweb.c), `vendor/CIVETWEB-LICENSE.md` (MIT).

- [ ] **Step 1.3 — build.sh: `vendor_objs` + `server` mode + c89check additions**

Near the top of `build.sh` (after the `MODE=` line, before the c89check block), add:

```sh
# Compile the vendored server deps (SQLite + CivetWeb) into cached .o files.
# Vendored code is not ours: compiled quietly (-w), rebuilt only when its
# source changes. Server-only; the engine build never touches these.
vendor_objs() {
    if [ ! -f vendor/sqlite3.o ] || [ vendor/sqlite3.c -nt vendor/sqlite3.o ]; then
        ${CC:-clang} -O2 -w -c vendor/sqlite3.c -o vendor/sqlite3.o \
            -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION
    fi
    if [ ! -f vendor/civetweb.o ] || [ vendor/civetweb.c -nt vendor/civetweb.o ]; then
        ${CC:-clang} -O2 -w -c vendor/civetweb.c -o vendor/civetweb.o \
            -Ivendor -DNO_SSL -DNO_CGI -DNO_FILES
    fi
}
```

Add the `server` mode as a new block (style-matched to the existing ones). The compile list grows in later tasks; Task 1's version:

```sh
# Build the solsrv server binary (sub-project 1 of the server arc). Same
# sources build on macOS (local dev) and Linux (deploy): CC=gcc ./build.sh
# server. POSIX + vendored CivetWeb/SQLite only — never links GL/GLFW.
if [ "$MODE" = "server" ]; then
    vendor_objs
    ${CC:-clang} -std=c11 -g -O0 -Wall -Wextra -isystem vendor \
        srv_main.c \
        vendor/sqlite3.o vendor/civetweb.o \
        -lpthread -lm \
        -o solsrv
    echo "built ./solsrv"
    exit 0
fi
```

In the c89check block, extend the clang line: add `-isystem vendor` after `$GLFW_CFLAGS`, and append `srv_main.c` to the end of the file list. (Each later task appends its own TUs; `srv_rand.c` is never listed — platform glue, like `platform_*.c`.)

- [ ] **Step 1.4 — .gitignore: all planned server artifacts at once**

Append to `.gitignore` under the build-artifacts section:

```
# Server (solsrv) artifacts
/solsrv
vendor/*.o
/sha256_test
/b64_test
/srv_db_test
/srv_events_test
/srv_auth_test
/srv_web_test
/srv_*_test.db*
/srv_itest.db*
/srv_itest.cookies
/solsrv.db*
```

- [ ] **Step 1.5 — write `srv_main.c` (minimal boot + /health)**

```c
/* srv_main.c — solsrv entry point: flags, CivetWeb boot, the route table.
   Sub-project 1 of the server arc (docs/superpowers/specs/
   2026-07-01-server-sync-web-mcp-architecture-design.md). Strict C89; binds
   loopback only — Caddy fronts it in production. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <civetweb.h>

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

static int h_health(struct mg_connection *c, void *ud) {
    (void)ud;
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                 "Content-Length: 3\r\nConnection: close\r\n\r\nok\n");
    return 200;
}

int main(int argc, char **argv) {
    const char *port = "127.0.0.1:8080";
    const char *dbpath = "solsrv.db";
    struct mg_callbacks cbs;
    struct mg_context *ctx;
    const char *opts[8];
    int i, n = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) dbpath = argv[++i];
        else {
            fprintf(stderr, "usage: solsrv [-p host:port] [-d dbfile]\n");
            return 2;
        }
    }
    (void)dbpath;   /* used from Task 8 on */

    opts[n++] = "listening_ports";    opts[n++] = port;
    opts[n++] = "num_threads";        opts[n++] = "8";
    opts[n++] = "request_timeout_ms"; opts[n++] = "10000";
    opts[n] = NULL;

    mg_init_library(0);
    memset(&cbs, 0, sizeof cbs);
    ctx = mg_start(&cbs, NULL, opts);
    if (!ctx) { fprintf(stderr, "solsrv: mg_start failed on %s\n", port); return 1; }
    mg_set_request_handler(ctx, "/health", h_health, NULL);

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    printf("solsrv listening on %s\n", port);
    while (!g_stop) sleep(1);

    mg_stop(ctx);
    mg_exit_library();
    return 0;
}
```

- [ ] **Step 1.6 — write `srv_test.sh` (smoke version)**

```sh
#!/bin/sh
# srv_test.sh — integration gauntlet for solsrv: boots the server on a scratch
# db and drives the real HTTP surface with curl. Grows with each task.
set -eu

PORT=18080
BASE="http://127.0.0.1:$PORT"
DB=srv_itest.db

rm -f "$DB" "$DB-wal" "$DB-shm"
./build.sh server >/dev/null

./solsrv -p 127.0.0.1:$PORT -d "$DB" &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1

fail() { echo "FAIL: $1"; exit 1; }

[ "$(curl -fsS $BASE/health)" = "ok" ] || fail "health"
echo "PASS health"

echo "ALL PASS"
```

Then: `chmod +x srv_test.sh`

- [ ] **Step 1.7 — build + run the gauntlet**

Run: `./build.sh server && ./srv_test.sh`
Expected: `built ./solsrv` then `PASS health` / `ALL PASS`.
Run: `./build.sh c89check` — expected `c89check: PASS`.
Run: `./build.sh` — expected `built ./solarium (debug)` (engine unharmed by the build.sh edit).

- [ ] **Step 1.8 — commit**

```bash
git add vendor/sqlite3.c vendor/sqlite3.h vendor/civetweb.c vendor/civetweb.h vendor/*.inl \
        vendor/CIVETWEB-LICENSE.md srv_main.c srv_test.sh build.sh .gitignore
git commit -m "Server foundation Task 1: vendor CivetWeb+SQLite, solsrv boots /health

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2 — `sha256.c`: SHA-256 + HMAC + PBKDF2 + constant-time compare (TDD)

**Files:**
- Create: `sha256.h`, `sha256.c`, `sha256_test.c`
- Modify: `build.sh` (new `sha256test` mode; add `sha256.c` to c89check)

**Interfaces:**
- Consumes: `sol_base.h` (`sol_u32`).
- Produces (Task 6 and sub-project 4 rely on these exact signatures):
  - `void sha256(const void *data, size_t len, unsigned char out[32]);`
  - `void sha256_init(Sha256 *s); void sha256_update(Sha256 *s, const void *data, size_t len); void sha256_final(Sha256 *s, unsigned char out[32]);`
  - `void sha256_hmac(const void *key, size_t key_len, const void *msg, size_t msg_len, unsigned char out[32]);`
  - `void sha256_pbkdf2(const void *pw, size_t pw_len, const void *salt, size_t salt_len, sol_u32 iters, unsigned char *out, size_t out_len);`
  - `int sha256_ct_equal(const void *a, const void *b, size_t len);` (1 = equal)

- [ ] **Step 2.1 — write the failing test first: `sha256_test.c`**

```c
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
```

- [ ] **Step 2.2 — build.sh: add the `sha256test` mode (after the `nidtest` block, same shape)**

```sh
# Build + run the standalone SHA-256/HMAC/PBKDF2 test under the sanitizers.
if [ "$MODE" = "sha256test" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        sha256.c sha256_test.c \
        -o sha256_test
    echo "built ./sha256_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

Run: `./build.sh sha256test` — expected: compile FAILURE (`sha256.h` not found). That's the failing state.

- [ ] **Step 2.3 — write `sha256.h`**

```c
/* sha256.h — SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104), PBKDF2-HMAC-SHA256
   (RFC 8018), and a constant-time compare. Strict C89, depends on the C
   library only. Serves the server arc: PKCE S256, token hashing at rest, the
   password KDF. Sibling in spirit to nid.c/json.c. */
#ifndef SHA256_H
#define SHA256_H

#include "sol_base.h"
#include <stddef.h>

typedef struct {
    sol_u32 state[8];
    sol_u32 count_lo, count_hi;   /* total input BYTES, 64-bit split across two u32 */
    unsigned char buf[64];
    unsigned buf_len;
} Sha256;

void sha256_init  (Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t len);
void sha256_final (Sha256 *s, unsigned char out[32]);
void sha256       (const void *data, size_t len, unsigned char out[32]);

void sha256_hmac  (const void *key, size_t key_len,
                   const void *msg, size_t msg_len, unsigned char out[32]);
void sha256_pbkdf2(const void *pw, size_t pw_len,
                   const void *salt, size_t salt_len,
                   sol_u32 iters, unsigned char *out, size_t out_len);

/* 1 if equal, 0 if not; runs in time independent of contents. */
int sha256_ct_equal(const void *a, const void *b, size_t len);

#endif /* SHA256_H */
```

- [ ] **Step 2.4 — write `sha256.c`**

```c
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
```

- [ ] **Step 2.5 — build + run the test**

Run: `./build.sh sha256test && ./sha256_test`
Expected: every `ok` line, then `sha256_test: all ok`, exit 0, no sanitizer output.

- [ ] **Step 2.6 — c89check + commit**

Append `sha256.c` to the c89check file list in `build.sh`. Run `./build.sh c89check` — expected PASS.

```bash
git add sha256.c sha256.h sha256_test.c build.sh
git commit -m "Server foundation Task 2: sha256/hmac/pbkdf2 pure module (FIPS+RFC vectors)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3 — `b64.c` (base64url) + `srv_rand.c` (CSPRNG bytes) (TDD)

**Files:**
- Create: `b64.h`, `b64.c`, `b64_test.c`, `srv_rand.h`, `srv_rand.c`
- Modify: `build.sh` (new `b64test` mode; add `b64.c` to c89check — NOT `srv_rand.c`)

**Interfaces:**
- Consumes: `sol_base.h`.
- Produces (Task 6 and sub-project 4 rely on these):
  - `#define B64URL_ENC_MAX(n) (((n) + 2) / 3 * 4 + 1)` — output buffer size for encode
  - `size_t b64url_encode(const unsigned char *in, size_t in_len, char *out);` — unpadded RFC 4648 §5; returns strlen, NUL-terminates
  - `int b64url_decode(const char *in, unsigned char *out, size_t *out_len);` — 0 ok / -1 malformed; `out` sized ≥ `strlen(in) / 4 * 3 + 2`
  - `void srv_rand_bytes(void *buf, size_t n);` — CSPRNG, aborts on failure

- [ ] **Step 3.1 — write the failing test first: `b64_test.c`**

```c
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
```

- [ ] **Step 3.2 — build.sh `b64test` mode (same shape as sha256test; sources `b64.c b64_test.c`, output `b64_test`)**

Run: `./build.sh b64test` — expected: compile failure (`b64.h` missing). Failing state confirmed.

- [ ] **Step 3.3 — write `b64.h` and `b64.c`**

`b64.h`:

```c
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
```

`b64.c`:

```c
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
```

- [ ] **Step 3.4 — run the test**

Run: `./build.sh b64test && ./b64_test`
Expected: all `ok` lines, `b64_test: all ok`, exit 0, no sanitizer output.

- [ ] **Step 3.5 — write `srv_rand.h` and `srv_rand.c` (platform glue, no test binary — smoke-checked in Task 6's test)**

`srv_rand.h`:

```c
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
```

`srv_rand.c`:

```c
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
```

- [ ] **Step 3.6 — c89check + commit**

Append `b64.c` (NOT `srv_rand.c`) to the c89check list. Run `./build.sh c89check` — expected PASS.

```bash
git add b64.c b64.h b64_test.c srv_rand.c srv_rand.h build.sh
git commit -m "Server foundation Task 3: base64url pure module + CSPRNG glue

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4 — `srv_db.c`: open/migrate, WAL, write lock, read conns (TDD)

**Files:**
- Create: `srv_db.h`, `srv_db.c`, `srv_db_test.c`
- Modify: `build.sh` (new `srvdbtest` mode; add `srv_db.c` to c89check)

**Interfaces:**
- Consumes: `vendor/sqlite3.h` (via `-isystem vendor`), `<pthread.h>`.
- Produces (every later server task relies on these):
  - `typedef struct SrvDb { sqlite3 *w; pthread_mutex_t wlock; char path[512]; } SrvDb;`
  - `int srv_db_open(SrvDb *db, const char *path);` — 0 ok / -1 (logged to stderr); opens, sets WAL/busy_timeout/foreign_keys, applies migrations
  - `void srv_db_close(SrvDb *db);`
  - `void srv_db_wlock(SrvDb *db); void srv_db_wunlock(SrvDb *db);` — guard EVERY use of `db->w`
  - `int srv_db_exec(SrvDb *db, const char *sql);` — 0/-1; **caller must hold wlock**
  - `sqlite3 *srv_db_ropen(SrvDb *db); void srv_db_rclose(sqlite3 *c);` — short-lived read-only conn (NULL on fail)
  - Schema v1 tables: `events`, `boards`, `users`, `sessions`, `login_attempts` (DDL below is normative — Tasks 5/6 SQL matches it)

- [ ] **Step 4.1 — write the failing test first: `srv_db_test.c`**

```c
/* srv_db_test.c — open/migrate on a scratch file, WAL mode active, migration
   idempotent across reopen, read conn sees the writer's committed rows.
   Built by `build.sh srvdbtest` with ASan/UBSan. */

#include "srv_db.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DBF "srv_db_test.db"

static int g_fail = 0;
static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail = 1; }
    else printf("ok   %s\n", what);
}

static int table_exists(sqlite3 *c, const char *name) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(c, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) found = 1;
    sqlite3_finalize(st);
    return found;
}

int main(void) {
    SrvDb db;
    sqlite3 *r;
    sqlite3_stmt *st;
    int rc;

    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");

    expect(srv_db_open(&db, DBF) == 0, "open + migrate");
    expect(table_exists(db.w, "events"), "events table");
    expect(table_exists(db.w, "boards"), "boards table");
    expect(table_exists(db.w, "users"), "users table");
    expect(table_exists(db.w, "sessions"), "sessions table");
    expect(table_exists(db.w, "login_attempts"), "login_attempts table");

    /* WAL is active */
    st = NULL;
    sqlite3_prepare_v2(db.w, "PRAGMA journal_mode;", -1, &st, NULL);
    sqlite3_step(st);
    expect(strcmp((const char *)sqlite3_column_text(st, 0), "wal") == 0, "journal_mode=wal");
    sqlite3_finalize(st);

    /* writer inserts under the lock; a read conn sees it */
    srv_db_wlock(&db);
    rc = srv_db_exec(&db, "INSERT INTO boards(nid,title,deleted,updated_at,updated_by)"
                          " VALUES('TESTNID','t',0,1,0);");
    srv_db_wunlock(&db);
    expect(rc == 0, "insert via write conn");

    r = srv_db_ropen(&db);
    expect(r != NULL, "ropen");
    st = NULL;
    sqlite3_prepare_v2(r, "SELECT title FROM boards WHERE nid='TESTNID'", -1, &st, NULL);
    expect(sqlite3_step(st) == SQLITE_ROW, "read conn sees committed row");
    sqlite3_finalize(st);

    /* read conn is really read-only */
    expect(sqlite3_exec(r, "INSERT INTO boards(nid,title,deleted,updated_at,updated_by)"
                           " VALUES('X','x',0,1,0);", NULL, NULL, NULL) != SQLITE_OK,
           "read conn rejects writes");
    srv_db_rclose(r);

    /* reopen: migrations are idempotent (user_version gates them) */
    srv_db_close(&db);
    expect(srv_db_open(&db, DBF) == 0, "reopen (no re-migrate error)");
    srv_db_close(&db);

    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (g_fail) { printf("srv_db_test: FAIL\n"); return 1; }
    printf("srv_db_test: all ok\n");
    return 0;
}
```

- [ ] **Step 4.2 — build.sh `srvdbtest` mode**

```sh
# Build + run the server DB layer test under the sanitizers. Links the
# vendored SQLite object (compiled without sanitizers — fine, coverage is
# on OUR code).
if [ "$MODE" = "srvdbtest" ]; then
    vendor_objs
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra -isystem vendor \
        srv_db.c srv_db_test.c vendor/sqlite3.o \
        -lpthread -lm \
        -o srv_db_test
    echo "built ./srv_db_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

Run: `./build.sh srvdbtest` — expected: compile failure (`srv_db.h` missing). Failing state confirmed.

- [ ] **Step 4.3 — write `srv_db.h`**

```c
/* srv_db.h — solsrv SQLite layer: open/migrate, WAL, ONE mutex-guarded write
   connection + short-lived read-only connections (spec §4). Server-only TU —
   never linked into the engine. Concurrency contract: EVERY touch of db->w
   happens between srv_db_wlock/srv_db_wunlock (multi-statement transactions
   included); reads go through srv_db_ropen so they never block the writer. */
#ifndef SRV_DB_H
#define SRV_DB_H

#include <pthread.h>
#include <sqlite3.h>

typedef struct SrvDb {
    sqlite3        *w;
    pthread_mutex_t wlock;
    char            path[512];
} SrvDb;

int      srv_db_open  (SrvDb *db, const char *path);   /* 0 ok, -1 fail (logged) */
void     srv_db_close (SrvDb *db);

void     srv_db_wlock  (SrvDb *db);
void     srv_db_wunlock(SrvDb *db);

/* Run sql on the write connection. Caller MUST hold wlock. 0 ok, -1 logged. */
int      srv_db_exec  (SrvDb *db, const char *sql);

sqlite3 *srv_db_ropen (SrvDb *db);                      /* NULL on fail (logged) */
void     srv_db_rclose(sqlite3 *c);

#endif /* SRV_DB_H */
```

- [ ] **Step 4.4 — write `srv_db.c`**

```c
/* srv_db.c — see srv_db.h. Migrations: MIGRATIONS[i] is applied when
   PRAGMA user_version == i, inside a transaction that also bumps
   user_version to i+1 — so the array only ever grows and old databases
   upgrade in order. */

#include "srv_db.h"

#include <stdio.h>
#include <string.h>

static const char *MIGRATIONS[] = {
    /* v1 — event log + first projection + auth tables (spec §5, §7).
       The events table is the source of truth AND the sync feed: id is the
       global cursor. Projections (boards) update in the same transaction as
       the append (srv_events.c). Auth tables are ordinary state. */
    "CREATE TABLE events("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts            INTEGER NOT NULL,"
    "  actor_id      INTEGER NOT NULL,"
    "  origin_device TEXT NOT NULL DEFAULT '',"
    "  entity_kind   TEXT NOT NULL,"
    "  entity_nid    TEXT NOT NULL,"
    "  op            TEXT NOT NULL,"
    "  payload       TEXT NOT NULL DEFAULT '{}');"
    "CREATE INDEX idx_events_entity ON events(entity_kind, entity_nid);"
    "CREATE TABLE boards("
    "  nid        TEXT PRIMARY KEY,"
    "  title      TEXT NOT NULL DEFAULT '',"
    "  deleted    INTEGER NOT NULL DEFAULT 0,"
    "  updated_at INTEGER NOT NULL,"
    "  updated_by INTEGER NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE users("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name       TEXT UNIQUE NOT NULL,"
    "  pw_salt    BLOB NOT NULL,"
    "  pw_hash    BLOB NOT NULL,"
    "  pw_iters   INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL);"
    "CREATE TABLE sessions("
    "  token_hash BLOB PRIMARY KEY,"
    "  user_id    INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  expires_at INTEGER NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE login_attempts("
    "  ip           TEXT PRIMARY KEY,"
    "  fails        INTEGER NOT NULL,"
    "  window_start INTEGER NOT NULL) WITHOUT ROWID;"
};

static int db_exec_on(sqlite3 *c, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(c, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "srv_db: %s\n", err ? err : "exec failed");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int srv_db_open(SrvDb *db, const char *path) {
    sqlite3_stmt *st;
    char bump[64];
    int ver, i, n;

    memset(db, 0, sizeof *db);
    if (strlen(path) >= sizeof db->path) {
        fprintf(stderr, "srv_db: path too long\n");
        return -1;
    }
    strcpy(db->path, path);

    if (sqlite3_open(path, &db->w) != SQLITE_OK) {
        fprintf(stderr, "srv_db: open %s: %s\n", path, sqlite3_errmsg(db->w));
        sqlite3_close(db->w);
        db->w = NULL;
        return -1;
    }
    if (db_exec_on(db->w, "PRAGMA journal_mode=WAL;"
                          "PRAGMA busy_timeout=5000;"
                          "PRAGMA foreign_keys=ON;"
                          "PRAGMA synchronous=NORMAL;") != 0) return -1;

    ver = 0;
    st = NULL;
    if (sqlite3_prepare_v2(db->w, "PRAGMA user_version;", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        ver = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);

    n = (int)(sizeof MIGRATIONS / sizeof MIGRATIONS[0]);
    for (i = ver; i < n; i++) {
        if (db_exec_on(db->w, "BEGIN;") != 0) return -1;
        if (db_exec_on(db->w, MIGRATIONS[i]) != 0) {
            db_exec_on(db->w, "ROLLBACK;");
            return -1;
        }
        sprintf(bump, "PRAGMA user_version=%d;", i + 1);
        if (db_exec_on(db->w, bump) != 0 || db_exec_on(db->w, "COMMIT;") != 0) {
            db_exec_on(db->w, "ROLLBACK;");
            return -1;
        }
    }

    pthread_mutex_init(&db->wlock, NULL);
    return 0;
}

void srv_db_close(SrvDb *db) {
    if (db->w) {
        sqlite3_close(db->w);
        db->w = NULL;
        pthread_mutex_destroy(&db->wlock);
    }
}

void srv_db_wlock  (SrvDb *db) { pthread_mutex_lock(&db->wlock); }
void srv_db_wunlock(SrvDb *db) { pthread_mutex_unlock(&db->wlock); }

int srv_db_exec(SrvDb *db, const char *sql) { return db_exec_on(db->w, sql); }

sqlite3 *srv_db_ropen(SrvDb *db) {
    sqlite3 *c = NULL;
    if (sqlite3_open_v2(db->path, &c, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "srv_db: ropen: %s\n", c ? sqlite3_errmsg(c) : "open_v2 failed");
        if (c) sqlite3_close(c);
        return NULL;
    }
    sqlite3_exec(c, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    return c;
}

void srv_db_rclose(sqlite3 *c) { if (c) sqlite3_close(c); }
```

Note: `pthread_mutex_init` runs AFTER migrations because open is single-threaded startup code; the lock only matters once CivetWeb threads exist. One subtlety the test exercises: `srv_db_open` must be safe to call on a database that already has the schema (reopen path).

- [ ] **Step 4.5 — run the test**

Run: `./build.sh srvdbtest && ./srv_db_test`
Expected: all `ok` lines, `srv_db_test: all ok`, exit 0, no sanitizer output.

- [ ] **Step 4.6 — c89check + commit**

Append `srv_db.c` to the c89check list. Run `./build.sh c89check` — expected PASS.

```bash
git add srv_db.c srv_db.h srv_db_test.c build.sh
git commit -m "Server foundation Task 4: SQLite layer — migrate, WAL, write lock, read conns

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 5 — `srv_events.c`: append + projections + query-after-cursor (TDD)

**Files:**
- Create: `srv_events.h`, `srv_events.c`, `srv_events_test.c`
- Modify: `build.sh` (new `srveventstest` mode; add `srv_events.c` to c89check)

**Interfaces:**
- Consumes: Task 4's `SrvDb` API; `json.c` (`json_parse`/`json_member`/`json_string`/`json_free`); `nid.h` (`NID_LEN`).
- Produces (Task 9's `--seed` and sub-projects 2/4 rely on these):
  - `SrvEventIn` / `SrvEventOut` structs as below
  - `int srv_events_append(SrvDb *db, const SrvEventIn *in, long *out_id);` — 0 ok; transactional (event + projection or neither)
  - `int srv_events_after(SrvDb *db, long cursor, int max, SrvEventOut **out, int *count);` — 0 ok; caller frees via `srv_events_free`
  - `void srv_events_free(SrvEventOut *evs, int count);`

- [ ] **Step 5.1 — write the failing test first: `srv_events_test.c`**

```c
/* srv_events_test.c — append is transactional and projects boards; unknown
   kinds are logged but not projected; query-after-cursor pages in id order.
   Built by `build.sh srveventstest` with ASan/UBSan. */

#include "srv_events.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DBF "srv_events_test.db"

static int g_fail = 0;
static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail = 1; }
    else printf("ok   %s\n", what);
}

static long append(SrvDb *db, const char *nid, const char *op, const char *payload) {
    SrvEventIn ev;
    long id = 0;
    memset(&ev, 0, sizeof ev);
    ev.actor_id = 7;
    ev.origin_device = "testdev";
    ev.entity_kind = "board";
    ev.entity_nid = nid;
    ev.op = op;
    ev.payload = payload;
    if (srv_events_append(db, &ev, &id) != 0) return -1;
    return id;
}

static int board_title_is(SrvDb *db, const char *nid, const char *want, int want_deleted) {
    sqlite3 *r = srv_db_ropen(db);
    sqlite3_stmt *st = NULL;
    int ok = 0;
    sqlite3_prepare_v2(r, "SELECT title, deleted FROM boards WHERE nid=?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, nid, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        ok = strcmp((const char *)sqlite3_column_text(st, 0), want) == 0
          && sqlite3_column_int(st, 1) == want_deleted;
    }
    sqlite3_finalize(st);
    srv_db_rclose(r);
    return ok;
}

int main(void) {
    SrvDb db;
    SrvEventOut *evs = NULL;
    SrvEventIn bad;
    int count = 0;
    long id1, id2, id3, id4, idu;

    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (srv_db_open(&db, DBF) != 0) { printf("FAIL: open\n"); return 1; }

    id1 = append(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "create", "{\"title\":\"Alpha\"}");
    id2 = append(&db, "BBBBBBBBBBBBBBBBBBBBBBBBBB", "create", "{\"title\":\"Beta\"}");
    id3 = append(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "update", "{\"title\":\"Alpha 2\"}");
    id4 = append(&db, "BBBBBBBBBBBBBBBBBBBBBBBBBB", "delete", "{}");
    expect(id1 > 0 && id2 > id1 && id3 > id2 && id4 > id3, "ids ascend");

    expect(board_title_is(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "Alpha 2", 0),
           "A projected: title updated, not deleted");
    expect(board_title_is(&db, "BBBBBBBBBBBBBBBBBBBBBBBBBB", "Beta", 1),
           "B projected: deleted, title kept");

    /* update with no title field must not clobber the title */
    idu = append(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "update", "{\"other\":1}");
    expect(idu > id4, "titleless update appended");
    expect(board_title_is(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "Alpha 2", 0),
           "titleless update keeps title");

    /* unknown entity kind: logged, not projected, no error */
    memset(&bad, 0, sizeof bad);
    bad.actor_id = 7;
    bad.origin_device = "testdev";
    bad.entity_kind = "gizmo";
    bad.entity_nid = "CCCCCCCCCCCCCCCCCCCCCCCCCC";
    bad.op = "create";
    bad.payload = "{}";
    {
        long idg = 0;
        expect(srv_events_append(&db, &bad, &idg) == 0 && idg > idu,
               "unknown kind appends fine");
    }

    /* cursor query */
    expect(srv_events_after(&db, 0, 100, &evs, &count) == 0 && count == 6,
           "after(0) returns all 6");
    expect(evs[0].id == id1 && strcmp(evs[0].op, "create") == 0
        && strcmp(evs[0].origin_device, "testdev") == 0
        && strcmp(evs[0].payload, "{\"title\":\"Alpha\"}") == 0,
           "first event roundtrips");
    srv_events_free(evs, count);

    expect(srv_events_after(&db, id2, 100, &evs, &count) == 0 && count == 4
        && evs[0].id == id3,
           "after(id2) starts at id3");
    srv_events_free(evs, count);

    expect(srv_events_after(&db, 0, 2, &evs, &count) == 0 && count == 2,
           "max caps the page");
    srv_events_free(evs, count);

    srv_db_close(&db);
    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (g_fail) { printf("srv_events_test: FAIL\n"); return 1; }
    printf("srv_events_test: all ok\n");
    return 0;
}
```

- [ ] **Step 5.2 — build.sh `srveventstest` mode (sources: `srv_events.c srv_db.c json.c srv_events_test.c vendor/sqlite3.o`, flags as srvdbtest, output `srv_events_test`)**

Run: `./build.sh srveventstest` — expected compile failure. Failing state confirmed.

- [ ] **Step 5.3 — write `srv_events.h`**

```c
/* srv_events.h — the append-only event store + projections (spec §5). Append
   writes the event row AND updates the projection tables in ONE transaction
   under the write lock, so they can never drift; projections stay rebuildable
   from the log. events.id is the global sync cursor. Unknown entity kinds
   append fine and simply aren't projected yet (forward compatibility).
   Client-op idempotency keys arrive with sub-project 2 as migration v2. */
#ifndef SRV_EVENTS_H
#define SRV_EVENTS_H

#include "srv_db.h"
#include "nid.h"

typedef struct SrvEventIn {
    long        ts;              /* 0 = stamp with time(NULL) */
    long        actor_id;        /* users.id; 0 = system (seed) */
    const char *origin_device;   /* "" ok; echo-suppression key for sync */
    const char *entity_kind;     /* "board" | future kinds */
    const char *entity_nid;
    const char *op;              /* "create" | "update" | "delete" */
    const char *payload;         /* JSON object text (changed fields) */
} SrvEventIn;

typedef struct SrvEventOut {
    long  id, ts, actor_id;
    char  origin_device[64];
    char  entity_kind[16];
    char  entity_nid[NID_LEN + 1];
    char  op[8];
    char *payload;               /* malloc'd; freed by srv_events_free */
} SrvEventOut;

int  srv_events_append(SrvDb *db, const SrvEventIn *in, long *out_id);
int  srv_events_after (SrvDb *db, long cursor, int max,
                       SrvEventOut **out, int *count);
void srv_events_free  (SrvEventOut *evs, int count);

#endif /* SRV_EVENTS_H */
```

- [ ] **Step 5.4 — write `srv_events.c`**

```c
/* srv_events.c — see srv_events.h. */

#include "srv_events.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *xstrdup(const char *s) {
    char *d = (char *)malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

static void copy_capped(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Project one board event into the boards table. Caller holds wlock and an
   open transaction. Returns 0 ok. Update touches only fields present in the
   payload; delete is a tombstone (deleted=1), never a row removal — the web
   list filters on it and sync history stays intact. */
static int project_board(SrvDb *db, const char *nid, const char *op,
                         const char *payload, long ts, long actor) {
    sqlite3_stmt *st = NULL;
    JsonValue *v = NULL;
    const char *title = NULL;
    int rc = SQLITE_ERROR;

    if (strcmp(op, "delete") == 0) {
        if (sqlite3_prepare_v2(db->w,
                "UPDATE boards SET deleted=1, updated_at=?, updated_by=? WHERE nid=?",
                -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)actor);
        sqlite3_bind_text(st, 3, nid, -1, SQLITE_STATIC);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE ? 0 : -1;
    }

    v = json_parse(payload);
    title = json_string(json_member(v, "title"));

    if (strcmp(op, "create") == 0) {
        if (sqlite3_prepare_v2(db->w,
                "INSERT INTO boards(nid,title,deleted,updated_at,updated_by)"
                " VALUES(?,?,0,?,?)"
                " ON CONFLICT(nid) DO UPDATE SET title=excluded.title, deleted=0,"
                "   updated_at=excluded.updated_at, updated_by=excluded.updated_by",
                -1, &st, NULL) != SQLITE_OK) { json_free(v); return -1; }
        sqlite3_bind_text(st, 1, nid, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, title ? title : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)actor);
    } else if (title) {   /* update, title present */
        if (sqlite3_prepare_v2(db->w,
                "UPDATE boards SET title=?, updated_at=?, updated_by=? WHERE nid=?",
                -1, &st, NULL) != SQLITE_OK) { json_free(v); return -1; }
        sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)actor);
        sqlite3_bind_text(st, 4, nid, -1, SQLITE_STATIC);
    } else {              /* update, nothing we project — bump attribution only */
        if (sqlite3_prepare_v2(db->w,
                "UPDATE boards SET updated_at=?, updated_by=? WHERE nid=?",
                -1, &st, NULL) != SQLITE_OK) { json_free(v); return -1; }
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)actor);
        sqlite3_bind_text(st, 3, nid, -1, SQLITE_STATIC);
    }
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    json_free(v);
    return rc == SQLITE_DONE ? 0 : -1;
}

int srv_events_append(SrvDb *db, const SrvEventIn *in, long *out_id) {
    sqlite3_stmt *st = NULL;
    long ts;
    int ok = 0;

    ts = in->ts ? in->ts : (long)time(NULL);

    srv_db_wlock(db);
    if (srv_db_exec(db, "BEGIN IMMEDIATE;") != 0) { srv_db_wunlock(db); return -1; }

    if (sqlite3_prepare_v2(db->w,
            "INSERT INTO events(ts,actor_id,origin_device,entity_kind,entity_nid,op,payload)"
            " VALUES(?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)in->actor_id);
        sqlite3_bind_text(st, 3, in->origin_device ? in->origin_device : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, in->entity_kind, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, in->entity_nid, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 6, in->op, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 7, in->payload ? in->payload : "{}", -1, SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }

    if (ok && strcmp(in->entity_kind, "board") == 0) {
        ok = project_board(db, in->entity_nid, in->op,
                           in->payload ? in->payload : "{}", ts, in->actor_id) == 0;
    }

    if (ok) {
        *out_id = (long)sqlite3_last_insert_rowid(db->w);
        ok = srv_db_exec(db, "COMMIT;") == 0;
    }
    if (!ok) srv_db_exec(db, "ROLLBACK;");
    srv_db_wunlock(db);
    return ok ? 0 : -1;
}

int srv_events_after(SrvDb *db, long cursor, int max,
                     SrvEventOut **out, int *count) {
    sqlite3 *r;
    sqlite3_stmt *st = NULL;
    SrvEventOut *evs;
    int n = 0;

    *out = NULL;
    *count = 0;
    if (max <= 0) return 0;

    r = srv_db_ropen(db);
    if (!r) return -1;

    evs = (SrvEventOut *)calloc((size_t)max, sizeof *evs);
    if (!evs) { srv_db_rclose(r); return -1; }

    if (sqlite3_prepare_v2(r,
            "SELECT id,ts,actor_id,origin_device,entity_kind,entity_nid,op,payload"
            " FROM events WHERE id > ? ORDER BY id LIMIT ?", -1, &st, NULL) != SQLITE_OK) {
        free(evs);
        srv_db_rclose(r);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cursor);
    sqlite3_bind_int(st, 2, max);

    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        SrvEventOut *e = &evs[n];
        e->id       = (long)sqlite3_column_int64(st, 0);
        e->ts       = (long)sqlite3_column_int64(st, 1);
        e->actor_id = (long)sqlite3_column_int64(st, 2);
        copy_capped(e->origin_device, sizeof e->origin_device,
                    (const char *)sqlite3_column_text(st, 3));
        copy_capped(e->entity_kind, sizeof e->entity_kind,
                    (const char *)sqlite3_column_text(st, 4));
        copy_capped(e->entity_nid, sizeof e->entity_nid,
                    (const char *)sqlite3_column_text(st, 5));
        copy_capped(e->op, sizeof e->op,
                    (const char *)sqlite3_column_text(st, 6));
        e->payload = xstrdup((const char *)sqlite3_column_text(st, 7));
        if (!e->payload) break;
        n++;
    }
    sqlite3_finalize(st);
    srv_db_rclose(r);

    *out = evs;
    *count = n;
    return 0;
}

void srv_events_free(SrvEventOut *evs, int count) {
    int i;
    if (!evs) return;
    for (i = 0; i < count; i++) free(evs[i].payload);
    free(evs);
}
```

- [ ] **Step 5.5 — run the test**

Run: `./build.sh srveventstest && ./srv_events_test`
Expected: all `ok` lines, `srv_events_test: all ok`, exit 0, no sanitizer output.

- [ ] **Step 5.6 — c89check + commit**

Append `srv_events.c` to the c89check list. Run `./build.sh c89check` — expected PASS.

```bash
git add srv_events.c srv_events.h srv_events_test.c build.sh
git commit -m "Server foundation Task 5: event store — transactional append, projections, cursor query

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 6 — `srv_auth.c`: users, sessions, login throttle (TDD)

**Files:**
- Create: `srv_auth.h`, `srv_auth.c`, `srv_auth_test.c`
- Modify: `build.sh` (new `srvauthtest` mode; add `srv_auth.c` to c89check)

**Interfaces:**
- Consumes: Tasks 2–4 (`sha256_pbkdf2`, `sha256`, `sha256_ct_equal`, `b64url_encode`, `srv_rand_bytes`, `SrvDb`).
- Produces (Task 8/9 and sub-project 4 rely on these):
  - `#define SRV_TOKEN_CHARS 43` (32 bytes base64url, unpadded)
  - `int srv_auth_user_create(SrvDb *db, const char *name, const char *password);` — 0 ok / -1 (bad name `[a-z0-9_-]{1,32}`, password < 8 chars, or duplicate)
  - `long srv_auth_login(SrvDb *db, const char *name, const char *password, char token_out[SRV_TOKEN_CHARS + 1]);` — user id > 0 on success (session minted), 0 on bad credentials
  - `long srv_auth_session_user(SrvDb *db, const char *token);` — user id > 0, or 0 (missing/expired)
  - `void srv_auth_logout(SrvDb *db, const char *token);`
  - `int srv_auth_throttle_ok(SrvDb *db, const char *ip);` / `void srv_auth_throttle_fail(SrvDb *db, const char *ip);` / `void srv_auth_throttle_clear(SrvDb *db, const char *ip);` — 10 fails per 15-minute window per IP
  - Test-only: `int srv_auth_user_create_iters(SrvDb *db, const char *name, const char *password, sol_u32 iters);` (public wrapper passes `SRV_PBKDF2_ITERS` = 600000)

- [ ] **Step 6.1 — write the failing test first: `srv_auth_test.c`**

```c
/* srv_auth_test.c — password roundtrip, session mint/validate/expire/logout,
   per-IP throttle. Uses the _iters variant with a small count so the test
   stays fast; production uses SRV_PBKDF2_ITERS. Built by `build.sh
   srvauthtest` with ASan/UBSan. */

#include "srv_auth.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DBF "srv_auth_test.db"
#define FAST_ITERS 1000u

static int g_fail = 0;
static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail = 1; }
    else printf("ok   %s\n", what);
}

int main(void) {
    SrvDb db;
    char tok[SRV_TOKEN_CHARS + 1], tok2[SRV_TOKEN_CHARS + 1];
    long uid;
    int i;

    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (srv_db_open(&db, DBF) != 0) { printf("FAIL: open\n"); return 1; }

    expect(srv_auth_user_create_iters(&db, "fran", "hunter2boogaloo", FAST_ITERS) == 0,
           "create user");
    expect(srv_auth_user_create_iters(&db, "fran", "whatever12", FAST_ITERS) != 0,
           "duplicate name rejected");
    expect(srv_auth_user_create_iters(&db, "Fran!", "whatever12", FAST_ITERS) != 0,
           "bad name rejected");
    expect(srv_auth_user_create_iters(&db, "shorty", "short", FAST_ITERS) != 0,
           "short password rejected");

    uid = srv_auth_login(&db, "fran", "hunter2boogaloo", tok);
    expect(uid > 0, "login good");
    expect(strlen(tok) == SRV_TOKEN_CHARS, "token is 43 chars");
    expect(srv_auth_session_user(&db, tok) == uid, "session validates");

    expect(srv_auth_login(&db, "fran", "wrongwrong", tok2) == 0, "login bad pass");
    expect(srv_auth_login(&db, "nobody", "hunter2boogaloo", tok2) == 0, "login bad user");

    /* two sessions coexist */
    expect(srv_auth_login(&db, "fran", "hunter2boogaloo", tok2) == uid, "second login");
    expect(srv_auth_session_user(&db, tok2) == uid, "second session validates");

    srv_auth_logout(&db, tok);
    expect(srv_auth_session_user(&db, tok) == 0, "logged-out token dead");
    expect(srv_auth_session_user(&db, tok2) == uid, "other session survives");

    expect(srv_auth_session_user(&db, "notatoken") == 0, "garbage token rejected");

    /* expiry: back-date a session directly and check it dies */
    srv_db_wlock(&db);
    srv_db_exec(&db, "UPDATE sessions SET expires_at = 1;");
    srv_db_wunlock(&db);
    expect(srv_auth_session_user(&db, tok2) == 0, "expired session rejected");

    /* throttle: 10 fails trip it; clear resets */
    expect(srv_auth_throttle_ok(&db, "10.0.0.1"), "fresh ip allowed");
    for (i = 0; i < 10; i++) srv_auth_throttle_fail(&db, "10.0.0.1");
    expect(!srv_auth_throttle_ok(&db, "10.0.0.1"), "10 fails trip the throttle");
    expect(srv_auth_throttle_ok(&db, "10.0.0.2"), "other ip unaffected");
    srv_auth_throttle_clear(&db, "10.0.0.1");
    expect(srv_auth_throttle_ok(&db, "10.0.0.1"), "clear resets");

    srv_db_close(&db);
    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (g_fail) { printf("srv_auth_test: FAIL\n"); return 1; }
    printf("srv_auth_test: all ok\n");
    return 0;
}
```

- [ ] **Step 6.2 — build.sh `srvauthtest` mode (sources: `srv_auth.c srv_db.c srv_rand.c sha256.c b64.c srv_auth_test.c vendor/sqlite3.o`, flags as srvdbtest, output `srv_auth_test`)**

Run: `./build.sh srvauthtest` — expected compile failure. Failing state confirmed.

- [ ] **Step 6.3 — write `srv_auth.h`**

```c
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
```

- [ ] **Step 6.4 — write `srv_auth.c`**

```c
/* srv_auth.c — see srv_auth.h. */

#include "srv_auth.h"
#include "sha256.h"
#include "b64.h"
#include "srv_rand.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SESSION_TTL      (30L * 24L * 3600L)   /* 30 days */
#define THROTTLE_WINDOW  900L                  /* 15 minutes */
#define THROTTLE_MAX     10

static int name_ok(const char *name) {
    size_t n = strlen(name);
    size_t i;
    if (n < 1 || n > 32) return 0;
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int srv_auth_user_create_iters(SrvDb *db, const char *name,
                               const char *password, sol_u32 iters) {
    unsigned char salt[16], hash[32];
    sqlite3_stmt *st = NULL;
    int ok = 0;

    if (!name_ok(name) || strlen(password) < 8) return -1;

    srv_rand_bytes(salt, sizeof salt);
    sha256_pbkdf2(password, strlen(password), salt, sizeof salt,
                  iters, hash, sizeof hash);

    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w,
            "INSERT INTO users(name,pw_salt,pw_hash,pw_iters,created_at)"
            " VALUES(?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_blob(st, 2, salt, (int)sizeof salt, SQLITE_STATIC);
        sqlite3_bind_blob(st, 3, hash, (int)sizeof hash, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)iters);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)time(NULL));
        ok = sqlite3_step(st) == SQLITE_DONE;   /* UNIQUE(name) rejects dupes */
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);
    return ok ? 0 : -1;
}

int srv_auth_user_create(SrvDb *db, const char *name, const char *password) {
    return srv_auth_user_create_iters(db, name, password, SRV_PBKDF2_ITERS);
}

long srv_auth_login(SrvDb *db, const char *name, const char *password,
                    char token_out[SRV_TOKEN_CHARS + 1]) {
    static const unsigned char DUMMY_SALT[16] = {0};
    unsigned char salt[16], stored[32], calc[32], raw[32], th[32];
    sqlite3_stmt *st = NULL;
    sol_u32 iters = SRV_PBKDF2_ITERS;
    long uid = 0, now;
    int found = 0;

    /* fetch credentials under the lock… */
    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w,
            "SELECT id,pw_salt,pw_hash,pw_iters FROM users WHERE name=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW
            && sqlite3_column_bytes(st, 1) == 16
            && sqlite3_column_bytes(st, 2) == 32) {
            uid = (long)sqlite3_column_int64(st, 0);
            memcpy(salt, sqlite3_column_blob(st, 1), 16);
            memcpy(stored, sqlite3_column_blob(st, 2), 32);
            iters = (sol_u32)sqlite3_column_int64(st, 3);
            found = 1;
        }
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);

    /* …but run the slow KDF OUTSIDE it: at 600k iterations it takes real
       wall-time, and holding wlock would stall every writer. Unknown user
       pays the same KDF so timing doesn't leak which names exist. */
    if (!found) {
        sha256_pbkdf2(password, strlen(password), DUMMY_SALT, 16, iters, calc, 32);
        return 0;
    }
    sha256_pbkdf2(password, strlen(password), salt, 16, iters, calc, 32);
    if (!sha256_ct_equal(calc, stored, 32)) return 0;

    /* mint the session: token to the caller, only its hash at rest */
    srv_rand_bytes(raw, sizeof raw);
    b64url_encode(raw, sizeof raw, token_out);       /* exactly 43 chars */
    sha256(token_out, SRV_TOKEN_CHARS, th);
    now = (long)time(NULL);

    srv_db_wlock(db);
    st = NULL;
    if (sqlite3_prepare_v2(db->w,
            "INSERT INTO sessions(token_hash,user_id,created_at,expires_at)"
            " VALUES(?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, th, 32, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)(now + SESSION_TTL));
        if (sqlite3_step(st) != SQLITE_DONE) uid = 0;
        sqlite3_finalize(st);
    } else uid = 0;
    srv_db_wunlock(db);
    return uid;
}

long srv_auth_session_user(SrvDb *db, const char *token) {
    unsigned char th[32];
    sqlite3 *r;
    sqlite3_stmt *st = NULL;
    size_t tl = strlen(token);
    long uid = 0, exp = 0;

    if (tl != SRV_TOKEN_CHARS) return 0;
    sha256(token, tl, th);

    r = srv_db_ropen(db);
    if (!r) return 0;
    if (sqlite3_prepare_v2(r,
            "SELECT user_id,expires_at FROM sessions WHERE token_hash=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, th, 32, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            uid = (long)sqlite3_column_int64(st, 0);
            exp = (long)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    srv_db_rclose(r);

    if (uid && exp <= (long)time(NULL)) {
        srv_auth_logout(db, token);   /* lazy expiry sweep, one row at a time */
        return 0;
    }
    return uid;
}

void srv_auth_logout(SrvDb *db, const char *token) {
    unsigned char th[32];
    sqlite3_stmt *st = NULL;
    size_t tl = strlen(token);

    if (tl != SRV_TOKEN_CHARS) return;
    sha256(token, tl, th);
    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w, "DELETE FROM sessions WHERE token_hash=?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, th, 32, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);
}

int srv_auth_throttle_ok(SrvDb *db, const char *ip) {
    sqlite3 *r;
    sqlite3_stmt *st = NULL;
    long fails = 0, start = 0;
    int seen = 0;

    r = srv_db_ropen(db);
    if (!r) return 0;   /* fail closed: no read path, no login attempts */
    if (sqlite3_prepare_v2(r,
            "SELECT fails,window_start FROM login_attempts WHERE ip=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ip, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            fails = (long)sqlite3_column_int64(st, 0);
            start = (long)sqlite3_column_int64(st, 1);
            seen = 1;
        }
        sqlite3_finalize(st);
    }
    srv_db_rclose(r);

    if (!seen) return 1;
    if (start + THROTTLE_WINDOW < (long)time(NULL)) return 1;   /* window lapsed */
    return fails < THROTTLE_MAX;
}

void srv_auth_throttle_fail(SrvDb *db, const char *ip) {
    sqlite3_stmt *st = NULL;
    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w,
            "INSERT INTO login_attempts(ip,fails,window_start) VALUES(?,1,?)"
            " ON CONFLICT(ip) DO UPDATE SET"
            "  fails = CASE WHEN login_attempts.window_start + 900 < excluded.window_start"
            "               THEN 1 ELSE login_attempts.fails + 1 END,"
            "  window_start = CASE WHEN login_attempts.window_start + 900 < excluded.window_start"
            "               THEN excluded.window_start ELSE login_attempts.window_start END",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ip, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);
}

void srv_auth_throttle_clear(SrvDb *db, const char *ip) {
    sqlite3_stmt *st = NULL;
    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w, "DELETE FROM login_attempts WHERE ip=?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ip, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);
}
```

Include this comment directly above `srv_auth_throttle_fail` in `srv_auth.c` (the `900` literal inside the upsert SQL mirrors `THROTTLE_WINDOW`, and SQL can't read the macro):

```c
/* NOTE: the 900 inside the upsert SQL is THROTTLE_WINDOW — SQL can't read the
   macro; change both together. */
```

- [ ] **Step 6.5 — run the test**

Run: `./build.sh srvauthtest && ./srv_auth_test`
Expected: all `ok` lines, `srv_auth_test: all ok`, exit 0, no sanitizer output. (Fast: the test uses 1000 iterations.)

- [ ] **Step 6.6 — c89check + commit**

Append `srv_auth.c` to the c89check list. Run `./build.sh c89check` — expected PASS.

```bash
git add srv_auth.c srv_auth.h srv_auth_test.c build.sh
git commit -m "Server foundation Task 6: users (PBKDF2), hashed sessions, login throttle

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 7 — `srv_web.c`: string builder, HTML escape, pages (pure, TDD)

**Files:**
- Create: `srv_web.h`, `srv_web.c`, `srv_web_test.c`
- Modify: `build.sh` (new `srvwebtest` mode; add `srv_web.c` to c89check)

**Interfaces:**
- Consumes: `nid.h` (`NID_LEN` only). NO sqlite, NO civetweb — this TU is pure (GL-free, network-free) so the test needs nothing vendored.
- Produces (Task 8 renders through these):
  - `typedef struct Sb { char *buf; size_t len, cap; } Sb;`
  - `void sb_init(Sb *s); void sb_free(Sb *s); void sb_puts(Sb *s, const char *str); void sb_put_html(Sb *s, const char *str); void sb_put_long(Sb *s, long v);`
  - `typedef struct WebBoardRow { char nid[NID_LEN + 1]; char title[256]; long updated_at; } WebBoardRow;`
  - `void web_page_login(Sb *out, const char *err_msg);` — err_msg NULL for none
  - `void web_page_boards(Sb *out, const WebBoardRow *rows, int n);`
  - `void web_page_error(Sb *out, int status, const char *msg);`

- [ ] **Step 7.1 — write the failing test first: `srv_web_test.c`**

```c
/* srv_web_test.c — the pure HTML layer: builder growth, escape correctness,
   page shape (form action, escaped titles, empty state). No server, no DB.
   Built by `build.sh srvwebtest` with ASan/UBSan. */

#include "srv_web.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail = 1; }
    else printf("ok   %s\n", what);
}

int main(void) {
    Sb s;
    WebBoardRow rows[2];
    int i;

    /* builder grows past its initial capacity */
    sb_init(&s);
    for (i = 0; i < 1000; i++) sb_puts(&s, "0123456789");
    expect(s.len == 10000 && strlen(s.buf) == 10000, "sb grows to 10k");
    sb_free(&s);

    /* escape: every special, embedded in text */
    sb_init(&s);
    sb_put_html(&s, "a<b>&\"c'd");
    expect(strcmp(s.buf, "a&lt;b&gt;&amp;&quot;c&#39;d") == 0, "html escape");
    sb_free(&s);

    sb_init(&s);
    sb_put_long(&s, -12345L);
    expect(strcmp(s.buf, "-12345") == 0, "sb_put_long");
    sb_free(&s);

    /* login page */
    sb_init(&s);
    web_page_login(&s, NULL);
    expect(strstr(s.buf, "<form method=\"post\" action=\"/login\">") != NULL,
           "login form present");
    expect(strstr(s.buf, "class=\"err\"") == NULL, "no error block by default");
    sb_free(&s);

    sb_init(&s);
    web_page_login(&s, "Bad username or password.");
    expect(strstr(s.buf, "Bad username or password.") != NULL, "error text shown");
    sb_free(&s);

    /* boards page: titles escaped, logout form present */
    memset(rows, 0, sizeof rows);
    strcpy(rows[0].nid, "AAAAAAAAAAAAAAAAAAAAAAAAAA");
    strcpy(rows[0].title, "<script>alert(1)</script>");
    rows[0].updated_at = 100;
    strcpy(rows[1].nid, "BBBBBBBBBBBBBBBBBBBBBBBBBB");
    strcpy(rows[1].title, "Reading & writing");
    rows[1].updated_at = 200;

    sb_init(&s);
    web_page_boards(&s, rows, 2);
    expect(strstr(s.buf, "<script>alert(1)</script>") == NULL, "script tag not raw");
    expect(strstr(s.buf, "&lt;script&gt;") != NULL, "script tag escaped");
    expect(strstr(s.buf, "Reading &amp; writing") != NULL, "ampersand escaped");
    expect(strstr(s.buf, "action=\"/logout\"") != NULL, "logout form present");
    sb_free(&s);

    /* empty state */
    sb_init(&s);
    web_page_boards(&s, rows, 0);
    expect(strstr(s.buf, "No boards yet") != NULL, "empty state");
    sb_free(&s);

    /* error page */
    sb_init(&s);
    web_page_error(&s, 404, "No such page.");
    expect(strstr(s.buf, "404") != NULL && strstr(s.buf, "No such page.") != NULL,
           "error page");
    sb_free(&s);

    if (g_fail) { printf("srv_web_test: FAIL\n"); return 1; }
    printf("srv_web_test: all ok\n");
    return 0;
}
```

- [ ] **Step 7.2 — build.sh `srvwebtest` mode (sources: `srv_web.c srv_web_test.c`, no vendor objects needed, output `srv_web_test`, same sanitizer flags)**

Run: `./build.sh srvwebtest` — expected compile failure. Failing state confirmed.

- [ ] **Step 7.3 — write `srv_web.h`**

```c
/* srv_web.h — the server's pure HTML layer: a growable string builder, the
   HTML escape, and whole-page renderers (spec §7 web surface). No sqlite, no
   civetweb, no GL — Task 8 feeds it rows and ships the bytes. Every dynamic
   value MUST go through sb_put_html. */
#ifndef SRV_WEB_H
#define SRV_WEB_H

#include "nid.h"
#include <stddef.h>

typedef struct Sb {
    char  *buf;   /* always NUL-terminated */
    size_t len, cap;
} Sb;

void sb_init    (Sb *s);   /* aborts on OOM (server posture) */
void sb_free    (Sb *s);
void sb_puts    (Sb *s, const char *str);
void sb_put_html(Sb *s, const char *str);   /* & < > " ' escaped */
void sb_put_long(Sb *s, long v);

typedef struct WebBoardRow {
    char nid[NID_LEN + 1];
    char title[256];
    long updated_at;
} WebBoardRow;

void web_page_login (Sb *out, const char *err_msg);   /* err_msg NULL = none */
void web_page_boards(Sb *out, const WebBoardRow *rows, int n);
void web_page_error (Sb *out, int status, const char *msg);

#endif /* SRV_WEB_H */
```

- [ ] **Step 7.4 — write `srv_web.c`**

```c
/* srv_web.c — see srv_web.h. */

#include "srv_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(Sb *s) {
    s->cap = 256;
    s->len = 0;
    s->buf = (char *)malloc(s->cap);
    if (!s->buf) abort();
    s->buf[0] = 0;
}

void sb_free(Sb *s) {
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
}

static void sb_grow(Sb *s, size_t need) {
    char *nb;
    if (s->len + need + 1 <= s->cap) return;
    while (s->cap < s->len + need + 1) s->cap *= 2;
    nb = (char *)realloc(s->buf, s->cap);
    if (!nb) abort();
    s->buf = nb;
}

void sb_puts(Sb *s, const char *str) {
    size_t n = strlen(str);
    sb_grow(s, n);
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = 0;
}

void sb_put_html(Sb *s, const char *str) {
    const char *p;
    char one[2];
    one[1] = 0;
    for (p = str; *p; p++) {
        switch (*p) {
        case '&':  sb_puts(s, "&amp;");  break;
        case '<':  sb_puts(s, "&lt;");   break;
        case '>':  sb_puts(s, "&gt;");   break;
        case '"':  sb_puts(s, "&quot;"); break;
        case '\'': sb_puts(s, "&#39;");  break;
        default:   one[0] = *p; sb_puts(s, one); break;
        }
    }
}

void sb_put_long(Sb *s, long v) {
    char buf[32];
    sprintf(buf, "%ld", v);
    sb_puts(s, buf);
}

/* ---- pages ------------------------------------------------------------- */

static void page_open(Sb *o, const char *title) {
    sb_puts(o,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>");
    sb_put_html(o, title);
    sb_puts(o,
        " — Solarium</title><style>"
        "body{font-family:Georgia,serif;background:#1a1714;color:#e8e0d0;"
        "max-width:40rem;margin:2rem auto;padding:0 1rem}"
        "a{color:#d4a94f}"
        "h1{font-weight:normal;border-bottom:1px solid #3a342c;padding-bottom:.4rem}"
        "li{margin:.4rem 0}"
        "form.inline{display:inline}"
        "input{background:#26221c;color:#e8e0d0;border:1px solid #3a342c;"
        "padding:.4rem;font:inherit}"
        "button{background:#3a342c;color:#e8e0d0;border:0;padding:.4rem 1rem;"
        "font:inherit;cursor:pointer}"
        ".err{color:#d46a4f}.muted{color:#8a8274;font-size:.9rem}"
        "</style></head><body>");
}

static void page_close(Sb *o) {
    sb_puts(o, "</body></html>");
}

void web_page_login(Sb *out, const char *err_msg) {
    page_open(out, "Sign in");
    sb_puts(out, "<h1>Solarium</h1>");
    if (err_msg) {
        sb_puts(out, "<p class=\"err\">");
        sb_put_html(out, err_msg);
        sb_puts(out, "</p>");
    }
    sb_puts(out,
        "<form method=\"post\" action=\"/login\">"
        "<p><input name=\"user\" placeholder=\"user\" autofocus></p>"
        "<p><input name=\"pass\" type=\"password\" placeholder=\"password\"></p>"
        "<p><button>Sign in</button></p>"
        "</form>");
    page_close(out);
}

void web_page_boards(Sb *out, const WebBoardRow *rows, int n) {
    int i;
    page_open(out, "Boards");
    sb_puts(out,
        "<h1>Boards</h1>"
        "<form class=\"inline\" method=\"post\" action=\"/logout\">"
        "<button>Sign out</button></form>");
    if (n == 0) {
        sb_puts(out, "<p class=\"muted\">No boards yet — the app syncs them here.</p>");
    } else {
        sb_puts(out, "<ul>");
        for (i = 0; i < n; i++) {
            sb_puts(out, "<li>");
            sb_put_html(out, rows[i].title[0] ? rows[i].title : "(untitled)");
            sb_puts(out, " <span class=\"muted\">");
            sb_put_html(out, rows[i].nid);
            sb_puts(out, "</span></li>");
        }
        sb_puts(out, "</ul>");
    }
    page_close(out);
}

void web_page_error(Sb *out, int status, const char *msg) {
    page_open(out, "Error");
    sb_puts(out, "<h1>");
    sb_put_long(out, (long)status);
    sb_puts(out, "</h1><p>");
    sb_put_html(out, msg);
    sb_puts(out, "</p><p><a href=\"/boards\">Back to boards</a></p>");
    page_close(out);
}
```

- [ ] **Step 7.5 — run the test**

Run: `./build.sh srvwebtest && ./srv_web_test`
Expected: all `ok` lines, `srv_web_test: all ok`, exit 0, no sanitizer output.

- [ ] **Step 7.6 — c89check + commit**

Append `srv_web.c` to the c89check list. Run `./build.sh c89check` — expected PASS.

```bash
git add srv_web.c srv_web.h srv_web_test.c build.sh
git commit -m "Server foundation Task 7: pure HTML layer — builder, escape, pages

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 8 — HTTP wiring: route table, login/logout/boards handlers

**Files:**
- Modify: `srv_main.c` (full rewrite below), `build.sh` (`server` compile list grows to all TUs), `srv_test.sh` (new assertions)

**Interfaces:**
- Consumes: everything Tasks 2–7 produced (exact signatures listed in those tasks).
- Produces: the live HTTP surface — `GET /health`, `GET /` (→/boards), `GET /login`, `POST /login` (form `user`/`pass`; 302+cookie, 403 bad creds, 429 throttled, 413 oversized), `POST /logout`, `GET /boards` (session-gated), 404 everything else. Task 9 adds the CLI into this same file.

- [ ] **Step 8.1 — update the failing integration test first: `srv_test.sh`**

Replace the assertions after the `fail()` definition (keep everything above it) with:

```sh
[ "$(curl -fsS $BASE/health)" = "ok" ] || fail "health"
echo "PASS health"

code=$(curl -s -o /dev/null -w '%{http_code}' $BASE/boards)
[ "$code" = "302" ] || fail "unauth /boards should 302, got $code"
echo "PASS unauth redirect"

curl -fsS $BASE/login | grep -q '<form method="post" action="/login">' || fail "login page form"
echo "PASS login page"

code=$(curl -s -o /dev/null -w '%{http_code}' -d 'user=ghost&pass=nothing123' $BASE/login)
[ "$code" = "403" ] || fail "bad login should 403, got $code"
echo "PASS bad login"

code=$(curl -s -o /dev/null -w '%{http_code}' $BASE/nowhere)
[ "$code" = "404" ] || fail "unknown path should 404, got $code"
echo "PASS 404"

echo "ALL PASS"
```

Run: `./srv_test.sh` — expected: `FAIL: unauth /boards should 302, got 404` (Task 1's binary has no routes). Failing state confirmed.

- [ ] **Step 8.2 — rewrite `srv_main.c`**

```c
/* srv_main.c — solsrv entry point: flags, CivetWeb boot, the route table and
   HTTP handlers, and (Task 9) the admin CLI. Strict C89; binds loopback only —
   Caddy fronts it in production (spec §8). One catch-all CivetWeb handler
   ("**") dispatches through ROUTES[] so all routing lives in one table. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <civetweb.h>

#include "srv_db.h"
#include "srv_events.h"
#include "srv_auth.h"
#include "srv_web.h"
#include "nid.h"

typedef struct SrvApp {
    SrvDb db;
    int   secure_cookies;   /* -s: append "; Secure" (production, behind Caddy) */
} SrvApp;

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

static void copy_capped(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- response helpers --------------------------------------------------- */

/* extra_hdr: NULL or one or more full "Name: value\r\n" lines. */
static void send_page(struct mg_connection *c, int status, const char *reason,
                      const Sb *body, const char *extra_hdr) {
    mg_printf(c, "HTTP/1.1 %d %s\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n"
                 "%s"
                 "Connection: close\r\n\r\n",
              status, reason, (unsigned long)body->len,
              extra_hdr ? extra_hdr : "");
    mg_write(c, body->buf, body->len);
}

static void send_redirect(struct mg_connection *c, const char *loc,
                          const char *set_cookie) {
    mg_printf(c, "HTTP/1.1 302 Found\r\n"
                 "Location: %s\r\n"
                 "%s"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n",
              loc, set_cookie ? set_cookie : "");
}

static void send_error_page(struct mg_connection *c, int status,
                            const char *reason, const char *msg) {
    Sb b;
    sb_init(&b);
    web_page_error(&b, status, msg);
    send_page(c, status, reason, &b, NULL);
    sb_free(&b);
}

/* The request's session user id (0 if none). If token_out is non-NULL it
   receives the raw cookie token (valid or not) — logout wants it either way. */
static long req_user(struct mg_connection *c, SrvApp *app,
                     char token_out[SRV_TOKEN_CHARS + 1]) {
    const char *hdr = mg_get_header(c, "Cookie");
    char tok[SRV_TOKEN_CHARS + 1];
    if (token_out) token_out[0] = 0;
    if (!hdr) return 0;
    if (mg_get_cookie(hdr, "sid", tok, sizeof tok) < 0) return 0;
    if (token_out) strcpy(token_out, tok);
    return srv_auth_session_user(&app->db, tok);
}

/* ---- handlers ----------------------------------------------------------- */

static int h_health(struct mg_connection *c, SrvApp *app) {
    (void)app;
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                 "Content-Length: 3\r\nConnection: close\r\n\r\nok\n");
    return 200;
}

static int h_root(struct mg_connection *c, SrvApp *app) {
    (void)app;
    send_redirect(c, "/boards", NULL);
    return 302;
}

static int h_login_get(struct mg_connection *c, SrvApp *app) {
    Sb b;
    if (req_user(c, app, NULL) > 0) {
        send_redirect(c, "/boards", NULL);
        return 302;
    }
    sb_init(&b);
    web_page_login(&b, NULL);
    send_page(c, 200, "OK", &b, NULL);
    sb_free(&b);
    return 200;
}

static int h_login_post(struct mg_connection *c, SrvApp *app) {
    const struct mg_request_info *ri = mg_get_request_info(c);
    char body[4096], probe[1], user[64], pass[256];
    char token[SRV_TOKEN_CHARS + 1];
    char cookie[256];
    Sb b;
    int n = 0, r = 0;
    long uid;

    if (!srv_auth_throttle_ok(&app->db, ri->remote_addr)) {
        sb_init(&b);
        web_page_login(&b, "Too many attempts. Try again later.");
        send_page(c, 429, "Too Many Requests", &b, NULL);
        sb_free(&b);
        return 429;
    }

    while (n < (int)sizeof body
           && (r = mg_read(c, body + n, sizeof body - (size_t)n)) > 0) n += r;
    if (n == (int)sizeof body && mg_read(c, probe, 1) > 0) {
        send_error_page(c, 413, "Payload Too Large", "Request too large.");
        return 413;
    }

    if (mg_get_var(body, (size_t)n, "user", user, sizeof user) < 0) user[0] = 0;
    if (mg_get_var(body, (size_t)n, "pass", pass, sizeof pass) < 0) pass[0] = 0;

    uid = srv_auth_login(&app->db, user, pass, token);
    if (uid == 0) {
        srv_auth_throttle_fail(&app->db, ri->remote_addr);
        sb_init(&b);
        web_page_login(&b, "Bad username or password.");
        send_page(c, 403, "Forbidden", &b, NULL);
        sb_free(&b);
        return 403;
    }
    srv_auth_throttle_clear(&app->db, ri->remote_addr);

    sprintf(cookie, "Set-Cookie: sid=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=2592000%s\r\n",
            token, app->secure_cookies ? "; Secure" : "");
    send_redirect(c, "/boards", cookie);
    return 302;
}

static int h_logout_post(struct mg_connection *c, SrvApp *app) {
    char tok[SRV_TOKEN_CHARS + 1];
    if (req_user(c, app, tok) > 0) srv_auth_logout(&app->db, tok);
    send_redirect(c, "/login", "Set-Cookie: sid=; Path=/; HttpOnly; Max-Age=0\r\n");
    return 302;
}

static int h_boards_get(struct mg_connection *c, SrvApp *app) {
    sqlite3 *r;
    sqlite3_stmt *st = NULL;
    WebBoardRow *rows;
    Sb b;
    int n = 0;

    if (req_user(c, app, NULL) == 0) {
        send_redirect(c, "/login", NULL);
        return 302;
    }

    r = srv_db_ropen(&app->db);
    if (!r) {
        send_error_page(c, 500, "Internal Server Error", "Database unavailable.");
        return 500;
    }
    rows = (WebBoardRow *)calloc(256, sizeof *rows);
    if (!rows) {
        srv_db_rclose(r);
        send_error_page(c, 500, "Internal Server Error", "Out of memory.");
        return 500;
    }

    if (sqlite3_prepare_v2(r,
            "SELECT nid,title,updated_at FROM boards WHERE deleted=0"
            " ORDER BY updated_at DESC, nid LIMIT 256", -1, &st, NULL) == SQLITE_OK) {
        while (n < 256 && sqlite3_step(st) == SQLITE_ROW) {
            copy_capped(rows[n].nid, sizeof rows[n].nid,
                        (const char *)sqlite3_column_text(st, 0));
            copy_capped(rows[n].title, sizeof rows[n].title,
                        (const char *)sqlite3_column_text(st, 1));
            rows[n].updated_at = (long)sqlite3_column_int64(st, 2);
            n++;
        }
        sqlite3_finalize(st);
    }
    srv_db_rclose(r);

    sb_init(&b);
    web_page_boards(&b, rows, n);
    send_page(c, 200, "OK", &b, NULL);
    sb_free(&b);
    free(rows);
    return 200;
}

/* ---- route table (spec §4: method + path -> handler) -------------------- */

typedef struct Route {
    const char *method;
    const char *path;
    int (*fn)(struct mg_connection *c, SrvApp *app);
} Route;

static const Route ROUTES[] = {
    { "GET",  "/health", h_health      },
    { "GET",  "/",       h_root        },
    { "GET",  "/login",  h_login_get   },
    { "POST", "/login",  h_login_post  },
    { "POST", "/logout", h_logout_post },
    { "GET",  "/boards", h_boards_get  }
};

static int route_all(struct mg_connection *c, void *ud) {
    SrvApp *app = (SrvApp *)ud;
    const struct mg_request_info *ri = mg_get_request_info(c);
    int i;
    int n = (int)(sizeof ROUTES / sizeof ROUTES[0]);

    for (i = 0; i < n; i++) {
        if (strcmp(ri->request_method, ROUTES[i].method) == 0
            && strcmp(ri->local_uri, ROUTES[i].path) == 0) {
            return ROUTES[i].fn(c, app);
        }
    }
    send_error_page(c, 404, "Not Found", "No such page.");
    return 404;
}

/* ---- entry -------------------------------------------------------------- */

int main(int argc, char **argv) {
    static SrvApp app;              /* zero-initialized, process-lifetime */
    const char *port = "127.0.0.1:8080";
    const char *dbpath = "solsrv.db";
    struct mg_callbacks cbs;
    struct mg_context *ctx;
    const char *opts[8];
    int i, n = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) dbpath = argv[++i];
        else if (strcmp(argv[i], "-s") == 0) app.secure_cookies = 1;
        else {
            fprintf(stderr, "usage: solsrv [-p host:port] [-d dbfile] [-s]\n");
            return 2;
        }
    }

    if (srv_db_open(&app.db, dbpath) != 0) return 1;

    opts[n++] = "listening_ports";    opts[n++] = port;
    opts[n++] = "num_threads";        opts[n++] = "8";
    opts[n++] = "request_timeout_ms"; opts[n++] = "10000";
    opts[n] = NULL;

    mg_init_library(0);
    memset(&cbs, 0, sizeof cbs);
    ctx = mg_start(&cbs, NULL, opts);
    if (!ctx) {
        fprintf(stderr, "solsrv: mg_start failed on %s\n", port);
        srv_db_close(&app.db);
        return 1;
    }
    mg_set_request_handler(ctx, "**", route_all, &app);

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    printf("solsrv listening on %s (db %s)\n", port, dbpath);
    while (!g_stop) sleep(1);

    mg_stop(ctx);
    mg_exit_library();
    srv_db_close(&app.db);
    return 0;
}
```

(Task 1 registered `/health` directly; this rewrite moves it into ROUTES and registers the single `"**"` catch-all — CivetWeb's glob that matches every URI — so route precedence is entirely ours.)

- [ ] **Step 8.3 — build.sh: grow the `server` compile list to its final form**

```sh
        srv_main.c srv_db.c srv_events.c srv_auth.c srv_web.c srv_rand.c \
        sha256.c b64.c json.c nid.c \
```

- [ ] **Step 8.4 — build + run the integration test**

Run: `./build.sh server && ./srv_test.sh`
Expected: `PASS health`, `PASS unauth redirect`, `PASS login page`, `PASS bad login`, `PASS 404`, `ALL PASS`. (The bad-login case takes ~0.5 s — that's the dummy PBKDF2 at full production iterations, working as designed.)

- [ ] **Step 8.5 — c89check + commit**

Run `./build.sh c89check` — expected PASS (all `srv_*.c` except `srv_rand.c` are in the list by now).

```bash
git add srv_main.c srv_test.sh build.sh
git commit -m "Server foundation Task 8: route table + login/logout/boards over CivetWeb

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 9 — CLI (`--adduser`, `--seed`) + the full integration gauntlet

**Files:**
- Modify: `srv_main.c` (CLI block in `main`), `srv_test.sh` (final version)

**Interfaces:**
- Consumes: `srv_auth_user_create`, `srv_events_append`, `nid_generate`.
- Produces: `solsrv -d <db> --adduser <name>` (password on stdin, then exits) and `solsrv -d <db> --seed` (two board-create events through the event store, refuses if boards exist). Deploy docs (Task 10) reference both.

- [ ] **Step 9.1 — write the failing test first: final `srv_test.sh`**

Replace the whole file:

```sh
#!/bin/sh
# srv_test.sh — integration gauntlet for solsrv: builds, seeds a scratch db,
# boots the server, and drives the real HTTP surface with curl. Full-strength
# PBKDF2 makes each login ~0.5s; the whole run is ~10s by design.
set -eu

PORT=18080
BASE="http://127.0.0.1:$PORT"
DB=srv_itest.db
JAR=srv_itest.cookies
PW=hunter2boogaloo

rm -f "$DB" "$DB-wal" "$DB-shm" "$JAR"
./build.sh server >/dev/null

printf '%s' "$PW" | ./solsrv -d "$DB" --adduser fran
./solsrv -d "$DB" --seed

./solsrv -p 127.0.0.1:$PORT -d "$DB" &
SRV=$!
trap 'kill $SRV 2>/dev/null || true; rm -f "$JAR"' EXIT
sleep 1

fail() { echo "FAIL: $1"; exit 1; }

[ "$(curl -fsS $BASE/health)" = "ok" ] || fail "health"
echo "PASS health"

code=$(curl -s -o /dev/null -w '%{http_code}' $BASE/boards)
[ "$code" = "302" ] || fail "unauth /boards should 302, got $code"
echo "PASS unauth redirect"

curl -fsS $BASE/login | grep -q '<form method="post" action="/login">' || fail "login page form"
echo "PASS login page"

code=$(curl -s -o /dev/null -w '%{http_code}' $BASE/nowhere)
[ "$code" = "404" ] || fail "unknown path should 404, got $code"
echo "PASS 404"

code=$(curl -s -o /dev/null -w '%{http_code}' -d "user=fran&pass=wrongwrong" $BASE/login)
[ "$code" = "403" ] || fail "bad login should 403, got $code"
echo "PASS bad login"

code=$(curl -s -o /dev/null -w '%{http_code}' -c "$JAR" -d "user=fran&pass=$PW" $BASE/login)
[ "$code" = "302" ] || fail "good login should 302, got $code"
curl -fsS -b "$JAR" $BASE/boards | grep -q 'Welcome to Solarium' || fail "boards shows seeded board"
curl -fsS -b "$JAR" $BASE/boards | grep -q 'Reading list' || fail "boards shows second board"
echo "PASS login + seeded boards"

./solsrv -d "$DB" --seed 2>/dev/null && fail "re-seed should refuse" || true
echo "PASS seed refuses twice"

code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" -X POST $BASE/logout)
[ "$code" = "302" ] || fail "logout should 302, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" $BASE/boards)
[ "$code" = "302" ] || fail "session dead after logout, got $code"
echo "PASS logout"

i=0
while [ $i -lt 10 ]; do
    curl -s -o /dev/null -d "user=fran&pass=wrongwrong" $BASE/login
    i=$((i+1))
done
code=$(curl -s -o /dev/null -w '%{http_code}' -d "user=fran&pass=$PW" $BASE/login)
[ "$code" = "429" ] || fail "throttle should 429 after 10 fails, got $code"
echo "PASS throttle"

echo "ALL PASS"
```

Run: `./srv_test.sh` — expected: FAIL at the `--adduser` line (`usage: solsrv …`, exit 2). Failing state confirmed.

Note the re-seed check runs against the live db while the server holds it open — that's deliberate: WAL + busy_timeout make a second process safe, and the seed guard must trip before any write.

- [ ] **Step 9.2 — add the CLI block to `srv_main.c`**

In `main`, extend the flag parser (two new cases before the `else`):

```c
        else if (strcmp(argv[i], "--adduser") == 0 && i + 1 < argc) adduser = argv[++i];
        else if (strcmp(argv[i], "--seed") == 0) seed = 1;
```

with declarations added at the top of `main`:

```c
    const char *adduser = NULL;
    int seed = 0;
```

and update the usage line:

```c
            fprintf(stderr, "usage: solsrv [-p host:port] [-d dbfile] [-s]"
                            " [--adduser name] [--seed]\n");
```

After the flag loop and BEFORE the server-boot `srv_db_open`, insert:

```c
    if (adduser) {
        char pw[256];
        size_t l;
        if (srv_db_open(&app.db, dbpath) != 0) return 1;
        if (!fgets(pw, sizeof pw, stdin)) {
            fprintf(stderr, "solsrv: --adduser reads the password from stdin\n");
            srv_db_close(&app.db);
            return 2;
        }
        l = strlen(pw);
        while (l > 0 && (pw[l - 1] == '\n' || pw[l - 1] == '\r')) pw[--l] = 0;
        if (srv_auth_user_create(&app.db, adduser, pw) != 0) {
            fprintf(stderr, "solsrv: adduser failed"
                    " (name [a-z0-9_-]{1,32}, unique; password >= 8 chars)\n");
            srv_db_close(&app.db);
            return 1;
        }
        printf("user '%s' created\n", adduser);
        srv_db_close(&app.db);
        return 0;
    }

    if (seed) {
        static const char *TITLES[2] = { "Welcome to Solarium", "Reading list" };
        char nidbuf[NID_LEN + 1];
        char payload[128];
        SrvEventIn ev;
        sqlite3 *r;
        sqlite3_stmt *st = NULL;
        long id;
        int existing = 0, k;

        if (srv_db_open(&app.db, dbpath) != 0) return 1;
        r = srv_db_ropen(&app.db);
        if (r) {
            if (sqlite3_prepare_v2(r, "SELECT COUNT(*) FROM boards", -1, &st, NULL) == SQLITE_OK
                && sqlite3_step(st) == SQLITE_ROW) {
                existing = sqlite3_column_int(st, 0);
            }
            sqlite3_finalize(st);
            srv_db_rclose(r);
        }
        if (existing > 0) {
            fprintf(stderr, "solsrv: boards exist, refusing to seed\n");
            srv_db_close(&app.db);
            return 1;
        }
        for (k = 0; k < 2; k++) {
            nid_generate(nidbuf);
            memset(&ev, 0, sizeof ev);
            ev.actor_id = 0;                 /* system */
            ev.origin_device = "seed";
            ev.entity_kind = "board";
            ev.entity_nid = nidbuf;
            ev.op = "create";
            /* static titles — nothing to JSON-escape */
            sprintf(payload, "{\"title\":\"%s\"}", TITLES[k]);
            ev.payload = payload;
            if (srv_events_append(&app.db, &ev, &id) != 0) {
                fprintf(stderr, "solsrv: seed append failed\n");
                srv_db_close(&app.db);
                return 1;
            }
        }
        printf("seeded 2 boards\n");
        srv_db_close(&app.db);
        return 0;
    }
```

Seeding goes through `srv_events_append` — never a direct projection INSERT — so seed data is real event-log history like everything else (spec §5).

- [ ] **Step 9.3 — run the full gauntlet**

Run: `./build.sh server && ./srv_test.sh`
Expected: all nine `PASS` lines, then `ALL PASS`. (~10 s: each real login pays the 600k-iteration KDF.)

- [ ] **Step 9.4 — c89check + commit**

Run `./build.sh c89check` — expected PASS.

```bash
git add srv_main.c srv_test.sh
git commit -m "Server foundation Task 9: --adduser/--seed CLI + full integration gauntlet

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 10 — deploy kit: Caddyfile, systemd unit, backups, DEPLOY.md

**Files:**
- Create: `deploy/Caddyfile`, `deploy/solsrv.service`, `deploy/backup.sh`, `deploy/DEPLOY.md`

**Interfaces:**
- Consumes: the `solsrv` flags from Tasks 8/9 (`-p`, `-d`, `-s`, `--adduser`, `--seed`) and `CC=gcc ./build.sh server`.
- Produces: the documented path from a fresh Debian/Ubuntu VPS to a running HTTPS deployment. Nothing else depends on this task; the human executes it.

- [ ] **Step 10.1 — write `deploy/Caddyfile`**

```
# Caddy fronts solsrv: automatic HTTPS (Let's Encrypt), reverse proxy to
# loopback. Replace the domain. Lives at /etc/caddy/Caddyfile on the VPS.
solarium.example.com {
	reverse_proxy 127.0.0.1:8080
}
```

- [ ] **Step 10.2 — write `deploy/solsrv.service`**

```
# systemd unit for solsrv. Lives at /etc/systemd/system/solsrv.service.
# -s: cookies are Secure because Caddy serves HTTPS.
[Unit]
Description=Solarium server (solsrv)
After=network.target

[Service]
User=solarium
WorkingDirectory=/home/solarium
ExecStart=/home/solarium/solsrv -p 127.0.0.1:8080 -d /home/solarium/data/solsrv.db -s
Restart=on-failure
RestartSec=2

# containment: solsrv needs nothing but its data dir
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=/home/solarium/data
NoNewPrivileges=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 10.3 — write `deploy/backup.sh`**

```sh
#!/bin/sh
# backup.sh <db-file> <backup-dir> — consistent SQLite snapshot via .backup
# (safe against a live WAL writer), date-stamped, keeps the newest 14.
set -eu

DB="$1"
OUT="$2"
STAMP=$(date +%Y%m%d-%H%M%S)

mkdir -p "$OUT"
sqlite3 "$DB" ".backup '$OUT/solsrv-$STAMP.db'"
ls -1t "$OUT"/solsrv-*.db | tail -n +15 | xargs -r rm --
echo "backup: $OUT/solsrv-$STAMP.db"
```

- [ ] **Step 10.4 — write `deploy/DEPLOY.md`**

````markdown
# Deploying solsrv (sub-project 1)

Target: a small Debian/Ubuntu VPS. Caddy terminates HTTPS and proxies to
solsrv on loopback. solsrv never touches TLS (spec §1).

## 1. Provision (once)

As root on the fresh VPS:

```sh
apt update && apt install -y build-essential clang rsync sqlite3 \
    debian-keyring debian-archive-keyring apt-transport-https curl
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
    | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
    | tee /etc/apt/sources.list.d/caddy-stable.list
apt update && apt install -y caddy

adduser --disabled-password --gecos '' solarium
mkdir -p /home/solarium/data /home/solarium/backups
chown -R solarium:solarium /home/solarium
```

Point your domain's A/AAAA records at the VPS before starting Caddy — it
needs them to obtain certificates.

## 2. Ship + build (every deploy)

From the repo on the Mac (server sources + vendor, no build artifacts):

```sh
rsync -av --exclude '*.o' \
    build.sh sol_base.h \
    json.c json.h nid.c nid.h sha256.c sha256.h b64.c b64.h \
    srv_rand.c srv_rand.h srv_db.c srv_db.h srv_events.c srv_events.h \
    srv_auth.c srv_auth.h srv_web.c srv_web.h srv_main.c \
    vendor deploy \
    solarium@YOUR_VPS:/home/solarium/src/
```

On the VPS as `solarium`:

```sh
cd ~/src && ./build.sh server && cp solsrv ~/solsrv
```

(`clang` is installed above; `CC=gcc ./build.sh server` works too.)

## 3. First run (once)

As root:

```sh
cp /home/solarium/src/deploy/solsrv.service /etc/systemd/system/
cp /home/solarium/src/deploy/Caddyfile /etc/caddy/Caddyfile   # edit the domain first
systemctl daemon-reload
systemctl enable --now solsrv
systemctl reload caddy
```

As `solarium` — create your user and the placeholder boards:

```sh
printf 'YOUR_PASSWORD' | ~/solsrv -d ~/data/solsrv.db --adduser fran
~/solsrv -d ~/data/solsrv.db --seed
```

(Run these while the service is up or down — WAL + busy_timeout make a
second process safe.)

Visit `https://your-domain/` → sign in → the seeded board list. Done =
spec sub-project 1 exit criterion.

## 4. Backups (once)

As `solarium`, `crontab -e`:

```
17 3 * * * /home/solarium/src/deploy/backup.sh /home/solarium/data/solsrv.db /home/solarium/backups
```

Restore = stop solsrv, copy a snapshot over `~/data/solsrv.db` (remove any
`-wal`/`-shm` sidecars), start solsrv.

## 5. Update code later

Repeat step 2, then as root: `systemctl restart solsrv`.
````

- [ ] **Step 10.5 — local rehearsal of the service line**

The unit's ExecStart flags must actually work. Rehearse locally:

```sh
mkdir -p /tmp/solsrv-rehearsal
./solsrv -p 127.0.0.1:18081 -d /tmp/solsrv-rehearsal/solsrv.db -s &
sleep 1
curl -fsS http://127.0.0.1:18081/health
kill %1
rm -rf /tmp/solsrv-rehearsal
```

Expected: `ok`. Also: `chmod +x deploy/backup.sh` and run
`sh -n deploy/backup.sh` (syntax check) — expected silence.

- [ ] **Step 10.6 — commit**

```bash
git add deploy/Caddyfile deploy/solsrv.service deploy/backup.sh deploy/DEPLOY.md
git commit -m "Server foundation Task 10: deploy kit — Caddyfile, systemd unit, backups, DEPLOY.md

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Final holistic review + human live-verify

Per the windows-arc / map-view lesson: after all tasks, a **final holistic review** by a fresh reviewer over the whole diff (`git diff main...server-foundation`), looking specifically at cross-task seams:

- Every `db->w` touch inside `srv_db_wlock`/`srv_db_wunlock`? (grep `db->w` / `->db.w`)
- Every `srv_db_ropen` paired with `srv_db_rclose` on all paths, including errors?
- Every dynamic HTML value through `sb_put_html`? (grep `sb_puts` calls in handlers for anything non-literal)
- Every `sqlite3_prepare_v2` paired with `sqlite3_finalize` on all paths?
- No `long long`, `snprintf`, `strdup`, `//` comments in c89check-listed files (c89check proves most of this — run it last).
- `srv_test.sh` passes from a clean checkout (`git stash -u` any local db files first).

**Human live-verify (Fran):**
1. Local: `./build.sh server && printf 'somepassword' | ./solsrv --adduser fran && ./solsrv --seed && ./solsrv` → browse `http://127.0.0.1:8080`, sign in, see the two boards, sign out.
2. VPS: follow `deploy/DEPLOY.md` end to end — this is the sub-project's exit criterion (spec §3: "deployed on the VPS, answering over HTTPS with a seeded placeholder board list").
3. Then ff-merge `server-foundation` to main per the house process.

## Out of scope (do not build these here)

Sync endpoints and client-op idempotency (sub-project 2, schema migration v2), `json_write` (sub-project 2), board detail pages / CSRF-tokened forms beyond login (sub-project 3), OAuth + MCP (sub-project 4), SSE/WebSockets, Docker/CI, Argon2.


