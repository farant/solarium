Low-risk — "a matter of doing the work"
These have textbook references, known failure modes, and bounded scope. The engineering risk is near zero; you get them right by following the literature carefully. Sorted roughly by creative payoff for your content:

Volumetric light (raymarched, non-temporal) — god-rays through the clerestory; your single highest-payoff item and it doesn't depend on baking. Caveat: the temporally-stable version is hard; the honest crude version is not — stay on the crude side.
Height / exponential fog + aerial perspective — makes the floating island read as floating in air with real scale.
Shadowed point/spot lights (cube + 2D shadow maps) — candlelight that actually casts; the Doom 3 / Amnesia night-abbey look.
SSAO / GTAO — dynamic contact occlusion where pier meets floor, trunk meets terrain.
MSAA / FXAA — the non-temporal anti-aliasing for your thin tracery and twigs.
Translucency via sorted alpha with known ordering — stained glass pouring color. (The easy version; see OIT in the hard tier for why you avoid the other one.)
Reflection probes — local cubemap reflections per room; you already bake cubemaps for IBL.
Planar reflection (single plane) — the pond mirroring the yews.
Color grading / LUT, vignette — the art-direction layer that does disproportionate work (the Witness/Eastshade lesson).
Cascaded shadow maps — tight sun shadows across a whole island.
Decals — ruin stains, moss, water streaks, grounding shadows under dynamic objects.
Soft particles — dust motes that fade against geometry instead of slicing through it.
Terrain material splatting / triplanar — grass/rock/dirt blended by slope and height.
Spatial audio (distance attenuation + auto-pan from a listener) — a located fountain or bell.
Reverb sends per room — cathedral acoustics, keyed off your existing room/KIND data.
In-world transform gizmos + object inspector — clean given your handle/nid identity and overlay doctrine.
Trigger volumes — spatial event zones.
Cloth via Position-Based Dynamics — banners, tapestries, vestments; a small bounded solver, not the rigid-body swamp.
Two-bone IK (foot planting) — feet meeting your uneven terrain.
Animation crossfade + small state machine — kills the pop between clips.
Basic navmesh + A* — when the creatures phase arrives.

Higher-risk — paired with the decisions that de-risk them
The pattern across this whole tier: the difficulty is almost always temporal (reprojecting last frame's data across motion and disocclusion), and your architecture's two superpowers — determinism and discrete/infrequent edits — let you sidestep the temporal version in most cases.

GI / bounced light — the highest-value item, and the one your architecture transforms most.


What makes it hard: player authorship kills static baking — you can't precompute against a building that doesn't exist until runtime, which normally forces you into research-grade fully-dynamic GI.
Decisions that rescue it: (a) determinism + the plan functions → bake once per param-hash and cache, so a given church-at-given-ruin only ever bakes once ever; (b) edits are discrete events, not per-frame → re-bake on mint, riding the IBL-rebake seam you already fire on the U/Z keys, not continuously; (c) the scene hierarchy → re-bake one room/island, never the world; (d) the static/dynamic split → bake the room's light field into the empty air as SH irradiance probes, and let trees/fox/characters sample it rather than baking them; (e) rhi_update_texture (your hot-reload primitive) → swap the new lightmap/probe set in live; (f) a job thread → bake progressively while the world stays interactive and the light visibly "resolves." Net: GI moves from "research swamp" to "incremental bake-on-mint," landing sub-second for local edits, 1–3s for a big novel mint on a worker, instant from cache for anything seen before.


TAA — the sleeper; the single most finicky item on the entire list.


What makes it hard: history reprojection, disocclusion, neighborhood clamping; ghosting on thin/animated geometry — your tracery and twigs are the literal worst case; no clean reference that "just works."
Decision that rescues it: don't build it. Your slow, deliberate pilgrim's-eye camera is where TAA's payoff is smallest and its artifacts most visible. MSAA (low tier) covers AA; declining TAA also removes the dependency that makes the next three items hard.


Volumetrics, the temporally-stable version — split across both tiers.


What makes it hard: stability and proper scattering usually lean on TAA, inheriting its problems.
Decision that rescues it: take the crude non-temporal raymarch (low tier) and lean on your slow camera, which forgives the noise a fast camera wouldn't. You get ~90% of the look with none of the TAA dependency.


SSR — deceptively hard, and low value for you.


What makes it hard: the march is easy; quality (thin-feature misses, edge fade, rough reflections, temporal stability) is where good SSR lives, and it usually wants TAA.
Decision that rescues it: substitute reflection probes + the single planar pond (both low tier). You have few reflective surfaces; the hard general solution buys almost nothing.


OIT (order-independent transparency) —


What makes it hard: per-pixel linked lists / MBOIT are fussy, memory-heavy, never perfectly robust.
Decision that rescues it: sorted alpha with known ordering (low tier). Stained glass is placed, parametric, and mostly planar — you can derive its draw order from the plan instead of solving the general per-pixel case.


Clustered / tiled forward (to lift the 8-light cap) — the honest middle: a real systems step, not research.


What makes it hard: the froxel grid, light culling, GPU data layout; subtle bugs read as slow or flickery.
Decisions that rescue it: not eliminable, but bounded by your content's light budget (a handful of sconces per room, not thousands) and by the hierarchy/visibility culling you already run — you may simply never need it, in which case the de-risking decision is "measure first, and don't build it until a real scene blows the cap."
