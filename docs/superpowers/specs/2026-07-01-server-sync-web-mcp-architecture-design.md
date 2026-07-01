# Server, Sync, Web & MCP — Umbrella Architecture Design

**Date:** 2026-07-01
**Status:** Approved direction; each sub-project gets its own spec → plan → build cycle.
**Scope of this doc:** the overall architecture, library decisions, and decomposition
for extending Solarium beyond the local app: a deployed server with a database,
two-way sync with the Mac app, a server-rendered web view, and an OAuth-gated MCP
server for LLM access.

---

## 1. Goals and decisions (settled in brainstorming)

- **Deployment:** Linux VPS behind a reverse proxy (Caddy). Caddy terminates TLS
  and proxies to the C server on localhost. **No TLS code in C, ever.**
- **Users:** multi-user from the start, shared data (not multi-tenant). Every write
  is attributed (GitLab-issue-style "who did what" is an early expansion target).
- **Auth:** real OAuth 2.1 for MCP from day one (claude.ai effectively requires it);
  simple session login for the web view.
- **Sync:** entity-level, **server is the source of truth** for synced entities.
  Event-sourced server database.
- **Web app:** server-rendered HTML from C. No JS toolchain, no framework.
- **Dependency policy:** stb-spirit vendored single-file C libraries are acceptable.
  The engine/app itself gains **zero** new C dependencies.

## 2. Library decisions (the landscape, resolved)

