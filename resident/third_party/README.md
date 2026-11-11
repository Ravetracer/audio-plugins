# Third-party code

| Folder | What | Licence |
|--------|------|---------|
| `libresidfp/` | reSIDfp from [libresidfp](https://github.com/libsidplayfp/libresidfp) 1.2.3 (Dag Lem, Antti S. Lankila, Leandro Nini), with the patches below applied | GPL-2.0-or-later, `libresidfp/COPYING` |
| `sidlite/` | SIDLite from [libsidplayfp](https://github.com/libsidplayfp/libsidplayfp) (Leandro Nini, after Mihaly Horvath's cRSID), `src/builders/sidlite-builder/sidlite/` at commit `b2f27b5` (2026-10-27), with the patches below applied; `sl_defs.h` written by hand from `sl_defs.h.in` | GPL-2.0-or-later, `sidlite/COPYING` |
| `hermidi/` | HerMIDI's PIC12F509 firmware (Mihaly Horvath), from the SID-Wizard package | WTF Public License (SID-Wizard's) |

## Patches to libresidfp

The sources in `libresidfp/src` are the release with these applied. Each
patch is the diff against the untouched 1.2.3 tree, so updating the library
means unpacking the new release and applying them again.

- `patches/0001-residfp-per-chip-dither.patch` -- the filter's dither
  position is per chip. reSIDfp dithers the conversion of filter voltages to
  table indices from a 1024-value noise table, and the position in it was a
  single `mutable` index in the model config, which is a process-wide
  singleton. Its tables are built on six threads at once, so they came out
  slightly different on every run, and every chip in the process advanced
  the same index once per sample, so one plugin instance changed another's
  output. Now the table builders each use their own sequence, the
  integrators' constants use a fixed draw, and each `Filter` keeps its own
  index, which `State` saves and restores.
- `patches/0002-residfp-per-chip-6581-range.patch` -- the 6581 filter range
  and the old 2200 pF caps are per chip. Upstream both changed the shared
  model (its uCox), so setting them on one chip changed every 6581 in the
  process. They only affect two numbers (the integrators' current factor
  and the VCR multiplier), so each `Integrator6581` now carries its own pair
  and the big tables stay shared. Also: `Params` had no initial values, and
  `State::restoreState` applies its `filterRange6581` and `old6581caps`, so
  restoring a chip applied whatever the heap held -- to the shared model,
  which changed the filters of every other instance when a project loaded.
  The fields now have defaults (the range's is 19/39, reSIDfp's own uCox of
  20e-6, so the default sound is bit-identical to before).
- `patches/0003-residfp-voice-mask.patch` -- a voice mask per chip
  (`residfp::setVoiceMask`, bit 0-2 voice 1-3, bit 3 EXT IN), for ReSIDent's
  voice outputs: three more instances per SID, each hearing one voice. A
  masked voice reaches the filter as if its envelope were closed (signal 0,
  the envelope-0 DC), so the summer and mixer still see the real chip's
  number of inputs. Leaving it out of the sums instead picks reSIDfp's
  op-amp tables for the wrong input count and a single voice comes out as
  DC. The mask is not part of the saved state; all on is the unpatched
  sound, bit for bit. Unlike 0001 and 0002 this one is the diff against the
  tree with them applied (all three touch `Filter.h`).
- `patches/0004-residfp-resampler-ratio.patch` -- exact sample count and
  pitch. `SincResampler` (both passes of the default resampler) and
  `ZeroOrderResampler` (Fast) kept the clock-to-sample ratio and the phase in
  1/1024 of a cycle, truncated: reSIDfp made 0.02 % (48 kHz) to 0.05 %
  (96 kHz) too many samples and played 0.4-0.8 cent flat, and ReSIDent's
  engine dropped the excess, about five samples every half second at
  48 kHz. Now the phase has as many fractional bits as an `int` holds for
  the ratio (`resamplerPhaseBits`, 25-29 here) and the ratio is rounded: ten
  minutes come out within one sample at 44.1-192 kHz (`resident-enginetest`).
  `fir()` and the zero-order interpolation take the top 10 bits, as before.
  The state's layout is unchanged; a phase saved by an older build is read
  in the new unit, a one-time offset of less than a sample. Against the tree
  with 0001-0003 applied.

## Patches to SIDLite

The sources in `sidlite/src/sidlite` are libsidplayfp's with these applied,
in order; each is the diff against the tree before it (`patch -p2` inside
`sidlite/src`). The folder is `sidlite/` so the headers are included as
`"sidlite/SID.h"`: reSIDfp's `src/` is on the include path and has a `SID.h`
of its own.

- `patches/sidlite-0001-clock-ratio.patch` -- exact time and pitch. The
  CPU clocks per output sample were kept with four fractional bits
  (985248/48000 = 20.526 became 20.5), so a chip made 0.13 % too many
  samples -- ReSIDent's FIFO filled until the engine dropped them -- and
  played about 2 cents flat, since the oscillators step by the same ratio.
  The ratio is now kept with sixteen more bits and each sample takes the
  integer part plus a carry from the remainder, so the average is exact:
  ten minutes come out within one sample of its own rate, A4 within 0.001 cent
  (`resident-enginetest`). Also: the clock is a `double` (NTSC's 1022727 Hz
  is not a whole number in Hz either), and the sample rate an `unsigned int`
  -- as an `unsigned short`, 96000 wrapped to 30464 and passed the 48 kHz
  check. The sample counter is an `int`, and its step uses the fractional
  bits' constant instead of a literal 4.
- `patches/sidlite-0002-state.patch` -- `stateSize`, `saveState`,
  `restoreState`: every field that changes while the chip runs, listed once
  per class in a `state(V&)` visitor (`sl_state.h`) and written field by
  field. `ADSR`, `Filter` and `WavGen` hold pointers into the `SID`, so the
  object can never be copied as memory. Not the configuration (model,
  sampling), which the caller sets first.
- `patches/sidlite-0003-voice-mask.patch` -- `setVoiceMask` (bit 0-2 voice
  1-3) for the voice outputs. A masked voice runs on -- OSC3, ENV3, sync and
  ring modulation are unchanged -- and is heard as a closed envelope, the
  same rule as reSIDfp's 0003. All on is the unpatched sound.
