# Text Cut/Copy/Paste for Notes (System Clipboard) — Design Spec

**Date:** 2026-06-27
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Cmd+C / Cmd+X / Cmd+V on a note's selected text, using the **system clipboard** (so you can
copy/paste between notes and other apps). The deferred clipboard piece from [[note-selection]].

## Decisions (from brainstorming)

- **System clipboard via GLFW** (`glfwGetClipboardString` / `glfwSetClipboardString`) — UTF-8,
  cross-platform, already available; **no new platform code** (`platform_clipboard.m` is image-only
  and stays untouched).
- **Handled in `on_key`'s edit block** (event-driven, has `window` + `mods`), gated by
  `edit_handle != 0` and `mods & GLFW_MOD_SUPER`. The board **card** Cmd+X/V (polled in `read_input`)
  is already suppressed while editing — the modal gate `return`s at main.c:10891 before that polling —
  so the two never collide.
- **Notes only** (reader-book editing stays deferred). No rich text. **No copy-the-line on an empty
  selection** (Cmd+C/X with no selection = no-op, standard). Paste replaces a selection, inserts at the
  caret, preserves UTF-8 + newlines, and truncates to fit `EDIT_BUF_CAP` on a codepoint boundary.

## Non-Goals

- No reader-book / codex clipboard; no cross-note multi-select clipboard.
- No internal text buffer — the OS clipboard is the source of truth (so cross-app paste works).
- No copy/cut of the current line on an empty selection; no formatting/rich text.
- No change to the board **card** cut/paste (`cmd_cut_selection`/`cmd_paste_cut`) or the
  image paste (`cmd_paste_image`) — they keep working when NOT editing a note.

## Background (current state — verified)

- A note edits via `edit_handle`/`edit_buf`/`edit_len`/`edit_cursor`/`edit_sel_anchor`. Selection
  helpers: `edit_sel_lo`/`edit_sel_hi`/`edit_has_sel`; `selection_delete` removes `[lo,hi)` and
  collapses (mirrors to `meta["text"]` + `note_autosize` + `caret_refresh_goal`).
- `on_char` inserts a single codepoint at `edit_cursor` (`selection_delete` first, then memmove tail +
  memcpy, advance cursor, `anchor = cursor`, mirror + autosize + refresh). The clipboard paste is the
  same operation generalized to a multi-byte string.
- The `on_key` edit block `if (st->edit_handle != 0) {` (main.c ~16775) handles Esc/Backspace/Delete/
  Enter/Left/Right/Up/Down then `return`. It has `window`, `key`, `action`, `mods`.
- `platform_clipboard.m` exposes only `clipboard_read_image` (images). GLFW gives text.
- `EDIT_BUF_CAP` = 2048.

## Architecture

### 1. `note_insert_text(AppState *st, const char *s, int len)` (main.c)

Generalizes `on_char`'s insert to an arbitrary UTF-8 string (paste reuses it; keeps the editing
invariants in one place):
```
selection_delete(st);                                   /* replace a selection */
room = EDIT_BUF_CAP - 1 - edit_len;                     /* bytes that fit */
if (len > room) len = room;                             /* truncate to fit ... */
while (len > 0 && (s[len] is a UTF-8 continuation byte 0x80..0xBF)) len--;  /* ...on a codepoint boundary */
if (len <= 0) return;
memmove(edit_buf + cursor + len, edit_buf + cursor, edit_len - cursor);
memcpy(edit_buf + cursor, s, len);
edit_len += len; edit_cursor += len; edit_sel_anchor = edit_cursor;
edit_buf[edit_len] = '\0';
scene_meta_set(text) + note_autosize + caret_refresh_goal;
```
(The continuation-byte back-off checks the byte AT the cut point `s[len]`: if it's a continuation byte
the truncation landed mid-codepoint, so shrink until `s[len]` is a lead byte or `len==0`.)

### 2. Copy / Cut / Paste in the `on_key` edit block

