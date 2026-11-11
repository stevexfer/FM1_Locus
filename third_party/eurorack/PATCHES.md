# third_party/eurorack

Emilie Gillet's DSP code, MIT (each file's header; `LICENSES/MIT-Emilie-Gillet.txt`):

| Directory | From | Commit |
|---|---|---|
| `rings/` (`dsp/`, `resources.*`) | <https://github.com/pichenettes/eurorack> | `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4` |
| `clouds/` (`dsp/`, `resources.*`) | <https://github.com/pichenettes/eurorack> | `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4` |
| `stmlib/` (the files these include, and `LICENSE`) | <https://github.com/pichenettes/stmlib> | `d18def816c51d1da0c108236928b2bbd25c17481` |

Only the DSP: none of the modules' drivers, UI, calibration or settings. The firmware builds every `*.cc` here
(`firmware/apps/locus/third_party.txt`) with `#include "rings/..."`, `"clouds/..."`, `"stmlib/..."`
(`include third_party/eurorack` in `build.txt`).

## Changes

Each changed line is marked `FM1_Locus`.

- `rings/dsp/dsp.h`: `kSampleRate` is the FM-1 codec's rate (`FW_FS_HZ`, `fw_rate.h`: 48076.9 Hz), not 48000, so
  the resonator stays in tune.
- `rings/dsp/part.cc`: `Part::Init(NULL)` leaves the reverb alone. Its 64 KB buffer is not there: the
  `RESONATOR_MODEL_STRING_AND_REVERB` model (the only one that uses it) is not offered.
- `rings/dsp/part.h`: `Configure()`, public, does the reconfiguration a new model or polyphony needs ahead of
  `Process` (the engine does it on the first core, before the second one's batch).
- `rings/dsp/string_synth_*` removed (the "Disastrous Peace" easter egg; not used).
- `clouds/dsp/granular_processor.cc`: no `clouds/drivers/debug_pin.h` (the STM32 debug pin; unused).
- `clouds/dsp/grain.h`: at a grain's envelope peak the window's `Interpolate` read one entry past `lut_window`
  (4097 entries; harmless on the module, ×0); now the last entry itself.
- `clouds/dsp/grain.h`, `granular_sample_player.h`, `granular_processor.h`: read-only `view_*` accessors for the
  display (the recording's size, head and samples; each grain's position and envelope).

## Notes

- The granular processor runs at two thirds of the codec's rate (32051 Hz, its 32 kHz), through a 3 : 2 polyphase
  resampler each way (`locus_engine.cc`).
- `GranularProcessor::Prepare` (buffers, the time-stretcher's correlation search, the spectral mode's FFTs) runs in
  the main loop, as on the module; `Process` in the audio interrupt.
- The firmware's C++ library is its own (`firmware/cxx/`): `algorithm` (`min`, `max`, `swap`, `fill`, `copy`,
  `sort`, `rotate`, `max_element`), `cmath` (float overloads), `cstdlib`, `cstring`, `cstdio`, `new`.
