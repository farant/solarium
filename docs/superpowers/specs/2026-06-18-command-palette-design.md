# Command Palette — Design Spec

**Date:** 2026-06-18
**Status:** Approved (brainstorm complete; ready for implementation plan)
**Author:** Fran Arant (with Claude)

## Problem

Solarium has accumulated ~25 discrete single-key commands, all living as an inline
polled `if`-chain in `read_input()` (`main.c:5718–7249`). There is no registry,
table, or enum — nothing in the code *knows* "here are the commands." This makes the
commands undiscoverable (you must memorize keys) and impossible to enumerate.

A fuzzy-searchable command palette — popped up with a keystroke, type to filter, run
with Enter — restores discoverability and gives the engine a real command system.

## Goals

- A keyboard-driven command palette: open, type to filter, navigate, run, cancel.
- A single **command registry** that is the one source of truth for "what commands
  exist," consumed by *both* the keyboard handler and the palette.
- Preserve every current hotkey's behavior exactly.
- Fit the engine's idioms: small focused TUs, the immediate-mode UI look, the
  one-author law, dual-backend cleanliness.

## Non-goals (v1)

- Mouse interaction (no cursor freeing, no click-to-run) — keyboard only.
- Rebindable keys / a settings UI (the registry *enables* this later; not built now).
- Migrating the gnarliest commands in the first pass (see v1 scope).
- Command categories, recent/frequent ordering, multi-step command args.

## Locked decisions (from brainstorm)

1. **Shared registry, grown incrementally** — one `Command` table both `read_input()`
   and the palette dispatch through; commands migrate into it over time, old inline
   blocks coexist until migrated.
2. **Keyboard-only** — the mouse stays captured for look; no cursor-mode change.
3. **Open key = `:`** (`Shift`+`;`) — Vim command-line feel.

---

## Architecture

### The command registry (the heart)

Key insight: **separate continuous input from discrete commands.** Continuous input
(WASD, Space/Ctrl, arrows, `[`/`]` exposure) is held-every-frame and is *not* a
palette concern — it stays hand-coded in `read_input()`. Only the ~25 discrete,
edge-triggered commands go in the registry.

```c
typedef struct {
    const char *name;            /* "Toggle bloom" — shown + fuzzy-matched */
    int         key;             /* GLFW_KEY_K; 0 = palette-only, no hotkey */
    void      (*run)(AppState *);          /* the action */
    sol_bool  (*can_run)(AppState *);      /* precondition; NULL = always available */
    sol_bool    was_down;        /* edge-detect state, lives with the entry */
} Command;

static Command g_commands[] = { ... };   /* in main.c */
```

Each command's body moves out of its inline `if`-block into a `cmd_*(AppState*)`
function and gets one table row. **One author, two consumers:**

- **Keyboard** — `read_input()`'s ~25 edge-blocks collapse into one loop:
  ```c
  for each cmd in g_commands with cmd.key:
      now = glfwGetKey(w, cmd.key) == GLFW_PRESS;
      if (now && !cmd.was_down && (!cmd.can_run || cmd.can_run(st)))
          cmd.run(st);
      cmd.was_down = now;
  ```
- **Palette** — Enter on a result calls `selected->run(st)`.

Same function, same precondition, reached two ways — no duplication.

**Preconditions replace scattered guards.** Today `G` no-ops without a selection,
`U` needs `current_terrain`, `Backspace` needs a selected tombstone/arrow. These
become `can_run` predicates. The palette uses the *same* predicate to dim/annotate
an unavailable command instead of silently doing nothing.

**Incremental migration.** The registry loop and the remaining inline blocks coexist
— a key still handled inline simply isn't in the table yet. Migrate a batch, verify
end-to-end, then move heavier blocks over in follow-up passes, deleting each inline
block as its `cmd_*` lands. Nothing breaks mid-migration.

### Input routing & focus mode

Palette state lives in a `Palette` struct (defined in `palette.h`) embedded in
`AppState` as `st->palette` — so the routing code reads `st->palette.open` / passes
`&st->palette` to the `palette_*` handlers:

```c
typedef struct {
    sol_bool open;
    char     query[128];
    int      len;
    int      sel;          /* highlighted result row */
    sol_bool eat_char;     /* swallow the leading ':' */
} Palette;
```

Three entry points, each gated by `palette_open` **first**:

- **Open** — in `on_key` (`main.c:10214`, already receives `mods`): when no other
  modal is active and `Shift`+`;` is pressed, set `palette_open`, clear the query,
  `sel = 0`, set `palette_eat_char`. Event-driven (not polled) for a clean edge.
- **Type** — in `on_char` (`main.c:10198`): a branch *above* the `edit_handle` one —
  if `palette_open`, the first char (`:`) is eaten via `palette_eat_char`, else
  append to `palette_query` (reuse `utf8_encode`, `main.c:10169`) and reset `sel`.
  The scene is never touched.
- **Navigate/run** — in `on_key`, a `palette_open` branch: `Esc` closes (discard),
  `↑`/`↓` move `palette_sel` (clamped to result count), `Backspace` deletes a char
  (reuse the UTF-8-aware logic at `main.c:10219`), `Enter` runs the highlighted
  command **only if its `can_run` passes** (a disabled command no-ops); it **closes
  first, then** calls `cmd->run(st)` so the action runs in normal context.

**Blackhole the world while open.** Like note-editing zeros camera input
(`main.c:5731–5736`), `read_input()` early-outs its movement/look polling **and skips
the command-dispatch loop** while `palette_open` — this stops a typed letter from
also firing its hotkey. No cursor-mode change; the mouse stays captured.

