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
