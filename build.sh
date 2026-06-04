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
        -fsyntax-only $GLFW_CFLAGS main.c rhi_gl.c mesh.c obj.c scene.c
    echo "c89check: PASS — all sources are C89-pedantic clean"
    exit 0
fi

# Build with AddressSanitizer + UndefinedBehaviorSanitizer for the §1.7
# definition-of-done. Debug-only; ~2x slower. Run ./solarium-asan; the
# sanitizers report any memory/UB error on stderr and abort.
if [ "$MODE" = "asan" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        main.c rhi_gl.c mesh.c obj.c scene.c \
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
    main.c rhi_gl.c mesh.c obj.c scene.c \
    $(pkg-config --cflags --libs glfw3) \
    -framework OpenGL -framework Cocoa -framework IOKit \
    -o solarium

echo "built ./solarium ($MODE)"
