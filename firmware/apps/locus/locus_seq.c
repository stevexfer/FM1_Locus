/* SPDX-License-Identifier: GPL-3.0-only */
/* LOCUS's sequencer and chance triggers: once a tick (48 frames, ~1 ms) from the engine, in the audio interrupt.
 *
 * The sequencer: 8 patterns of 1..32 sixteenths, each step on / off, accent, grain and its note; a pattern chosen
 * while one plays starts when that one ends. The chance triggers, beside it (their notes from the playing
 * pattern's steps):
 *   1 GEIGER     clicks at random (Poisson), RATE their mean rate; SHAPE the chance a click sets off another
 *   2 LORENZ     the Lorenz system, a trigger each time it rises through the plane z = rho - 1 (the centre of its
 *                lobes); RATE its speed, SHAPE rho (24: it settles, slowly; up to 50: wilder); the note from y
 *   3 AUTOMATON  an elementary cellular automaton, one cell a clock (RATE: 1/4x .. 4x the sixteenths), a row as
 *                long as the pattern; a live cell plays that step's note. SHAPE the rule: 30, 90, 110
 *   4 DRIP       the dripping faucet's intervals through its return map (after Shaw): x' = r x (1 - x), r from
 *                SHAPE (the flow: 2.8, one period, to 4, chaos, through the period doublings); RATE the drip rate */
#include <math.h>
#include "fw.h"
#include "locus.h"

#define TICK_S (48.0f / FW_FS_HZ)

ls_pattern_t ls_pat[LS_PATTERNS] FW_POOL;
volatile int16_t ls_param[LS_COUNT];
ls_view_t ls_view;

static int was_playing, step_i = -1, playing_pat;
static float step_phase;
static uint32_t rng = 0x2545F491u;

