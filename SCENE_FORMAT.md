# Solarium Scene Format

The scene persists as **STML** (Structured Text Markup Language — the project's
own tag markup; full language spec at `../stml-parser/STML_SPEC.md`). That spec
is a whole language + query engine; this document defines the **small structural
subset** our C89 engine actually parses (the "STML data profile") and the
**scene schema** (the tag/attribute vocabulary) built on it.

Two design rules sit behind everything here:
- **Human-readable, entity-graph-shaped, text** (TODO2.md §1.4/item 2). Each
  object is an addressable record with stable identity, transform, references,
  and metadata — not a binary dump, not array-index identity.
- **The file stores *arrangement and references*, never geometry.** A mesh is a
  reference (`<mesh ref="box"/>`) that is regenerated/loaded, so scene files stay
  small and durable.

---

## 1. The STML data profile (what the C89 parser supports)

A generic, reusable parser (`stml.c` / `stml.h`) produces a DOM tree
(`StmlNode`: tag, attributes, children, optional text). The scene loader is its
first client; the future document/content model (item 5 / Part 5) is the second.

**Supported syntax:**

| Construct | Form | Notes |
|---|---|---|
| Element | `<tag ...> ... </>` | children between open and `</>` |
| Self-closing element | `<tag ... />` | leaf, no children |
| Auto-close | `</>` | closes the nearest open tag (no name needed); a named `</tag>` is *accepted* and validated against the stack |
| Attribute | `name="value"` or `name='value'` | either quote; backslash escapes `\\` `\"` `\'` are **always** decoded inside quoted values (an escape that is only sometimes active cannot round-trip); any other escape is a parse error |
| Boolean attribute | `name` | no `=` → value is `true` |
| Text capture | `<tag ... (>text` | the element's text content is everything up to the **next tag**, trimmed/dedented; the element self-closes. For free-text values (titles, notes) — sidesteps attribute escaping, since STML has **no character entities**. |
| Raw line capture | `<tag! ... (>text` | the rest of the current line, **verbatim** — no trim, no dedent, `<` is a plain byte; the newline is the terminator (not content) and the element self-closes |
| Raw block | `<tag! ...>text</tag>` | everything between `>` and the first `</` is text, **byte-for-byte** (newlines, indentation, `<`); the close is validated as usual. The one string raw text cannot contain is its own terminator `</` (every verbatim scheme has exactly this blind spot — CDATA's is `]]>`) |
| Comment | `<!-- ... -->` | skipped |

**`&` is never special.** Not in attributes, not in content, not in raw text —
it is a literal byte everywhere. The `&…;` sigil is *reserved* for a future
STML feature (`&some name here;` referencing a deduplicated singleton
`<entity>` — a semantic wikilink, not XML character substitution), and that
resolution will apply only in normal content, never raw. No writer or reader
in this codebase may escape, encode, or rewrite `&`.

**Explicitly NOT supported (live in the TS tooling, not our C parser):**
N-sibling / backward / sandwich capture operators, transclusion (`<<...>>`),
CSS selectors, the fluent manipulation API, UUID/NanoID auto-generation on
format. Unknown tags and attributes are **preserved in the DOM and ignored by
the loader** — this is how the format stays forward-compatible (a future field
is a new attribute that old code skips).

---

## 2. Identity

The runtime/persistent split (the Unity-style "asset GUID + runtime handle"
pattern):

- **Persistent identity** (in the file): a **ULID-style `nid`** — a
  timestamp + random ID (like UUIDv7, but a compact base32 string). It is
  globally unique (mergeable across files/devices, no central counter),
  lexicographically **time-sortable** (creation order falls out for free), and
  collision-resistant: the timestamp carries most of the uniqueness, so a weak
  C89 `rand()` for the random bits is acceptable.
- **Runtime identity** (in memory): a `sol_u32` index/handle for O(1) lookup,
  **mapped from the `nid` on load** and reassigned fresh each session.

The C89 generator (`nid.c`) emits 26 Crockford-Base32 chars in three fixed-width
fields, so plain `strcmp` sorts chronologically:

```
TTTTTTTTTT CCCCCC RRRRRRRRRR
 timestamp counter  random
   (10)      (6)     (10)
```

- **timestamp** — 32-bit `time()` seconds, big-endian, zero-padded (seconds, not
  ms — see the caveat below).
- **counter** — a per-run monotonic tie-breaker, so ids minted in the *same*
  second are still strictly ordered and never collide within a run.
- **random** — `rand()` bits for cross-process/device collision resistance.

Consequences:
- **No next-ID counter is persisted.** (ULIDs are self-unique; the brief's
  counter requirement applied to monotonic integer IDs.)
- **References are stored as `nid`s** (`parent`, `rel target`) and resolved
  lazily via `scene_get` — no pointer fixup, no two-pass load.
- C89 caveat: millisecond timestamps aren't portable C89 (`time()` is seconds).
  Second resolution is plenty at human authoring rates; sub-second can be sourced
  from the platform layer later.

---

## 3. Scene schema

```
<scene version="1">
  <object nid="..." parent="..." kind="file">
    <pos x="" y="" z="" />
    <rot x="" y="" z="" w="" />          quaternion
    <scale x="" y="" z="" />
    <mesh ref="room" w="12" d="10" h="3.5" />   geometry by reference + params
    <mat r="" g="" b="" metal="" rough="" />    scalar PBR factors (P3 6e)
    <meta key="..." (>free text value     repeatable; string -> string
    <rel type="..." target="nid" />       repeatable; typed edge to another object
    <content path="..." />                FILE/ALIAS: the real path; else doc/note
  </object>
  ...
</scene>
```

**`kind`** (P3 item 6) — the object's semantic type: `file`, `folder`, `alias`,
`note`, `tombstone`. Absent = a plain prop (old files unchanged); unknown
values degrade to plain. `content` carries the real path for
file/folder/alias.

**Parametric mesh refs** (P3 item 5) — the ref's parameters are named
attributes; the names and defaults come from the mesh registry's SCHEMA
(mesh.c), so the writer, loader, and resolver read one table. Absent params
take defaults (a schema can GROW without breaking old files); the loader
stores only the file's own attribute prefix, keeping re-saves byte-identical.
Current vocabulary: `box`, `grid`, `page` (no params), `room` (w,d,h + wall/
ceiling presence flags wn,we,ws,ww,ceil), `wall` (w,h,ox,ow,oh,t), `path`
(len,w,t), `card` (w,h,t).

**`<mat>`** (P3 6e) — scalar factors only, written when they differ from the
default; texture handles are runtime (glb parts re-derive theirs on
re-import, the page image rebinds by name at load).

**Derived objects are never written** (P3 6e): a glb import's part children
(meta `derived=1`) regenerate from the anchor's `glb` meta on every load —
the file stores arrangement and references, never geometry, including
imports.

**Meta vocabulary in use** (all optional, all plain string->string):
- `name` — display label (the selection pin; the prop-binding key)
- `room_type` (`room` | `mirror` | `workspace`) + `source_path` — room
  anchors (§1.10); a mirror reconciles membership against its directory
- `ambient` — the room's interior light scale (overrides the seal-derived
  default)
