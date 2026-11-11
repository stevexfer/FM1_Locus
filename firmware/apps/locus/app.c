/* SPDX-License-Identifier: GPL-3.0-only */
/* LOCUS: a resonator (Rings' DSP) into a granular processor (Clouds' DSP), the computer's audio routed into either
 * and to the outputs, a step sequencer and chance triggers to play them (locus_engine.cc, locus_seq.c). The
 * computer's audio comes in over USB, and the outputs go back to it as a USB audio input (sys/usb.c, build.txt).
 *
 * Pages: HOME the resonator, FX the granular processor (again: its blend), SCL the routing, SEQ the sequencer,
 * ARP the chance triggers; GLO the visualizer, again the global settings, again back. PLAY runs the sequencer,
 * REC freezes the granular processor's buffer, SAVE (twice) keeps it all. The keys play the resonator (OCT- OCT+);
 * on the sequencer's page the white keys are the steps (OCT- OCT+: 1-16, 17-32), the black ones the patterns and,
 * the last of them, the step's accent and grain; with EDIT held the keys set the step's note and go to the next. */
#include "ab_fw.h"
#include "avis.h"
#include "locus.h"

#if !FW_USB_PLAY || !FELUCCA_UAC
#error "locus: build.txt defines FW_USB_PLAY=1 and FELUCCA_UAC=1"
#endif

enum { P_RES, P_LOC_A, P_LOC_B, P_ROUTE, P_SEQ, P_CHANCE, P_GLOBAL, P_VIS, P_COUNT };

/* the colours: Locus's (a near-black ground, warm for the sound, cool when frozen) */
#define C_BG RGB(10, 10, 10)
#define C_LINE RGB(34, 34, 38)
#define C_WARM RGB(255, 115, 0)
#define C_WARM_DIM RGB(110, 50, 0)
#define C_BAND RGB(38, 22, 8)
#define C_COOL RGB(80, 170, 255)
#define C_COOL_DIM RGB(30, 64, 110)

#define VIS_Y 144u
#define VIS_H 96u
#define WAVE_N 120

/* ---- the audio interrupt's (the main loop only reads them, but the gain's target) */
static volatile int32_t ab_gain_target;     /* Q15: the main loop's, from MASTER */
static int32_t ab_gain;                     /* Q15: ramped to the target over a block */
static int16_t ab_in[2u * FW_BLOCK] FW_POOL;
static int16_t ab_out[2u * FW_BLOCK] FW_POOL;
static int16_t ab_scope[AB_SCOPE] FW_POOL;
static volatile uint32_t ab_scope_w;

/* ---- for avis.c (ab_fw.h) */
uint16_t* ui_fb(void) { return cv_px; }
void ui_lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t* px) { lcd_blit(x, y, w, h, px); }
void ui_lcd_sync(void) { lcd_sync(); }
void ui_lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { lcd_fill(x, y, w, h, c); }

void ab_scope_read(int16_t* dst, int n)
{
    const uint32_t w = ab_scope_w;
    int i;
    for (i = 0; i < n; i++)
        dst[i] = ab_scope[(w - (uint32_t)n + (uint32_t)i) & (AB_SCOPE - 1u)];
}

/* MASTER (0..1023) as a gain, Q15: squared, so the knob's travel is near even in dB; the bottom is silence */
static int32_t master_gain(uint32_t m)
{
    return m < 4u ? 0 : (int32_t)(m * m >> 5);
}

/* 0..32768 (32768 = 0 dB, more is more) in tenths of a dB, from -960; within about 0.1 dB */
static int32_t db10(uint32_t v)
{
    uint32_t b = 0, f;
    int32_t l;
    if (!v)
        return -960;
    while (b < 31u && (v >> (b + 1u)))
        b++;
    f = b >= 16u ? (v >> (b - 16u)) & 0xFFFFu : (v << (16u - b)) & 0xFFFFu;   /* the mantissa past 1, Q16 */
    f += ((f * (65536u - f)) >> 16) * 22u >> 6;                                /* log2(1 + f), near enough */
    l = ((int32_t)b - 15) * 65536 + (int32_t)f;                                 /* log2(v / 32768), Q16 */
    l = (l * 602 >> 16) / 10;
    return l < -960 ? -960 : l;
}

/* ---- the parameters the encoders turn */
enum { B_LE, B_LS, B_APP };
enum { A_LENGTH, A_STEP_NOTE, A_STEP_FLAGS, A_MIDI_CH, A_KEYS_GRAIN, A_OCT };
enum { F_PCT, F_ENUM, F_SEMI, F_INT, F_LEVEL, F_NOTE, F_PLUS1, F_SHAPE, F_MIDI };

typedef struct {
    const char* label;
    uint8_t bank, idx, fmt;
    int16_t lo, hi, step;
    const char* const* names;
} prm_t;

static const char* const N_MODEL[] = { "MODAL", "SYMPATHY", "STRING", "FM VOICE", "CHORDS" };
static const char* const N_POLY[] = { "1 VOICE", "2 VOICES", "4 VOICES" };
static const char* const N_EXCITE[] = { "STRIKE", "INPUT", "ONSETS" };
static const char* const N_MODE[] = { "GRANULAR", "STRETCH", "LOOPER", "SPECTRAL" };
static const char* const N_QUALITY[] = { "HI STEREO", "HI MONO", "LO STEREO", "LO MONO" };
static const char* const N_CHANCE[] = { "OFF", "GEIGER", "LORENZ", "AUTOMATON", "DRIP" };
static const char* const N_TARGET[] = { "RES", "GRAINS", "BOTH" };
static const char* const N_FLAGS[] = { "PLAIN", "ACCENT", "GRAIN", "ACC+GRAIN" };
static const char* const N_OFFON[] = { "OFF", "ON" };

enum {
    R_MODEL, R_POLY, R_TUNE, R_STRUCT, R_BRIGHT, R_DAMP, R_POS,
    L_MODE, L_PITCH, L_QUALITY, L_POS, L_SIZE, L_DENS, L_TEX, L_BLEND, L_SPREAD, L_FB, L_VERB,
    O_RES_OUT, O_USB_OUT, O_EXCITE, O_USB_RES, O_USB_LOC, O_RES_LOC, O_LOC_OUT,
    S_PATTERN, S_LENGTH, S_BPM, S_SWING, S_TRANSPOSE, S_NOTE, S_FLAGS,
    C_MODE, C_RATE, C_SHAPE, C_TARGET,
    G_MIDI, G_KEYS_GRAIN, G_OCT,
    PRM_COUNT
};

