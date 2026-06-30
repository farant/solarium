/* diskpath.c — pure path string math (see diskpath.h). C89, libc string only. */
#include "diskpath.h"
#include <string.h>

/* copy `len` bytes of src into out[cap], always NUL-terminated, clamped. */
static void dp_copy(char *out, int cap, const char *src, int len) {
    int n;
    if (cap <= 0) return;
    n = len;
    if (n < 0) n = 0;
    if (n > cap - 1) n = cap - 1;
    if (n > 0) memcpy(out, src, (size_t)n);
    out[n] = '\0';
}

sol_bool diskpath_is_root(const char *path) {
    if (!path) return SOL_FALSE;
    return (path[0] == '/' && path[1] == '\0') ? SOL_TRUE : SOL_FALSE;
}

void diskpath_parent(const char *path, char *out, int cap) {
    int len, i;
    if (cap <= 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    len = (int)strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;     /* ignore trailing slashes */
    i = len - 1;
    while (i > 0 && path[i] != '/') i--;
    if (i <= 0) { dp_copy(out, cap, "/", 1); return; } /* "/foo" -> "/" */
    dp_copy(out, cap, path, i);                        /* up to (excluding) the slash */
}

void diskpath_join(const char *dir, const char *name, char *out, int cap) {
    int dl, n, i;
    if (cap <= 0) return;
    out[0] = '\0';
    if (!dir)  dir  = "";
    if (!name) name = "";
    dl = (int)strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dl--;          /* trim trailing '/', keep root */
    if (dl == 1 && dir[0] == '/') dl = 0;               /* root: sep alone gives "/name" */
    n = 0;
    for (i = 0; i < dl && n < cap - 1; i++) out[n++] = dir[i];
    if (n < cap - 1) out[n++] = '/';
    for (i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = '\0';
}

const char *diskpath_basename(const char *path) {
    const char *slash;
    if (!path) return "";
    slash = strrchr(path, '/');
    if (!slash) return path;
    return slash[1] ? slash + 1 : "";
}
