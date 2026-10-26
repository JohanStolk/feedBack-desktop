# Virtual tuning

Play a song in another tuning without retuning: the engine pitch-shifts the
instrument input in real time, so a bass or guitar in E standard sounds like
Eb, D, Drop D, B standard and so on. Everything downstream (amp sims, the tuner,
note detection, scoring) hears the shifted signal.

This document covers how it works, what was measured, and the known limits. The
user-facing help is the text in the 🎸 Audio screen (`src/renderer/screen.html`).

## Modes

Audio → **Virtual tuning**:

- **Off** (default). The shifter is a bit-exact passthrough.
- **✨ Match each song.** The shift comes from the loaded arrangement's tuning
  (plus `centOffset`; −1200 is the octave-down bass convention). Uniform tunings
  (Eb, D, B standard, the octave-down bass charts) get one shift. Mixed tunings
  (Drop D and other drop/open tunings) switch per note at chart time: a note gets
  its string's shift, a chord its lowest string's. It assumes the instrument is in
  standard tuning.
- **Fixed shift**, −12 … +12 semitones, for a bass that is already tuned down,
  jamming without a chart, or playing in one tuning with every chart re-fretted
  to it.

**Re-fret the chart** (Auto / Always / Off). The instrument becomes a plain
standard tuning shifted down (−2 = D standard), and a chart-transform provider
moves the chart's frets to fit. Drop D at −2 means the low string as written and
the other strings +2 frets, so the one-finger D5 becomes the normal power-chord
shape. Auto re-frets a Match-each-song part only when it has chords across the
dropped string (single-note and bass parts keep per-note shifting and their drop
fingering). For a fixed shift, Auto re-frets whenever the song's tuning differs
from the shift. A song tuned lower than a fixed shift can't be re-fretted (frets
would go below 0), and the status says so. A song exactly a whole number of
octaves from the shift fits as written: same frets, and the player sounds an
octave higher or lower than the record (Meshuggah's bass, written +1 with
`centOffset` −1200, played with a fixed +1).

## Signal path

```
renderer (src/renderer/screen.js)
  vtRefresh(): tuning → per-string shifts → shift schedule + note hints
  rAF tick:    setDropTuneClock(songTime, playing, playbackRate)
        │  IPC  audio:setDropTuneSemitones / setDropTuneChart / setDropTuneClock
        ▼
main (src/main/audio-bridge.ts)  →  addon (ControlBindings.cpp)
        ▼
AudioEngine  →  every pooled SourceChain  →  DropTuner (src/audio/DropTuner.*)
```

In `SourceChain::processBlock` the shifter runs on the mono instrument source
right after channel selection and input gain, **before** every consumer: the ML
detector, the input-frame ring and note verifier, the noise gate and YIN tuner,
the signal chain (NAM/IR/VST) and monitoring. The setting, chart and clock are
applied to every pooled source, so additional inputs (split-screen) are shifted
too.

## The shifter (DropTuner)

