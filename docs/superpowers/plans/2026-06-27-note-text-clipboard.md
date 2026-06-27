# Text Cut/Copy/Paste for Notes (System Clipboard) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cmd+C / Cmd+X / Cmd+V on a note's selected text via the system clipboard (GLFW).

**Architecture:** Three small `main.c` helpers (`note_insert_text` — string insert that reuses `selection_delete` + the `on_char` insert pattern; `note_clip_copy`/`note_clip_paste` — GLFW clipboard) plus three Cmd-gated branches in `on_key`'s edit block. No new platform code (GLFW does text clipboard); `text_shape`/`text_wrap`/the board card clipboard are untouched.

**Tech Stack:** C89 (`main.c` only); `glfwGetClipboardString`/`glfwSetClipboardString`. Spec: `docs/superpowers/specs/2026-06-27-note-text-clipboard-design.md`.

**House rules:** strict C89 (declarations at block top, no `//`, no mid-block decls; `-std=c89 -pedantic-errors -Werror -Wextra`). NEVER `git add NOTES.stml`/`paper-picture.png`. Commit body ends with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Feature branch off `main`.

**Verified anchors:** `on_char` (main.c:16754; param `w`; note path after `if (st->edit_handle == 0) return;` at 16765). `selection_delete` (16618), `edit_has_sel`/`edit_sel_lo`/`edit_sel_hi`, `caret_refresh_goal` (16601), `note_autosize`. The `on_key` edit block `if (st->edit_handle != 0) {` (16873; params `window`/`key`/`action`/`mods`; `st` already bound), its `if/else if` chain ends with the Up/Down branch closing `}` (16969) then `return; /* everything else stays quiet */` (16970). `EDIT_BUF_CAP` 2048.

---

## Task 1: Text clipboard for notes

**Files:** Modify `main.c`.

- [ ] **Step 1: Guard `on_char` against Cmd+letter**

