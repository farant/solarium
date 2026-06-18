/* palette.c — the command palette overlay + state machine. Pure UI + the fuzzy
   matcher; AppState is opaque (only shuttled to command callbacks). */
#include "palette.h"

#include "fuzzy.h"
#include "ui.h"
#include "text.h"

#include <string.h>

#define PALETTE_MAX_COMMANDS 64
#define PALETTE_MAX_ROWS     12

void palette_open_now(Palette *p) {
    p->open     = SOL_TRUE;
    p->query[0] = '\0';
    p->len      = 0;
    p->sel      = 0;
    p->eat_char = SOL_TRUE;   /* the ':' that opened us arrives next as a char */
    p->prompt   = SOL_FALSE;
}

void palette_prompt(Palette *p, const char *label,
                    void (*cb)(struct AppState *, const char *)) {
    p->open         = SOL_TRUE;
    p->query[0]     = '\0';
    p->len          = 0;
    p->sel          = 0;
    p->eat_char     = SOL_FALSE;   /* no ':' to swallow — a command opened us */
    p->prompt       = SOL_TRUE;
    p->prompt_label = label;
    p->prompt_cb    = cb;
}

void palette_input_char(Palette *p, unsigned int cp) {
    if (!p->open) return;
    if (p->eat_char) { p->eat_char = SOL_FALSE; return; }
    if (cp < 0x20 || cp > 0x7e) return;             /* v1: printable ASCII only */
    if (p->len + 1 >= PALETTE_QUERY_CAP) return;
    p->query[p->len++] = (char)cp;
    p->query[p->len]   = '\0';
    p->sel = 0;
}

/* Build the filtered, score-sorted list of command indices (best first). Returns
   the total match count; writes up to `cap` indices into out[]. Stable on ties
   (registry order preserved). */
static int palette_rank(const Palette *p, const Command *cmds, int ncmds,
                        int *out, int cap) {
    int idx[PALETTE_MAX_COMMANDS];
    int score[PALETTE_MAX_COMMANDS];
    int n = 0, i, j;

    /* Registry must stay <= PALETTE_MAX_COMMANDS; commands beyond it are
       silently dropped from the palette. Bump the cap if g_commands grows. */
    if (ncmds > PALETTE_MAX_COMMANDS) ncmds = PALETTE_MAX_COMMANDS;
    for (i = 0; i < ncmds; i++) {
        int sc;
        if (fuzzy_match(p->query, cmds[i].name, &sc, NULL, 0)) {
            idx[n] = i; score[n] = sc; n++;
        }
    }
    for (i = 1; i < n; i++) {                        /* stable insertion sort */
        int ti = idx[i], ts = score[i];
        j = i - 1;
        while (j >= 0 && score[j] < ts) {
            idx[j + 1] = idx[j]; score[j + 1] = score[j]; j--;
        }
        idx[j + 1] = ti; score[j + 1] = ts;
    }
    if (cap > n) cap = n;
    for (i = 0; i < cap; i++) out[i] = idx[i];
    return n;
}

sol_bool palette_input_key(Palette *p, PaletteKey k, struct AppState *st,
                           const Command *cmds, int ncmds) {
    int order[PALETTE_MAX_COMMANDS];
    int n;

    if (!p->open) return SOL_FALSE;

    if (k == PALETTE_KEY_CANCEL) {
        p->open = SOL_FALSE; p->prompt = SOL_FALSE;
        p->prompt_cb = NULL; p->prompt_label = NULL;
        return SOL_TRUE;
    }

    if (p->prompt) {                                 /* text-prompt mode */
        if (k == PALETTE_KEY_BACKSPACE) {
            if (p->len > 0) { p->len--; p->query[p->len] = '\0'; }
        } else if (k == PALETTE_KEY_ENTER) {
            void (*cb)(struct AppState *, const char *) = p->prompt_cb;
            p->open = SOL_FALSE; p->prompt = SOL_FALSE;
            p->prompt_cb = NULL; p->prompt_label = NULL;
            if (cb) cb(st, p->query);
        }
        return SOL_TRUE;                             /* ignore UP/DOWN in a prompt */
    }

    n = palette_rank(p, cmds, ncmds, order, PALETTE_MAX_COMMANDS);

    if (k == PALETTE_KEY_DOWN) { if (p->sel + 1 < n) p->sel++; return SOL_TRUE; }
    if (k == PALETTE_KEY_UP)   { if (p->sel > 0)     p->sel--; return SOL_TRUE; }
    if (k == PALETTE_KEY_BACKSPACE) {
        if (p->len > 0) { p->len--; p->query[p->len] = '\0'; p->sel = 0; }
        return SOL_TRUE;
    }
    if (k == PALETTE_KEY_ENTER) {
        p->open = SOL_FALSE;                         /* close first */
        if (n > 0 && p->sel < n) {
            const Command *cmd = &cmds[order[p->sel]];
            if (cmd->can_run == NULL || cmd->can_run(st))
                cmd->run(st);
        }
        return SOL_TRUE;
    }
    return SOL_TRUE;                                  /* swallow anything else */
}

