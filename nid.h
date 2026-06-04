/* nid.h — ULID-style persistent identifier generator. Produces a 26-char
   Crockford-Base32 string: a time-sortable prefix + a per-run monotonic
   tie-breaker + randomness, so ids are globally unique-ish, mergeable across
   files/devices, and sort chronologically by plain strcmp. Scene-agnostic and
   above every seam: depends on the C library only. See SCENE_FORMAT.md §2. */
#ifndef NID_H
#define NID_H

#include "sol_base.h"

#define NID_LEN 26      /* characters, not counting the NUL terminator */

/* Write a fresh id (NID_LEN chars + '\0') into out. Caller owns the buffer,
   which must hold at least NID_LEN + 1 chars. Uses the current time(). */
void nid_generate(char *out);

/* Deterministic core: the caller supplies the timestamp (seconds). Lets tests
   prove that a later timestamp yields a lexicographically-greater id. */
void nid_generate_at(sol_u32 seconds, char *out);

/* Seed the randomness explicitly (for reproducible tests). If never called,
   the first nid_generate* lazily seeds from time(). */
void nid_seed(unsigned int seed);

#endif /* NID_H */
