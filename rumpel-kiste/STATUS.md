# RumpelKiste -- status

Version 0.2.1. Linux and Windows, CLAP and VST3. 76 parameters, 19 presets.
Builds clean on both platforms; `render --selftest` passes all 190 checks on
the Linux build and on the Windows build under wine.

0.2.1 makes the controls glide, as SäureKiste 0.15.2 does. Every control
jumped straight to each new value, so host automation and a dragged knob put a
step into every voice still ringing: a zipper under a tom or a cymbal while its
Tune is swept, a click on a Level or the Volume. The controls a ringing voice
reads -- the Levels, the Tunes, the pitch and colour mods, Snappy, BD Attack
and Shape, Tom Sweep and Noise, the drive bus and the Volume -- now follow
their value through a 20 ms one-pole stepped every 8 samples (`glidedFields()`
in `dsp/drums.cpp`, frequencies in octaves). What a hit takes at its start --
the decays, Rim Gate, Clap Spread, BD Sweep -- and the switches act at once.
`derive()` is the old `setParams()`; a glide step runs only `deriveLive()`,
which rederives the ROM voices and the drive stage only when their inputs have
moved. The first values after `prepare()` or `reset()` snap, so every preset
and every voice renders bit for bit what 0.2.0 rendered, at 44.1, 48 and
96 kHz. The glide checks were confirmed by putting the jump back and watching
them fail.

## What works, and what is measured

Every voice has been measured against 96 kHz recordings of a machine (a
commercial sample pack, every knob stepped across its travel; see
`tools/analysis/README.md`, "Measured") and fitted to them. What changed from
0.1.0, where the schematic alone had been read:

- **Kick**: sweeps from 5.6x its landing pitch in one exponential (was 2.4x),
  the same at every accent; holds 45 ms at full level, then falls in two
  stages; Decay's audible range is 16-64 ms, not the RC's 15-345 ms; rounder
  and slightly asymmetric; the first half-cycle negative.
- **Snare**: 174 Hz and exactly 1.5x (the caps say 1.47); the upper oscillator
  stays 10 dB under the lower instead of dying in 10 ms; the Tone tail is
  24-77 ms, not 47-282 ms; accent lifts the noise far less than the drum.
- **Toms**: the pitch heard is C18's oscillator, with C19's a fifth *under* it
  and lagging in (was a partial a fifth above); the sweep is hertz, not a
  ratio; the VCA holds, then falls at 46-114 ms (LT), not 38-378; the noise is
  a 20 ms wash plus a tick at the trigger's end, not a 0.5 ms spike.
- **Rim shot**: resonators at 487 / 228 / 1014 Hz, clipped lopsidedly (1.58:1)
  for ~8 ms, then ringing on at 228 Hz; no 495 Hz high-pass (that reading of
  IC50b was wrong, and the individual output shows none); gate 47 ms.
- **Clap**: bursts at 0 / 10.0 / 21.5 / 31.8 ms; each falls fast and then
  slowly; the tail comes in with the last burst and lasts 105 ms.
- **Hats**: ROM clock 31.5 kHz, open-hat run 16384 samples (0.52 s); linear-dB
  decays of 9-45 ms (CH) and 120 ms (OH at the top); the stand-in's spectrum
  fitted to the recordings (1.3 dB mean error).
- **Cymbals**: clock 28.5-45.8 kHz; Tune now transposes the stand-in instead of
  only shortening it (it did not before -- a bug); envelopes that fall faster
  towards the end; the ride's bell partials where the recording has them.
- **Accent and balance**: every voice's accent span and its level against the
  kick, all parts at 50 %, as recorded.
- **Help line**: a fixed three-line area under the controls, cut with an
  ellipsis after the third line, instead of wrapping upward over the controls
  (the fix SäureKiste 0.15.1 has). The window is 26 px taller.
- **Defaults**: Tom Pitch 86 Hz, SD Pitch 174 Hz, BD Sweep 5.6x, Clap Spread
  10.6 ms, Rim Gate 47 ms (C119 into R403), all written into the factory
  presets. Rim Gate's range is now 2-200 ms; the state version went to 2, and
  a version-1 session's gate is converted so it keeps its length.

The self-test holds the voices to those numbers and to the ones the service
notes give, at 44.1, 48 and 96 kHz. The measured checks were each run against
the 0.1.0 engine first and fail there.

and the sequencer and the plugin around it:

- hits land on their frame to the sample; Shuffle, Scale and Last Step place
  steps where they should; a chain moves on and wraps; a flam's second stroke
  arrives Flam × Flam Unit later; a looping host drops no step at the seam
- MIDI keys play their voices, unmapped keys play nothing
- state round-trips byte for byte; the same events render bit-identical
  samples; the dragged MIDI file carries every hit
- every factory preset loads, makes a sound and stays under full scale

The window was driven on Xvfb and photographed on both backends: the X11 one
natively and the win32 one under wine. Two instances in one process show two
different banks, and `nm -C --defined-only RumpelKiste.clap | grep " [bB] "`
shows nothing per-instance at file scope.

## What has not been done

- **No DAW has loaded it.** wine is not Windows and guihost is not a host.
- **It has not been compared against a real machine by ear.** It has been
  measured against recordings of one, which is not the same thing.
- The hi-hat and cymbal content is a synthesised stand-in (see the manual,
  §5), fitted to the recordings' spectra band by band; the fit is 1.3-2.9 dB
  on average, and a band average says nothing about how metallic it sounds.
- The drive bus sees the kick about 8 dB lower than in 0.1.0, because the kick
  now sits at the machine's balance. The presets that use Master or Selected
  drive were not re-trimmed. Factory presets are 2-5 dB lower in RMS.

## Bugs found and fixed on the way

- The cymbals' Tune did not transpose them: the stand-in's oscillators and
  filters were specified against the clock Tune moved, so only the length
  changed. They are now computed at a fixed design clock and replayed at the
  tuned one.
- IC50b, after the rim shot, was read as a 495 Hz high-pass; R422 feeds back
  from the gain-of-two divider, which makes it 229 Hz, Q 1.58 -- and the
  recordings show neither.
- The rim shot's excitation time constant used R395 || R396; the two are in
  series (0.79 ms, measured 0.48 ms).

- Every row label read as permanently lit on Windows: the flash timer kept a
  steady clock in milliseconds in a `long`, which is 32 bits there and wraps
  after 24.8 days of uptime. Found by photographing the win32 window under
  wine; now `int64_t`.
- `near()` in the renderer collided with `<windows.h>`'s macro of the same name.
