#!/bin/sh
set -eu

MODE="${1:-debug}"

# Verify our code stays C89 / Dependable-C compatible. We don't BUILD in c89
# mode (the normal build is c11) — we just check that the code COULD. GLFW/GL
# headers go through -isystem so their own non-C89-isms don't trip -pedantic.
if [ "$MODE" = "c89check" ]; then
    GLFW_CFLAGS=$(pkg-config --cflags glfw3 | sed 's/-I/-isystem /g')
    # Documented dependable exception: C90 only guarantees 509-char string
    # literals, but every real compiler supports far longer. Our GLSL shader
    # sources exceed that, so we allow overlength strings — all other C89
    # constraints stay strict.
    clang -std=c89 -pedantic-errors -Werror -Wall -Wextra -Wno-overlength-strings \
        -fsyntax-only $GLFW_CFLAGS main.c rhi_gl.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c mesh_gpu.c ui.c text.c wtext.c wtcache.c tmcache.c scene.c mirror.c material.c mapmath.c scene_io.c stml.c nid.c sol_math.c camera.c collide.c bvh.c asset.c component.c particles.c synth.c wav.c mixer.c reverb.c skel.c json.c glb.c fuzzy.c browser.c palette.c route.c editor.c descend.c workspace.c furniture.c inventory.c boardpage.c caret.c diskpath.c multiselect.c widget.c app_synth.c
    echo "c89check: PASS — all sources are C89-pedantic clean"
    # Shader twin-lint: both backends bind uniforms BY NAME (GL:
    # glGetUniformLocation; Metal: struct-member reflection + u-named texture
    # args), so a name that exists in one twin but not the other renders wrong
    # SILENTLY — no compile error, no runtime error. Enforce set-symmetry: in
    # every '#ifdef SOL_RHI_METAL / #else / #endif' block, the u-prefixed
    # identifiers (uModel, uAlbedoTex, ...) of the MSL half must equal those
    # of the GLSL half. Names mentioned only in a comment count for their
    # half, so keep twin commentary beside the twin that declares the thing.
    awk '
        /^#ifdef SOL_RHI_METAL/ { half = 1; start = FNR; delete msl; delete glsl; next }
        /^#else/  { if (half == 1) half = 2; next }
        /^#endif/ {
            if (half == 2) {
                bad = ""
                for (t in msl)  if (!(t in glsl)) bad = bad " " t "(MSL-only)"
                for (t in glsl) if (!(t in msl)) bad = bad " " t "(GLSL-only)"
                if (bad != "") {
                    printf "twin-lint: %s:%d asymmetric uniforms:%s\n", FILENAME, start, bad
                    fails++
                }
            }
            half = 0; next
        }
        half > 0 {
            line = $0
            while (match(line, /u[A-Z][A-Za-z0-9_]*/)) {
                pre = (RSTART > 1) ? substr(line, RSTART - 1, 1) : " "
                t   = substr(line, RSTART, RLENGTH)
                if (pre !~ /[A-Za-z0-9_]/) {
                    if (half == 1) msl[t] = 1; else glsl[t] = 1
                }
                line = substr(line, RSTART + RLENGTH)
            }
        }
        END {
            if (fails) { print "twin-lint: FAIL — " fails " shader twin block(s) drifted"; exit 1 }
            print "twin-lint: PASS — all shader twins declare matching uniform names"
        }
    ' shaders.h ui.c
    exit 0
fi