JUCE-free, real-time safe (`process()` doesn't allocate or lock), unit-tested
standalone.

**Pitch-synchronous delay line.** A read head runs at rate r = 2^(semitones/12)
against the write head. Once it has drifted one splice span away, it crossfades
(4 ms) to a second head exactly one span closer. The span is a whole number of
periods of the note being played, so the splice lands on the same point of the
waveform and the pitch stays clean. Added delay averages about the crossfade plus
half a span.

**Pitch tracking.** YIN on a lowpassed copy decimated to about 11 kHz (a
windowed-sinc FIR, 8·factor+1 taps) with a right-sized FFT, every ~5 ms. The
decimation made the per-hop burst about 10× cheaper with unchanged quality.

**Chart priming.** With a song loaded, the renderer sends each note's expected
*input* pitch (the song's pitch minus the shift on that string) and the shift
change points, plus the song clock.
- 10 ms before each chart note, its period is loaded, so the splice is right from
  the pluck instead of after the tracker settles.
- The tracker then searches ±1.5 semitones around the expected note and falls
  back to a free search when the player plays something else.
- On a detected pluck near a chart note (onset detector: fast/slow envelope
  ratio), the head restarts right behind the input, crossfaded, which cuts the
  delay at the attack.
- Shift changes land at sample-exact chart times. The engine extrapolates the
  clock between renderer pushes at the practice speed (`rate`), so slowed
  practice stays in sync.

Changing the amount crossfades from the current head to a fresh one, so it can
change note by note without clicks.

## Readouts

- **Delay:** `DropTuner::getDelayStats()`. The current block's mean lag, a ~1 s
  average while shifting, and the mean over the first 20 ms after each matched
  pluck ("attack"). Measured from the read heads, crossfades included.
- **CPU:** `CallbackLoad` (header-only) times the input callback and, in split
  mode, the output callback against the block duration: a smoothed average
  (~0.5 s) and the worst block since the last read.
- Both come through one call, `getEngineStats()` (addon → `audio:getEngineStats`
  → preload). They show in the Audio screen's status bar and in a line under the
  song's "Tuning: …" in the player, polled every 500 ms and only while visible.
- **Unshifted input warning:** red when NAM Tone runs its browser (WASM) amp on a
  live input stream, amber for any other raw input stream opened in the page
  (`getUserMedia` is wrapped to count them).

## Measurements

All on an M1 Pro unless noted. The offline prototype and evaluation scripts are
in a separate (local) prototype; the C++ numbers come from `tests/drop_tuner/`.

**Accuracy.** Steady notes within 0.3 cents (synthetic, −1/−4/−6); single guitar
notes E2–E6 within 0.7 cents at −1/−2/−6/+3. Real bass take at −6, chart-primed:
96 % of notes in tune, median error 2.2 cents (offline Rubber Band on the same
take: 77 %, 4.4 cents). Fixed shifts over the whole ±12 range: 91–96 % in tune,
1–4 cents median error.

**Delay.** Real bass take at −6: 12 ms at the pluck and 6 ms to the shifted pitch
with priming, against 17 ms and 22 ms without. Priming is robust to ±60 ms chart
jitter and 15–30 % wrong chart notes (91–96 % in tune; no note ever stuck on a
wrong pitch).

The shift's delay doesn't depend on the buffer size (`drop_tuner_bench`, second
table: 30 s of synthetic bass, 55 plucks, identical at 32–512 samples):

| Shift | Mean while shifting | Attack (first 20 ms after a pluck, chart-primed) |
|---|---|---|
| −1 | 11.5 ms | 5.2 ms |
| −6 | 12.4 ms | 7.6 ms |

The total from string to ear is the device latency (one input and one output
buffer, 1.3–21.3 ms for 32–512 samples at 48 kHz, plus the driver and
converters) **plus** this. In the app the live average on a guitar at −1 read
about 9–14 ms and the attack about 5 ms, matching the bench.
For comparison, forum measurements of hardware pitch pedals: DigiTech Drop
12–17 ms, Boss XS 13–14 ms, DigiTech Whammy 4 17 ms.

**Output purity.** Recorded live from the engine at −1 (guitar, effects
bypassed): open A came out as Ab2 104.3 Hz with its overtones on the Ab series and
no energy at 110/220/330 Hz; A4 came out as Ab4 413.2 Hz with nothing at 440 Hz.
Only a faint trace (−29 dB) appears in the first moment of a pluck, before the
tracker locks.

**Guitar.** Single notes, bends, slides, hammer-ons/pull-offs, vibrato and pinch
harmonics: 94–100 % in tune at −1/−2/−6, equal to or better than offline Rubber
Band. Chords at −1/−2: on par with Rubber Band (real strings, calibrated purity
metric). Full chords at −6 break down (open C 35 %, strumming 53 %, against about
75–80 % for Rubber Band). In a ~2050-song library, 94 % of guitar chords at −5/−6
are power chords or octaves, which splice cleanly.

**CPU** (`drop_tuner_bench`, worst callback (p99.9), primed, % of the callback's
time budget):

| Buffer | M1 Pro | i5-8400T (2018, 6 cores, 35 W) | i5-8400T before the decimated analysis |
|---|---|---|---|
| 32 | 6.7 % | 12.5 % | 153 % |
| 64 | 4.1 % | 6.5 % | 98 % |
| 128 | 2.6 % | 4.3 % | 43 % |
| 256 | 1.5 % | 3.8 % | 24 % |

One run per machine (Release builds, 48 kHz, synthetic bass at −6); repeated runs
on the M1 Pro vary by a few tenths of a percent. So the shifter fits at any common
buffer size even on a low-end PC; the amp sim and the audio driver decide the
smallest usable buffer.

## Limits

- **One mixed input.** The shifter hears all strings mixed and applies one shift
  at a time. A chord across strings with different shifts (a guitar Drop D power
  chord) gets the lowest string's shift for all its notes. Re-fret avoids that by
  shifting everything uniformly. A real per-string fix needs per-string signals
  (a hexaphonic pickup).
- **Full chords from about 3 semitones** sound rougher (one note's period is
  spliced for all). Power chords and octaves stay clean. A planned chord mode
  would splice on the chord's common period from the chart (open C at −6 in the
  prototype: 35 → 85 %).
- **Large shifts** (from about 7 semitones) change the tone: muffled down, thin
  up. The status warns from 7. A formant-preserving step would be needed.
- **The strings' own sound isn't shifted.** An electric guitar's strings are
  audible unamplified at their real pitch, so with speakers or open headphones a
  −1 shift can sound like two notes. Hardware pedals have the same effect.
- **Natural harmonics at −6** track less well (53–74 % in tune).

## Settings

Saved with the device settings (`slopsmith-audio-settings.json`):
`dropTuneSemitones`, `dropTuneAuto`, `dropTuneRefretMode` (`auto` / `always` /
`off`; the older boolean `dropTuneRefret` is still written and read for
migration), `songFollowsOutput`.

## Tests

- `tests/drop_tuner/drop_tuner_test.cpp`: pitch, latency bound, sample rates,
  block-size independence, clamping, click-free switching, chart shift points,
  priming, pluck restart, wrong-chart fallback, input sanitising, ±12, delay
  stats, practice-speed clock.
- `tests/drop_tuner/callback_load_test.cpp`: the CPU meter.
- `tests/drop_tuner/drop_tuner_bench.cpp`: per-callback cost table (not a test).
- `tests/drop_tuner/drop_tuner_render.cpp`: offline render of a raw float32 take,
  for listening and scoring.
- `tests/virtual-tuning.test.js`, `tests/engine-stats.test.js`: the renderer's
  pure helpers (shifts, schedule, hints, chord profile, warnings, re-fret plans,
  readout text).

## Files

| File | Role |
|---|---|
| `src/audio/DropTuner.h/.cpp` | the shifter |
| `src/audio/CallbackLoad.h` | callback time-budget meter |
| `src/audio/SourceChain.*`, `src/audio/AudioEngine.*` | placement, fan-out to sources, `takeEngineStats()` |
| `src/audio/addon/ControlBindings.cpp`, `DeviceBindings.cpp` | N-API bindings |
| `src/main/audio-bridge.ts`, `src/main/preload.ts` | IPC and the renderer API |
| `src/renderer/screen.js` / `.html` | settings, per-song logic, re-fret provider, readouts |
