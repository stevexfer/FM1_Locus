/* SPDX-License-Identifier: GPL-3.0-only */
/* LOCUS: what app.c (the unity build, no FPU) and the FPU side (locus_engine.cc, locus_seq.c, the drawing) share:
 * integers and pointers only. */
#ifndef LOCUS_H
#define LOCUS_H
#include <stdint.h>

/* the engine's parameters: the main loop writes them, the audio interrupt reads them once a tick.
   Levels and knobs 0..1000; the rest as their comments say. */
enum {
    LP_RES_MODEL,       /* 0..4: modal, sympathetic strings, string, FM voice, sympathetic strings quantized */
    LP_RES_POLY,        /* 0..2: 1, 2, 4 voices */
    LP_RES_STRUCT,
    LP_RES_BRIGHT,
    LP_RES_DAMP,
    LP_RES_POS,
    LP_RES_EXCITE,      /* 0 strike (the internal exciter, plus the input), 1 input only, 2 input, strummed by its onsets */
    LP_RES_TUNE,        /* -24..24 semitones on every note */
    LP_LOC_MODE,        /* 0..3: granular, pitch-shifter / time-stretcher, looping delay, spectral */
    LP_LOC_QUALITY,     /* 0..3: 16-bit stereo, 16-bit mono, 8-bit stereo, 8-bit mono */
    LP_LOC_POS,
    LP_LOC_SIZE,
    LP_LOC_PITCH,       /* -24..24 semitones */
    LP_LOC_DENS,
    LP_LOC_TEX,
    LP_LOC_BLEND,       /* dry / wet */
    LP_LOC_SPREAD,
    LP_LOC_FB,
    LP_LOC_VERB,
    LP_LOC_FREEZE,      /* 0 / 1 */
    LP_USB_RES,         /* the routing: the computer's audio into the resonator ... */
    LP_USB_LOC,         /* ... into the granular processor */
    LP_USB_OUT,         /* ... straight to the outputs */
    LP_RES_LOC,         /* the resonator into the granular processor */
    LP_RES_OUT,         /* the resonator straight to the outputs */
    LP_LOC_OUT,         /* the granular processor to the outputs */
    LP_COUNT
};
extern volatile int16_t le_param[LP_COUNT];

/* events into the engine, from the main loop (keys) or the sequencer (in the interrupt) */
#define LE_EV_STRUM 1u  /* strum the resonator */
#define LE_EV_GRAIN 2u  /* seed a grain (granular) / retrigger (the other modes) */
#define LE_EV_ACCENT 4u
void le_event(uint32_t flags, int32_t note);   /* main loop only: queued for the next tick */

/* ---- the sequencer and the chance triggers (locus_seq.c, run once a tick in the interrupt) */
#define LS_STEPS 32
#define LS_PATTERNS 8
#define LS_ON 1u
#define LS_ACCENT 2u
#define LS_GRAIN 4u       /* the step seeds a grain too */
typedef struct {
    uint8_t flags;
    uint8_t note;         /* MIDI */
} ls_step_t;
typedef struct {
    ls_step_t step[LS_STEPS];
    uint8_t length;       /* 1..32 */
    uint8_t pad[3];
} ls_pattern_t;
extern ls_pattern_t ls_pat[LS_PATTERNS];   /* the main loop edits them; the interrupt reads them */

enum {
    LS_PLAY,              /* 0 / 1 */
    LS_BPM,               /* 30..300 */
    LS_SWING,             /* 0..700: how much later the off-beat 16ths come, permille of a step */
    LS_PATTERN,           /* 0..7: the next one to play (from the end of the playing one) */
    LS_TRANSPOSE,         /* -24..24 semitones on the steps' and the chance triggers' notes */
    LS_CHANCE,            /* 0 off, 1 geiger, 2 lorenz, 3 automaton, 4 drip */
    LS_RATE,              /* 0..1000 */
    LS_SHAPE,             /* 0..1000: each mode's own (bursts, chaos, rule, the faucet's flow) */
    LS_TARGET,            /* 0 resonator, 1 grains, 2 both */
    LS_COUNT
};
extern volatile int16_t ls_param[LS_COUNT];

/* what the interrupt shows the display */
typedef struct {
    volatile uint8_t step;          /* the playing step, 0..31 */
    volatile uint8_t pattern;       /* the playing pattern */
    volatile uint32_t steps;        /* steps played (a change: a new one) */
    volatile uint32_t chances;      /* chance triggers fired */
    volatile uint32_t cells;        /* the automaton's row, a bit a cell */
    volatile int16_t x, y, z;       /* the Lorenz state x 100 / the faucet's 0..1000 in x */
} ls_view_t;
extern ls_view_t ls_view;
void ls_init(void);

void le_init(void);                                       /* app_init: placement new, the pool's buffers */
void le_render(int16_t* out, const int16_t* usb, uint32_t frames);   /* the interrupt: LR LR, usb NULL = silence */
void le_prepare(void);   /* the main loop, as often as it can: the granular processor's background work (buffers, the
                            time-stretcher's search, the spectral mode's FFTs); the interrupt's Process runs over it */

/* the display's view of the granular processor's recording: n peaks of |first channel|, 0..255, oldest first;
   and its grains: up to max (position 0..65535 of the buffer, envelope 0..255) */
int le_view_wave(uint8_t* peaks, int n);              /* returns the write head 0..65535 */
int le_view_grains(uint16_t* pos, uint8_t* env, int max);
int le_frozen(void);
uint32_t le_ticks(void);                              /* engine ticks rendered (48 frames each) */
void le_load_us(uint32_t* resonator, uint32_t* granular);   /* the last batch's time on each, microseconds */

#endif
