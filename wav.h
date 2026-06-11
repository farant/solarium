/* wav.h — WAV, writer-first (P4 item 8, amended): export any minted sound
   as a playable file — the debug-screenshot reflex for audio, and the
   audition path before the mixer exists (afplay plays them). The READER
   is deliberately absent until a real recorded asset wants in. Pure C89
   + stdio; bytes written endian-EXPLICITLY (the glb reader's discipline,
   pointed the other way). */

#ifndef WAV_H
#define WAV_H

#include "sol_base.h"

/* mono float [-1,1] -> 16-bit PCM RIFF/WAVE. SOL_FALSE on any I/O
   failure (partial files are removed — the scene writer's rule). */
sol_bool wav_write_pcm16(const char *path, const float *samples,
                         int frames, int rate);

#endif /* WAV_H */
