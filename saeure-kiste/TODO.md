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

- **The pattern map has no chromatic-run shortcut.** Every pad is learned one
  at a time, which is right for an arbitrary layout and tedious for the common
  one -- sixteen pads onto patterns 1 to 16 is sixteen clicks and sixteen
  notes. "Learn this note as pattern N and let the next notes follow" would be
  a handful of lines on top of what is there.
- **Nothing maps to anything but patterns.** Mute, a chain mode, the octave and
  GEN are all things a pad could usefully reach, and the map's format already
  has room: the negative half of the action byte has two values in it and 125
  spare.
- **The delay's stereo ratio is fixed at 3:2.** In Stereo routing the right-hand
  line runs at two thirds of the left, which is one of the simple ratios
  [Pirkle] 14.4.2 recommends and is a good default -- but it is a constant in
  `dsp/delay.h` rather than a control, which is the opposite of the rule this
  plugin is built on. It was left fixed because the DELAY panel has no room for
  a ninth column and the window is already 60 px wider than it was. A ratio
  chip stacked under Routing would be the cheapest way to expose it.
- **The delay has no filter in its feedback path.** Every repeat is a copy of
  the last one with the same spectrum, which is a digital delay and is what the
  sources print. A tape or analogue echo loses its top on each pass, and one
  shelving filter in the loop is the whole of the difference. There is no
  column for the knob either.
- **No factory preset uses the delay.** It is off in all twenty-seven, which is
  deliberate -- it is what makes them render byte for byte what they did before
  the stage existed -- but it means nothing in the library demonstrates it.
- **The VCO waveforms are ideal.** A linear falling ramp and a hard square. The
  real integrator's ramp curves slightly and its reset takes a finite time.
  Worth measuring off reference audio rather than guessing, and it is the one
  remaining part of the signal path with no control over it.
- **Filter coefficients update every 8 samples.** Inaudible on the envelopes
  this instrument has, but a fast automated cutoff sweep would show it.
- **Only ABL's formats are read.** *Pattern folder...* takes ABL2 text, ABL3
  text, the Reason JukeboxPatch and the `.param` sidecar, and nothing else.
  Anything new goes in `src/abl.cpp` beside them: the format is picked per
  file from its content rather than per import, so a folder may hold a
  mixture, and the shared tail -- octave fitting, knob mapping, preset text --
  is reached by filling one `KnobList` and one vector of `RawStep`.
- **A `.param` with no `.pat` beside it is skipped.** It is knob settings with
  no pattern, so importing it would make a preset with somebody else's
  sequence in it. Turning it into a preset over the *default* pattern would be
  defensible and is not done, because nothing in the reference library needs
  it.
- **ABL's `Highpass`, `Pregain`, `Noise Level`, `Reso Trim`, `Gate Trim`,
  `Detune`, `Bass Boost`, `VCF Trim` and `Treble Boost` are dropped.** The
  `.param` sidecar names all of them. Some have no counterpart here at all;
  `Detune` and `Reso Trim` arguably map onto Tuning and Res Range but not at
  the same scale, and a wrong mapping is worse than none.
- **An imported preset's drive type is whatever the default is.** ABL's
  `DistType` is a normalised index into *its* list of models and this plugin's
  seven are different models in a different order, so mapping the number would
  be worse than not mapping it. A named table -- ABL's mode N is this plugin's
  Type X -- would need somebody to sit and listen to both.
- **No clear-all for the bank.** COPY and PASTE landed in 0.3.0 and DEL, which
  empties the selected slot, in 0.5.0; emptying the whole bank in one go still
  means DEL on each pattern, or loading Blank Slate.
- **The chain has no per-pattern repeat count.** A pattern plays once before the
  chain moves on. Playing one twice and the next one once is the arrangement
  people reach for first, and it would want a number per slot rather than one
  Chain Length for the lot.
