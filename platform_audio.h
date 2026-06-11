/* platform_audio.h — the FIFTH quarantine (P4 item 8 piece 2): CoreAudio
   behind a three-function seam. The render callback runs on a real-time
   thread with a hard deadline — it never allocates, never locks, never
   touches engine state; it drains a lock-free SPSC command ring and calls
   the pure mixer core. ALL threading lives inside platform_audio.c
   (TODO4 §1.5); above this header the engine remains single-threaded and
   thread-ignorant. C11 atomics are used INSIDE the quarantine only (the
   stb dispensation), so the TU is excluded from c89check like its four
   siblings (image, font, platform_fs, rhi_gl's GL exception). */

#ifndef PLATFORM_AUDIO_H
#define PLATFORM_AUDIO_H

#include "sol_base.h"
#include "mixer.h"

/* opens the default output unit at MIX_RATE stereo float (CoreAudio
   resamples to the device behind the scenes) and starts the callback.
   SOL_FALSE = no audio device / API failure: the palace stays silent
   but runs — callers warn, never abort. */
sol_bool audio_init(void);

/* stops the callback BEFORE disposal — call this first in teardown, so
   no render is in flight while the rest of the engine dies. */
void audio_shutdown(void);

/* main-thread-only: queue one command for the callback. SOL_FALSE = the
   ring was full and the command was DROPPED — a lost footstep beats a
   blocked frame, so callers may ignore the return for one-shots. */
sol_bool audio_push(const MixCmd *cmd);

#endif /* PLATFORM_AUDIO_H */
