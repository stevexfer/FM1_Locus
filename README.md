<h1 align="center">FM1_Locus</h1>

<p align="center">
  A modal resonator into a granular processor, for the M-VAVE FM-1<br>
  <a href="https://stevexfer.github.io/FM1_Locus/"><b>Web installer</b></a> ·
  <a href="https://github.com/stevexfer/FM1_Locus/releases">Releases</a> ·
  <a href="BUILDING.md">Building</a> ·
  <a href="docs/DEVELOPING.md">Developing</a>
</p>

LOCUS turns the FM-1 into two instruments in a row: **RESONATOR**, a modal / sympathetic-string / string / FM
resonator that the keys, the sequencer or the computer's audio excite, and **LOCUS**, a granular processor (grains,
time-stretch, looping delay, spectral) that records and replays it. The computer's USB audio can be sent into either,
both, or straight to the outputs. An XOX step sequencer and four chance triggers play them.

The DSP is Emilie Gillet's code for the Rings and Clouds Eurorack modules (MIT), unchanged in what it computes:
the resonator at 48 kHz, the granular processor at 32 kHz with its full 184 KB of buffers, as on the modules.

> [!NOTE]
> **Not yet tested on an FM-1.** It runs in the FM-1 emulator (FM-1Ception) and the host simulator; the numbers below
> are from the emulator.

