# Sync — Design (Server Arc, Sub-project 2)

**Date:** 2026-07-10
**Status:** Approved design; feeds the sync implementation plan.
**Parent:** `docs/superpowers/specs/2026-07-01-server-sync-web-mcp-architecture-design.md`
(umbrella architecture; sub-project 1, the solsrv foundation, is merged).

Two-way entity sync between the Solarium app and solsrv: boards, pages, and
cards flow through the server's event log; edits made anywhere (another
machine, another user, later MCP) arrive back in the world.

---

## 1. Identity model (the quartet)

| Identity | What | Where it lives |
|---|---|---|
| **nid** | one entity (board/card) | on the object, minted by `scene_add`, serialized in scene.stml |
| **world_id** | one world | a nid in the scene file's world header (§3) |
| **machine tag** | one device | `~/.solarium/machine`, opaque nid minted at first launch — NEVER in the scene file |
| **user** | one person | server `users` table (sub-project 1) |

Events carry `actor_id` (user) + `origin_device` (machine tag) — "who, from
where" attribution, and the echo-suppression key.

## 2. Core rules (settled)

- **Op-journal deltas, never file-diffs.** The app records ops as they
  happen; absence from a file never implies deletion. File swaps and
  reloads cannot fabricate deletes (the diff shadow resets on load, §6).
- **Multi-world server.** Every event is world-scoped; any number of scene
  files can opt in.
- **Copies share identity** (= second-device semantics; converges via
  idempotent-by-nid applies). Deliberate divergence is the explicit
  `:` **Fork world** command: mints a new world_id, clears that world's
  journal/cursor. The app's local registry detects two paths with one
  world_id on the same machine and prompts.
- **Sync is opt-in per world**, default off; zero network unless enabled.
- **Conflicts v1: last-writer-wins by server event order.** Remote applies
  over local; still-queued local ops push afterward and win in turn.
  Deliberately naive; the event log permits smarter merges later.
- **Snapshot-replace is out**; a "make server match this file" command may
  exist someday as an explicit confirmable action, never ambient behavior.

## 3. The world header

New optional element at the top of scene.stml (SCENE_FORMAT.md gains a
section; scene_io reads/writes it):

```
<world id="01J..." name="the abbey" sync="on" />
```

- `id`: minted **in memory on first load** when absent; persisted with the
  file's next save (loads never write files — that stays true).
- `name`: human label, hand-editable.
- `sync`: `on`/`off`, default off. The `:` **"Sync this world"** command
  registers the world with the server (`POST /worlds`) and flips it.

## 4. v1 entity scope

Syncs: **boards** (mesh_ref `"board"`: name meta, workspace, `pages` list,
`active_page`, size params), **cards** (mesh_ref `"card"`/`"picture"`/
`"folderbook"` with a board parent: kind, parent board nid, `page`, `text`,
`text_size`, `min_h`, `link`, content path, position, size). Pages are board
meta, not objects — they ride on board/card updates.

Does NOT sync in v1: image **pixels** (a picture card syncs as a reference;
its `library/<nid>.png` stays local — blob sync is a future slice), the 3D
world (rooms, terrain, furniture), a board's world position/rotation
(spatial placement is per-world and stays local — only card positions ON a
board sync, since those are board-local data), pins/maps, creatures.

## 5. Local state: `~/.solarium/`

```
machine                      the machine tag (one nid)
token                        server base URL + device token (after sign-in)
worlds/<world_id>/journal    pending ops, one JSON object per line, append-only, fsync'd
worlds/<world_id>/cursor     last-applied server event id
worlds/<world_id>/arrivals   parked events for boards not in this world (§9)
registry                     world_id -> last-seen scene file path (fork detection)
```

The journal survives offline stretches and crashes; it truncates only after
the server acknowledges a push. The cursor advances only after a pull's
events are fully applied. The diff **shadow is in-memory only** — rebuilt
from the scene at every world load. Consequence (accepted): hand-edits to
scene.stml made outside the app do not journal; hand-edits are local-only.

## 6. Change capture: snapshot-diff at the save seam

Verified codebase facts this design leans on: every discrete mutation ends
with an eager `scene_save` (~60 sites; no autosave timer; note text saves on
blur); positions/parents are direct field writes (no setter); `scene_load`
itself calls `scene_add` per object (so add/remove hooks would misfire on
load).

Therefore: **one hook, after successful `scene_save`**, when the world has
sync on —

1. `snap_take(scene)` extracts the synced-entity subset into flat records
   (canonical field order).
2. `snap_diff(shadow, new)` yields ops: new nid → `create` (full payload);
   changed fields → `update` (changed fields only); vanished nid →
   `delete`.
3. Ops append to the journal (each stamped with a fresh `client_op` nid);
   the shadow swaps.

`sync_snap.c` is pure (GL-free, scene-read-only) with its own test binary.
The same record→JSON serializer feeds the journal and push bodies, so the
diff and the wire format cannot drift. Invariant under test: a fresh
`snap_take` immediately after a world load produces zero ops. During remote
apply (§8) the hook is suppressed — remote changes never re-journal.

## 7. The engine: `sync.c` + `platform_http.m`

**Threading:** main-thread state machine, ticked once per frame.
`platform_http.m` (NSURLSession behind a C API, the `platform_clipboard.m`
seam pattern) runs requests async; completions land in a small thread-safe
inbox drained on tick. All scene access stays on the main thread; no locks
outside the platform file.

