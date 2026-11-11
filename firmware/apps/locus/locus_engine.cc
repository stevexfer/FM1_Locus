// SPDX-License-Identifier: GPL-3.0-only
/* LOCUS's engine: a resonator (Rings' DSP) into a granular processor (Clouds' DSP), both MIT, Emilie Gillet
 * (third_party/eurorack); the computer's audio routed into either and to the outputs.
 *
 * A tick is 48 frames at the codec's rate: two of the resonator's blocks (24) and one of the granular processor's
 * (32 at two thirds of the rate, its own 32 kHz: through a 3 : 2 polyphase resampler each way). The interrupt's
 * 256 frames are 5 or 6 ticks, a batch. This core renders the resonator for this interrupt's batch while the
 * second one runs the granular processor and the mix on the last one: the output is a batch and a tick behind the
 * input (about 6 ms), through FIFOs. Without the second core it all runs here.
 *
 * The granular processor's background work (Prepare: its buffers, the time-stretcher's search, the spectral
 * mode's FFTs) is the main loop's, as on the module: le_prepare. */
#include <new>
#include <algorithm>
#include "rings/dsp/part.h"
#include "rings/dsp/strummer.h"
#include "clouds/dsp/granular_processor.h"
extern "C" {
#include "fw.h"
#include "locus.h"
uint32_t lseq_tick(int32_t* note);
int fw_core1_run(void (*fn)(uint32_t), uint32_t arg);
int fw_core1_wait(void);
uint32_t fw_ticks(void);
}

