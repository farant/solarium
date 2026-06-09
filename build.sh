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
        -fsyntax-only $GLFW_CFLAGS main.c rhi_gl.c mesh.c ui.c scene.c material.c scene_io.c stml.c nid.c sol_math.c camera.c json.c glb.c
    echo "c89check: PASS — all sources are C89-pedantic clean"
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

# Build + run the headless scene serialization test under the sanitizers. Links
# only scene.c + scene_io.c + nid.c (no GL): it builds empties and saves them.
if [ "$MODE" = "iotest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        scene.c material.c scene_io.c nid.c stml.c sol_math.c scene_io_test.c \
        -o scene_io_test
    echo "built ./scene_io_test (ASan + UBSan) — run it; sanitizers report on stderr"
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
        scene.c material.c nid.c sol_math.c pick_test.c \
        -o pick_test
    echo "built ./pick_test (ASan + UBSan) — run it; sanitizers report on stderr"
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

# Build with AddressSanitizer + UndefinedBehaviorSanitizer for the §1.7
# definition-of-done. Debug-only; ~2x slower. Run ./solarium-asan; the
# sanitizers report any memory/UB error on stderr and abort.
if [ "$MODE" = "asan" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        main.c rhi_gl.c mesh.c ui.c scene.c material.c scene_io.c nid.c stml.c sol_math.c camera.c image.c font.c json.c glb.c \
        $(pkg-config --cflags --libs glfw3) \
        -framework OpenGL -framework Cocoa -framework IOKit \
        -o solarium-asan
    echo "built ./solarium-asan (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi

if [ "$MODE" = "release" ]; then
    FLAGS="-O2 -DNDEBUG"
else
    FLAGS="-g -O0"
fi

clang -std=c11 $FLAGS -Wall -Wextra \
    main.c rhi_gl.c mesh.c ui.c scene.c material.c scene_io.c nid.c stml.c sol_math.c camera.c image.c font.c json.c glb.c \
    $(pkg-config --cflags --libs glfw3) \
    -framework OpenGL -framework Cocoa -framework IOKit \
    -o solarium

echo "built ./solarium ($MODE)"
