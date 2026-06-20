/* workspace.c — see workspace.h. */
#include "workspace.h"
#include <string.h>

const char *workspace_of(Scene *s, sol_u32 handle) {
    sol_u32 h = handle;
    int     guard = 0;                 /* parent-chain runaway bound */
    while (h != 0 && guard++ < 64) {
        const char  *w = scene_meta_get(s, h, "workspace");
        SceneObject *o;
        if (w) return w;
        o = scene_get(s, h);
        if (!o) break;
        h = o->parent;
    }
    return "home";
}

sol_bool scene_object_active(Scene *s, sol_u32 handle) {
    if (s->active_ws[0] == '\0') return SOL_TRUE;     /* unfiltered */
    return (sol_bool)(strcmp(workspace_of(s, handle), s->active_ws) == 0);
}