**State machine** (pure, tested against a fake transport): idle → pushing
(journal non-empty, online) → pulling (after each push; on a ~30s timer; on
`:` "Sync now") → idle. HTTP failure → exponential backoff (capped),
journal keeps accumulating. Push sends a bounded batch of journal lines;
acknowledgment truncates exactly those lines. Nothing about sync — off,
unreachable, signed out — ever blocks or interrupts the world.

**Auth:** `:` **"Sync sign-in"** prompts user + password in-app (existing
text-input machinery), POSTs to `/auth/device` with the machine tag and a
device name, stores the returned long-lived token in `~/.solarium/token`.
A 401 anywhere flips status to "signed out — run Sync sign-in". Passwords
touch the app once and are never stored.

## 8. Applying remote events

Pull returns events in server order. Skip events whose `origin_device` is
this machine. Then, per event, with the save hook suppressed:

- **Known entity, update:** apply fields via the same paths local mutations
  use (`scene_meta_set`, `scene_mesh_params_set`, direct pos write).
- **Card create under a placed board:** `scene_add` + field sets (the
  `spawn_note`/`spawn_image_picture` sequence), preserving the event's nid.
- **Delete:** the `delete_board_card` cleanup path (transient-ref clears,
  asset release, `scene_remove`).
- **Board create/updates for an unknown board nid:** park (§9).

One `scene_save` at batch end; cursor file advances after that save
succeeds.

## 9. Arrivals: parked events, not ghost objects

Events for a board nid this world doesn't contain (and events for its
cards) append to `worlds/<id>/arrivals` instead of touching the scene. The
entity browser gains an **Arrivals** TypeProvider: parked boards listed by
name; **Place** replays that board's parked events through the normal apply
path — the board materializes at the carry target, its cards onto it — then
removes them from the parking file.

Rejected alternative, recorded: unplaced boards inside the scene behind an
`unplaced` flag would add a filter axis every scene reader must respect
(the FILTER LAW's known failure mode). Parked-outside-the-scene adds zero
reader gates.

## 10. Server: migration v2 + endpoints

Schema v2 (one new entry in the srv_db migration array):
- `worlds(world_id TEXT PK, name TEXT, created_by INTEGER, created_at INTEGER)`
- `events` + `world_id TEXT NOT NULL DEFAULT ''`, + `client_op TEXT NOT NULL
  DEFAULT ''`, + partial unique index on `client_op` where non-empty
- `device_tokens(token_hash BLOB PK, user_id, machine TEXT, name TEXT,
  created_at, last_seen)` — long-lived, revocable per device, distinct from
  web sessions
- `boards` + `world_id`; new `cards` projection (nid PK, world_id,
  board_nid, page, kind, text, x, y, w, h, deleted, updated_at, updated_by)

Endpoints (Bearer device token; same handler conventions as sub-project 1):
- `POST /auth/device` `{user, pass, machine, name}` → `{token}` — login
  throttle applies; KDF outside the write lock; token hashed at rest
- `POST /worlds` `{world_id, name}` — idempotent registration
- `POST /sync/push` `{world_id, ops:[{client_op, entity_kind, entity_nid,
  op, payload}]}` → `{cursor}` — validates (known world, sane kinds/ops,
  length caps on client-supplied fields), appends events skipping
  already-seen client_ops, projections update in-transaction as ever
- `GET /sync/pull?world=W&after=N` → `{events:[...], cursor}` —
  `srv_events_after` + world filter, serialized via the new `json_write`

Also in this sub-project (deferred items from sub-project 1's final
review): `json_write` (the json.c writer), a periodic sweep of expired
`sessions` / stale `login_attempts`, and the `srv_auth_login` return
contract split (0 = bad credentials, -1 = internal error) so DB trouble
can't count toward lockout.

## 11. Error posture

App: sync errors are status, never modals; `:` **"Sync status"** shows
world, device, journal depth, last push/pull, last error. Journal/cursor
file writes are fsync'd and tolerate partial trailing lines (ignored on
read). Server: the sub-project 1 posture holds (prepared statements,
bounded bodies, escaped output, constant-time compares); push bodies get a
size cap and per-field length caps.

## 12. Testing

- Pure, sanitized: `sync_snap_test` (diff correctness; changed-fields-only
  updates; zero-ops-after-load invariant), `sync_test` (state machine on a
  fake transport: batching, backoff, cursor advance, journal truncation,
  echo suppression, 401 handling), `json_write` vectors, `diskpath`-style
  tests for the `~/.solarium` path helpers.
- Server: `srv_sync_test` (push idempotency by client_op, world scoping,
  validation rejects, pull ordering).
- Integration: `srv_test.sh` grows a two-device scenario — A registers a
  world, pushes board+card; B pulls both; B pushes an edit; A pulls it;
  a re-pushed client_op does not duplicate.
- Human live-verify: two checkouts against a local solsrv; then the VPS.

## 13. Out of scope for this sub-project

Image blob sync; web views of the synced data (sub-project 3); OAuth + MCP
(sub-project 4); field-level merge/CRDTs; syncing the 3D world; board
rename UX (boards have no rename flow today — sync carries the name meta
regardless); a board-delete UX in the app (none exists today; remote board
deletes still apply); hand-edit journaling; SSE/push notification (pull is
polled).