| Need | Decision | Rejected alternatives (why) |
|---|---|---|
| HTTP server | **CivetWeb** (MIT, single .c/.h, thread-per-request) vendored | Mongoose (GPLv2-or-commercial; event loop must never block, awkward with SQLite); libmicrohttpd (LGPL system lib, not stb-shaped); full from-scratch HTTP (edge cases: chunked encoding, header limits, slow clients — learning budget better spent on OAuth/sync/event sourcing) |
| Database | **SQLite amalgamation** (public domain) vendored; WAL mode | — (settled up front) |
| JSON | **Extend our own json.c** with a writer (`json_write`) | cJSON/parson/jansson (vendoring a JSON lib next to our own 315-line parser is out of character) |
| Crypto | **Hand-written:** sha256.c (FIPS 180-4 + official test vectors), HMAC + PBKDF2-HMAC-SHA256 built on it, base64url, constant-time compare. Randomness: `getrandom(2)` (Linux) / `arc4random_buf` (macOS) | Monocypher (excellent single-file lib, the upgrade path if we later want Argon2); libsodium (system lib, conventional not educational) |
| Tokens | **Opaque random tokens** (32 bytes, base64url), stored SHA-256-hashed with expiry | JWTs (would drag in HMAC-at-minimum or RSA/ECDSA + ASN.1; pointless when the authorization server and resource server are the same binary) |
| HTTP client (Mac app) | **NSURLSession behind `platform_http.m`** — system TLS, async, zero new deps, follows the `platform_clipboard.m` seam precedent | system libcurl (fine fallback, slightly off-pattern); vendored C client (none good, TLS is why) |
| HTML | **Hand-rolled emit functions + `html_escape`** into a string builder | mustach / template engines (earn their keep at team scale, not here) |
| MCP | **Hand-written** JSON-RPC 2.0 over Streamable HTTP | No C SDK exists (official SDKs: TS/Python/Go/Rust/C#/…); community C attempts immature |
| WebSockets / SSE | **Deferred.** Sync polls; MCP answers each POST with plain `application/json` (SSE is optional per spec) | — |

Vendored deps total: **two** (`vendor/civetweb.c/.h`, `vendor/sqlite3.c/.h`), plus the
two existing stb headers.

## 3. Decomposition — four sub-projects, in order

1. **Server foundation (`solsrv`)** — CivetWeb + SQLite embedded, migrations, the
   event store, users/sessions, config, deploy (systemd + Caddy on the VPS).
   Done = the server deployed on the VPS, answering over HTTPS with a seeded
   placeholder board list (real data arrives with sync).
2. **Sync** — `platform_http.m` + pure `sync.c` in the app; `/sync/push` and
   `/sync/pull` on the server; boards/cards/pages/notes mapped to events.
   Scope: information entities only — the 3D world stays local.
3. **Web views** — `GET /boards`, `GET /boards/<nid>`, session login, read-mostly.
4. **MCP + OAuth** — OAuth 2.1 endpoints first, then the MCP JSON-RPC surface.

Sync precedes web views because until the app pushes data the server has nothing
to show.

## 4. Server architecture

One binary, sources flat in the repo like everything else, prefixed `srv_`. The
server shares pure modules with the engine (`json.c`, `nid.c`, `sol_base.h`,
new `sha256.c`/`b64.c`) and **never links GL/Metal/GLFW**. Vendored TUs compile
separately and are exempt from c89check (like GLFW headers via -isystem); all
`srv_*.c` stays C89-pedantic clean and joins the c89check list.

| TU | Job |
|---|---|
| `srv_main.c` | config (port, db path, base URL, secrets file), CivetWeb boot, route table (method + path → handler) |
| `srv_db.c` | SQLite open/migrate, prepared-statement helpers. WAL mode; per-thread read connections; **one mutex-guarded write connection** (CivetWeb is thread-per-request; SQLite serializes writes anyway — we make it explicit) |
| `srv_events.c` | event store: append (same transaction as projection updates) + query-after-cursor. Pure logic split out for testing |
| `srv_auth.c` | users (PBKDF2), sessions, OAuth grants/tokens |
| `srv_sync.c` | push/pull endpoints |
| `srv_web.c` | HTML rendering |
| `srv_mcp.c` | JSON-RPC dispatch + tool implementations |

## 5. Data model — event log + projections

The append-only log is the source of truth and doubles as the sync feed:

```sql
events(id INTEGER PRIMARY KEY AUTOINCREMENT,  -- global cursor, server-assigned
       ts, actor_id, origin_device,           -- attribution + echo suppression
       entity_kind, entity_nid,               -- 'board' | 'card' | ... , nid
       op,                                    -- create | update | delete
       payload TEXT)                          -- JSON of changed fields
```

- Projection tables (`boards`, `cards`, …) update **in the same transaction** as
  the append: always consistent, always rebuildable from the log.
- **Existing nids are the global entity IDs** across app, server, web, and MCP.
- `actor_id` + the log = attribution history for free.
- Auth tables (`users`, `sessions`, `oauth_clients`, `oauth_codes`, `tokens`) are
  ordinary state, not event-sourced.

## 6. Sync protocol

- **Push:** app batches local ops → `POST /sync/push`. Each op carries a
  client-generated op id (idempotency) and `origin_device`. Server validates,
  appends events, returns the new cursor.
- **Pull:** `GET /sync/pull?after=<cursor>` → ordered events. App applies them,
  skipping events whose `origin_device` is itself.
- **Conflicts:** last-writer-wins per entity by server event order — deliberately
  naive v1; the event log permits smarter merges later without schema change.
- App side: `sync.c` is a pure module (queue, cursor, apply) tested with a faked
  network; `platform_http.m` does real HTTPS on a background thread and marshals
  results to the main loop. Sync triggers (on save / timer / manual) are details
  for that sub-project's spec.

## 7. Surfaces

**Web** — `GET /boards`, `GET /boards/<nid>` (cards by page). Login form →
PBKDF2 verify → session cookie (`HttpOnly`, `Secure`, `SameSite=Lax`); CSRF token
on any future POST form.

**OAuth 2.1** — discovery (`/.well-known/oauth-authorization-server` RFC 8414,
`/.well-known/oauth-protected-resource` RFC 9728), `/oauth/register` (dynamic
client registration RFC 7591 — what claude.ai expects), `/oauth/authorize`
(login + consent page, issues a code), `/oauth/token` (code + PKCE S256 exchange,
refresh). Opaque tokens, hashed at rest.

**MCP** — `POST /mcp`, Bearer-gated, JSON-RPC 2.0: `initialize`, `tools/list`,
`tools/call`. v1 tools: `list_boards`, `read_board`, `add_card`, `update_card`,
`search_cards`. Every tool appends events like any other actor, so MCP edits
reach the app through the same sync feed, with attribution. Plain JSON responses;
SSE deferred.

## 8. Build & deploy

- `build.sh server` compiles `srv_*.c` + shared pure modules + vendored TUs.
  Same sources build on macOS (local dev) and Linux (deploy) — POSIX + CivetWeb,
  no GL. c89check grows the `srv_*.c` list; run it in every gauntlet.
- Deploy v1: rsync sources to the VPS, build there, systemd unit, Caddy in front
  with automatic HTTPS (~5-line Caddyfile).
- Backups: nightly `sqlite3 .backup` + copy off-box.
- No Docker, no CI in v1.

## 9. Security posture

Prepared statements only; `html_escape` every dynamic output; bounded request
bodies + CivetWeb timeouts; constant-time compares for tokens/hashes; per-IP
login throttling (small table); Origin validation on `/mcp` per spec; secrets in
a server-side config file, never in the repo. API routes return a small JSON
error shape; web routes return an error page.

## 10. Testing

- Pure `*_test` binaries under ASan/UBSan: `json_write`, `sha256` (FIPS vectors),
  `b64`, PBKDF2, PKCE verify, event-store append/query, `sync.c`.
- One integration script (`srv_test.sh`): boot the server on a local port, drive
  it with curl (login → push → pull → MCP tools/call), assert on responses.
- c89check in every gauntlet (portal-material lesson).

## 11. Explicitly out of scope for v1

TLS in C, WebSockets/SSE, JWTs, Docker/CI, multi-tenancy, syncing the 3D world,
web-side editing beyond browsing, Argon2 (PBKDF2 iteration count documented as
the interim), rate limiting beyond login throttling.