# Build + run the standalone STML parser test under the sanitizers. The parser
# is scene-agnostic and links nothing but libc, so it builds on its own.
if [ "$MODE" = "stmltest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        stml.c stml_test.c \
        -o stml_test
    echo "built ./stml_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the standalone nid generator test under the sanitizers.
if [ "$MODE" = "nidtest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        nid.c nid_test.c \
        -o nid_test
    echo "built ./nid_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the standalone fuzzy-matcher test under the sanitizers.
if [ "$MODE" = "fuzzytest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        fuzzy.c fuzzy_test.c \
        -o fuzzy_test
    echo "built ./fuzzy_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless scene serialization test under the sanitizers. Links
# only scene.c + scene_io.c + nid.c (no GL): it builds empties and saves them.
if [ "$MODE" = "iotest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        scene.c material.c scene_io.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c mirror.c platform_fs.c component.c particles.c nid.c stml.c sol_math.c scene_io_test.c \
        -o scene_io_test
    echo "built ./scene_io_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless scene handle-map test under the sanitizers. Same link
# set as iotest (scene.c + deps, no GL): it churns the graph and checks scene_get.
if [ "$MODE" = "scenetest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        scene.c material.c scene_io.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c mirror.c platform_fs.c component.c particles.c nid.c stml.c sol_math.c scene_test.c \
        -o scene_test
    echo "built ./scene_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the pure note-caret layout test under the sanitizers. caret.c is
# GL-free (libc only), so this links just it + the test.
if [ "$MODE" = "carettest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        caret.c caret_test.c \
        -o caret_test
    echo "built ./caret_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# mapmathtest: the equirect map-math pure module (scene-free C89). libc only.
if [ "$MODE" = "mapmathtest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        mapmath.c mapmath_test.c \
        -o mapmath_test
    echo "built ./mapmath_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# browsertest: the entity-browser pure nav/filter module (scene-free C89). links fuzzy.
if [ "$MODE" = "browsertest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        browser.c browser_test.c fuzzy.c \
        -o browser_test
    echo "built ./browser_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# diskpathtest: the Files-browser pure path module (scene-free C89). libc only.
if [ "$MODE" = "diskpathtest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        diskpath.c diskpath_test.c \
        -o diskpath_test
    echo "built ./diskpath_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the pure world-text glyph cache test under the sanitizers.
# wtcache.c calls no rhi_* and includes only header-declared types, so it links
# with no GL.
if [ "$MODE" = "wtcachetest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        wtcache.c wtcache_test.c \
        -o wtcache_test
    echo "built ./wtcache_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the pure text-measure cache test under the sanitizers.
if [ "$MODE" = "tmcachetest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        tmcache.c tmcache_test.c \
        -o tmcache_test
    echo "built ./tmcache_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless camera-math test under the sanitizers. Links only
# camera.c + sol_math.c (no GLFW/GL): pure state + math.
if [ "$MODE" = "camtest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        camera.c sol_math.c camera_test.c \
        -o camera_test
    echo "built ./camera_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless picking-math test under the sanitizers. 4a links only
# sol_math.c (ray_vs_aabb); 4b will add scene.c + nid.c for scene_pick.
if [ "$MODE" = "picktest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c bvh.c nid.c sol_math.c pick_test.c \
        -o pick_test
    echo "built ./pick_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless collision test under the sanitizers. The math
# core is scene-free; collide_rebuild derives from the scene spine, so the
# test links the pure-CPU half of it (mesh.c has no GL since the gpu split).
if [ "$MODE" = "coltest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        collide.c route.c workspace.c scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c nid.c sol_math.c collide_test.c \
        -o collide_test
    echo "built ./collide_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# route_test: routing math over a headless scene (no GL). Links the scene
# spine + mesh.c (RoomOpening) it derives from.
if [ "$MODE" = "routetest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        route.c workspace.c route_test.c scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c nid.c sol_math.c \
        -o route_test
    echo "built ./route_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# editortest: the top-down editor's geometry + scene ops (no GL). Links the
# scene spine + mesh.c (RoomOpening/params) + camera.c (ortho rays).
if [ "$MODE" = "editortest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        editor.c editor_test.c scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c nid.c sol_math.c camera.c workspace.c \
        -o editor_test
    echo "built ./editor_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# descendtest: fs-tree Phase 4 geometry + scene ops (no GL, no fs). Links the
# scene spine + editor.c (RoomRect) + camera.c.
if [ "$MODE" = "descendtest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        descend.c descend_test.c editor.c scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c nid.c sol_math.c camera.c workspace.c \
        -o descend_test
    echo "built ./descend_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# furnituretest: the furniture geometry module (scene-free). Links mesh.c for
# the mesh-build assertion (Task 2) + sol_math.
if [ "$MODE" = "furnituretest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        furniture.c furniture_test.c mesh.c flora.c rock.c gothic.c sweep.c sol_math.c \
        -o furniture_test
    echo "built ./furniture_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# campustest: the campus heightfield (scene-free). Links mesh.c for
# campus_height/make_campus + sol_math.
if [ "$MODE" = "campustest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        campus_test.c mesh.c flora.c rock.c gothic.c sweep.c sol_math.c \
        -o campus_test
    echo "built ./campus_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# inventorytest: the inventory grid math (scene-free C89). Links nothing but libc.
if [ "$MODE" = "inventorytest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        inventory.c inventory_test.c \
        -o inventory_test
    echo "built ./inventory_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# boardpagetest: the board-page pure logic (scene-free C89). libc only.
if [ "$MODE" = "boardpagetest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        boardpage.c boardpage_test.c \
        -o boardpage_test
    echo "built ./boardpage_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# multiselecttest: the board multi-select pure logic (scene-free C89). libc only.
if [ "$MODE" = "multiselecttest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        multiselect.c multiselect_test.c \
        -o multiselect_test
    echo "built ./multiselect_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# widgettest: the pure immediate-mode widget core (scene-free C89). libc only.
if [ "$MODE" = "widgettest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        widget.c widget_test.c \
        -o widget_test
    echo "built ./widget_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# imagetest: the pure aspect-fit math in image.c (image_fit_box). image.c pulls
# stb, so this rides -std=c11 like skeltest, not c89-pedantic.
if [ "$MODE" = "imagetest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        image.c image_test.c \
        -o image_test
    echo "built ./image_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# workspacetest: the workspace module — membership + portal-pair authoring
# (no GL). Links the scene spine + mesh.c the gates reference.
if [ "$MODE" = "workspacetest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        workspace.c workspace_test.c scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c nid.c sol_math.c \
        -o workspace_test
    echo "built ./workspace_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless asset-registry test under the sanitizers. Pure
# bookkeeping (keys, refcounts, injected destructors) — links libc only.
if [ "$MODE" = "assettest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        asset.c asset_test.c \
        -o asset_test
    echo "built ./asset_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless particle-pool test under the sanitizers. Pure
# arithmetic (the arena, Euler, the fill site, spread math) — libc only.
if [ "$MODE" = "parttest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        particles.c particles_test.c \
        -o particles_test
    echo "built ./particles_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless synthesizer test under the sanitizers, then the
# AUDITION: it exports every preset as a .wav — listen with afplay.
if [ "$MODE" = "synthtest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        synth.c wav.c synth_test.c \
        -o synth_test
    echo "built ./synth_test (ASan + UBSan) — run it; it also exports preset .wavs"
    exit 0
fi

# appsynthtest: the synth book's page layout (scene/GL-free C89). Links the
# widget core + the synth schema it introspects.
if [ "$MODE" = "appsynthtest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        app_synth.c widget.c synth.c app_synth_test.c \
        -o app_synth_test
    echo "built ./app_synth_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless skeletal-animation test under the sanitizers.
# Links glb.c for the skin/clip parsing; the two GPU seams glb.c references
# (texture create, mesh upload) are stubbed inside the test — skeleton
# loading never executes them.
if [ "$MODE" = "skeltest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        skel.c glb.c json.c image.c mesh.c flora.c rock.c gothic.c sweep.c sol_math.c skel_test.c \
        -o skel_test
    echo "built ./skel_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless mixer test under the sanitizers. The SAME pure
# core the real-time callback calls, asserted on output samples.
if [ "$MODE" = "mixtest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        mixer.c reverb.c mixer_test.c \
        -o mixer_test
    echo "built ./mixer_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless JSON parser test under the sanitizers.
if [ "$MODE" = "jsontest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        json.c json_test.c \
        -o json_test
    echo "built ./json_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless branch-graph test under the sanitizers (P7
# item 2, 15th suite). tree_plan is pure arithmetic — libc + sol_math.
if [ "$MODE" = "floratest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        flora.c rock.c mesh.c gothic.c sweep.c sol_math.c flora_test.c \
        -o flora_test
    echo "built ./flora_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless rock test under the sanitizers (P7 item 6,
# 16th suite). Boulders are pure CPU geometry — libc + sol_math.
if [ "$MODE" = "rocktest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        rock.c mesh.c gothic.c sweep.c flora.c sol_math.c rock_test.c \
        -o rock_test
    echo "built ./rock_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless texture-synthesis test under the sanitizers (the
# texture side-quest, 14th suite). Pure pixel arithmetic — libc only.
if [ "$MODE" = "texgentest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        texgen.c texgen_test.c \
        -o texgen_test
    echo "built ./texgen_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# Build + run the headless gothic-kit test under the sanitizers (P6 item 1).
# Links the kit + mesh.c flora.c rock.c (the registry row) — pure CPU, no GL.
if [ "$MODE" = "gothictest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        gothic.c sweep.c mesh.c flora.c rock.c sol_math.c gothic_test.c \
        -o gothic_test
    echo "built ./gothic_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

# The Metal smoke test (P4 item 10 stage a): a cleared window and one
# triangle through the UNCHANGED rhi.h — no GL linked. rhi_metal.m is
# Objective-C (the SIXTH quarantine, ARC) and sits outside c89check
# exactly as platform_audio.c does; the smoke test itself is plain C
# against the seam.
if [ "$MODE" = "metalsmoke" ]; then
    clang -fobjc-arc -g -O0 -Wall -Wextra \
        -c rhi_metal.m $(pkg-config --cflags glfw3) -o rhi_metal.o
    clang -std=c11 -g -O0 -Wall -Wextra \
        -c metal_smoke.c $(pkg-config --cflags glfw3) -o metal_smoke.o
    clang metal_smoke.o rhi_metal.o $(pkg-config --libs glfw3) \
        -framework Metal -framework QuartzCore -framework Cocoa -framework IOKit \
        -o metal_smoke
    rm -f metal_smoke.o rhi_metal.o
    echo "built ./metal_smoke — cleared window + triangle, zero GL (ESC quits)"
    exit 0
fi

# The Metal palace (item 10): the SAME app, rhi_metal.m linked in place of
# rhi_gl.c — rhi.h untouched by the switch (that untouchedness is the
# proof). Stage (a) proves the LINK: it builds with zero GL. It exits at
# init until stage (b) lands the core MSL twin (init_scene rightly treats
# a failed core shader as fatal — that diagnostic protects the GL build
# and we keep it). Stages b-e light the palace up.
if [ "$MODE" = "metal" ]; then
    clang -fobjc-arc -g -O0 -Wall -Wextra \
        -c rhi_metal.m $(pkg-config --cflags glfw3) -o rhi_metal.o
    clang -fobjc-arc -g -O0 -Wall -Wextra \
        -c platform_clipboard.m $(pkg-config --cflags glfw3) -o platform_clipboard.o
    clang -fobjc-arc -g -O0 -Wall -Wextra \
        -c platform_window.m $(pkg-config --cflags glfw3) -o platform_window.o
    clang -std=c11 -g -O0 -Wall -Wextra -DSOL_RHI_METAL \
        main.c rhi_metal.o platform_clipboard.o platform_window.o mesh.c flora.c rock.c gothic.c sweep.c texgen.c mesh_gpu.c ui.c text.c wtext.c wtcache.c tmcache.c scene.c mirror.c material.c mapmath.c scene_io.c nid.c stml.c sol_math.c camera.c collide.c bvh.c asset.c component.c particles.c synth.c wav.c mixer.c reverb.c skel.c platform_audio.c image.c font.c platform_fs.c json.c glb.c fuzzy.c browser.c palette.c route.c editor.c descend.c workspace.c furniture.c inventory.c boardpage.c caret.c diskpath.c multiselect.c widget.c app_synth.c \
        $(pkg-config --cflags --libs glfw3) \
        -framework Metal -framework QuartzCore -framework Cocoa -framework IOKit -framework AudioToolbox \
        -o solarium-metal
    rm -f rhi_metal.o platform_clipboard.o platform_window.o
    echo "built ./solarium-metal (stage a: links clean, zero GL; runs from stage b)"
    exit 0
fi

# Build with AddressSanitizer + UndefinedBehaviorSanitizer for the §1.7
# definition-of-done. Debug-only; ~2x slower. Run ./solarium-asan; the
# sanitizers report any memory/UB error on stderr and abort.
if [ "$MODE" = "asan" ]; then
    clang -fobjc-arc -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
        -Wall -Wextra \
        -c platform_clipboard.m $(pkg-config --cflags glfw3) -o platform_clipboard.o
    clang -fobjc-arc -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
        -Wall -Wextra \
        -c platform_window.m $(pkg-config --cflags glfw3) -o platform_window.o
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        main.c rhi_gl.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c mesh_gpu.c ui.c text.c wtext.c wtcache.c tmcache.c scene.c mirror.c material.c mapmath.c scene_io.c nid.c stml.c sol_math.c camera.c collide.c bvh.c asset.c component.c particles.c synth.c wav.c mixer.c reverb.c skel.c platform_audio.c image.c font.c platform_fs.c json.c glb.c fuzzy.c browser.c palette.c route.c editor.c descend.c workspace.c furniture.c inventory.c boardpage.c caret.c diskpath.c multiselect.c widget.c app_synth.c platform_clipboard.o platform_window.o \
        $(pkg-config --cflags --libs glfw3) \
        -framework OpenGL -framework Cocoa -framework IOKit -framework AudioToolbox \
        -o solarium-asan
    rm -f platform_clipboard.o platform_window.o
    echo "built ./solarium-asan (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

if [ "$MODE" = "release" ]; then
    FLAGS="-O2 -DNDEBUG"
else
    FLAGS="-g -O0"
fi

clang -fobjc-arc $FLAGS -Wall -Wextra \
    -c platform_clipboard.m $(pkg-config --cflags glfw3) -o platform_clipboard.o
clang -fobjc-arc $FLAGS -Wall -Wextra \
    -c platform_window.m $(pkg-config --cflags glfw3) -o platform_window.o
clang -std=c11 $FLAGS -Wall -Wextra \
    main.c rhi_gl.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c mesh_gpu.c ui.c text.c wtext.c wtcache.c tmcache.c scene.c mirror.c material.c mapmath.c scene_io.c nid.c stml.c sol_math.c camera.c collide.c bvh.c asset.c component.c particles.c synth.c wav.c mixer.c reverb.c skel.c platform_audio.c image.c font.c platform_fs.c json.c glb.c fuzzy.c browser.c palette.c route.c editor.c descend.c workspace.c furniture.c inventory.c boardpage.c caret.c diskpath.c multiselect.c widget.c app_synth.c platform_clipboard.o platform_window.o \
    $(pkg-config --cflags --libs glfw3) \
    -framework OpenGL -framework Cocoa -framework IOKit -framework AudioToolbox \
    -o solarium
rm -f platform_clipboard.o platform_window.o

echo "built ./solarium ($MODE)"