static const prm_t PRM[PRM_COUNT] = {
    [R_MODEL] = { "MODEL", B_LE, LP_RES_MODEL, F_ENUM, 0, 4, 1, N_MODEL },
    [R_POLY] = { "VOICES", B_LE, LP_RES_POLY, F_ENUM, 0, 2, 1, N_POLY },
    [R_TUNE] = { "TUNE", B_LE, LP_RES_TUNE, F_SEMI, -24, 24, 1, 0 },
    [R_STRUCT] = { "STRUCT", B_LE, LP_RES_STRUCT, F_PCT, 0, 1000, 10, 0 },
    [R_BRIGHT] = { "BRIGHT", B_LE, LP_RES_BRIGHT, F_PCT, 0, 1000, 10, 0 },
    [R_DAMP] = { "DAMPING", B_LE, LP_RES_DAMP, F_PCT, 0, 1000, 10, 0 },
    [R_POS] = { "POSITN", B_LE, LP_RES_POS, F_PCT, 0, 1000, 10, 0 },
    [L_MODE] = { "MODE", B_LE, LP_LOC_MODE, F_ENUM, 0, 3, 1, N_MODE },
    [L_PITCH] = { "PITCH", B_LE, LP_LOC_PITCH, F_SEMI, -24, 24, 1, 0 },
    [L_QUALITY] = { "QUALITY", B_LE, LP_LOC_QUALITY, F_ENUM, 0, 3, 1, N_QUALITY },
    [L_POS] = { "POSITN", B_LE, LP_LOC_POS, F_PCT, 0, 1000, 10, 0 },
    [L_SIZE] = { "SIZE", B_LE, LP_LOC_SIZE, F_PCT, 0, 1000, 10, 0 },
    [L_DENS] = { "DENSITY", B_LE, LP_LOC_DENS, F_PCT, 0, 1000, 10, 0 },
    [L_TEX] = { "TEXTURE", B_LE, LP_LOC_TEX, F_PCT, 0, 1000, 10, 0 },
    [L_BLEND] = { "BLEND", B_LE, LP_LOC_BLEND, F_PCT, 0, 1000, 10, 0 },
    [L_SPREAD] = { "SPREAD", B_LE, LP_LOC_SPREAD, F_PCT, 0, 1000, 10, 0 },
    [L_FB] = { "FEEDBK", B_LE, LP_LOC_FB, F_PCT, 0, 1000, 10, 0 },
    [L_VERB] = { "REVERB", B_LE, LP_LOC_VERB, F_PCT, 0, 1000, 10, 0 },
    [O_RES_OUT] = { "RES>OUT", B_LE, LP_RES_OUT, F_LEVEL, 0, 1000, 10, 0 },
    [O_USB_OUT] = { "USB>OUT", B_LE, LP_USB_OUT, F_LEVEL, 0, 1000, 10, 0 },
    [O_EXCITE] = { "EXCITER", B_LE, LP_RES_EXCITE, F_ENUM, 0, 2, 1, N_EXCITE },
    [O_USB_RES] = { "USB>RES", B_LE, LP_USB_RES, F_LEVEL, 0, 1000, 10, 0 },
    [O_USB_LOC] = { "USB>LOC", B_LE, LP_USB_LOC, F_LEVEL, 0, 1000, 10, 0 },
    [O_RES_LOC] = { "RES>LOC", B_LE, LP_RES_LOC, F_LEVEL, 0, 1000, 10, 0 },
    [O_LOC_OUT] = { "LOC>OUT", B_LE, LP_LOC_OUT, F_LEVEL, 0, 1000, 10, 0 },
    [S_PATTERN] = { "PATTERN", B_LS, LS_PATTERN, F_PLUS1, 0, LS_PATTERNS - 1, 1, 0 },
    [S_LENGTH] = { "LENGTH", B_APP, A_LENGTH, F_INT, 1, LS_STEPS, 1, 0 },
    [S_BPM] = { "BPM", B_LS, LS_BPM, F_INT, 30, 300, 1, 0 },
    [S_SWING] = { "SWING", B_LS, LS_SWING, F_PCT, 0, 700, 10, 0 },
    [S_TRANSPOSE] = { "TRANSP", B_LS, LS_TRANSPOSE, F_SEMI, -24, 24, 1, 0 },
    [S_NOTE] = { "NOTE", B_APP, A_STEP_NOTE, F_NOTE, 24, 108, 1, 0 },
    [S_FLAGS] = { "STEP", B_APP, A_STEP_FLAGS, F_ENUM, 0, 3, 1, N_FLAGS },
    [C_MODE] = { "CHANCE", B_LS, LS_CHANCE, F_ENUM, 0, 4, 1, N_CHANCE },
    [C_RATE] = { "RATE", B_LS, LS_RATE, F_PCT, 0, 1000, 10, 0 },
    [C_SHAPE] = { "SHAPE", B_LS, LS_SHAPE, F_SHAPE, 0, 1000, 10, 0 },
    [C_TARGET] = { "TARGET", B_LS, LS_TARGET, F_ENUM, 0, 2, 1, N_TARGET },
    [G_MIDI] = { "MIDI", B_APP, A_MIDI_CH, F_MIDI, 0, 16, 1, 0 },
    [G_KEYS_GRAIN] = { "GRAINS", B_APP, A_KEYS_GRAIN, F_ENUM, 0, 1, 1, N_OFFON },
    [G_OCT] = { "OCTAVE", B_APP, A_OCT, F_SEMI, -2, 2, 1, 0 },
};

