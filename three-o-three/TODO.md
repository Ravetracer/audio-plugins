# ThreeOhThree -- to do

## 1. Listen to it against a real 303

Every measurement in `STATUS.md` is the build agreeing with a number in the
service notes, and a number agreeing with a number proves nothing about the
sound.

This is less urgent than it was. The four constants that were guesses are all
controls now -- Env Bias, Env Depth, Acc Sweep, Acc Build, Droop -- so a
reference recording would set their *defaults* rather than correct a mistake,
and anyone who disagrees with a default can already turn it. What is still
wanted is audio in `!dev/` and A/B pairs rendered from the `Dry Reference`
preset, which exists for exactly this.

## 2. Smaller things

- **The VCO waveforms are ideal.** A linear falling ramp and a hard square. The
  real integrator's ramp curves slightly and its reset takes a finite time.
  Worth measuring off reference audio rather than guessing, and it is the one
  remaining part of the signal path with no control over it.
- **Filter coefficients update every 8 samples.** Inaudible on the envelopes
  this instrument has, but a fast automated cutoff sweep would show it.
- **The sequencer has one pattern.** No pattern bank, no chaining, no
  copy/paste between slots. The machine had sixty-four patterns and a track
  mode. A bank would need somewhere to put it: the preset carries one pattern
  as five text lines today, and several would want a different arrangement.
- **No playback direction.** Forward only; the reference plugins offer reverse,
  ping-pong and random.
- **Pattern edits are not automatable or undoable.** They live outside the
  parameter table on purpose -- eighty parameters for sixteen steps would make a
  mess of the host's list -- so the host's undo does not see them. A hardware
  sequencer offers the same deal.
- **No VST3 and no Windows build.** Both are wanted, but not yet. The repository
  already carries what they need -- `setup-winbuild.sh` for the cross-built
  Cairo, `shared/cmake/mingw-w64-x86_64.cmake`, and the clap-wrapper patch in
  `shared/patches/` -- so this is a block of CMake and a test pass, not new
  groundwork.

## 3. Fixed, and worth remembering

**A looping host used to wedge the sequencer.** The first pass through the loop
played, the second turned into one endless slide that faded out over about
twenty seconds, and nothing short of reloading the plugin brought it back --
changing preset did not, because the damage was in the engine's held-note stack
rather than in any parameter.

The cause: the pending note-offs are absolute positions on the step timeline.
When the host looped, the position jumped backwards and those offs sat in the
future, so their notes were never released. One stale entry in the held stack is
enough -- every later note then looks like a slide, so nothing retriggers, and
the amplifier's 1.5 second sag runs unchecked at about -21 dB per pass.

**And then the fix for it broke the note at the seam.** Discarding what was
scheduled, rather than leaving it out of reach, cut the first note of every
second loop from 216 ms to about 40 ms. When the host's loop is a whole number
of patterns long, the position before the jump and the position after it are the
*same step*: the note across the seam has legitimately just started and has to
play on. The trace said it plainly -- `fire step 0 at pos 32.0000` immediately
followed by `JUMP 32.0320 -> 0.0320`.

The answer that is right in both cases is to **rebase**. Everything the
sequencer schedules is a position on the step timeline, so a jump moves all of
it -- pending note-offs and the record of what has already fired -- by the same
amount. It neither invalidates it nor leaves it stranded.

Two lessons, both worth keeping:

- **A slide is detected by a note arriving while another is held**, so anything
  that leaks a held note turns the instrument into one long slide and silences
  it. Any new path that sends a note-on must be certain its note-off happens.
- **A jump in the host's timeline is not an interruption.** It moves the frame
  of reference; it does not mean anything sounding should stop. Never assume the
  timeline only moves forwards, and never assume a jump means silence.

Both are covered by `--selftest`, and both tests were checked by putting the bug
back and watching them fail -- a regression test nobody has seen fail is not
evidence of anything.

## 4. Things that are deliberate, so they do not get "fixed"

- **The window is a fork** of `shared/src/gui/window.cpp`, not a change to it.
  The step grid cannot be expressed as panels of knobs, and a plugin that is not
  part of the suite, and lives on an orphan branch of it, does not get to change
  what all the others look like. `src/gui/seqwindow.cpp` is the shared file with
  the namespace changed plus the grid; diff them to see exactly what was added.
- **The pattern rides in the preset as five extra text lines.** The shared
  preset reader ignores keys it does not recognise, which is what keeps older
  presets loadable -- and is exactly the hook needed here. Nothing in `shared/`
  changed.
- **`install.sh` is self-contained** rather than calling
  `shared/tools/install-plugin.sh`, because that script derives a shell variable
  name from the folder and `three-o-three` is not an identifier. The same reason
  stops `shared/tools/make-manual.sh` working, so `docs/manual.md` is written
  out in full instead of generated. Renaming the folder to `threeohthree` would
  fix both; nothing else would change.