- **Pattern length is global.** Steps runs 1 to 128 as of 0.5.0, but it applies
  to every pattern in the bank, so a chain still cannot mix a sixteen-step
  pattern with a twelve-step one. Making it per pattern is the next step and
  the reason it was not taken now: the length would stop being a plain
  parameter -- not automatable, and another field in the state blob and the
  preset text -- and that is a bigger change than widening a range. It was
  deliberately left for later.
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

## What 0.3.0 added, and what is not proven about it

- **The Windows drag has never dropped anything.** The OLE half of
  `src/gui/dragfile.cpp` compiles, the window it belongs to opens under wine and
  draws its MIDI button, and that is all that is known. The X11 half is verified
  end to end against a test XDND target -- the drop arrives and carries the
  file URI -- and that harness is not in the repository because it needs a live
  X server and a window to drag from. It is fifty lines: a window with
  `XdndAware` set that accepts anything and prints the `text/uri-list` it is
  given, driven with `xdotool` from the MIDI button to the target. Put the two
  windows on the same monitor and do not let them overlap, or the pointer lands
  on the wrong window and nothing happens for a reason that has nothing to do
  with the code.
- **The note output port is untested in a DAW**, like everything else here. The
  self-test checks what it emits: balanced notes, an accent louder than the
  threshold, the overlap a slide is made of, and nothing at all in MIDI mode.
- **The generator still writes one octave either way.** The lane now takes two,
  by hand; Octaves, the generator's density control, does not reach them.

## What 0.5.0 added, and what is not proven about it

- **The file chooser has only been run on Linux.** `src/gui/filedialog.cpp`
  asks zenity or kdialog there, found in PATH by hand, and calls
  `GetOpenFileNameW` / `GetSaveFileNameW` on Windows. The Windows half
  compiles and links against comdlg32 and has never been opened. The browser
  works without any chooser at all -- EXPORT and IMPORT use the plugin's own
  packs folder, and EXPORT AS... is hidden when there is nothing to open -- so
  the worst case is one missing button.
- **Nothing has imported a pack somebody else wrote.** The round trip is tested
  both ways, in the self-test on the format and by hand through the window, but
  every pack either end has seen was written by this build.
- **The drive models are equations, not measurements of circuits.** Each one is
  out of [DAFX] chapter 4 or [Pirkle] chapter 19, which is a far better source
  than the fourteen invented shapes they replaced -- but a published model of a
  triode is still not a measurement of one. Nobody has put a real Fuzz Face or
  a real valve preamp next to these. What the self-test pins down is that each
  model is bounded, distorts, keeps the character its source describes, and has
  a harmonic distribution no other model shares at the same THD.
- **The oversampling is two times and gentle.** Both books ask for it and there
  was none at all before, so this is a large improvement over nothing -- but the
  filters are two cascaded state-variable lowpasses at 0.45 of the base rate,
  24 dB/octave, not the steep half-band filters [Pirkle] chapter 22 uses at 4x.
  Hard models at high drive still fold a little. Soft Clip is not oversampled at
  all, deliberately, so that presets written before the models existed render
  what they always did.
- **Six of the seven models carry a hand-measured loudness trim.** They are one
  number each, taken through the whole plugin on the Machine Running preset at
  55 % drive so that all six land within a decibel of each other. They are not
  wrong -- a comparison between two distortions at different levels is a
  comparison of level -- but they are fitted to one preset and one drive
  setting, and a proper loudness match would measure the model rather than the
  render.
- **A 128-step pattern has never been through a DAW's loop.** The sequencer is
  a pure function of the host's beat position and the length is only a number
  in that function, so there is no reason for it to behave differently at 128
  than at 16 -- but nobody has watched it.

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

**The sequencer used to play on its own after browsing presets.** Reported from
a Windows session, 0.2.0: sequencer mode worked, a preset was saved, presets
were browsed, and then the sequencer presets started playing with no key pressed
and no transport running. Reloading the plugin cleared it; changing preset did
not.