namespace {

const int kTick = 48;
const int kCloudsTick = 32;
const int kRingsBlock = 24;
const int kMaxBatch = 6;             // (256 + 47) / 48
const size_t kCloudsLarge = 118784;
const size_t kCloudsSmall = 65536 - 128;

// one prototype low-pass at the 2x / 3x common rate (96 kHz): 2 phases of 48 down, 3 of 32 up
const int kTaps = 96;
const int kDownTaps = kTaps / 2;
const int kUpTaps = kTaps / 3;

alignas(8) uint8_t rings_mem[sizeof(rings::Part)] FW_POOL;
alignas(8) uint8_t clouds_mem[sizeof(clouds::GranularProcessor)] FW_POOL;
alignas(8) uint8_t clouds_large[kCloudsLarge] FW_POOL;
alignas(8) uint8_t clouds_small[kCloudsSmall];
alignas(8) uint8_t strummer_mem[sizeof(rings::Strummer)] FW_POOL;

rings::Part* part;
rings::Strummer* strummer;
clouds::GranularProcessor* clouds_proc;
volatile int ready;

// what a tick of the resonator needs, taken by this core when the batch starts
struct RingsTick {
  rings::Patch patch;
  float note, tune, gain;
  uint8_t strum, accent, model, poly, excite;
};

// what a tick of the granular processor and the mix need
struct LocusTick {
  float position, size, pitch, density, texture, blend, spread, feedback, reverb;
  float usb_loc, res_loc, usb_out, res_out, loc_out;
  uint8_t mode, quality, freeze, grain;
};

struct Batch {
  int n;
  RingsTick rt[kMaxBatch];
  LocusTick lt[kMaxBatch];
  int16_t usb[kMaxBatch][2 * kTick];
  float res_out[kMaxBatch][kTick];
  float res_aux[kMaxBatch][kTick];
};
Batch batch[2] FW_POOL;
int cur;

float down_h[2][kDownTaps] FW_POOL;
float up_h[3][kUpTaps] FW_POOL;
float down_x[2][kDownTaps - 1 + kTick] FW_POOL;      // the 48 kHz input, its history first
float up_x[2][kUpTaps - 1 + kCloudsTick] FW_POOL;    // the 32 kHz output, its history first

// the FIFOs, LR LR
const uint32_t kFifo = 768;
const uint32_t kLatency = 304;       // the out FIFO's start: 256 + 47 frames, the batch's
int16_t in_fifo[2 * kFifo] FW_POOL;
int16_t out_fifo[2 * kFifo] FW_POOL;
uint32_t in_w, in_r, out_w, out_r;

// events from the main loop
const uint32_t kEvents = 16;
uint32_t ev_buf[kEvents];
volatile uint32_t ev_w, ev_r;

// the parameters as the engine follows them
float sm[LP_COUNT];
float last_note = 60.0f;
uint32_t ticks;
volatile uint32_t rings_us, locus_us;

int model_set = -1, poly_set = -1;
float in_level;
float res_in[kTick] FW_POOL;

// the granular processor's tick at 48 kHz (the second core's, while a batch runs)
float usb_l[kTick] FW_POOL;
float usb_r[kTick] FW_POOL;
float loc_l[kTick] FW_POOL;
float loc_r[kTick] FW_POOL;
clouds::ShortFrame loc_in[kCloudsTick] FW_POOL;
clouds::ShortFrame loc_out[kCloudsTick] FW_POOL;

inline float level(float v) {   // 0..1000 -> 0..2, squared: 707 is unity
  v *= 0.001f;
  return 2.0f * v * v;
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

void design_filters() {
  const float fs = 2.0f * FW_FS_HZ;
  const float fc = 14500.0f / fs;
  float h[kTaps], sum = 0.0f;
  for (int k = 0; k < kTaps; ++k) {
    const float t = static_cast<float>(k) - 0.5f * (kTaps - 1);
    const float x = 2.0f * static_cast<float>(M_PI) * fc * t;
    const float sinc = t == 0.0f ? 2.0f * fc : sinf(x) / (static_cast<float>(M_PI) * t);
    const float a = 2.0f * static_cast<float>(M_PI) * (k + 0.5f) / kTaps;
    const float w = 0.42f - 0.5f * cosf(a) + 0.08f * cosf(2.0f * a);   // Blackman
    h[k] = sinc * w;
    sum += h[k];
  }
  for (int k = 0; k < kTaps; ++k) {
    h[k] /= sum;
  }
  for (int i = 0; i < kDownTaps; ++i) {
    for (int p = 0; p < 2; ++p) {
      down_h[p][i] = 2.0f * h[2 * i + p];
    }
  }
  for (int i = 0; i < kUpTaps; ++i) {
    for (int q = 0; q < 3; ++q) {
      up_h[q][i] = 3.0f * h[3 * i + q];
    }
  }
}

inline int16_t to_s16(float x) {
  int32_t v = static_cast<int32_t>(x * 32768.0f);
  return v > 32767 ? 32767 : v < -32768 ? -32768 : static_cast<int16_t>(v);
}

// down_x's 48 frames at 48 kHz -> loc_in, 32 at 32 kHz
void downsample() {
  for (int m = 0; m < kCloudsTick; ++m) {
    const int base = kDownTaps - 1 + (3 * m) / 2;
    const float* h = down_h[(3 * m) & 1];
    float l = 0.0f, r = 0.0f;
    for (int i = 0; i < kDownTaps; ++i) {
      l += h[i] * down_x[0][base - i];
      r += h[i] * down_x[1][base - i];
    }
    loc_in[m].l = to_s16(l);
    loc_in[m].r = to_s16(r);
  }
  for (int c = 0; c < 2; ++c) {
    std::copy(&down_x[c][kTick], &down_x[c][kTick + kDownTaps - 1], &down_x[c][0]);
  }
}

// loc_out's 32 frames at 32 kHz -> loc_l / loc_r, 48 at 48 kHz
void upsample() {
  for (int m = 0; m < kCloudsTick; ++m) {
    up_x[0][kUpTaps - 1 + m] = loc_out[m].l * (1.0f / 32768.0f);
    up_x[1][kUpTaps - 1 + m] = loc_out[m].r * (1.0f / 32768.0f);
  }
  for (int m = 0; m < kTick; ++m) {
    const int base = kUpTaps - 1 + (2 * m) / 3;
    const float* h = up_h[(2 * m) % 3];
    float l = 0.0f, r = 0.0f;
    for (int i = 0; i < kUpTaps; ++i) {
      l += h[i] * up_x[0][base - i];
      r += h[i] * up_x[1][base - i];
    }
    loc_l[m] = l;
    loc_r[m] = r;
  }
  for (int c = 0; c < 2; ++c) {
    std::copy(&up_x[c][kCloudsTick], &up_x[c][kCloudsTick + kUpTaps - 1], &up_x[c][0]);
  }
}

void follow_params() {
  for (int i = 0; i < LP_COUNT; ++i) {
    const float t = static_cast<float>(le_param[i]);
    switch (i) {
      case LP_RES_MODEL: case LP_RES_POLY: case LP_RES_EXCITE: case LP_RES_TUNE:
      case LP_LOC_MODE: case LP_LOC_QUALITY: case LP_LOC_FREEZE:
        sm[i] = t;
        break;
      default:
        sm[i] += 0.05f * (t - sm[i]);
        break;
    }
  }
}

// a tick's parameters and events, as this tick of the batch will hear them
void take_tick(RingsTick* rt, LocusTick* lt) {
  follow_params();
  bool strum = false, grain = false, accent = false;
  int32_t note;
  const uint32_t f = lseq_tick(&note);
  if (f) {
    strum = f & LE_EV_STRUM;
    grain = f & LE_EV_GRAIN;
    accent = f & LE_EV_ACCENT;
    if (strum) {
      last_note = static_cast<float>(note);
    }
  }
  if (ev_r != ev_w) {
    const uint32_t e = ev_buf[ev_r % kEvents];
    ev_r = ev_r + 1;
    if (e & (LE_EV_STRUM << 8)) {
      strum = true;
      last_note = static_cast<float>(e & 0xFFu);
    }
    grain = grain || (e & (LE_EV_GRAIN << 8));
    accent = accent || (e & (LE_EV_ACCENT << 8));
  }
  rt->patch.structure = std::min(sm[LP_RES_STRUCT] * 0.001f, 0.9995f);
  rt->patch.brightness = sm[LP_RES_BRIGHT] * 0.001f;
  rt->patch.damping = sm[LP_RES_DAMP] * 0.001f;
  rt->patch.position = sm[LP_RES_POS] * 0.001f;
  rt->note = last_note;
  rt->tune = sm[LP_RES_TUNE];
  rt->gain = level(sm[LP_USB_RES]);
  rt->strum = strum;
  rt->accent = accent;
  rt->model = static_cast<uint8_t>(sm[LP_RES_MODEL]);
  rt->poly = static_cast<uint8_t>(sm[LP_RES_POLY]);
  rt->excite = static_cast<uint8_t>(sm[LP_RES_EXCITE]);

  lt->position = sm[LP_LOC_POS] * 0.001f;
  lt->size = sm[LP_LOC_SIZE] * 0.001f;
  lt->pitch = std::max(-48.0f, std::min(48.0f, sm[LP_LOC_PITCH]));
  lt->density = sm[LP_LOC_DENS] * 0.001f;
  lt->texture = sm[LP_LOC_TEX] * 0.001f;
  lt->blend = clamp01(sm[LP_LOC_BLEND] * 0.00105f - 0.025f);
  lt->spread = sm[LP_LOC_SPREAD] * 0.001f;
  lt->feedback = sm[LP_LOC_FB] * 0.001f;
  lt->reverb = sm[LP_LOC_VERB] * 0.001f;
  lt->usb_loc = level(sm[LP_USB_LOC]);
  lt->res_loc = level(sm[LP_RES_LOC]);
  lt->usb_out = level(sm[LP_USB_OUT]);
  lt->res_out = level(sm[LP_RES_OUT]);
  lt->loc_out = level(sm[LP_LOC_OUT]);
  lt->mode = static_cast<uint8_t>(sm[LP_LOC_MODE]);
  lt->quality = static_cast<uint8_t>(sm[LP_LOC_QUALITY]);
  lt->freeze = sm[LP_LOC_FREEZE] != 0.0f;
  lt->grain = grain;
}

// a new model or polyphony for the resonator: reconfigured here, before its batch (its filters' Init is ~2 ms of
// tanf with 4 voices: on the second core it could outlast the wait)
void configure_rings(int model, int poly) {
  model = std::min(model, static_cast<int>(rings::RESONATOR_MODEL_SYMPATHETIC_STRING_QUANTIZED));
  if (model == model_set && poly == poly_set) {
    return;
  }
  part->set_model(static_cast<rings::ResonatorModel>(model));
  part->set_polyphony(1 << poly);
  part->Configure();
  model_set = model;
  poly_set = poly;
}

// the resonator, for a batch
void rings_batch(uint32_t which) {
  const uint32_t t0 = fw_ticks();
  Batch* b = &batch[which];
  for (int t = 0; t < b->n; ++t) {
    const RingsTick& rt = b->rt[t];
    const int16_t* usb = b->usb[t];
    for (int i = 0; i < kTick; ++i) {
      // through Rings' noise gate
      const float x = rt.gain * (0.5f / 32768.0f) * (usb[2 * i] + usb[2 * i + 1]);
      const float error = x * x - in_level;
      in_level += error * (error > 0.0f ? 0.1f : 0.0001f);
      const float gate = in_level <= 0.00003f ? in_level * (1.0f / 0.00003f) : 1.0f;
      res_in[i] = gate * x;
    }
    rings::PerformanceState ps;
    ps.internal_exciter = rt.excite == 0;
    ps.internal_strum = rt.excite == 2;
    ps.internal_note = true;
    ps.tonic = rt.tune;
    ps.note = rt.note;
    ps.fm = 0.0f;
    ps.chord = static_cast<int32_t>(rt.patch.structure * (rings::kNumChords - 1) + 0.5f);
    for (int k = 0; k < kTick; k += kRingsBlock) {
      rings::Patch p = rt.patch;
      ps.strum = k == 0 && rt.strum;
      if (ps.strum && rt.accent) {
        p.brightness = std::min(p.brightness + 0.3f, 1.0f);
      }
      const bool explicit_strum = ps.strum;
      strummer->Process(&res_in[k], kRingsBlock, &ps);
      ps.strum = ps.strum || explicit_strum;
      part->Process(ps, p, &res_in[k], &b->res_out[t][k], &b->res_aux[t][k], kRingsBlock);
    }
  }
  rings_us = (fw_ticks() - t0) / 24u;
}

// the granular processor and the mix, for a batch whose resonator is done: into the out FIFO. The second core's job
// (or this core's)
void locus_batch(uint32_t which) {
  const uint32_t t0 = fw_ticks();
  const Batch* b = &batch[which];
  for (int t = 0; t < b->n; ++t) {
    const LocusTick& lt = b->lt[t];
    const int16_t* usb = b->usb[t];
    const float* ro = b->res_out[t];
    const float* ra = b->res_aux[t];
    for (int i = 0; i < kTick; ++i) {
      usb_l[i] = usb[2 * i] * (1.0f / 32768.0f);
      usb_r[i] = usb[2 * i + 1] * (1.0f / 32768.0f);
      down_x[0][kDownTaps - 1 + i] = lt.usb_loc * usb_l[i] + lt.res_loc * ro[i];
      down_x[1][kDownTaps - 1 + i] = lt.usb_loc * usb_r[i] + lt.res_loc * ra[i];
    }
    downsample();
    clouds_proc->set_playback_mode(static_cast<clouds::PlaybackMode>(lt.mode));
    clouds_proc->set_quality(lt.quality);
    clouds::Parameters* cp = clouds_proc->mutable_parameters();
    cp->position = lt.position;
    cp->size = lt.size;
    cp->pitch = lt.pitch;
    cp->density = lt.density;
    cp->texture = lt.texture;
    cp->dry_wet = lt.blend;
    cp->stereo_spread = lt.spread;
    cp->feedback = lt.feedback;
    cp->reverb = lt.reverb;
    cp->freeze = lt.freeze;
    cp->trigger = lt.grain;
    cp->gate = lt.grain;
    clouds_proc->Process(loc_in, loc_out, kCloudsTick);
    upsample();
    for (int i = 0; i < kTick; ++i) {
      const float l = lt.usb_out * usb_l[i] + lt.res_out * ro[i] + lt.loc_out * loc_l[i];
      const float r = lt.usb_out * usb_r[i] + lt.res_out * ra[i] + lt.loc_out * loc_r[i];
      const uint32_t w = (out_w + i) % kFifo;
      out_fifo[2 * w] = to_s16(stmlib::SoftLimit(l));
      out_fifo[2 * w + 1] = to_s16(stmlib::SoftLimit(r));
    }
    out_w += kTick;
    ++ticks;
  }
  locus_us = (fw_ticks() - t0) / 24u;
}

}  // namespace

extern "C" {

volatile int16_t le_param[LP_COUNT];

void le_init(void) {
  static const int16_t kDefaults[LP_COUNT] = {
    0, 0, 400, 500, 600, 300, 0, 0,                     // the resonator
    0, 0, 500, 500, 0, 600, 500, 500, 300, 0, 300, 0,   // the granular processor
    0, 0, 707, 707, 0, 707,                             // the routing
  };
  for (int i = 0; i < LP_COUNT; ++i) {
    le_param[i] = kDefaults[i];
    sm[i] = kDefaults[i];
  }
  design_filters();
  stmlib::Random::Seed(0x4C4F4355u);
  part = new (rings_mem) rings::Part;
  part->Init(NULL);
  configure_rings(kDefaults[LP_RES_MODEL], kDefaults[LP_RES_POLY]);
  strummer = new (strummer_mem) rings::Strummer;
  strummer->Init(0.01f, rings::kSampleRate / rings::kMaxBlockSize);
  clouds_proc = new (clouds_mem) clouds::GranularProcessor;
  clouds_proc->Init(clouds_large, kCloudsLarge, clouds_small, kCloudsSmall);
  clouds_proc->set_playback_mode(clouds::PLAYBACK_MODE_GRANULAR);
  clouds_proc->set_quality(0);
  clouds_proc->Prepare();
  out_w = kLatency;
  ready = 1;
}

void le_event(uint32_t flags, int32_t note) {
  if (ev_w - ev_r >= kEvents) {
    return;
  }
  ev_buf[ev_w % kEvents] = (flags << 8) | (static_cast<uint32_t>(note) & 0xFFu);
  RING_PUBLISH();
  ev_w = ev_w + 1;
}

void le_render(int16_t* out, const int16_t* usb, uint32_t frames) {
  for (uint32_t i = 0; i < frames; ++i) {
    const uint32_t w = (in_w + i) % kFifo;
    in_fifo[2 * w] = usb ? usb[2 * i] : 0;
    in_fifo[2 * w + 1] = usb ? usb[2 * i + 1] : 0;
  }
  in_w += frames;

  // this interrupt's batch: its input, its parameters and events
  Batch* b = &batch[cur];
  b->n = 0;
  while (in_w - in_r >= static_cast<uint32_t>(kTick) && b->n < kMaxBatch) {
    int16_t* dst = b->usb[b->n];
    for (int i = 0; i < kTick; ++i) {
      const uint32_t r = (in_r + i) % kFifo;
      dst[2 * i] = in_fifo[2 * r];
      dst[2 * i + 1] = in_fifo[2 * r + 1];
    }
    in_r += kTick;
    take_tick(&b->rt[b->n], &b->lt[b->n]);
    ++b->n;
  }

  // the last batch's granular processor there, this one's resonator here: the longer of the two, the resonator,
  // here, so the wait is short (it gives up after 4 ms: the second core's first batches, from cold caches, are
  // up to ~3x slower)
  const int other = fw_core1_run(locus_batch, static_cast<uint32_t>(cur ^ 1));
  if (b->n) {
    configure_rings(b->rt[b->n - 1].model, b->rt[b->n - 1].poly);
  }
  rings_batch(static_cast<uint32_t>(cur));
  if (!other || fw_core1_wait() < 0) {
    locus_batch(static_cast<uint32_t>(cur ^ 1));
  }
  cur ^= 1;

  for (uint32_t i = 0; i < frames; ++i) {
    const uint32_t r = (out_r + i) % kFifo;
    out[2 * i] = out_fifo[2 * r];
    out[2 * i + 1] = out_fifo[2 * r + 1];
  }
  out_r += frames;
}

void le_prepare(void) {
  if (ready) {
    clouds_proc->Prepare();
  }
}

int le_view_wave(uint8_t* peaks, int n) {
  if (!ready || n <= 0) {
    return 0;
  }
  const int32_t size = clouds_proc->view_size();
  if (size <= 0) {
    return 0;
  }
  const int32_t per = size / n;
  const int32_t step = per > 16 ? per / 16 : 1;
  for (int s = 0; s < n; ++s) {
    float peak = 0.0f;
    for (int32_t j = 0; j < per; j += step) {
      const float v = fabsf(clouds_proc->view_sample((s * per + j) % size));
      peak = v > peak ? v : peak;
    }
    const int32_t p = static_cast<int32_t>(peak * 255.0f);
    peaks[s] = static_cast<uint8_t>(p > 255 ? 255 : p);
  }
  return static_cast<int>(static_cast<float>(clouds_proc->view_head() % size) * 65535.0f / size);
}

int le_view_grains(uint16_t* pos, uint8_t* env, int max) {
  if (!ready) {
    return 0;
  }
  const int32_t size = clouds_proc->view_size();
  int n = 0;
  if (size <= 0) {
    return 0;
  }
  for (int32_t i = 0; i < clouds::kMaxNumGrains && n < max; ++i) {
    const clouds::Grain& g = clouds_proc->view_grain(i);
    if (!g.view_active()) {
      continue;
    }
    int32_t p = g.view_position() % size;
    p = p < 0 ? p + size : p;
    pos[n] = static_cast<uint16_t>(static_cast<float>(p) * 65535.0f / size);
    const int32_t e = static_cast<int32_t>(g.view_envelope() * 255.0f);
    env[n] = static_cast<uint8_t>(e < 0 ? 0 : e > 255 ? 255 : e);
    ++n;
  }
  return n;
}

int le_frozen(void) { return ready && clouds_proc->frozen(); }

uint32_t le_ticks(void) { return ticks; }

void le_load_us(uint32_t* resonator, uint32_t* granular) {
  *resonator = rings_us;
  *granular = locus_us;
}

}  // extern "C"