- `unplaced` (`1`/`0`) — tray state: set on arrival, cleared by the first drag
- `stale` (`1`/`0`) — an alias whose target path vanished (flagged, never
  removed)
- `glb` — an import anchor's source file; `derived` — its regenerated parts

- **`<scene version>`** — format version, for future migration.
- **`<object>`** — `nid` (required, persistent ID), `parent` (a `nid`; absent or
  empty = root), `labels` (optional space-separated classification, `::`
  namespaces allowed).
- **Transform** — `pos`/`scale` are vec3, `rot` is a quaternion `(x,y,z,w)`.
  Composed at render time as `M = T · R · S`. Stored as TRS (not a baked matrix)
  so authoring tools can edit components and item 8 can build a correct normal
  matrix from real scale.
- **`<mesh>`** — geometry by reference + schema-named parameters; always
  reconstructed, never serialized. **`<mat>`** — scalar factors only.
- **Overbuilt slots** (`<meta>`, `<rel>`, `<content>`) — present in the model and
  serialized though mostly empty this phase; they cannot be retrofitted onto a
  render-only scene (TODO2.md §1.4/§1.5).

---

## 4. Worked example

```
<scene version="1">
  <object nid="01HZ3K9QF0R2 shelf placeholder" parent="" labels="furniture::shelf">
    <pos x="0.0" y="0.0" z="-2.0" />
    <rot x="0.0" y="0.0" z="0.0" w="1.0" />
    <scale x="1.0" y="1.0" z="1.0" />
    <mesh ref="box" />
  </object>

  <object nid="01HZ3K9QG7 book placeholder" parent="01HZ3K9QF0R2..." labels="readable">
    <pos x="0.2" y="1.1" z="-2.0" />
    <rot x="0.0" y="0.0" z="0.0" w="1.0" />
    <scale x="0.6" y="0.9" z="0.1" />
    <mesh ref="box" />
    <meta key="title" (>Plato's Republic
    <rel type="shelved_on" target="01HZ3K9QF0R2..." />
    <content path="notes/republic.txt" />
  </object>
</scene>
```

(`nid`s above are illustrative; real ones are 26-char base32 ULIDs.)

---

## 5. Serialization rules

- Floats are written with `%.9g` so `pos`/`rot`/`scale` round-trip exactly.
- Save → load → save must be **byte-identical** (stronger than semantic: it
  proves nothing was dropped, reordered, or mangled). This works because the
  writer's form selection is a *pure function of the value* — the same string
  always serializes the same way.
- The loader tolerates unknown tags/attributes (forward compatibility) and a
  missing optional element (defaults: identity transform, no parent, no slots).
- Geometry is never in the file; `mesh ref` names a generator/asset.

**The writer is a form selector**, not an escaper: per value it picks the
cheapest representation that round-trips, and the DOM always holds the
*logical* string (encode/decode only at the file boundary).

Attribute values — quote selection first, escaping last:

| Value contains | Written as |
|---|---|
| no `"` | `"value"` |
| `"` but no `'` | `'value'` |
| both quotes | `"value"` with `\"` |
| a literal `\` (any case) | always doubled to `\\` |

Meta text values — the capture/raw ladder:

| Value | Form |
|---|---|
| single line, no `<`, no edge whitespace (trim/dedent is identity) | `<meta key="k" (>value` |
| any other single-line value | `<meta! key="k" (>value` (raw line, verbatim) |
| multiline | `<meta! key="k">value</meta>` — written **tight**: inside raw every byte is content, so no pretty-printing between the delimiters |
| multiline containing `</` | **unrepresentable** — `scene_save` fails loudly and removes the partial file rather than write a lying one |
