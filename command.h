/* command.h — the shared command registry: one descriptor per discrete,
   edge-triggered command, consumed by BOTH read_input (keyboard) and the
   command palette. Continuous input (movement, exposure) is NOT here. */
#ifndef SOL_COMMAND_H
#define SOL_COMMAND_H

#include "sol_base.h"

/* AppState is the engine god-struct (main.c). Referenced opaquely by tag so this
   header needs no engine internals and creates no duplicate typedef. */
struct AppState;

typedef struct {
    const char *name;     /* shown + fuzzy-matched, e.g. "Toggle bloom" */
    const char *hint;     /* key label for display, e.g. "K"; NULL = none */
    int         key;      /* GLFW key code for dispatch; 0 = palette-only */
    void      (*run)(struct AppState *);
    sol_bool  (*can_run)(struct AppState *);   /* NULL = always available */
    sol_bool    was_down; /* edge-detect state */
} Command;

#endif /* SOL_COMMAND_H */
