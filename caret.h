#ifndef CARET_H
#define CARET_H

/* Pure (GL-free) caret layout for note text editing. caret_build (font-bound)
   lives in main.c and feeds these the wrapped string + per-char advances. */

#define CARET_MAX_SLOTS 2304   /* >= EDIT_BUF_CAP (2048) + per-line leading slots */
#define CARET_MAX_LINES 256

typedef struct { int src; float x; } CaretSlot;        /* caret position: source byte + note-local x */
typedef struct { int slot0, nslots, line; } CaretLine; /* slots[slot0 .. slot0+nslots) */
typedef struct {
    CaretSlot slots[CARET_MAX_SLOTS];
    int       slot_count;
    CaretLine lines[CARET_MAX_LINES];
    int       line_count;
    float     line_h;                  /* note-local line height */
} CaretField;

/* byte length of the UTF-8 codepoint whose lead byte is `lead` (1..4; 1 on a
   stray continuation byte, so a walk always makes progress). */
int caret_cplen(unsigned char lead);

/* source<->wrapped reconciliation. text_wrap only INSERTS '\n' (replacing a run
   of break-spaces) and passes source '\n' through, so a byte-lockstep walk
   recovers each wrapped BYTE's source offset. out_src[i] = source byte offset of
   wrapped[i]. Returns the wrapped byte length (entries written), capped at cap. */
int caret_reconcile(const char *src, const char *wrapped, int *out_src, int cap);

/* Assemble a CaretField. wrapped + out_src (map) from the two calls above; adv[i]
   = the x to add at wrapped byte i (a codepoint's advance at its lead byte, 0 at
   continuation bytes and '\n'); wlen = the wrapped byte length; line_h in metres;
   space_adv = the width of one space (note-local) for caret slots over trailing
   spaces text_wrap dropped. A leading slot opens each line at x=0; a trailing slot
   follows each codepoint. Pure. Returns the line count. */
int caret_field_build(const char *src, const char *wrapped, const int *map,
                      const float *adv, int wlen, float line_h, float space_adv,
                      CaretField *out);

int caret_slot_for_offset(const CaretField *cf, int cursor);            /* slot with .src==cursor (nearest fallback) */
int caret_line_of_slot(const CaretField *cf, int slot);                 /* slot -> visual line */
int caret_slot_nearest_x(const CaretField *cf, int line, float goal_x); /* nearest .x on `line`; -1 if line OOB */

#endif