/* each page: SELECT, ALGO, PRESET, 1, 2, 3, 4 (-1: none) */
static const int8_t PAGE_PRM[P_COUNT][7] = {
    [P_RES] = { R_MODEL, R_POLY, R_TUNE, R_STRUCT, R_BRIGHT, R_DAMP, R_POS },
    [P_LOC_A] = { L_MODE, L_PITCH, L_QUALITY, L_POS, L_SIZE, L_DENS, L_TEX },
    [P_LOC_B] = { L_MODE, L_PITCH, L_QUALITY, L_BLEND, L_SPREAD, L_FB, L_VERB },
    [P_ROUTE] = { O_RES_OUT, O_USB_OUT, O_EXCITE, O_USB_RES, O_USB_LOC, O_RES_LOC, O_LOC_OUT },
    [P_SEQ] = { S_PATTERN, S_LENGTH, S_BPM, S_SWING, S_TRANSPOSE, S_NOTE, S_FLAGS },
    [P_CHANCE] = { C_MODE, -1, -1, C_RATE, C_SHAPE, C_TARGET, S_TRANSPOSE },
    [P_GLOBAL] = { -1, -1, -1, G_MIDI, G_KEYS_GRAIN, G_OCT, -1 },
    [P_VIS] = { -1, -1, -1, -1, -1, -1, -1 },
};
static const char* const PAGE_TITLE[P_COUNT] = { "RESONATOR", "LOCUS", "LOCUS BLEND", "ROUTING", "SEQUENCER",
                                                 "CHANCE", "GLOBAL", "" };

/* ---- the main loop's */
static int page = P_RES, page_back = P_RES;
static int sel_step;                         /* the sequencer's selected step, 0..31 */
static int step_page;                        /* 0: steps 1-16, 1: 17-32 */
static int midi_ch, keys_grain = 1, key_oct;
static int touched = -1;                     /* the encoder slot turned last, and when */
static uint32_t touched_ms;
static uint32_t save_ask_ms = 0u - 10000u, saved_ms = 0u - 10000u;
static uint32_t frame_n;
static uint32_t chance_seen;

/* what each part of the screen shows now, to draw only what changed */
static int32_t shown_val[7];
static int8_t shown_prm[7];
static int shown_touch = -2;
static char shown_status[24];
static int shown_page = -1;

static uint32_t key_steps_bit(int white);

static ls_pattern_t* ed_pat(void) { return &ls_pat[ls_param[LS_PATTERN] & (LS_PATTERNS - 1)]; }

static int32_t prm_get(int id)
{
    const prm_t* p = &PRM[id];
    switch (p->bank) {
    case B_LE: return le_param[p->idx];
    case B_LS: return ls_param[p->idx];
    default: break;
    }
    switch (p->idx) {
    case A_LENGTH: return ed_pat()->length;
    case A_STEP_NOTE: return ed_pat()->step[sel_step].note;
    case A_STEP_FLAGS: return (ed_pat()->step[sel_step].flags >> 1) & 3;
    case A_MIDI_CH: return midi_ch;
    case A_KEYS_GRAIN: return keys_grain;
    case A_OCT: return key_oct;
    default: return 0;
    }
}

static void prm_set(int id, int32_t v)
{
    const prm_t* p = &PRM[id];
    v = v < p->lo ? p->lo : v > p->hi ? p->hi : v;
    switch (p->bank) {
    case B_LE: le_param[p->idx] = (int16_t)v; return;
    case B_LS: ls_param[p->idx] = (int16_t)v; return;
    default: break;
    }
    switch (p->idx) {
    case A_LENGTH: ed_pat()->length = (uint8_t)v; break;
    case A_STEP_NOTE: ed_pat()->step[sel_step].note = (uint8_t)v; break;
    case A_STEP_FLAGS: {
        ls_step_t* s = &ed_pat()->step[sel_step];
        s->flags = (uint8_t)((s->flags & LS_ON) | (uint32_t)v << 1);
        break;
    }
    case A_MIDI_CH: midi_ch = (int)v; break;
    case A_KEYS_GRAIN: keys_grain = (int)v; break;
    case A_OCT: key_oct = (int)v; break;
    default: break;
    }
}

