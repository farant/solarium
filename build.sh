#!/bin/sh
set -eu

MODE="${1:-debug}"
if [ "$MODE" = "release" ]; then
    FLAGS="-O2 -DNDEBUG"
else
    FLAGS="-g -O0"
fi

clang -std=c11 $FLAGS -Wall -Wextra \
    main.c \
    $(pkg-config --cflags --libs glfw3) \
    -framework OpenGL -framework Cocoa -framework IOKit \
    -o solarium

echo "built ./solarium ($MODE)"