Add three `Cmd`-gated branches to the edit `if/else if` chain (each `(mods & GLFW_MOD_SUPER) &&
action == GLFW_PRESS`; distinct keys C/X/V, so they slot in cleanly and never shadow Esc/Backspace/etc.):

- **Cmd+C (copy):** if `edit_has_sel`, copy `edit_buf[lo..hi)` into a NUL-terminated scratch buffer
  (`char cb[EDIT_BUF_CAP]`), `glfwSetClipboardString(window, cb)`. No selection → nothing.
- **Cmd+X (cut):** same copy, then `selection_delete(st)`. No selection → nothing.
- **Cmd+V (paste):** `const char *cb = glfwGetClipboardString(window);` if `cb && cb[0]`, copy `cb`
  into a bounded scratch buffer (`char scratch[EDIT_BUF_CAP]`, stop at `EDIT_BUF_CAP-1` bytes)
  **stripping `'\r'`** (so `\r\n` → `\n`, no stray carriage returns), NUL-terminate, then
  `note_insert_text(st, scratch, <bytes written>)`. (`note_insert_text` truncates again to the room
  actually left in `edit_buf`, on a codepoint boundary.)

These are `PRESS`-only (not `REPEAT`), so holding the combo doesn't repeat.

### 3. Plain letters vs Cmd combos

`Cmd+C/X/V` are gated by `GLFW_MOD_SUPER`, so a plain `c`/`x`/`v` keystroke matches no edit-block
branch and falls through to `on_char` (which inserts the letter) as today. On macOS GLFW, a `Cmd+letter`
combo does NOT emit a char event, so `on_char` won't also insert the letter — **verify this in
live-verify**; if a stray letter appears, guard `on_char` to ignore input while `GLFW_MOD_SUPER`/the
Super key is held.

## Data Flow

```
Cmd+C (editing, has sel) -> copy edit_buf[lo,hi) -> glfwSetClipboardString
Cmd+X (editing, has sel) -> copy -> selection_delete
Cmd+V (editing) -> glfwGetClipboardString -> strip '\r' -> note_insert_text (replaces selection)
NOT editing: Cmd+X/V still -> board CARD cut/paste + image paste (unchanged, suppressed-while-editing)
```

## File Touch List

- **`main.c`**: `note_insert_text` helper; the three `Cmd+C/X/V` branches in the `on_key` edit block.
- No `platform_clipboard.*` change (GLFW handles text). No `build.sh` change.

## Testing

- **Build gauntlet**: `c89check` / GL / Metal (+ `carettest` stays green — no caret.c change).
- **No new unit test**: the clipboard + GLFW are inherently GUI; `note_insert_text`'s buffer math is
  tightly coupled to AppState. Covered by live-verify (and it reuses the already-tested
  `selection_delete` + the on_char insert pattern).
- **Human live-verify** (both backends):
  - Select text → Cmd+C → paste into another app (clipboard has it); Cmd+V in the note inserts at the
    caret / replaces a selection.
  - Cmd+X removes the selection AND the clipboard holds it; paste it back.
  - Multi-line clipboard text pastes with newlines; paste from another app (incl. `\r\n` → `\n`).
  - A very long paste truncates to fit without splitting a multibyte glyph; a plain `c`/`x`/`v`
    keystroke still types the letter (no Cmd → on_char); Cmd+letter doesn't double-insert.
  - **Regression:** board CARD Cmd+X/V and image paste still work when NOT editing a note; caret /
    selection / autosize unaffected.

## Risks

- **Cmd+letter also firing `on_char`** (a stray pasted letter): unlikely on macOS GLFW (Cmd-combos
  aren't text), but flagged for live-verify with a guard fallback.
- **UTF-8 truncation** splitting a codepoint on an over-long paste: handled by the continuation-byte
  back-off in `note_insert_text`.
- **`glfwGetClipboardString` returns NULL** (no text / non-text clipboard): the `cb && cb[0]` guard
  makes paste a no-op.
- No change to `text_shape`/`text_wrap`/the card clipboard; no new shader.
