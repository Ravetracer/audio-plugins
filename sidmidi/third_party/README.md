# Third-party code

| Folder | What | Licence |
|--------|------|---------|
| `libresidfp/` | reSIDfp from [libresidfp](https://github.com/libsidplayfp/libresidfp) 1.2.3 (Dag Lem, Antti S. Lankila, Leandro Nini), with the patches below applied | GPL-2.0-or-later, `libresidfp/COPYING` |
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
  (`residfp::setVoiceMask`, bit 0-2 voice 1-3, bit 3 EXT IN), for SidMidi's
  voice outputs: three more instances per SID, each hearing one voice. A
  masked voice reaches the filter as if its envelope were closed (signal 0,
  the envelope-0 DC), so the summer and mixer still see the real chip's
  number of inputs. Leaving it out of the sums instead picks reSIDfp's
  op-amp tables for the wrong input count and a single voice comes out as
  DC. The mask is not part of the saved state; all on is the unpatched
  sound, bit for bit. Unlike 0001 and 0002 this one is the diff against the
  tree with them applied (all three touch `Filter.h`).
