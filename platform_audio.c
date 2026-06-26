/* platform_audio.c — see platform_audio.h. QUARANTINED: CoreAudio + C11
   atomics live here and nowhere else. Excluded from c89check. */

#include "platform_audio.h"

#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include <string.h>

/* ---- the SPSC ring ----------------------------------------------------
   One producer (the main thread, audio_push), one consumer (the render
   callback). Each index is WRITTEN by exactly one side and read by the
   other — that single-writer property is what makes lock-free easy here
   (multi-producer rings need CAS loops; we deliberately don't).
   Ordering: the producer publishes the command bytes BEFORE the write
   index (release), the consumer reads the index BEFORE the bytes
   (acquire) — so a command is always fully visible when announced.
   Indices run free and wrap by masking; "full" is w - r == size, correct
   across unsigned wraparound. */

#define RING_SIZE 256u                /* power of two */
#define RING_MASK (RING_SIZE - 1u)

static MixCmd        g_ring[RING_SIZE];
static atomic_uint   g_wr;            /* written by producer only */
static atomic_uint   g_rd;            /* written by consumer only */

static Mixer         g_mixer;         /* consumer-owned after init */
static AudioUnit     g_unit;
static int           g_running = 0;
static atomic_int    g_muted;         /* 0 audible, 1 silent; main writes, render reads */

sol_bool audio_push(const MixCmd *cmd) {
    unsigned int w = atomic_load_explicit(&g_wr, memory_order_relaxed);
    unsigned int r = atomic_load_explicit(&g_rd, memory_order_acquire);
    if (!g_running) return SOL_FALSE;
    if (w - r >= RING_SIZE) return SOL_FALSE;        /* full: drop, never block */
    g_ring[w & RING_MASK] = *cmd;
    atomic_store_explicit(&g_wr, w + 1u, memory_order_release);
    return SOL_TRUE;
}

void audio_set_muted(sol_bool muted) {
    atomic_store_explicit(&g_muted, muted ? 1 : 0, memory_order_relaxed);
}

/* THE REAL-TIME THREAD. Everything it touches is preallocated; the only
   shared state is the ring. It must return in well under
   inNumberFrames / 48000 seconds, every time, forever. */
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 frames, AudioBufferList *io) {
    unsigned int w = atomic_load_explicit(&g_wr, memory_order_acquire);
    unsigned int r = atomic_load_explicit(&g_rd, memory_order_relaxed);
    (void)ref; (void)flags; (void)ts; (void)bus;

    while (r != w) {
        mixer_apply(&g_mixer, &g_ring[r & RING_MASK]);
        r++;
    }
    atomic_store_explicit(&g_rd, r, memory_order_release);

    if (io->mNumberBuffers >= 1 &&
        io->mBuffers[0].mDataByteSize >= frames * 2u * sizeof(float)) {
        mixer_render(&g_mixer, (float *)io->mBuffers[0].mData, (int)frames);
        /* mute zeroes the OUTPUT after rendering — voices still advanced
           above, so looping wind/water stay in phase across an unmute. */
        if (atomic_load_explicit(&g_muted, memory_order_relaxed))
            memset(io->mBuffers[0].mData, 0, frames * 2u * sizeof(float));
    }
    return noErr;
}

sol_bool audio_init(void) {
    AudioComponentDescription   desc;
    AudioComponent              comp;
    AudioStreamBasicDescription fmt;
    AURenderCallbackStruct      cb;

    memset(&desc, 0, sizeof desc);
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    comp = AudioComponentFindNext(NULL, &desc);
    if (comp == NULL) return SOL_FALSE;
    if (AudioComponentInstanceNew(comp, &g_unit) != noErr) return SOL_FALSE;

    /* we declare OUR format on the unit's input scope; CoreAudio converts
       to whatever the device actually runs (44.1k panels included) */
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate       = (Float64)MIX_RATE;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel   = 32;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = 8;                       /* 2ch x float, interleaved */
    fmt.mBytesPerPacket   = 8;
    if (AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0,
                             &fmt, sizeof fmt) != noErr) goto fail;

    cb.inputProc       = render_cb;
    cb.inputProcRefCon = NULL;
    if (AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0,
                             &cb, sizeof cb) != noErr) goto fail;

    mixer_init(&g_mixer);
    atomic_store(&g_wr, 0u);
    atomic_store(&g_rd, 0u);

    if (AudioUnitInitialize(g_unit) != noErr) goto fail;
    if (AudioOutputUnitStart(g_unit) != noErr) {
        AudioUnitUninitialize(g_unit);
        goto fail;
    }
    g_running = 1;
    return SOL_TRUE;

fail:
    AudioComponentInstanceDispose(g_unit);
    g_unit = NULL;
    return SOL_FALSE;
}

void audio_shutdown(void) {
    if (!g_running) return;
    g_running = 0;
    AudioOutputUnitStop(g_unit);          /* no render in flight past here */
    AudioUnitUninitialize(g_unit);
    AudioComponentInstanceDispose(g_unit);
    g_unit = NULL;
}
