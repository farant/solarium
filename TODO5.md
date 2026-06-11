# TODO5.md — Phase 5 prospectus: The App Engine

> **Status: PROSPECTUS, not yet the brief.** Seeded 2026-06-11, mid-Phase-4 (items 8–10
> of TODO4.md still in flight), to capture a direction decided in conversation before it
> cooled. The binding brief — constitution, items, reserved decisions, the full
> treatment — gets written at the phase boundary, the way TODO3 and TODO4 were. Until
> then this document collects the vision and the constraints Phase 4 must honor for it.

---

## The organizing idea: apps live in books

Phase 5 swings back from engine to application — and the proposal (Fran, during the
item-8 audio design) is that it opens by making "application" a *thing the world
contains*. Not HUD panels, not a desktop bolted onto the palace: **a book that, opened,
shows an app** — its panels paginated onto the pages, flipped with the same arrows that
turn any other book's pages. Pick up the synthesizer book and the oscillator page,
envelope page, and patch library are literally pages.

This is approached as a **general-purpose app engine, not a one-off synth UI**. The
synth is the first customer and the forcing function; the deliverable is the framework
any future app rides:

- a **settings book** (exposure, bloom strength, walk speed — the `[ ]`/K knobs get a home)
- a **mirror-browser book** (the archive, navigable as pages instead of a card field)
- a **material/palette editor** (point it at an object, edit its `<mat>` on a page)
- a **sequencer/tracker book** (music's door, deliberately left open by item 8's synth)

Pagination is not a gimmick here: it genuinely solves the problem every dense UI has
(too many controls, too little surface), and it keeps apps *diegetic* — objects in the
palace with positions, owners, and shelf space, like everything else worth keeping.

## What already exists (the inventory)

The reason this is buildable: most of it is standing machinery from Phases 3–4.

- **The reader rig** is already a view host: lift-and-face, spreads, arrow-key page
  flips, per-page content rendering. "Page = panel" is routing, not new machinery.
- **The page surface is flat where it matters** — the fan rises from the gutter pinch to
  the flat text field (BOOK_GUTTER_FRAC), which is where ink already renders and where
  widgets would live. The curl exists only mid-turn, when interaction is off anyway.
- **Ray → surface → local 2D** is solved: the whiteboard converts world hits to board
  rects every frame a card is dragged. Pointing at a page and getting page-local
  coordinates is the same transform.
- **The focus model**: note-editing proved input-changes-meaning-by-mode (keys are
  buttons vs chars are text, the early-return gate). An open app book is one more mode.
- **wtext** draws SDF-crisp labels on surfaces at any scale; **ui.c**'s immediate-mode
  philosophy (re-batch per frame, no retained tree) is exactly the widget substrate.
- **No new RHI**: widgets are quads + text through existing batching — zero Metal debt,
  which is why this can safely follow the item-10 capstone.

## What is genuinely new

1. **A world-space immediate-mode widget layer** — button, slider, checkbox, label (a
   knob if we're feeling baroque), laid out in page-local 2D, hit by the transformed
   pick ray, the classic imgui hot/active interaction state machine. A few hundred
   lines; geometry-agnostic core so the same widgets could later run screen-space.
2. **Cursor-over-page input mode** — while an app book is open, the mouse unlocks as a
   2D pointer over the page (the orbit/edit-mode precedent). Pointing at sliders by
   turning your head would be miserable; a cursor over a page is a desktop app with a
   beautiful projector.
3. **The app routing seam in the reader** — a book carries its app identity (meta, the
   lights pattern); the reader renders panels-per-page instead of file text; arrows
   flip panels. Unknown app ids degrade to an ordinary book (the format's standing rule).
4. **Book-binds-a-file lands here** — the long-deferred dream, and it arrives with a
   twist: apps need documents (the synth's patches), so the first file-bound book is one
   that *writes* its file. Patch library as STML; save-on-blur reflex applies.

## The first app: the synth book

The face of TODO4 item 8's synthesizer (which ships faceless: params in a watchable
text file, sounds minted at load). Pages sketch as:

- **Oscillator / envelope / effects pages** — sliders and toggles over the item-8 param
  schema; every change re-renders the cached buffer (the watcher path, now driven by UI).
- **An audition page** — playable keys, or at minimum a big "sound" button per patch.
- **The mint page** — the sfxr genius: *randomize until it sounds right*. Seeded rolls,
  the codex pattern; a "again" button and a "keep" button may be the whole interface.
- **The patch library** — the bound file, listed as pages; this is where book-binds-a-file
  proves itself.

## What Phase 4 item 8 must leave behind (the contract)

- The synth param schema in a **registry** (the pattern's fourth application) with
  schema introspection — the GUI will enumerate it to build its pages, the way scene_io
  enumerates component schemas. **No GUI-shaped code in Phase 4**, but no hardcoded
  param handling that a GUI couldn't reach either.
- Render-to-buffer as a callable, re-render-on-param-change as a cheap operation
  (the watcher already requires both).
- The WAV writer (export is also the audition fallback).

## Also on Phase 5's slate (boundary-brainstorm material, NOT commitments)

Carried from TODO4 Part 5 and older deferrals, to be weighed when the real brief is
written: the **gothic kit** (Tiny-Glade-ish modular pieces; single-ref multi-part
assemblies; the follies vision — the long-stated heart of the project), **HarfBuzz +
SheenBidi** complex scripts at the text_shape seam (lapide.org wants Hebrew/Arabic/
Indic), per-room files, wayfinding, soft/sorted particles, omni shadows, the scripting
VM question. The app engine does not automatically outrank these — the boundary
brainstorm decides the order; this prospectus only makes sure the app engine arrives
at that conversation fully formed.
