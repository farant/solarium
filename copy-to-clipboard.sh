#!/bin/sh
# copy-to-clipboard.sh — concatenate every C source/header in the project, each
# prefixed with a banner naming the file, and copy the whole bundle to the
# clipboard (pbcopy on macOS). Handy for pasting the codebase into a review/chat.
#
# Skips vendor/ (the third-party stb_image.h). Run from the project root.
# To include vendor too, drop the "-path ./vendor -prune -o" clause below.
# To exclude the *_test.c files, add: ! -name '*_test.c'

set -eu

# Source list: all .c/.h at or below the cwd, vendor/ pruned, sorted for a
# stable order. Project filenames have no spaces, so plain iteration is safe.
files=$(find . -path ./vendor -prune -o \( -name '*.c' -o -name '*.h' \) -print | sort)

bundle() {
    printf '%s\n' "$files" | while IFS= read -r f; do
        [ -n "$f" ] || continue
        printf '/* ===== %s ===== */\n' "${f#./}"
        cat "$f"
        printf '\n\n'
    done
}

count=$(printf '%s\n' "$files" | grep -c . || true)

if command -v pbcopy >/dev/null 2>&1; then
    bundle | pbcopy
    echo "copied $count files to the clipboard" >&2
else
    # No pbcopy (not macOS): fall back to stdout so the output is still usable.
    bundle
    echo "pbcopy not found; printed $count files to stdout instead" >&2
fi