- **RESONATOR**: MODEL (modal, sympathetic strings, string, FM voice, quantized sympathetic strings "chords"),
  VOICES (1, 2, 4), TUNE, STRUCT, BRIGHT, DAMPING, POSITN. EXCITER (on ROUTING): STRIKE (its own exciter, struck
  by each note), INPUT (only what is routed in), ONSETS (the input's own onsets strum it).
- **LOCUS**: MODE (granular, stretch, looper, spectral), PITCH, QUALITY (hi / lo fidelity, stereo / mono: about 1 to
  8 s of buffer), POSITN, SIZE, DENSITY, TEXTURE; and a second page: BLEND, SPREAD, FEEDBK, REVERB. REC freezes the
  buffer.
- **ROUTING**: USB>RES, USB>LOC, RES>LOC, LOC>OUT, RES>OUT, USB>OUT, each off to +6 dB. The default: the resonator
  into LOCUS, LOCUS and the USB audio to the outputs.
- **SEQUENCER**: 8 patterns of 1 to 32 steps, each step on / off, a note, an accent, and a grain (it also fires a
  grain in LOCUS). BPM 30 to 300, swing, transpose. A pattern change waits for the end of the pattern.
- **CHANCE**: triggers from four processes, with RATE, a SHAPE of their own, TARGET (the resonator, grains, or both):
  - **GEIGER**: Poisson clicks with a dead time; SHAPE is BURSTS (a click sometimes sets off a short run).
  - **LORENZ**: the Lorenz attractor; a trigger each time it crosses up through a height. SHAPE is CHAOS (ρ 24 to 50),
    the note follows y, the accent x's lobe.
  - **AUTOMATON**: an elementary cellular automaton (RULE 30, 90, 110) as wide as the pattern; a live cell is a
    trigger, read one cell per clock, the note from that step. It reseeds when a row dies or repeats.
  - **DRIP**: the dripping faucet, a logistic map's intervals; SHAPE is FLOW (from a steady drip to chaos).
- **GLO**: the visualizer (the sound fed back on itself), GLO again: GLOBAL (MIDI channel, keys fire grains, key
  octave, CPU and buffer), GLO again: back.
- **The screen** is laid out after LOCUS's: the buffer's waveform, the grains as dots, the play head and the
  POSITN / SIZE window (blue when frozen); on ROUTING the signal paths lit by their levels, on SEQUENCER the steps,
  on CHANCE each process drawn (the attractor, the automaton's rows, the drip's return map) over its triggers.
- **USB audio** both ways, named "LOCUS FM-1": the computer plays into the routing; it records what the FM-1 plays.
- **MIDI in** (USB and TRS): notes strum (velocity 100 and up accented), start / stop the sequencer, program change
  selects the pattern.
- **SAVE**, twice: the settings and the 8 patterns, kept in flash and loaded at power-on (the sound stops for a
  moment while the flash is written).

## Controls

| Button | |
| --- | --- |
| HOME | RESONATOR |
| FX | LOCUS; again: its second page (BLEND, SPREAD, FEEDBK, REVERB) |
| SCL | ROUTING |
| SEQ | SEQUENCER |
| ARP | CHANCE |
| GLO | the visualizer, then GLOBAL, then back |
| PLAY | the sequencer: start / stop (the chance triggers run whenever CHANCE is not OFF) |
| REC | freeze LOCUS's buffer |
| SAVE | twice within 2.5 s: save |
| OCT− / OCT+ | the keys' octave; on SEQUENCER: steps 1–16 / 17–32 |

Each page puts its parameters on KNOB 1–4 and SELECT, ALGORITHM, PRESETS (the three below them on the screen).
The keys play the resonator. On SEQUENCER the 16 white keys are the steps (press: select; press the selected step,
or an off one: on / off) and the black keys pick the pattern (the first 8), toggle the step's accent and grain; with
EDIT held the keys write their note into the steps one after another.

## Differences from the modules

- **No CV and no audio jacks of their own**: the knobs, keys, sequencer, chance triggers and MIDI play it, the USB
  audio is its input. The parameters move once per block (smoothed), as the modules read their CVs.
- **The granular processor** runs at 32 kHz as on the module, through a 48 / 32 kHz resampler: slightly softer at the
  very top, about 6 ms of latency through the whole chain.
- **The resonator** has no "string and reverb" model (its 64 KB reverb would not fit beside the granular buffers) and
  no easter egg. The tuning follows the FM-1's codec clock (48 076.9 Hz).
- **LOCUS's BLEND, SPREAD, FEEDBK, REVERB** each have a knob (one shared knob on the module).
- In SPECTRAL mode the waveform view shows the output instead of the buffer.
- **No MIDI clock** yet: the sequencer runs on its own BPM.

**CPU (emulator):** the resonator on one core and the granular processor on the other, 37–41% of the audio
interrupt's time and at most 2.6 ms of its 5.3 ms, in every model and mode, 4 voices included.

> [!WARNING]
> **Installing custom firmware is at your own risk.** FM1_Locus is unofficial firmware, provided as is, without
> any warranty (GPL-3.0 sections 15 and 16). It is not made, endorsed or supported by M-VAVE. A failed or interrupted
> install can leave the FM-1 unable to start until it is recovered with a USB tool. You can return to the official
> firmware at any time.

## Install

1. Charge the FM-1 and plug it straight into the computer with a data cable. Close apps that use MIDI.
2. Open the **[web installer](https://stevexfer.github.io/FM1_Locus/)** in Chrome or Edge (Safari and Firefox have
   no Web MIDI with SysEx).
3. Press **Install**. Leave the cable alone until it says Done.

From the command line, with a package from [Releases](https://github.com/stevexfer/FM1_Locus/releases) or your own
build:

```bash
pip install mido python-rtmidi
python3 tools/fm1_install.py build/locus.fwsc
```

**Back to the official firmware:** download FM-1 V15 from [m-vave.com/download](https://www.m-vave.com/download) and
write it with the web installer's "Back to the official firmware". FM1_Locus uses the Felucca platform's update
loader, so other Felucca-platform firmware (SLOOP, ORBIT, ChoralRoot, GATOR, MOOD_FM1, AMB-1, FM1_AudioBase)
installs over it the usual way.

**If something goes wrong:** an interrupted write resumes when you press Install again. After two failed boots in a
row the FM-1 opens USB RESCUE by itself (or hold OCT− at power-on), and the installer works there too. An FM-1 that
does not start at all can be recovered with [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker).

## Build

See **[BUILDING.md](BUILDING.md)**: the toolchain, `./build.sh`, the host simulator and tests, the emulator, and how
a release is made. [docs/DEVELOPING.md](docs/DEVELOPING.md) describes the platform and its app API.

## Credits

FM1_Locus is a fork of [FM1_AudioBase](https://github.com/stevexfer/FM1_AudioBase), a fork of MOOD_FM1, the FM-1
port of MOOD, which was built on [GATOR](https://github.com/npi3pak/fm1-gator) by npi3pak. It stands on the work of:

- **Emilie Gillet**: the DSP of Mutable Instruments' Rings and Clouds and the stmlib library
  ([eurorack](https://github.com/pichenettes/eurorack), MIT).
- **Leo Kuroshita, Hügelton Instruments**: [Felucca](https://github.com/hugelton/Felucca), the FM-1 platform: HAL,
  update loader, USB-MIDI, USB audio, OTA, storage and installers (GPL-3.0).
- **npi3pak**: [GATOR](https://github.com/npi3pak/fm1-gator), the platform cut down and its release tooling.
- **dspaudio**: [ORBIT](https://github.com/dspaudio/orbit), the platform GATOR was cut down from.
- **isod89**: [SLOOP](https://github.com/isod89/sloop-fm1), the web install protocol and the stock supply levels.
- **Quixotic7**: [ChoralRoot](https://github.com/Quixotic7/ChoralRootFM1), the audio interrupt fix and the release
  layout; [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker).
- **Charles Vestal**: [AMB-1](https://github.com/charlesvestal/fm1-amb), which showed the FPU in use on the FM-1.
- **Dimitar Toshkov Zhekov**: the Terminus font (SIL OFL 1.1).
- **JieLi Technology**: three AC79 SDK files in every package (Apache-2.0).

## License

FM1_Locus is free software under the [GNU General Public License v3.0 only](LICENSE), provided without any
warranty. The parts it is built from keep their licences and copyright notices:

| Part | Author | Licence | Text |
|---|---|---|---|
| FM1_Locus, FM1_AudioBase, MOOD_FM1's visualizer, GATOR (the app, web installer, release tooling) | stevexfer; npi3pak | GPL-3.0-only | [LICENSE](LICENSE) |
| Rings and Clouds DSP, stmlib (`third_party/eurorack`, patched: [PATCHES.md](third_party/eurorack/PATCHES.md)) | Emilie Gillet | MIT | [LICENSES/MIT-Emilie-Gillet.txt](LICENSES/MIT-Emilie-Gillet.txt) |
| Felucca platform: HAL, update loader, USB-MIDI, USB audio, OTA, storage, tools (through ORBIT and SLOOP, modified) | Leo Kuroshita, Hügelton Instruments | GPL-3.0-only | [LICENSE](LICENSE) |
| Web install protocol `fm1ota.js`, `fm1pkg.js` (from SLOOP 2.7, unchanged) | Leo Kuroshita, Hügelton Instruments; isod89 | GPL-3.0-only | [LICENSE](LICENSE) |
| Terminus font (rasterised into the firmware) | Dimitar Toshkov Zhekov | SIL OFL 1.1 | [LICENSES/OFL-Terminus.txt](LICENSES/OFL-Terminus.txt) |
| Three AC79 SDK files in every package (`tools/sdk`, unmodified) | JieLi Technology | Apache-2.0 | [LICENSES/Apache-2.0-JieLi-SDK.txt](LICENSES/Apache-2.0-JieLi-SDK.txt) |

Every release and the web installer carry these licence texts next to the packages, and each release's complete
source is its tag in this repository. [LICENSING.md](LICENSING.md) has the details.

"Mutable Instruments", "Rings" and "Clouds" are Emilie Gillet's; FM1_Locus uses her MIT-licensed code and is not
affiliated with or endorsed by Mutable Instruments. "M-VAVE" and "FM-1" are trademarks of their respective owners.
FM1_Locus is not affiliated with, endorsed by or supported by them.