**The `:` leak gotcha.** Pressing `Shift`+`;` fires `on_key` (which opens the palette)
*and then* `on_char` with `:`. `palette_eat_char` makes the first `on_char` after open
ignore that one character so it doesn't land in the field.

**Precedence — one modal at a time:**

| State | Can `:` open the palette? |
|---|---|
| Reader open (`reader_state != READER_IDLE`) | No — book owns the keyboard |
| Note editing (`edit_handle != 0`) | No — card owns the keyboard |
| Orbit or FP camera (`camera.mode`) | Yes, either — palette is camera-agnostic |

Open-trigger guard: `!palette_open && edit_handle == 0 && reader_state == READER_IDLE`.
While the palette is open, every entry point checks `palette_open` first, so the
others are suppressed automatically. No new global mode enum — just the flag,
consistent with how `edit_handle`/`reader_state` already gate each other.

### Fuzzy matching

A pure, case-insensitive *subsequence* scorer (fzf/Sublime-style): query chars must
appear in order, not necessarily contiguous (`"bl"` → "Toggle **bl**oom"). Scoring:
bonuses for start-of-string and word-boundary matches, a contiguous-run bonus, a
per-gap penalty. Sort by score, stable tiebreak on registry order. Empty query
returns *all* commands in registry order (so `:` then nothing = a full menu).
Returns matched byte positions (so matched chars *can* be brightened). Trivially
cheap for ~25 commands every keystroke.

```c
sol_bool fuzzy_match(const char *query, const char *cand,
                     int *out_score, int *out_pos, int max_pos);
```

### Rendering / layout

Follows the debug-panel idiom (`main.c:9924`): translucent dark quad + warm outline +
SDF text, scaled by `us = fb_height / 1080`, drawn in the `ui_begin/ui_end` 2D pass
*after* the debug panel (on top). A centered box ~55% width anchored in the upper
third. Top row: the typed query + the `_` caret used on note cards (`main.c:9576`).
Result rows below: command **name** (left) + **key hint** (right-aligned, dimmed, via
`text_measure`); the highlighted row gets a brighter backing quad; commands whose
`can_run(st)` is false render dimmed. Show up to ~12 rows, windowed to keep the
selection visible. **Mono font** for the list so the key-hint column aligns.

---

## File / TU organization

Two new focused TUs plus a small header; the rest stays in `main.c`:

- **`fuzzy.c` / `fuzzy.h`** — the pure matcher. Decoupled + algorithmic → its own
  unit and a headless test.
- **`palette.c` / `palette.h`** — owns a `Palette` struct (query/len/sel/open/
  eat_char) and the open/type/key/draw handlers. Takes `AppState*` **opaquely** (only
  shuttles it to command callbacks) plus the `Command[]` and `Font*`. Well-bounded;
  does not poke engine internals.
- **`command.h`** — the `Command` struct typedef + an `AppState` forward-decl.
- **`main.c`** — keeps `g_commands[]`, the `cmd_*` functions, the dispatch loop, and
  wires `on_key`/`on_char`/draw to the `palette_*` calls. The `cmd_*` bodies stay
  here because they are fused to the minting machinery; `read_input()` net *shrinks*
  (≈25 inline blocks → one loop).

## v1 command scope

Migrate everything discrete **except** the gnarly few, so the palette feels complete
on day one:

**In v1:** toggles — bloom (`K`), day/night (`` ` ``), color grade (`9`), irradiance
(`I`), prefilter inspector (`P`), text inspector (`T`), floor-plan (`J`), shadow-map
inspector (`M`), ghost (`X`), walk/fly (`F`); scene ops — rescan mirrors (`R`), reload
scene (`L`); one-shot mints — whiteboard (`B`), note (`N`), lantern (`O`), pond (`Q`),
dust (`E`), fox (`Y`), island (`H`), church (`U`), codex (`V`).

**Deferred to fast-follow migration:** mint abbey (`Z`, the ~350-line block, highest
extraction risk), gather-workspace (`G`), connection-arm (`C`), contextual `Backspace`.
These keep working by hotkey via their inline blocks until migrated.

## Testing

- **`fuzzy_test`** — new headless ASan/UBSan target (mirrors `stml_test`/`nid_test`):
  subsequence correctness, case-insensitivity, ranking order, empty-query-returns-all,
  no-match, position reporting.
- **Live-verified** (consistent with how overlays/instruments are checked): `:` opens,
  typing filters, arrows/Enter/Esc, every migrated command behaves exactly as its
  hotkey did, hotkeys still fire when closed, no `:` leak, modal mutual-exclusion.
- **No new MSL twin** — palette uses only `ui_quad`/`ui_text`, whose GL+Metal shaders
  already exist. Tax-free on the dual-backend front.

## Build changes

- Add `fuzzy.c` and `palette.c` to the c89check source list and the main link list in
  `build.sh`.
- Add a `fuzzytest` mode to `build.sh` (clang + ASan/UBSan over `fuzzy.c` +
  `fuzzy_test.c`).
- Gauntlet unchanged otherwise: `c89check` + GL build + Metal build (all C changes).

## Future work (enabled, not built)

- Rebindable keys (the registry already separates key from action).
- Recent/frequent command ordering.
- Command categories / sections in the palette.
- A `palette.c` already exists to grow into a richer launcher if desired.
- Migrate the deferred commands (abbey, gather, connection, backspace) into the
  registry.