void palette_draw(const Palette *p, struct AppState *st, Font *font,
                  const Command *cmds, int ncmds, int fb_w, int fb_h) {
    int   order[PALETTE_MAX_COMMANDS];
    int   n, shown, top, i;
    float us, pad, row_h, ts, box_w, box_h, box_x, box_y;

    if (!p->open || font == NULL) return;

    us    = (float)fb_h / 1080.0f;
    pad   = 14.0f * us;
    row_h = 26.0f * us;
    ts    = 0.45f * us;

    if (p->prompt) {
        n = 0; shown = 0;
    } else {
        n     = palette_rank(p, cmds, ncmds, order, PALETTE_MAX_COMMANDS);
        shown = (n < PALETTE_MAX_ROWS) ? n : PALETTE_MAX_ROWS;
    }

    box_w = (float)fb_w * 0.55f;
    box_h = pad * 2.0f + row_h * (float)(shown + 1);
    box_x = ((float)fb_w - box_w) * 0.5f;
    box_y = (float)fb_h * 0.18f;

    ui_quad(box_x, box_y, box_w, box_h, 0.05f, 0.07f, 0.10f, 0.92f);
    ui_quad_outline(box_x, box_y, box_w, box_h, 1.0f * us, 0.95f, 0.80f, 0.45f, 0.9f);

    {   /* query row: command mode ":<typed>_", prompt mode "<label>: <typed>_" */
        char  line[PALETTE_QUERY_CAP + 40];
        float qy = box_y + pad + font_ascent(font) * ts;
        int   ll = 0, q;
        if (p->prompt) {
            const char *lbl = p->prompt_label ? p->prompt_label : "input";
            while (*lbl && ll < (int)sizeof line - 4) line[ll++] = *lbl++;
            line[ll++] = ':';
            line[ll++] = ' ';
        } else {
            line[ll++] = ':';
        }
        for (q = 0; q < p->len && ll < (int)sizeof line - 2; q++)
            line[ll++] = p->query[q];
        line[ll++] = '_';
        line[ll]   = '\0';
        ui_text(font, line, box_x + pad, qy, ts, 0.95f, 0.92f, 0.80f, 1.0f);
    }

    top = 0;
    if (p->sel >= PALETTE_MAX_ROWS) top = p->sel - PALETTE_MAX_ROWS + 1;

    for (i = 0; i < shown; i++) {
        int          ri = top + i;
        const Command *cmd;
        float        ry, ty;
        sol_bool     enabled;
        if (ri >= n) break;
        cmd     = &cmds[order[ri]];
        enabled = (cmd->can_run == NULL) || cmd->can_run(st);
        ry      = box_y + pad + row_h * (float)(i + 1);
        ty      = ry + font_ascent(font) * ts;
        if (ri == p->sel)
            ui_quad(box_x + pad * 0.5f, ry, box_w - pad, row_h,
                    0.20f, 0.24f, 0.30f, 0.9f);
        if (enabled)
            ui_text(font, cmd->name, box_x + pad, ty, ts, 0.92f, 0.92f, 0.92f, 1.0f);
        else
            ui_text(font, cmd->name, box_x + pad, ty, ts, 0.50f, 0.50f, 0.50f, 1.0f);
        if (cmd->hint != NULL) {
            float hw, hh;
            text_measure(font, cmd->hint, ts, &hw, &hh);
            ui_text(font, cmd->hint, box_x + box_w - pad - hw, ty, ts,
                    0.70f, 0.62f, 0.40f, 1.0f);
        }
    }
}