In `on_char` (main.c:16754), the note path begins:
```c
    if (st->edit_handle == 0) return;
    n = utf8_encode(cp, enc);
    if (n <= 0) return;
```
Insert a Super-key guard right after `if (st->edit_handle == 0) return;`:
```c
    if (st->edit_handle == 0) return;
    if (glfwGetKey(w, GLFW_KEY_LEFT_SUPER)  == GLFW_PRESS ||
        glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS) return;  /* Cmd+key is a command, not text */
    n = utf8_encode(cp, enc);
    if (n <= 0) return;
```
(Belt-and-suspenders: on macOS GLFW a Cmd+letter combo doesn't emit a char anyway, but this makes a stray pasted letter impossible.)

- [ ] **Step 2: Add the three helpers after `on_char`**

`on_char` ends at main.c:16781 with `caret_refresh_goal(st);\n}`. Immediately after its closing `}`, add:
```c
/* Insert a UTF-8 string at the caret, replacing any selection. Truncates to the
   buffer's remaining room on a codepoint boundary. Mirrors + autosizes like on_char. */
static void note_insert_text(AppState *st, const char *s, int len) {
    int room;
    selection_delete(st);                                   /* replace a selection */
    room = EDIT_BUF_CAP - 1 - st->edit_len;
    if (len > room) len = room;
    while (len > 0 && ((unsigned char)s[len] & 0xC0u) == 0x80u) len--;  /* don't split a codepoint */
    if (len <= 0) return;
    memmove(st->edit_buf + st->edit_cursor + len,
            st->edit_buf + st->edit_cursor,
            (size_t)(st->edit_len - st->edit_cursor));
    memcpy(st->edit_buf + st->edit_cursor, s, (size_t)len);
    st->edit_len    += len;
    st->edit_cursor += len;
    st->edit_sel_anchor = st->edit_cursor;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
    note_autosize(st, st->edit_handle);
    caret_refresh_goal(st);
}

/* Copy the selection to the system clipboard. Returns 1 if there was a selection. */
static int note_clip_copy(AppState *st, GLFWwindow *w) {
    int  lo, hi, k;
    char cb[EDIT_BUF_CAP];
    if (!edit_has_sel(st)) return 0;
    lo = edit_sel_lo(st);
    hi = edit_sel_hi(st);
    k  = hi - lo;
    if (k > EDIT_BUF_CAP - 1) k = EDIT_BUF_CAP - 1;
    memcpy(cb, st->edit_buf + lo, (size_t)k);
    cb[k] = '\0';
    glfwSetClipboardString(w, cb);
    return 1;
}

/* Paste the system clipboard's text at the caret (replacing a selection), stripping '\r'. */
static void note_clip_paste(AppState *st, GLFWwindow *w) {
    const char *cb = glfwGetClipboardString(w);
    const char *p;
    char        s[EDIT_BUF_CAP];
    int         n = 0;
    if (!cb || !cb[0]) return;
    for (p = cb; *p != '\0' && n < EDIT_BUF_CAP - 1; p++)
        if (*p != '\r') s[n++] = *p;     /* drop carriage returns: \r\n -> \n */
    s[n] = '\0';
    if (n > 0) note_insert_text(st, s, n);
}
```
(All three are defined BEFORE `on_key` [16873], which calls them — no forward decls needed. They use `selection_delete`/`edit_has_sel`/`edit_sel_lo`/`edit_sel_hi`, all already defined above.)

- [ ] **Step 3: Add the Cmd+C/X/V branches to the `on_key` edit block**

In the `on_key` edit block, the `if/else if` chain ends with the Up/Down branch (main.c:16959-16969) then `return;`. The tail currently is:
```c
                    if (!shift) st->edit_sel_anchor = st->edit_cursor;
                }
            }
        }
        return;                                 /* everything else stays quiet */
```
Insert the three Cmd branches between that final `}` (closing the Up/Down `else if`) and `return;`:
```c
                    if (!shift) st->edit_sel_anchor = st->edit_cursor;
                }
            }
        } else if ((mods & GLFW_MOD_SUPER) && key == GLFW_KEY_C && action == GLFW_PRESS) {
            note_clip_copy(st, window);                       /* copy selection (no-op if none) */
        } else if ((mods & GLFW_MOD_SUPER) && key == GLFW_KEY_X && action == GLFW_PRESS) {
            if (note_clip_copy(st, window)) selection_delete(st);   /* cut = copy + delete */
        } else if ((mods & GLFW_MOD_SUPER) && key == GLFW_KEY_V && action == GLFW_PRESS) {
            note_clip_paste(st, window);                      /* paste at the caret */
        }
        return;                                 /* everything else stays quiet */
```
(They're gated on `GLFW_MOD_SUPER`, so a plain `c`/`x`/`v` keystroke matches no branch, falls to `return`, and `on_char` types the letter as today. `PRESS`-only so holding doesn't repeat. `window` is `on_key`'s param.)

- [ ] **Step 4: Build gauntlet**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test
```
All five must pass (carettest unchanged — no caret.c edit). Fix any C89/-Werror/brace issue and re-run until green. Do NOT run `./solarium` (no display).

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Note clipboard: Cmd+C/X/V on selected text via the system clipboard

Copy/cut the selection to (and paste from) the GLFW system clipboard
while editing a note. note_insert_text generalizes on_char's insert to a
UTF-8 string (replaces a selection, truncates to fit on a codepoint
boundary); paste strips '\r'. Handled in on_key's edit block, gated by
edit_handle + GLFW_MOD_SUPER; on_char ignores input while Cmd is held.
The board CARD Cmd+X/V (suppressed while editing) is untouched. No new
platform code (GLFW does text), no shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review notes (for the implementer)

- **Spec coverage:** Step 1 = the Cmd+letter guard (spec §3). Step 2 = `note_insert_text`/`note_clip_copy`/`note_clip_paste` (spec arch §1-2: GLFW clipboard, UTF-8 truncate-on-boundary, `\r` strip, bounded scratch). Step 3 = the Cmd+C/X/V branches (spec §2, PRESS-only, SUPER-gated). Non-goals respected (notes only, no copy-line, card/image clipboard untouched).
- **Memory safety:** `note_insert_text` runs `selection_delete` first (frees room), then `room = CAP-1-edit_len ≥ 0`; `len` clamped to `room` then backed off a UTF-8 continuation byte; `s[len]` is read within the NUL-terminated `s` (len ≤ n ≤ CAP-1). `note_clip_copy`'s `k = hi-lo ≤ edit_len ≤ CAP-1`; `cb[CAP]` holds it + NUL. `note_clip_paste` bounds the copy to `CAP-1`. No overruns.
- **Cursor stays codepoint-aligned:** insert advances by whole `len` from a boundary; `selection_delete` collapses to a boundary; copy reads `[lo,hi)` (both boundaries from selection ops). `selection_delete`'s memmove never splits a codepoint.
- **C89:** all decls at block top (`int room;` / `int lo,hi,k; char cb[];` / `const char *cb,*p; char s[]; int n;`); no `//`; the for-loop body is a single statement.
- **Human live-verify (after build, both backends):** select → Cmd+C → paste into another app + back into the note; Cmd+X removes + clipboard holds it; Cmd+V replaces a selection / inserts at caret; multi-line + `\r\n` paste; over-long paste truncates cleanly (no split glyph); a plain `c`/`x`/`v` still types; Cmd+letter doesn't double-insert; **regression** — board CARD Cmd+X/V + image paste still work when NOT editing.
