# SäureKiste -- to do

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
- **No copy or paste between pattern slots.** The bank holds sixty-four
  patterns and chains them, but filling slot 12 with a variation on slot 11
  still means writing it again or generating a new one. A copy, a paste and a
  clear-all are the obvious next three buttons.
- **The chain has no per-pattern repeat count.** A pattern plays once before the
  chain moves on. Playing one twice and the next one once is the arrangement
  people reach for first, and it would want a number per slot rather than one
  Chain Length for the lot.
- **Pattern length is global.** Steps applies to every pattern in the bank, so a
  chain cannot mix a sixteen-step pattern with a twelve-step one.
- **No playback direction.** Forward only; the reference plugins offer reverse,
  ping-pong and random. The chain has Random; the steps inside a pattern do not.
- **Pattern edits are not automatable or undoable.** They live outside the
  parameter table on purpose -- eighty parameters for sixteen steps would make a
  mess of the host's list -- so the host's undo does not see them. A hardware
  sequencer offers the same deal.
- **The demos are WAVs, not the MP3 set.** `demos/` is rendered straight out of
  `saeurekiste-render --all --demo-moves` and gitignored.
  `shared/tools/make-demos.sh saeure-kiste --render-arg --demo-moves` would
  produce the website set, but it wants a `presets/demo-descriptions.txt` first
  or every blurb falls back to the preset's own technical description.
- **The demo sweep is one shape for every preset.** Relative to each preset's
  own settings, so nothing loses its character, but Teeth only manages a 1.3x
  brightness swing because it already sits near the top of everything. A
  per-preset override file -- the same idea as `demo-descriptions.txt` -- would
  fix the handful that deserve a different move.
- **The Devil Fish's jacks are not modelled.** External audio into the filter,
  the audio Filter FM input, the Filter Out tap and the CV/gate sockets. The
  first two want an audio input port on the plugin and the third wants a second
  output, so this is a change to what shape of plugin this is rather than more
  DSP. Whittle's MIDI retrofits need nothing: a plugin has that already.
- **Fast and Slow sweep speeds are fitted to a description, not a circuit.**
  Whittle documents what the three modes *do* -- there is no Devil Fish
  schematic -- so Normal is the machine and the other two are the behaviour he
  describes, reached with a time constant and a charge law. If a real one ever
  turns up to measure, that is where to look first.
- **Nobody has run either build in a DAW.** The Windows binaries pass the whole
  self-test under wine and the window draws and resizes there, which is a lot
  more than nothing -- but wine emulates the API, it does not prove that Bitwig
  or Ableton will load the file. A real Windows machine has never seen it.
  `source ./winetest.env` at the repository root sets up the wine environment
  this was tested with; the file is untracked because its paths are specific to
  one machine.
- **The manual note in section 4 was stale and is fixed.** `make-manual.sh`
  derives the namespace from the CMake project name, not the folder, so it does
  work on `saeure-kiste`; `./shared/tools/make-manual.sh saeure-kiste` builds
  the PDF and `release.sh` ships it.

## 3. What the self-test does and does not cover

Seven fixes were backed out one at a time on 2026-09-18 to see which the suite
noticed. Six of seven were not caught on the first pass; after strengthening,
six are. What each strengthened check exists for is written beside it in
`tools/render.cpp`.

**The ladder input knee is the one with no test, on purpose.** Removing
`softKnee(osc * oscDrive, kLadderInputKnee)` changes nothing a test can assert:
the output stays finite and bounded either way (already checked), the response
to Overdrive stays monotonic either way (measured: 0.197 / 0.257 / 0.273 / 0.278
with it, 0.197 / 0.259 / 0.284 / 0.299 without), and at the stock setting it is
the identity in both — which the byte-for-byte preset comparison already proves.
It is a voicing decision about how the input pair saturates, not a contract, and
a test written to pin it down would only be asserting today's numbers.

## 4. Fixed, and worth remembering

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

## 5. Things that are deliberate, so they do not get "fixed"

- **The window is a fork** of `shared/src/gui/window.cpp`, not a change to it.
  The step grid cannot be expressed as panels of knobs, and a plugin that is not
  part of the suite, and lives on an orphan branch of it, does not get to change
  what all the others look like. `src/gui/seqwindow.cpp` is the shared file with
  the namespace changed plus the grid; diff them to see exactly what was added.
- **The bank rides in the preset as extra text lines.** The shared preset reader
  ignores keys it does not recognise, which is what keeps older presets loadable
  -- and is exactly the hook needed here. Nothing in `shared/` changed. Pattern 1
  keeps the original `seq_pitch` keys so a preset written before the bank existed
  still loads and one written now still opens in an older build; patterns 2
  upwards are `seq2_pitch` and so on, and an empty pattern is not written at all.
- **A preset load starts from the defaults.** Anything a preset file does not
  mention goes back to its default rather than keeping what the last preset left
  behind. That is what a preset means, and it is what stops a parameter added
  later -- the bank's three were -- from inheriting a stale value: loading a
  factory preset written before the bank, while pattern 12 was selected, would
  otherwise leave the sequencer on a pattern the preset had just emptied.
- **The Devil Fish controls default to the stock machine, and that is load
  bearing.** Whittle's manual has a table of where to leave each control to keep
  a Devil Fish sounding like a 303, and those are the defaults. It is what lets
  the whole preset library be compared byte for byte against the build from
  before the modification existed, which is the only real proof that adding it
  broke nothing. Do not "improve" a default here.
- **The state format restarted at version 1 with the rename.** The magic changed
  from 'T303' to 'SKST', so no blob any earlier build wrote can reach the reader
  -- the magic check rejects it before the version is even looked at. Four
  versions of history that nothing can produce were not worth keeping, and the
  range-migration path that went with version 4 went with them, because it could
  only ever have fired on a blob whose magic no longer matches.

  The reason it existed still applies before touching any range: a blob stores a
  parameter's **raw** value, which for a logarithmic one is a position on its own
  curve rather than a frequency, so widening the curve moves every saved value
  unless it is converted. Preset files are written in real units and were never
  at risk.
- **The chain is derived, not counted.** Which pattern plays is
  `chainPatternAt(mode, selected, length, cycle)` where `cycle` comes from the
  host's beat position, for exactly the reason the steps are re-read every block.
  A counter bumped at the end of each pattern would be simpler and would come
  apart the first time somebody looped a bar.
- **`install.sh` is self-contained** rather than calling
  `shared/tools/install-plugin.sh`, because that script derives a shell variable
  name from the folder and `saeure-kiste` is not an identifier. Renaming the
  folder to `saeurekiste` would let it use the shared script; nothing else
  would change. `shared/tools/make-manual.sh` is *not* affected -- it takes the
  namespace from the CMake project name -- and works on this folder as it is.
