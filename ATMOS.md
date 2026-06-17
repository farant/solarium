# ATMOS.md — the felt world's index

Phase 8 ("The Felt World", TODO8.md) spent the dual-backend trust that the
two pure-CPU phases (Gothic, Flora) had banked: it is the renderer/atmosphere
phase — cast light, moving air, and sound. Like GOTHIC.md and FLORA.md this is
the **index**, not a manual; every table restates something the source already
says, and the source wins.

## The constitution (§1)

- **NO TAA — a constitutional refusal.** The slow pilgrim camera makes crude,
  non-temporal forms acceptable, so nothing reprojects across frames. Shadow
  shimmer is killed *geometrically* (texel-snap), god-rays and SSAO are honest
  per-frame noise the eye forgives. This de-risked every item.
- **One depth buffer, many readers.** Item 2 made the HDR target's depth a
  *sampleable texture*; items 3–5 (god-rays, soft particles, SSAO) all read
  that one resource instead of re-deriving it.
- **The dual-backend tax.** Every visual item is a GL impl + a runtime-compiled
  MSL twin. The trap: `build.sh metal` compiles only C/ObjC — the MSL compiles
  on-device, so a struct/body mismatch passes the build and surfaces at launch.
  Grep struct AND body for every uniform field. **The one exception: item 8
  (audio) — it lives in the audio quarantine, not the RHI seam, so it has no
  twin at all.**
- **GI is the marquee successor phase**, not this one — bake-on-mint global
  illumination is out of scope here.

## The frame, in order

| # | pass | target | reads | notes |
|---|---|---|---|---|
| 0 | sun shadow cascades (×2) | `shadow_rt[0..1]` | scene | directional sun, ortho, fit to camera-frustum slices |
| 0b | spot sconce shadow | `spot_shadow_rt` | scene | one perspective map, only if a `cast=1` light exists |
| 1 | HDR opaque | `hdr_rt` (RGBA16F) | cascades, spot, IBL | skybox + lit scene; **writes the sampleable depth** |
| 1b | depth copy | `depthcopy_rt` | `hdr_rt` depth | so particles can read depth they also draw into |
| 1c | particles | `hdr_rt` (load) | depth copy | additive-in-HDR (spark bloom survives), soft-faded |
| — | water | `hdr_rt` (load) | prefilter IBL | ponds: fresnel reflection, alpha-blended (P7 item 8) |
| 2 | god-rays | `godray_rt` (½) | scene depth, cascades | raymarch the sun's lit fog, near-field capped |
| 3 | SSAO + bilateral blur | `ssao_rt`/`ssao_blur_rt` (½) | scene depth | hemisphere AO, reconstructed normals |
| 4 | bloom chain | `bloom_rt[]` | `hdr_rt` | Karis down / additive up |
| 5 | post / tonemap | screen | hdr·AO + bloom + godray + fog | ACES, sRGB encode |
| 6 | UI overlay | screen (load) | — | HUD, text |

## Light

- **The sun is DIRECTIONAL** (item 6): no position, cone, or inverse-square —
  parallel rays, an orthographic shadow box per cascade. `light_pos`→
  `light_target` encodes only a *direction*. **Cascaded shadow maps**: 2 boxes
  fit to nested slices of the camera frustum (near = tight/crisp, far = island-
  wide). Anti-shimmer is a bounding-**sphere** fit (radius is invariant to
  camera rotation) + a world-origin **texel-snap** (the box jumps in whole
  texels, edges never crawl). The PBR FS and the god-ray march both read the
  cascades through a bounds-test near→far ladder.
- **One sconce CASTS** (item 7): a point light tagged `cast=1` becomes a
  shadow-casting *spot* — the perspective shadow path item 6 retired from the
  sun, re-homed on a candle. Its flame breathes (intensity) and SWAYS
  (position jitter → the pier shadows swing). The other ≤7 point lights stay
  cheap un-cast fills.

## Air

- **Height fog** (item 2): analytic, in linear space pre-tonemap; a *layer*
  (`max(y − H, 0)`) so it never thickens without bound below floating islands.
- **God-rays** (item 3): a half-res raymarch of the sun's shadow volume through
  the same fog field; HG forward-scatter (brightest looking toward the sun),
  capped to a 45 m near field so a long ray into the open doesn't pile a column
  onto the sun-direction hotspot.
- **Soft particles** (item 4): dust fades where it meets a surface (reads the
  depth copy), additive in HDR so sparks bloom.
- **SSAO** (item 5): half-res hemisphere AO, post-multiplied on the lit scene
  only; best-neighbour reconstructed normals + a LINEAR-depth bilateral blur
  (the fix for the grain/band/static-mask saga).

## Sound (the tax-free item, item 8)

- **Reverb** = a Freeverb (8 comb + 4 allpass) in `reverb.c`, one global
  instance the `Mixer` owns, run on the summed bus each callback. The
  *listener's* room picks the preset (kinds-are-presets): a church rings long +
  bright, a small room is short + dark, open air is dry — eased like the wind,
  sent over the SPSC ring as `MIX_CMD_REVERB`.
- **Spatial** = `spatialize()` (windowed inverse-square + constant-power
  auto-pan from the camera's right) — one law for every located source: the
  lantern crackle, and now the nearest pond's water.

## The day/night lever (item 9, reserved decision #7 = swap-only)

The backtick **`` ` ``** toggles `st->night`: `apply_time_of_day()` swaps both
the sky/IBL (`hdr_reload` + full re-bake — `horn-koppe_spring` ↔
`rogland_clear_night`) and the sun (warm bright ↔ dim cool **moonlight**). Not
a day/night *cycle* (that brushes GI, deferred) — just the two curated ends.
`Z` (mint abbey) sets night, so `` ` `` flips you to the daylight counter-shot.

## Keys (atmosphere-relevant)

| key | does |
|---|---|
| `` ` `` | day ↔ night (sky + sun) |
| `M` | shadow-map inspector (cascade 0) |
| `K` | bloom on/off |
| `[` `]` | exposure scrub |
| `Z` | mint the abbey (sets a raking sun + night) |
| `Q` | mint a pond (water source) |

## Budgets (the M2, both backends)

The phase's whole thesis is that the felt world is still **16.7 ms** (vsync).
The HUD yardstick (item 1) reads `frame / cpu / gpu / swap` and
`up / shadow / spot / hdr / post`. On GL, GPU timing is unavailable (macOS GL
stubs the timer queries) — watch CPU + the vsync-pinned `frame`. On Metal,
`gpu` is whole-frame and a spike there while `frame` holds 16.7 is a transient
the triple-buffer absorbed (watch `frame`, not `gpu`). The dressed night abbey
holds interactive on both; each added pass lands in its stated slice.