static void note_name(char* b, int n)
{
    static const char NAMES[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    char o[8];
    str_cpy(b, NAMES[n % 12], 4);
    fmt_int(o, n / 12 - 1);
    str_cpy(b + str_len(b), o, 6);
}

static void prm_text(char* b, int id, int32_t v)
{
    const prm_t* p = &PRM[id];
    char t[12];
    switch (p->fmt) {
    case F_ENUM:
        str_cpy(b, p->names[v], 16);
        return;
    case F_PCT:
        fmt_int(b, v / 10);
        return;
    case F_SEMI:
        fmt_int(t, v);
        str_cpy(b, v > 0 ? "+" : "", 2);
        str_cpy(b + str_len(b), t, 12);
        if (id != G_OCT)
            str_cpy(b + str_len(b), " st", 4);
        return;
    case F_LEVEL:
        if (!v) {
            str_cpy(b, "OFF", 4);
            return;
        }
        fmt_int(t, (db10((uint32_t)(v * v / 1000 * 65536 / 1000)) + (v < 707 ? -5 : 5)) / 10);
        str_cpy(b, t[0] != '-' && t[0] != '0' ? "+" : "", 2);
        str_cpy(b + str_len(b), t, 12);
        str_cpy(b + str_len(b), " dB", 4);
        return;
    case F_NOTE:
        note_name(b, (int)v);
        return;
    case F_PLUS1:
        fmt_int(b, v + 1);
        return;
    case F_MIDI:
        if (v)
            fmt_int(b, v);
        else
            str_cpy(b, "OMNI", 5);
        return;
    case F_SHAPE:
        switch (ls_param[LS_CHANCE]) {
        case 2:   /* rho */
            fmt_int(t, 24 + v * 26 / 1000);
            str_cpy(b, "RHO ", 5);
            str_cpy(b + 4, t, 8);
            return;
        case 3:
            str_cpy(b, v < 333 ? "RULE 30" : v < 666 ? "RULE 90" : "RULE 110", 10);
            return;
        case 4:   /* r 2.80 .. 4.00 */
            fmt_fix(t, 280 + v * 120 / 1000, 2);
            str_cpy(b, "R ", 3);
            str_cpy(b + 2, t, 8);
            return;
        default:
            fmt_int(b, v / 10);
            return;
        }
    default:
        fmt_int(b, v);
        return;
    }
}

static const char* prm_label(int id)
{
    if (id == C_SHAPE)
        switch (ls_param[LS_CHANCE]) {
        case 1: return "BURSTS";
        case 2: return "CHAOS";
        case 3: return "RULE";
        case 4: return "FLOW";
        default: break;
        }
    return PRM[id].label;
}

/* ---- the project, kept in the store's slot 0 */
#define STORE_MAGIC 0x4C4F4331u   /* "LOC1" */
typedef struct {
    uint32_t magic;
    uint16_t size, pad;
    int16_t le[LP_COUNT];
    int16_t ls[LS_COUNT];
    uint8_t midi_ch, keys_grain;
    int8_t key_oct, pad2;
    ls_pattern_t pat[LS_PATTERNS];
} store_t;
static store_t st FW_POOL;

static void project_save(void)
{
    int i;
    st.magic = STORE_MAGIC;
    st.size = sizeof st;
    for (i = 0; i < LP_COUNT; i++)
        st.le[i] = le_param[i];
    st.le[LP_LOC_FREEZE] = 0;
    for (i = 0; i < LS_COUNT; i++)
        st.ls[i] = ls_param[i];
    st.ls[LS_PLAY] = 0;
    st.midi_ch = (uint8_t)midi_ch;
    st.keys_grain = (uint8_t)keys_grain;
    st.key_oct = (int8_t)key_oct;
    memcpy(st.pat, ls_pat, sizeof st.pat);
    fw_store_save(0, &st, sizeof st);
}

static void project_load(void)
{
    int i;
    if (fw_store_load(0, &st, sizeof st) != (int)sizeof st || st.magic != STORE_MAGIC || st.size != sizeof st)
        return;
    for (i = 0; i < LP_COUNT; i++) {
        const prm_t* p = 0;
        int j;
        for (j = 0; j < PRM_COUNT; j++)
            if (PRM[j].bank == B_LE && PRM[j].idx == i)
                p = &PRM[j];
        if (p)
            le_param[i] = (int16_t)(st.le[i] < p->lo ? p->lo : st.le[i] > p->hi ? p->hi : st.le[i]);
    }
    for (i = 0; i < LS_COUNT; i++)
        if (i != LS_PLAY)
            ls_param[i] = st.ls[i];
    midi_ch = st.midi_ch <= 16u ? st.midi_ch : 0;
    keys_grain = st.keys_grain ? 1 : 0;
    key_oct = st.key_oct < -2 || st.key_oct > 2 ? 0 : st.key_oct;
    memcpy(ls_pat, st.pat, sizeof st.pat);
    for (i = 0; i < LS_PATTERNS; i++)
        if (ls_pat[i].length < 1u || ls_pat[i].length > LS_STEPS)
            ls_pat[i].length = 16;
}

/* ---- drawing: the header, the parameters, the page's picture */
static void draw_header(void)
{
    char s[24], t[8];
    const int playing = ls_param[LS_PLAY] != 0;
    const ls_pattern_t* p = &ls_pat[ls_view.pattern & (LS_PATTERNS - 1)];
    if (fm1_ms - save_ask_ms < 2500u)
        str_cpy(s, "SAVE? AGAIN", sizeof s);
    else if (fm1_ms - saved_ms < 1500u)
        str_cpy(s, "SAVED", sizeof s);
    else {
        str_cpy(s, "P", sizeof s);
        fmt_int(t, (playing ? ls_view.pattern : ls_param[LS_PATTERN]) + 1);
        str_cpy(s + str_len(s), t, sizeof s - str_len(s));
        if (playing) {
            str_cpy(s + str_len(s), " ", sizeof s - str_len(s));
            fmt_int(t, ls_view.step + 1);
            str_cpy(s + str_len(s), t, sizeof s - str_len(s));
            str_cpy(s + str_len(s), "/", sizeof s - str_len(s));
            fmt_int(t, p->length);
            str_cpy(s + str_len(s), t, sizeof s - str_len(s));
        }
    }
    if (shown_page == page && str_eq(s, shown_status))
        return;
    str_cpy(shown_status, s, sizeof shown_status);
    cv_begin(240, 24, C_BG);
    cv_text(8, 4, &FONT_S, PAGE_TITLE[page], C_WHITE);
    cv_text(232 - text_w(&FONT_S, s), 4, &FONT_S, s, playing ? C_WARM : C_GRAY);
    if (playing)
        cv_rect(222 - text_w(&FONT_S, s), 9, 5, 7, C_WARM);
    cv_rect(0, 23, 240, 1, C_LINE);
    cv_blit(0, 0);
}

/* slot 0..2 (the left encoders, 80 wide, at y 90) or 3..6 (the right ones, 60 wide, at y 26) */
static void draw_slot(int slot, int id, int32_t v)
{
    char b[20];
    const int knob = slot >= 3;
    const uint32_t w = knob ? 60u : 80u, x = knob ? (uint32_t)(slot - 3) * 60u : (uint32_t)slot * 80u;
    const int hot = slot == touched && fm1_ms - touched_ms < 1500u;
    cv_begin(w, knob ? 60u : 50u, C_BG);
    if (id >= 0) {
        const prm_t* p = &PRM[id];
        const char* l = prm_label(id);
        prm_text(b, id, v);
        cv_text(((int32_t)w - text_w(&FONT_S, l)) / 2, 4, &FONT_S, l, hot ? C_ACCENT : C_GRAY);
        cv_text(((int32_t)w - text_w(&FONT_S, b)) / 2, 24, &FONT_S, b, C_HI);
        if (knob) {
            const int32_t span = p->hi - p->lo, fill = span ? (v - p->lo) * 48 / span : 0;
            cv_rect(6, 48, 48, 4, C_LINE);
            if (p->lo < 0) {   /* from the middle */
                const int32_t mid = (0 - p->lo) * 48 / span;
                if (fill >= mid)
                    cv_rect(6 + mid, 48, fill - mid + 1, 4, hot ? C_ACCENT : C_WARM);
                else
                    cv_rect(6 + fill, 48, mid - fill, 4, hot ? C_ACCENT : C_WARM);
            } else
                cv_rect(6, 48, fill, 4, hot ? C_ACCENT : C_WARM);
        }
    }
    if (!knob)
        cv_rect(0, 49, (int32_t)w, 1, C_LINE);
    cv_blit(x, knob ? 26u : 90u);
}

static void draw_slots(void)
{
    int s;
    const int touch_now = fm1_ms - touched_ms < 1500u ? touched : -1;
    for (s = 0; s < 7; s++) {
        const int id = PAGE_PRM[page][s];
        const int32_t v = id >= 0 ? prm_get(id) : 0;
        int force = shown_page != page || shown_prm[s] != id;
        if (touch_now != shown_touch && (s == touch_now || s == shown_touch))
            force = 1;
        if (id == C_SHAPE && shown_val[s] != v + 4096 * ls_param[LS_CHANCE])
            force = 1;
        if (!force && shown_val[s] == (id == C_SHAPE ? v + 4096 * ls_param[LS_CHANCE] : v))
            continue;
        draw_slot(s, id, v);
        shown_prm[s] = (int8_t)id;
        shown_val[s] = id == C_SHAPE ? v + 4096 * ls_param[LS_CHANCE] : v;
        le_prepare();
    }
    shown_touch = touch_now;
}

/* the granular processor's buffer as Locus draws it: the waveform mirrored about its middle, the band POSITION
   and SIZE read from, the grains, the record head; cool when frozen */
static void draw_wave(void)
{
    static uint8_t peaks[WAVE_N] FW_POOL;
    static uint16_t gpos[64] FW_POOL;
    static uint8_t genv[64] FW_POOL;
    const int frozen = le_frozen(), mode = le_param[LP_LOC_MODE];
    const uint16_t c_wave = frozen ? C_COOL : C_WARM, c_dim = frozen ? C_COOL_DIM : C_WARM_DIM;
    const int32_t mid = VIS_H / 2;
    int32_t i, n, head;
    cv_begin(240, VIS_H, C_BG);
    if (mode == 3) {   /* spectral: no buffer to show; the output instead */
        int16_t sc[120];
        ab_scope_read(sc, 120);
        for (i = 0; i < 120; i++) {
            int32_t a = sc[i] < 0 ? -sc[i] : sc[i];
            a = a * (mid - 6) / 32768;
            cv_rect(i * 2, mid - a, 1, 2 * a + 1, c_wave);
        }
        cv_text(6, 2, &FONT_S, "SPECTRAL", c_dim);
        cv_blit(0, VIS_Y);
        return;
    }
    head = le_view_wave(peaks, WAVE_N);
    {   /* the band: from POSITION behind the head, SIZE wide (Locus's 0.05 + 0.25 size) */
        const int32_t hx = head * 240 / 65536;
        const int32_t back = le_param[LP_LOC_POS] * 240 / 1000;
        const int32_t bw = 12 + le_param[LP_LOC_SIZE] * 60 / 1000;
        int32_t x0 = hx - back - bw;
        while (x0 < 0)
            x0 += 240;
        cv_rect(x0, 2, bw, (int32_t)VIS_H - 4, C_BAND);
        if (x0 + bw > 240)
            cv_rect(0, 2, x0 + bw - 240, (int32_t)VIS_H - 4, C_BAND);
    }
    cv_rect(0, mid, 240, 1, C_LINE);
    for (i = 0; i < WAVE_N; i++) {
        /* a soft knee (Locus's tanh): quiet sound still shows */
        const int32_t p = peaks[i], h = (p * (510 - p) / 255) * (mid - 4) / 255;
        if (h)
            cv_rect(i * 2, mid - h, 2, 2 * h + 1, p > 96 ? c_wave : c_dim);
    }
    n = le_view_grains(gpos, genv, 64);
    for (i = 0; i < n; i++) {
        const int32_t x = gpos[i] * 240 / 65536, e = genv[i];
        const int32_t y = mid + (int32_t)((((uint32_t)i * 2654435761u) >> 26) & 63u) * (mid - 10) / 32 - (mid - 10);
        const int32_t r = 1 + e / 110;
        const uint16_t c = e > 170 ? C_WHITE : e > 80 ? c_wave : c_dim;
        cv_rect(x - r, y - r, 2 * r + 1, 2 * r + 1, c);
    }
    cv_rect(head * 240 / 65536, 0, 1, (int32_t)VIS_H, frozen ? C_COOL : C_HI);
    cv_text(6, 2, &FONT_S, N_MODE[mode], c_dim);
    if (frozen)
        cv_text(234 - text_w(&FONT_S, "FROZEN"), 2, &FONT_S, "FROZEN", C_COOL);
    cv_blit(0, VIS_Y);
}

/* the routing: the computer, the resonator, the granular processor and the outputs, each route as bright as its level */
static void draw_route(void)
{
    static const int16_t NX[4] = { 10, 92, 92, 176 }, NY[4] = { 38, 6, 70, 38 };
    static const char* const NN[4] = { "USB", "RES", "LOCUS", "OUT" };
    static const uint8_t RA[6] = { 0, 0, 0, 1, 1, 2 }, RB[6] = { 1, 2, 3, 2, 3, 3 };
    static const uint8_t RP[6] = { LP_USB_RES, LP_USB_LOC, LP_USB_OUT, LP_RES_LOC, LP_RES_OUT, LP_LOC_OUT };
    int i;
    cv_begin(240, VIS_H, C_BG);
    for (i = 0; i < 6; i++) {
        const int32_t v = le_param[RP[i]], a = RA[i], b = RB[i];
        const uint32_t k = (uint32_t)(v > 1000 ? 1000 : v) * 255u / 1000u;
        const uint16_t c = v ? RGB(40 + k * 215 / 255, 20 + k * 95 / 255, 10) : C_LINE;
        const int32_t x0 = NX[a] + 52, y0 = NY[a] + 10, x1 = NX[b], y1 = NY[b] + 10;
        cv_line(x0, y0, x1, y1, c);
        cv_line(x0, y0 + 1, x1, y1 + 1, c);
    }
    for (i = 0; i < 4; i++) {
        cv_rect(NX[i], NY[i], 52, 20, C_LINE);
        cv_rect(NX[i] + 1, NY[i] + 1, 50, 18, C_BG);
        cv_text(NX[i] + (52 - text_w(&FONT_S, NN[i])) / 2, NY[i] + 2, &FONT_S, NN[i], C_HI);
    }
    cv_blit(0, VIS_Y);
}

/* the steps: two rows of 16; on, accent (bright), grain (a dot), past the length (dim), the playing one, the
   selected one; under it the selected step */
static void draw_steps(void)
{
    const ls_pattern_t* p = ed_pat();
    const int playing = ls_param[LS_PLAY] && ls_view.pattern == (ls_param[LS_PATTERN] & (LS_PATTERNS - 1));
    char b[32], t[8];
    int i;
    cv_begin(240, VIS_H, C_BG);
    for (i = 0; i < LS_STEPS; i++) {
        const int32_t x = (i & 15) * 15, y = (i >> 4) * 30 + 4;
        const ls_step_t* s = &p->step[i];
        const int in = i < p->length;
        uint16_t c = !in ? RGB(18, 18, 20) : s->flags & LS_ON ? (s->flags & LS_ACCENT ? C_WARM : C_WARM_DIM) : C_LINE;
        if ((i >> 4) == step_page)
            cv_rect(x, y - 3, 14, 1, C_GRAY);
        cv_rect(x + 1, y, 13, 24, c);
        if (in && (s->flags & LS_ON) && (s->flags & LS_GRAIN))
            cv_rect(x + 6, y + 17, 3, 3, C_COOL);
        if (playing && i == ls_view.step)
            cv_rect(x + 1, y + 24, 13, 2, C_WHITE);
        if (i == sel_step) {
            cv_rect(x + 1, y, 13, 1, C_ACCENT);
            cv_rect(x + 1, y + 23, 13, 1, C_ACCENT);
        }
    }
    str_cpy(b, "STEP ", sizeof b);
    fmt_int(t, sel_step + 1);
    str_cpy(b + str_len(b), t, sizeof b - str_len(b));
    str_cpy(b + str_len(b), "  ", sizeof b - str_len(b));
    note_name(t, p->step[sel_step].note);
    str_cpy(b + str_len(b), t, sizeof b - str_len(b));
    str_cpy(b + str_len(b), p->step[sel_step].flags & LS_ON ? "  ON" : "  OFF", sizeof b - str_len(b));
    cv_text(6, 72, &FONT_S, b, C_GRAY);
    cv_blit(0, VIS_Y);
}

/* the chance triggers: a strip of the last triggers scrolling, over each mode's own picture */
#define TRAIL 128
static int16_t trail_x[TRAIL] FW_POOL;
static int16_t trail_y[TRAIL] FW_POOL;
static uint32_t trail_w;
static uint32_t ca_rows[22] FW_POOL;
static uint32_t ca_seen;
static uint8_t hits[240] FW_POOL;
static uint32_t hits_w;

static void chance_follow(void)
{
    const uint32_t c = ls_view.chances;
    hits[hits_w++ % 240u] = (uint8_t)(c != chance_seen);
    chance_seen = c;
    if (ls_param[LS_CHANCE] == 2) {
        trail_x[trail_w % TRAIL] = ls_view.x;
        trail_y[trail_w % TRAIL] = ls_view.z;
        trail_w++;
    }
    if (ls_param[LS_CHANCE] == 4 && hits[(hits_w - 1u) % 240u]) {
        trail_x[trail_w % TRAIL] = trail_y[(trail_w - 1u) % TRAIL];
        trail_y[trail_w % TRAIL] = ls_view.x;
        trail_w++;
    }
    if (ls_param[LS_CHANCE] == 3 && ls_view.cells != ca_seen) {
        int r;
        for (r = 21; r > 0; r--)
            ca_rows[r] = ca_rows[r - 1];
        ca_rows[0] = ls_view.cells;
        ca_seen = ls_view.cells;
    }
}

static void draw_chance(void)
{
    const int mode = ls_param[LS_CHANCE];
    const int32_t ph = 76;   /* the picture's rows; the strip under it */
    int32_t i;
    cv_begin(240, VIS_H, C_BG);
    switch (mode) {
    case 2:   /* lorenz: x against z */
        for (i = TRAIL - 1; i >= 1; i--) {
            const uint32_t k0 = (trail_w - 1u - (uint32_t)i) % TRAIL, k1 = (trail_w - (uint32_t)i) % TRAIL;
            if ((uint32_t)i >= trail_w)
                continue;
            cv_line(120 + trail_x[k0] * 4 / 100, ph - 4 - trail_y[k0] * (ph - 8) / 5500,
                    120 + trail_x[k1] * 4 / 100, ph - 4 - trail_y[k1] * (ph - 8) / 5500,
                    i < 16 ? C_WHITE : i < 64 ? C_WARM : C_WARM_DIM);
        }
        break;
    case 3: {   /* the automaton's rows, the newest on top */
        const int32_t w = ls_pat[ls_view.pattern & (LS_PATTERNS - 1)].length < 4 ? 16
                          : ls_pat[ls_view.pattern & (LS_PATTERNS - 1)].length;
        const int32_t cw = 240 / w;
        int32_t r;
        for (r = 0; r < 22; r++)
            for (i = 0; i < w; i++)
                if (ca_rows[r] >> i & 1u)
                    cv_rect(i * cw, r * 3 + 6, cw - 1, 2, r ? C_WARM_DIM : C_WARM);
        break;
    }
    case 4:   /* drip: the return map, x(n) against x(n+1) */
        cv_rect(60, 2, 1, ph - 4, C_LINE);
        cv_rect(60, ph - 3, 120, 1, C_LINE);
        for (i = 0; i < 48 && (uint32_t)i < trail_w; i++) {
            const uint32_t k = (trail_w - 1u - (uint32_t)i) % TRAIL;
            const int32_t x = 60 + trail_x[k] * 120 / 1000, y = ph - 3 - trail_y[k] * (ph - 6) / 1000;
            cv_rect(x - 1, y - 1, 3, 3, i < 4 ? C_WHITE : C_WARM);
        }
        break;
    case 1:   /* geiger: the clicks are the strip */
        cv_text(6, 2, &FONT_S, "GEIGER", C_WARM_DIM);
        break;
    default:
        cv_text(6, 2, &FONT_S, "CHANCE OFF", C_DIM);
        break;
    }
    cv_rect(0, ph + 2, 240, 1, C_LINE);
    for (i = 0; i < 240; i++)
        if (hits[(hits_w + (uint32_t)i) % 240u])
            cv_rect(i, ph + 4, 1, (int32_t)VIS_H - ph - 6, C_WARM);
    cv_blit(0, VIS_Y);
}

static void draw_global(void)
{
    char b[40], t[12];
    uint32_t res_us, loc_us;
    static const char* const BUF[4] = { "1.9 S STEREO", "3.7 S MONO", "3.7 S STEREO", "7.4 S MONO" };
    le_load_us(&res_us, &loc_us);
    cv_begin(240, VIS_H, C_BG);
    str_cpy(b, "CPU ", sizeof b);
    fmt_int(t, (int32_t)(fw_stats.cpu_q8 * 100u >> 8));
    str_cpy(b + str_len(b), t, sizeof b - str_len(b));
#ifdef FW_HOST
    str_cpy(b + str_len(b), "%", sizeof b - str_len(b));
#else
    str_cpy(b + str_len(b), fm1_c1_on ? "%  TWO CORES" : "%  ONE CORE", sizeof b - str_len(b));
#endif
    cv_text(8, 4, &FONT_S, b, C_GRAY);
    str_cpy(b, "RES ", sizeof b);
    fmt_fix(t, (int32_t)res_us / 100, 1);
    str_cpy(b + str_len(b), t, sizeof b - str_len(b));
    str_cpy(b + str_len(b), " MS  LOCUS ", sizeof b - str_len(b));
    fmt_fix(t, (int32_t)loc_us / 100, 1);
    str_cpy(b + str_len(b), t, sizeof b - str_len(b));
    str_cpy(b + str_len(b), " MS", sizeof b - str_len(b));
    cv_text(8, 24, &FONT_S, b, C_GRAY);
    str_cpy(b, "BUFFER ", sizeof b);
    str_cpy(b + str_len(b), BUF[le_param[LP_LOC_QUALITY] & 3], sizeof b - str_len(b));
    cv_text(8, 44, &FONT_S, b, C_GRAY);
    cv_text(8, 64, &FONT_S, fw_stats.usb_up ? "USB AUDIO IN + OUT" : "USB: NOT CONNECTED", fw_stats.usb_up ? C_ACCENT : C_DIM);
    cv_blit(0, VIS_Y);
}

static void draw_page(void)
{
    if (shown_page != page)
        lcd_fill(0, 0, 240, 240, C_BG);
    draw_header();
    draw_slots();
    if (shown_page != page || frame_n % 3u == 0u) {
        switch (page) {
        case P_ROUTE: draw_route(); break;
        case P_SEQ: draw_steps(); break;
        case P_CHANCE: draw_chance(); break;
        case P_GLOBAL: draw_global(); break;
        default: draw_wave(); break;
        }
        le_prepare();
    }
    shown_page = page;
}

/* ---- the keys */
static const uint8_t WHITE[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
static const uint8_t BLACK[11] = { 1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25 };

static uint32_t key_steps_bit(int white) { return FW_BIT(WHITE[white]); }

static void play_key(uint32_t k)
{
    const int32_t n = (int32_t)(FW_KEY_BASE_NOTE + k) + 12 * key_oct;
    le_event(LE_EV_STRUM | (keys_grain ? LE_EV_GRAIN : 0u), n < 0 ? 0 : n > 127 ? 127 : n);
}

static void seq_keys(const fw_input_t* in)
{
    uint32_t k;
    int i;
    for (k = 0; k < FW_KEYS; k++) {
        if (!(in->keys_pressed & FW_BIT(k)))
            continue;
        if (in->held & FW_BIT(FW_BTN_EDIT)) {   /* step record: the key's note into the step, then the next */
            const int32_t n = (int32_t)(FW_KEY_BASE_NOTE + k) + 12 * key_oct;
            ls_step_t* s = &ed_pat()->step[sel_step];
            s->note = (uint8_t)(n < 24 ? 24 : n > 108 ? 108 : n);
            s->flags |= LS_ON;
            play_key(k);
            sel_step = (sel_step + 1) % ed_pat()->length;
            step_page = sel_step >> 4;
            continue;
        }
        for (i = 0; i < 16; i++)
            if (WHITE[i] == k) {
                const int st_i = step_page * 16 + i;
                ls_step_t* s = &ed_pat()->step[st_i];
                if (st_i == sel_step || !(s->flags & LS_ON))
                    s->flags ^= LS_ON;
                sel_step = st_i;
            }
        for (i = 0; i < 11; i++)
            if (BLACK[i] == k) {
                if (i < LS_PATTERNS) {
                    ls_param[LS_PATTERN] = (int16_t)i;
                    if (sel_step >= ed_pat()->length)
                        sel_step = 0;
                } else if (i == 8)
                    ed_pat()->step[sel_step].flags ^= LS_ACCENT;
                else if (i == 9)
                    ed_pat()->step[sel_step].flags ^= LS_GRAIN;
            }
    }
}

static void key_leds(const fw_input_t* in)
{
    uint32_t mask = 0;
    int i;
    if (page == P_SEQ && !(in->held & FW_BIT(FW_BTN_EDIT))) {
        const ls_pattern_t* p = ed_pat();
        const int playing = ls_param[LS_PLAY] && ls_view.pattern == (ls_param[LS_PATTERN] & (LS_PATTERNS - 1));
        for (i = 0; i < 16; i++) {
            const int s = step_page * 16 + i;
            int on = s < p->length && (p->step[s].flags & LS_ON);
            if (playing && s == ls_view.step)
                on = !on;
            if (on)
                mask |= key_steps_bit(i);
        }
        mask |= FW_BIT(BLACK[ls_param[LS_PATTERN] & (LS_PATTERNS - 1)]);
    } else
        mask = in->keys;
    fw_leds_keys(mask);
}

/* ---- MIDI: notes play the resonator (velocity 100 up: accented), start / stop the sequencer, program changes
   the pattern */
static void midi_in(void)
{
    fw_midi_t m;
    while (fw_midi_read(&m)) {
        const uint8_t type = m.status & 0xF0u, ch = (uint8_t)((m.status & 0x0Fu) + 1u);
        if (m.status == 0xFAu || m.status == 0xFBu)
            ls_param[LS_PLAY] = 1;
        else if (m.status == 0xFCu)
            ls_param[LS_PLAY] = 0;
        if (m.status >= 0xF0u || (midi_ch && ch != midi_ch))
            continue;
        if (type == 0x90u && m.d2)
            le_event(LE_EV_STRUM | (keys_grain ? LE_EV_GRAIN : 0u) | (m.d2 >= 100u ? LE_EV_ACCENT : 0u), m.d1);
        else if (type == 0xC0u)
            ls_param[LS_PATTERN] = (int16_t)(m.d1 & (LS_PATTERNS - 1));
    }
}

#define APP_IDLE
static void app_idle(void)
{
    le_prepare();
}

static void app_init(void)
{
    usb_play_enable(1);
    ls_init();
    le_init();
    project_load();
}

static void app_frame(const fw_input_t* in)
{
    static const int PAGE_BTN[] = { FW_BTN_HOME, FW_BTN_FX, FW_BTN_SCL, FW_BTN_SEQ, FW_BTN_ARP };
    static const int PAGE_OF[] = { P_RES, P_LOC_A, P_ROUTE, P_SEQ, P_CHANCE };
    int i;
    le_prepare();
    ab_gain_target = master_gain(in->master);
    midi_in();
    frame_n++;

    /* pages */
    for (i = 0; i < 5; i++)
        if (in->pressed & FW_BIT(PAGE_BTN[i])) {
            if (PAGE_OF[i] == P_LOC_A && (page == P_LOC_A || page == P_LOC_B))
                page = page == P_LOC_A ? P_LOC_B : P_LOC_A;
            else
                page = PAGE_OF[i];
        }
    if (in->pressed & FW_BIT(FW_BTN_GLO)) {
        if (page == P_VIS)
            page = P_GLOBAL;
        else if (page == P_GLOBAL)
            page = page_back;
        else {
            page_back = page;
            page = P_VIS;
            avis_start(in->ms);
        }
    }
    if (page != P_VIS && page != P_GLOBAL)
        page_back = page;

    /* the transport, freeze, save */
    if (in->pressed & FW_BIT(FW_BTN_PLAY))
        ls_param[LS_PLAY] = (int16_t)!ls_param[LS_PLAY];
    if (in->pressed & FW_BIT(FW_BTN_REC))
        le_param[LP_LOC_FREEZE] = (int16_t)!le_param[LP_LOC_FREEZE];
    if (in->pressed & FW_BIT(FW_BTN_SAVE)) {
        if (in->ms - save_ask_ms < 2500u) {
            project_save();
            save_ask_ms = in->ms - 10000u;
            saved_ms = in->ms;
        } else
            save_ask_ms = in->ms;
    }
    if (in->pressed & FW_BIT(FW_BTN_OCT_DOWN)) {
        if (page == P_SEQ)
            step_page = 0;
        else if (key_oct > -2)
            key_oct--;
    }
    if (in->pressed & FW_BIT(FW_BTN_OCT_UP)) {
        if (page == P_SEQ)
            step_page = ed_pat()->length > 16;
        else if (key_oct < 2)
            key_oct++;
    }

    /* the encoders: faster when turned fast */
    for (i = 0; i < 7; i++) {
        const int32_t d = in->knob[i];
        const int id = PAGE_PRM[page][i];
        if (!d || id < 0)
            continue;
        prm_set(id, prm_get(id) + d * PRM[id].step * (d > 2 || d < -2 ? 4 : 1));
        touched = i;
        touched_ms = in->ms;
        if (id == S_LENGTH && sel_step >= ed_pat()->length)
            sel_step = ed_pat()->length - 1;
    }

    /* the keys */
    if (page == P_SEQ)
        seq_keys(in);
    else {
        uint32_t k;
        for (k = 0; k < FW_KEYS; k++)
            if (in->keys_pressed & FW_BIT(k))
                play_key(k);
    }
    key_leds(in);
    chance_follow();

    fw_led_button(FW_BTN_HOME, page == P_RES);
    fw_led_button(FW_BTN_FX, page == P_LOC_A || page == P_LOC_B);
    fw_led_button(FW_BTN_SCL, page == P_ROUTE);
    fw_led_button(FW_BTN_SEQ, page == P_SEQ);
    fw_led_button(FW_BTN_ARP, page == P_CHANCE);
    fw_led_button(FW_BTN_GLO, page == P_VIS || page == P_GLOBAL);
    fw_led_button(FW_BTN_PLAY, ls_param[LS_PLAY] && (ls_view.step & 3u) != 3u);
    fw_led_button(FW_BTN_REC, le_param[LP_LOC_FREEZE] != 0);
    fw_led_button(FW_BTN_SAVE, in->ms - save_ask_ms < 2500u);
    fw_led_button(FW_BTN_EDIT, page == P_SEQ && (in->held & FW_BIT(FW_BTN_EDIT)));

    if (page == P_VIS) {
        avis_frame(in->ms);
        shown_page = P_VIS;
    } else
        draw_page();
}

/* the engine (or silence before the first frames), its scope, then the volume */
static void app_audio(int32_t* lr, uint32_t frames)
{
    const int32_t g1 = ab_gain_target, step = (g1 - ab_gain) / (int32_t)frames;
    int32_t g = ab_gain;
    uint32_t i, w = ab_scope_w;
    const int heard = usb_play_begin() && usb_play_read(ab_in, frames);
    le_render(ab_out, heard ? ab_in : 0, frames);
    for (i = 0; i < frames; i++) {
        const int32_t l = ab_out[2u * i], r = ab_out[2u * i + 1u];
        ab_scope[w++ & (AB_SCOPE - 1u)] = (int16_t)((l + r) >> 1);
        g += step;
        lr[2u * i] = l * g >> 15;
        lr[2u * i + 1u] = r * g >> 15;
    }
    ab_gain = g1;
    ab_scope_w = w;
}
