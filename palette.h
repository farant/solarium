/* palette.h — keyboard-driven, fuzzy-searchable command palette over the shared
   command registry. Owns the keyboard while open; never touches mouse capture.
   AppState is opaque here (only passed through to command callbacks). GLFW key
   codes are translated to PaletteKey by main.c, so this unit needs no GLFW. */
#ifndef SOL_PALETTE_H
#define SOL_PALETTE_H

#include "sol_base.h"
#include "command.h"
#include "font.h"

#define PALETTE_QUERY_CAP 128

typedef struct {
    sol_bool open;
    char     query[PALETTE_QUERY_CAP];
    int      len;
    int      sel;       /* highlighted result row */
    sol_bool eat_char;  /* swallow the leading ':' that opened the palette */
} Palette;

typedef enum {
    PALETTE_KEY_NONE = 0,
    PALETTE_KEY_UP,
    PALETTE_KEY_DOWN,
    PALETTE_KEY_ENTER,
    PALETTE_KEY_BACKSPACE,
    PALETTE_KEY_CANCEL
} PaletteKey;

void     palette_open_now(Palette *p);
sol_bool palette_is_open(const Palette *p);
void     palette_input_char(Palette *p, unsigned int cp);
/* Returns SOL_TRUE if the palette consumed the key (always true while open). */
sol_bool palette_input_key(Palette *p, PaletteKey k, struct AppState *st,
                           const Command *cmds, int ncmds);
void     palette_draw(const Palette *p, struct AppState *st, Font *font,
                      const Command *cmds, int ncmds, int fb_w, int fb_h);

#endif /* SOL_PALETTE_H */