static float burst_s = -1.0f, dead_s;                     /* geiger */
static float lx = 0.1f, ly = 0.0f, lz = 20.0f;            /* lorenz */
static int lz_armed;
static float ca_phase;                                    /* automaton */
static int ca_i;
static uint32_t ca_row = 1u << 8;
static float drip_x = 0.31f, drip_s = 0.5f;               /* drip */

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static float frand(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int pattern_len(int p)
{
    const int n = ls_pat[p].length;
    return n < 1 ? 1 : n > LS_STEPS ? LS_STEPS : n;
}

/* a note for a chance trigger, u 0..1: one of the playing pattern's steps that are on (by u), C4 if none */
static int chance_note(float u)
{
    const ls_pattern_t* p = &ls_pat[playing_pat];
    const int len = pattern_len(playing_pat);
    int on[LS_STEPS], n = 0, i;
    for (i = 0; i < len; i++)
        if (p->step[i].flags & LS_ON)
            on[n++] = p->step[i].note;
    if (!n)
        return 60;
    i = (int)(clampf(u, 0.0f, 0.9999f) * (float)n);
    return on[i];
}

static uint32_t target_flags(void)
{
    switch (ls_param[LS_TARGET]) {
    case 1: return LE_EV_GRAIN;
    case 2: return LE_EV_STRUM | LE_EV_GRAIN;
    default: return LE_EV_STRUM;
    }
}

/* one step of the elementary automaton over w cells (wrapping) */
static uint32_t ca_next(uint32_t row, int w, int rule)
{
    uint32_t out = 0;
    int i;
    for (i = 0; i < w; i++) {
        const uint32_t l = row >> ((i + w - 1) % w) & 1u, c = row >> i & 1u, r = row >> ((i + 1) % w) & 1u;
        out |= (uint32_t)(rule >> (l << 2 | c << 1 | r) & 1) << i;
    }
    return out;
}

static void lorenz(float h, float rho)
{
    const float sigma = 10.0f, beta = 8.0f / 3.0f;
    /* midpoint steps */
    const float dx = sigma * (ly - lx), dy = lx * (rho - lz) - ly, dz = lx * ly - beta * lz;
    const float mx = lx + 0.5f * h * dx, my = ly + 0.5f * h * dy, mz = lz + 0.5f * h * dz;
    lx += h * sigma * (my - mx);
    ly += h * (mx * (rho - mz) - my);
    lz += h * (mx * my - beta * mz);
    if (!(fabsf(lx) < 200.0f && fabsf(ly) < 200.0f && fabsf(lz) < 200.0f)) {
        lx = 0.1f;
        ly = 0.0f;
        lz = 20.0f;
    }
}

/* the chance trigger this tick: flags (0 none), *note, *accent */
static uint32_t chance_tick(float steps_per_s, int* note, int* accent)
{
    const int mode = ls_param[LS_CHANCE];
    const float rate = clampf(ls_param[LS_RATE] * 0.001f, 0.0f, 1.0f);
    const float shape = clampf(ls_param[LS_SHAPE] * 0.001f, 0.0f, 1.0f);
    *accent = 0;
    switch (mode) {
    case 1: {   /* geiger: 0.2 .. 50 a second */
        const float lambda = 0.2f * expf(rate * 5.52f);
        int fire = 0;
        dead_s -= TICK_S;
        if (burst_s >= 0.0f) {
            burst_s -= TICK_S;
            if (burst_s < 0.0f)
                fire = 1;
        }
        if (!fire && dead_s <= 0.0f && frand() < lambda * TICK_S)
            fire = 1;
        if (!fire)
            return 0;
        dead_s = 0.003f;
        burst_s = frand() < shape * 0.9f ? 0.008f + 0.05f * frand() : -1.0f;
        *note = chance_note(frand());
        *accent = frand() < 0.2f;
        return target_flags();
    }
    case 2: {   /* lorenz: 0.05 .. 6.4 time units a second */
        const float speed = 0.05f * exp2f(rate * 7.0f), rho = 24.0f + 26.0f * shape;
        float h = speed * TICK_S;
        int fire = 0, k, n = (int)(h / 0.004f) + 1;
        h /= (float)n;
        for (k = 0; k < n; k++) {
            const float before = lz;
            lorenz(h, rho);
            if (lz < rho - 3.0f)
                lz_armed = 1;
            if (lz_armed && before < rho - 1.0f && lz >= rho - 1.0f) {
                fire = 1;
                lz_armed = 0;
            }
        }
        ls_view.x = (int16_t)(lx * 100.0f);
        ls_view.y = (int16_t)(ly * 100.0f);
        ls_view.z = (int16_t)(lz * 100.0f);
        if (!fire)
            return 0;
        *note = chance_note((ly + 25.0f) * (1.0f / 50.0f));
        *accent = lx > 0.0f;
        return target_flags();
    }
    case 3: {   /* automaton */
        static const int RULES[3] = { 30, 90, 110 };
        static const float MULT[5] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
        const ls_pattern_t* p = &ls_pat[playing_pat];
        int w = pattern_len(playing_pat), fired = 0;
        if (w < 4)
            w = 16;
        ca_phase += steps_per_s * MULT[(int)(rate * 4.999f)] * TICK_S;
        if (ca_phase < 1.0f)
            return 0;
        ca_phase -= 1.0f;
        if (ca_phase > 1.0f)
            ca_phase = 0.0f;
        if (ca_i >= w) {
            const uint32_t mask = w >= 32 ? 0xFFFFFFFFu : (1u << w) - 1u;
            uint32_t next = ca_next(ca_row & mask, w, RULES[(int)(shape * 2.999f)]) & mask;
            if (!next || next == (ca_row & mask))
                next = (rnd() & rnd() & mask) | 1u << (rnd() % (uint32_t)w);
            ca_row = next;
            ca_i = 0;
        }
        ls_view.cells = ca_row;
        if (ca_row >> ca_i & 1u) {
            *note = p->step[ca_i].note ? p->step[ca_i].note : 60;
            *accent = (p->step[ca_i].flags & LS_ACCENT) != 0;
            fired = 1;
        }
        ca_i++;
        return fired ? target_flags() : 0;
    }
    case 4: {   /* drip: 0.5 .. 32 drops a second on average */
        const float r = 2.8f + 1.2f * shape, mean = 1.0f / (0.5f * exp2f(rate * 6.0f));
        drip_s -= TICK_S;
        ls_view.x = (int16_t)(drip_x * 1000.0f);
        if (drip_s > 0.0f)
            return 0;
        drip_x = r * drip_x * (1.0f - drip_x);
        if (!(drip_x > 0.001f && drip_x < 0.999f))
            drip_x = 0.1f + 0.8f * frand();
        drip_s = mean * (0.4f + 1.2f * drip_x);
        *note = chance_note(drip_x);
        *accent = drip_x > 0.8f;
        return target_flags();
    }
    default:
        return 0;
    }
}

void ls_init(void)
{
    static const uint8_t NOTES[16] = { 48, 60, 55, 60, 63, 60, 67, 70, 48, 60, 55, 72, 63, 67, 58, 60 };
    int p, i;
    for (p = 0; p < LS_PATTERNS; p++) {
        ls_pat[p].length = 16;
        for (i = 0; i < LS_STEPS; i++) {
            ls_pat[p].step[i].note = NOTES[i & 15];
            ls_pat[p].step[i].flags = 0;
        }
    }
    for (i = 0; i < 16; i += 4)
        ls_pat[0].step[i].flags = LS_ON;
    ls_pat[0].step[0].flags |= LS_ACCENT;
    ls_pat[0].step[6].flags = LS_ON;
    ls_pat[0].step[10].flags = LS_ON | LS_GRAIN;
    ls_param[LS_BPM] = 110;
    ls_param[LS_TARGET] = 0;
    ls_param[LS_RATE] = 400;
    ls_param[LS_SHAPE] = 500;
}

uint32_t lseq_tick(int32_t* note)
{
    const int play = ls_param[LS_PLAY] != 0;
    const float steps_per_s = clampf((float)ls_param[LS_BPM], 30.0f, 300.0f) * (4.0f / 60.0f);
    const int transpose = ls_param[LS_TRANSPOSE];
    uint32_t out = 0;
    int n = -1, cn = 60, accent = 0, cflags;

    if (play && !was_playing) {
        step_i = -1;
        step_phase = 1.0f;
        playing_pat = ls_param[LS_PATTERN] & (LS_PATTERNS - 1);
    }
    was_playing = play;
    if (play) {
        const float swing = clampf(ls_param[LS_SWING] * 0.001f, 0.0f, 0.7f);
        const float dur = step_i & 1 ? 1.0f - swing : 1.0f + swing;
        step_phase += steps_per_s * TICK_S / dur;
        if (step_phase >= 1.0f) {
            const ls_step_t* s;
            step_phase -= 1.0f;
            if (step_phase > 1.0f)
                step_phase = 0.0f;
            step_i++;
            if (step_i >= pattern_len(playing_pat)) {
                step_i = 0;
                playing_pat = ls_param[LS_PATTERN] & (LS_PATTERNS - 1);
            }
            s = &ls_pat[playing_pat].step[step_i];
            if (s->flags & LS_ON) {
                out |= LE_EV_STRUM;
                if (s->flags & LS_ACCENT)
                    out |= LE_EV_ACCENT;
                if (s->flags & LS_GRAIN)
                    out |= LE_EV_GRAIN;
                n = s->note;
            }
            ls_view.step = (uint8_t)step_i;
            ls_view.pattern = (uint8_t)playing_pat;
            ls_view.steps++;
        }
    } else {
        playing_pat = ls_param[LS_PATTERN] & (LS_PATTERNS - 1);
    }

    cflags = (int)chance_tick(steps_per_s, &cn, &accent);
    if (cflags) {
        out |= (uint32_t)cflags | (accent ? LE_EV_ACCENT : 0u);
        if (n < 0 && (cflags & LE_EV_STRUM))
            n = cn;
        ls_view.chances++;
    }
    if (out & LE_EV_STRUM) {
        n += transpose;
        *note = n < 12 ? 12 : n > 120 ? 120 : n;
    }
    return out;
}
