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
#define PALETTE_PICK_CAP  64
#define PALETTE_FORM_FIELDS 4
#define PALETTE_FIELD_CAP   64

typedef struct {
    sol_bool open;
    char     query[PALETTE_QUERY_CAP];
    int      len;
    int      sel;       /* highlighted result row */
    sol_bool eat_char;  /* swallow the leading ':' that opened the palette */
    sol_bool prompt;    /* prompt mode: collect a typed line, fire prompt_cb on Enter */
    const char *prompt_label;                         /* shown before the typed text */
    void      (*prompt_cb)(struct AppState *, const char *);
    sol_bool pick;      /* pick mode: fuzzy-choose one of a copied name/ref list */
    char     pick_names[PALETTE_PICK_CAP][48];        /* shown + fuzzy-filtered */
    char     pick_refs[PALETTE_PICK_CAP][24];         /* returned to the callback */
    int      pick_n;
    void     (*pick_cb)(struct AppState *, const char *);
    sol_bool form;      /* form mode: N labeled fields, submit all on Enter */
    char     form_labels[PALETTE_FORM_FIELDS][32];
    char     form_vals[PALETTE_FORM_FIELDS][PALETTE_FIELD_CAP];
    int      form_n;
    int      form_field;   /* the field being edited */
    void     (*form_cb)(struct AppState *, const char *const *vals, int n);
} Palette;

typedef enum {
    PALETTE_KEY_NONE = 0,
    PALETTE_KEY_UP,
    PALETTE_KEY_DOWN,
    PALETTE_KEY_ENTER,
    PALETTE_KEY_BACKSPACE,
    PALETTE_KEY_CANCEL,
    PALETTE_KEY_TAB
} PaletteKey;

void     palette_open_now(Palette *p);
/* Open the palette as a text PROMPT: collect one typed line shown after `label`,
   and call cb(st, line) on Enter (Esc cancels). Reuses the palette text field.
   The caller retains ownership of `label`: pass a string literal or a pointer that
   outlives the prompt session (the palette stores it, it does not copy it). */
void     palette_prompt(Palette *p, const char *label,
                        void (*cb)(struct AppState *, const char *));
/* Open the palette as a PICKER: fuzzy-choose one of `n` rows (names shown +
   filtered, refs returned), firing cb(st, ref) on Enter (Esc cancels). Copies up
   to PALETTE_PICK_CAP rows in immediately — the caller's arrays need not outlive
   the call. A sibling of palette_prompt for focus-mode command sub-choices. */
void     palette_pick(Palette *p, const char *label,
                      const char *const *names, const char *const *refs, int n,
                      void (*cb)(struct AppState *, const char *ref));
/* Open the palette as a multi-field FORM: `nfields` labeled text fields (Tab/
   arrows move between them, Enter submits all values to cb, Esc cancels). Labels
   are copied in; the `vals` pointers passed to cb are valid only during the call. */
void     palette_form(Palette *p, const char *const *labels, int nfields,
                      void (*cb)(struct AppState *, const char *const *vals, int n));
void     palette_input_char(Palette *p, unsigned int cp);
/* Returns SOL_TRUE if the palette consumed the key (always true while open). */
sol_bool palette_input_key(Palette *p, PaletteKey k, struct AppState *st,
                           const Command *cmds, int ncmds);
void     palette_draw(const Palette *p, struct AppState *st, Font *font,
                      const Command *cmds, int ncmds, int fb_w, int fb_h);

#endif /* SOL_PALETTE_H */