The cause is which side of the plugin holds a key. In Sequencer mode a MIDI note
does not sound, it transposes, so it is held in the plugin's own transpose stack
rather than in the engine's held stack -- and which stack a note event goes to
is decided per event, by reading the Mode parameter at the time. Change Mode
between a note-on and its note-off and the two do not match: the on went to one
stack and the off arrives at the other, so the first keeps the key for ever. A
leftover transpose entry means the plugin believes a key is held, and a held key
is what runs the pattern without a transport -- by design, so a line can be
auditioned without putting the song into play.

Browsing presets is enough to trigger it, because a preset carries the Mode:
nineteen of the twenty-seven factory presets are in MIDI mode and eight are in
Sequencer.

Three lessons:

- **A parameter that changes how an event is routed has to be watched, not just
  read.** `updateSequencerClock()` now compares Mode against what it was on the
  previous block and drops everything held when it moves. Polled rather than
  hooked onto the parameter event, because the mode also moves without one:
  loading a preset and loading the host's state both write the parameter table
  directly from the main thread.
- **`reset()` means nothing is held**, including the transpose stack, which it
  did not clear.
- **Taking the keyboard loses key releases.** When the save field opens, the
  window grabs the keyboard on X11 and takes the focus on Windows, so a note
  played from the host's own computer keyboard gets its note-on and never its
  note-off. The window now tells the plugin through `GuiDelegate::
  guiKeyboardTaken()`, and the plugin drops its transpose stack -- but not the
  engine's held notes, which a host sequencer still releases properly.

The first of the three is covered by `--selftest`, checked the same way: the
test was watched failing at an RMS of 0.298 with nothing pressed before the fix
went in. The other two have no contract to assert from outside the plugin.

**A key press did not start the pattern over.** Found in the same session. With
no transport the pattern free-runs off a held key, and pressing another one left
the position exactly where it was: a key was a transposition and nothing else,
so there was no way to decide where the line began. It now restarts at step one.

Only when it is free-running, never when the host's beat timeline is driving the
position: there the position belongs to the song, a key is a transposition, and
a pattern that jumped back to step one in the middle of a bar would be out of
step with the project -- and the next block would drag it back anyway.

The test times it rather than measuring a level, so it cannot pass by accident:
the key is pressed 80 % of the way through a step, where nothing is due. A
restart puts a note onset on the key; without one the next onset is the step
boundary, a fifth of a step later. It was watched failing at 46 ms before the
fix and reads 0 ms after it. The step length is measured from a run with no key
press in it rather than assumed, because the free-running tempo is whatever
transport the plugin last saw.

**And the tempo it free-ran at was the wrong one.** Reported from the same
session: changing the project tempo did nothing, the pattern went on at the
speed it had when the plugin was instantiated.

`mTempo` was read inside the `IS_PLAYING` branch. A stopped DAW still hands over
its tempo -- only the playing flag goes away -- and a pattern running off a held
key is the one case where the plugin works the step length out for itself, so
the audition ran at whatever tempo the plugin had last seen while the host was
rolling, or at 120 if it never had been. The tempo is now read whenever the
transport carries one.

The test renders a held key against a stopped transport at two tempos and
measures the gap between note onsets, which has to be `60 / bpm / 2` seconds at
the 1/8 rate. It read 230 ms at both before the fix and 300 / 187 ms after it.

**Two things fell out of writing those tests, both worth keeping:**

- **Reconciling a mode change once per block was the wrong place for it.** The
  first version compared Mode against the previous block from the sequencer
  clock, which runs after the block's frame-0 events -- so a host that loads a
  preset and plays a note in the same block had the note released a moment
  after it arrived. It is now done as the parameter event is handled, with the
  poll kept at the *top* of `process()` for the paths that move the mode with no
  event at all. The suite caught it, at a check that had nothing to do with
  either: "all parameters mid-range: still audible".
- **A test that leaves a key down poisons the next one.** The sequencer tests
  hold a key to run the pattern, and the first drafts never released it, so
  later checks ran with the sequencer already going and measured numbers that
  had nothing to do with what they were testing -- an "output stays bounded"
  check read 1.49 that way. They release their keys now, and the
  all-parameters sweep resets first.

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
