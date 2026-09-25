# SäureKiste -- status

Version 0.15.2. Linux and Windows, CLAP and VST3. 65 parameters, 30 presets,
builds clean, self-test passes with no failures across 311 checks, and
`tools/check-instances.sh` passes with two editors open at once.

Six of seven fixes were confirmed by backing the bug out and watching the suite
fail; the seventh has no contract to assert. See `TODO.md` §3.

0.15.2 makes the continuous controls glide. Every one of them jumped straight
to each new value, so host automation (a staircase) and a dragged knob (one
value per block) put a step into the cutoff every few milliseconds, heard on a
fast sweep as a zipper or a bubbling. `AcidEngine::setParams()` now takes
switches and times at once and glides the rest through a 20 ms one-pole on the
control block -- the fields are listed in `kGlided` in `acid_engine.cpp`,
frequencies in octaves. The drive stage's Drive and Bias glide on a 64-sample
grid and it is rederived only when they move, because its level match costs up
to 30 us. The delay glides Mix, Feedback and Width per sample and Enable off
fades the mix. The first values after `prepare()` or `reset()` snap, so every
factory preset renders bit for bit what 0.15.1 rendered, at 44.1, 48 and
96 kHz. Seven new checks; the three that assert the glide were confirmed by
putting the jump back and watching them fail.

0.15.1 gives the help line a fixed three-line area under the controls. In
0.15.0 a long tip or preset description wrapped upward over the controls
above it; now it wraps downward into its own space and is cut with an ellipsis
after the third line. The window is 26 px taller, collapsed and expanded.

0.15.0 makes CHAIN and REPEAT per pattern, adds a semitone transpose, and wraps
the help line.

**Per-pattern chains.** Each pattern carries its own chain mode and repeat
count, in the bank rather than the parameter table: `mChainMode[]` and
`mChainRepeat[]` in the plugin, `seqN_chain` / `seqN_repeat` lines in the
preset (pattern 1 always writes both, which is how a reader knows the preset
has them), and state version 4 appends them after the pattern map. The bank's
CHAIN and REPEAT rows edit the pattern on screen: the window still draws them
as the old `chain_mode` / `chain_repeat` parameters, and the plugin's
GuiDelegate redirects those two ids to the shown pattern's slot without telling
the host. The two parameters are kept for their ids and flagged
`CLAP_PARAM_IS_HIDDEN`; an old preset or blob without per-pattern data copies
them into every pattern, and so does a host event that *changes* one, so old
automation and `render --param chain=...` still work. LENGTH stays global. The
rows are now CHAIN, REPEAT, LENGTH, TRIGGER.

`chainPatternAt()` walks the chain from the start pattern, carrying a
`ChainWalk` from call to call: forward playback advances one cycle at a time,
so editing a pattern's chain changes what comes next rather than the history,
and a seek backwards walks again from the top, so it is still a function of the
beat position. The walk starts from the song's top when the sequencer starts
and from the takeover cycle when a pattern is picked while it runs -- which
means a picked pattern now plays next under Next, where 0.14.0 offset the chain
by it instead. Next from a pattern outside the chain now goes to pattern 1.

**Transpose.** TRANSP - / + in the grid's title row moves every note of the
shown pattern a semitone, octave switches included. `transposePattern()` in
`pattern.cpp` clamps at C-2 and B+2 and keeps a per-pattern `TransposeMemory`
of each step's unclamped pitch, so N presses up and N down restore a pattern
that was flattened against an edge. The memory resets when any pitch changes
by other means; flags do not reset it. Main thread only, not persisted.

**Layout.** The grid's title row is in sections with a divider between each:
OCT, TRANSP, the two shifts, MIDI, MAP, and the seed with GEN, right-aligned by
`place()` in the layout. CLEAR is gone (DEL in the bank does the same). PREV and
NEXT moved into the bank, in a row of their own under the cells. A semitone row
is 15 px rather than 13, so the pane is 315 px and the window 24 px taller
(739, 1011 open); `make-screenshots.sh` has the new coordinates.

**Help line.** Long tips wrap at the window's width. The last line stays where
a one-line message always was and extra lines grow upward on the background, so
the window height did not change. The shared window has the same one-line help
and has not been changed.

0.14.0 adds TRIGGER, under CHAIN and LENGTH in the bank, for when a pattern
selected while the sequencer runs takes over. At End is 0.13.0's behaviour and
the default. Instant switches on the next step and keeps the place in the bar,
which is how every version before 0.13.0 behaved. Restart switches on the next
step and plays the new pattern from its first: the audio thread keeps
`mStepOffset`, the step the restart landed on, and counts the step index and
the chain cycle from there. A stop or a transport jump clears it, so a seek
still lands on the step the song says. The bank cells are 22 px rather than 24
to make room for the extra rows. The self-test checks both new modes note for
note against reference renders.

It also adds REPEAT (`chain_repeat`, 1 to 256, default 1): how many cycles each
pattern in a chain plays before the chain moves on. The chain is now worked out
from the pass, `cycle / repeat` with floor division, so it is still a pure
function of the beat position. The bank cells are 21 px and the controls 18 px
to fit a fourth row.

And the window follows a running chain. `seqShownPattern()` is the playing
pattern while the chain mode is not Stay and the sequencer runs, the selected
one otherwise; the grid, the bank cursor, GEN, the MIDI drag and PREV/NEXT all
go through it. Before, the grid stayed on the selected pattern and the playhead
disappeared as soon as the chain moved on. `guihost --note key@start+hold`
holds a note long enough to watch it.

The switch test in the self-test used to run against an empty pattern 2, so
"takes over on the next pattern's first step" compared silence with silence. It
now loads its own two-pattern bank and puts the state back afterwards.

0.13.0 makes a pattern change wait for the end of the pattern.

Selecting a pattern while the sequencer runs -- a bank click, PREV and NEXT, a
mapped pad in Live mode, or host automation -- no longer switches on the spot.
The playing pattern runs to its last step and the new one starts on its first,
which is what the machine did and what lets a change be played by hand without
hitting the bar line to the sample. Until then the waiting pattern's cell and
the PATTERN title flash. The audio thread keeps the pattern it took as
`mActivePattern` and only re-reads the Pattern parameter on step 1 of a cycle,
or on whatever step the sequencer starts on; with the sequencer stopped a
selection still takes effect at once. A chain counts from the taken pattern, so
it re-bases at the same boundary. The self-test checks it note for note against
a reference render, and fails with the change backed out.

Automation placed mid-pattern now waits for the pattern's end like everything
else. Automation on a pattern boundary lands exactly as before.

0.12.0 rearranges the window and adds a RESET button to the preset bar.

The front of the window is the machine's own panel and nothing else now: the
Accent knob is back in the VCF row where the hardware prints it, sixth after
Tuning, Cutoff, Resonance, Env Mod and Decay, and the three controls that were
standing in its place -- Tracking, Overdrive and Filter FM -- moved into a
VCF MOD panel in the collapsible half. What shapes an accent rather than
applying it (Accent At, Sweep Time, Sweep Speed, Accent Hold) joined the mods,
which is where the four accent numbers already were, and the ACCENT panel is
gone.

A row no longer stretches its last panel out to the right margin. That is a
change to the forked window (`layoutRow` in `src/gui/seqwindow.cpp`) and not to
the shared one: the collapsible rows are nowhere near full, and a SLIDE panel
544 pixels wide around one knob read as a panel with something missing from it.
Rows now end in background.

**RESET** puts every parameter back to its default, which for this instrument
is the machine before anybody was inside it -- no mods, no modification, no
drive and no delay. It arms on the first click (the button reads SURE?) and
acts on the second, because there is no undo for it inside the plugin. The
pattern bank, Mode, Rate and the selected pattern are left alone; they say what
is playing rather than what it sounds like. The keep list is
`kResetKeep` in `src/gui/gui.cpp` and the window takes it from the spec, so a
parameter added later is reset unless it is named -- the safer default of the
two.

The manual's screenshots were regenerated for all of it, and
`tools/make-screenshots.sh` carries the new crop coordinates.

0.11.0 gives the three pedal-derived drive models presets of their own, which
the library did not have: every one of the twenty-seven was written before they
existed and every one of them was on Soft Clip.

| | |
|---|---|
| **Stompbox** | Germanium at 70 % drive, where its gain corner sits up around 700 Hz. Written low with two octave jumps a bar, because the point of the model is that both octaves keep their weight. Tone wide open -- the model has a top-end roll-off of its own. |
| **Dig In** | Crunch at 45 %, which puts its 1.6 V clipper right between an accented step and a plain one. Seven accents in sixteen, and the plain steps left quiet enough to hear that they are not distorting. The distortion is played rather than set. |
| **Halo** | Lead at 30 % Dist Mix, underneath the line rather than instead of it. On its own the model is a thin saturated buzz with nothing under 200 Hz; mixed in, it is a band of upper harmonics over the filter's own output. Four tied runs in the bar, because it flatters long notes. |

They are 1 to 2 dB under the rest of the library rather than level with it, and
that is the models rather than the presets: the seven book models are matched
to each other and Soft Clip -- which every other preset uses -- is deliberately
the loud one. Their Volume sits higher to make most of it back.

0.10.0 also imports MIDI files, and imports one file as readily as a folder.

**Standard MIDI Files** join the three shapes of `.pat` the browser already
read. Format 0 or 1, every track merged, quantised to sixteenths, one pattern
per file. The reader is in `src/midifile.cpp` beside the writer that was
already there, which is the point of putting it there: **the three conventions
are read back exactly as they are written.** A velocity at or above the accent
threshold is an accent, a note still sounding when the next step begins is a
slide, and a step bracketed by CC1 carries a vibrato.

That symmetry is the test, and it is the strongest one in this file because it
cost nothing: a pattern is written out with `patternToMidiFile()` and read back
with `parseMidiPattern()`, and every step has to come back as it went --
pitches, octaves, slides, accents and vibratos. Breaking the slide rule or the
accent threshold fails it, both confirmed by breaking them.

**A file that repeats is reduced to what repeats.** The smallest period that
divides the file's length and reproduces it exactly becomes the pattern, so
four bars of a one-bar figure import as one bar; a file that does not repeat
exactly is left at its full length. Over the sixty-file library this was
written against: 29 files reduce to 16 steps, 7 to 8, 4 to 4, 2 to 32, and 18
are genuinely four bars of different material and are kept whole.

**One file or a folder.** `importPatternFiles()` takes either; a single file is
a folder of one and lands on the shelf its own directory would have made, so
importing one pattern now and the rest of its folder later puts them together.
The browser's IMPORT menu grew a "Pattern file..." row beside "Pattern
folder...".

`src/abl.{h,cpp}` is now `src/patternimport.{h,cpp}`, because a file called
`abl.cpp` that reads MIDI is a lie. Everything the formats have in common once
they are a list of steps -- the octave fitting, the packing, the parameters
that describe the pattern -- is one `buildImport()` they all call.

One thing the round trip caught before anything else did: the vibrato flag was
read by asking what CC1 was doing at the middle of the step, and with the gate
at a half the controller's release landed exactly on the point being sampled
and won. The rule is now the same one the notes get -- quantise the *up* to its
nearest step -- which is both simpler and symmetric with everything else here.

0.10.0 adds the ninth and tenth drive models, and they are one pedal.

**Crunch and Lead** are the two channels of the BOSS SD-2 Dual OverDrive,
modelled from its service notes (May 1993, First Edition) -- the same kind of
document the rest of this instrument is built from, measured waveforms and all.

They are two models because the pedal is two *circuits*. Its MODE switch does
not re-voice one chain, it selects between two complete ones, each with its own
gain, tone and level pot section; the pots are dual-gang for that reason. Two
entries in the Type list cost nothing and keep a switch off a panel with no
room for one.

**Crunch** is one stage. Op-amp 2a non-inverting, the gain leg VR1b (250 k) and
R36 (10 k) over R37 (680 R) with C28 (4.7 uF) beneath it, and the clipper in
the feedback loop: D7 (an LED) and D6 (silicon) in series one way against D4
(an LED) alone the other. Those thresholds are about 2.2 V against 1.6 V, where
a green-box overdrive clips at 0.6 -- which is the whole difference between
crunch and distortion. It stops distorting entirely as a note decays, and the
suite measures that: its quiet-signal ratio is 0.00.

**Lead** is three gain stages with two clippers between them. 3b
non-inverting (up to 54, cornering at 413 Hz), 3a inverting (another 67, with a
884 Hz lowpass across it), then D14 and D15 -- red LEDs, shunt to ground behind
R52 -- then 4a with asymmetric silicon in its feedback loop, then 4b. Six
thousand times the gain before the first clipper. It is the most
frequency-selective model in the set by a distance: its THD at 80 Hz is 0.27 of
its THD at 1 kHz, against Germanium's 0.89 and 1.00 for a plain clipper.

**The service notes' own scope traces are the test.** The appendix photographs
the output of both modes fed a 200 Hz square at 20 mV peak to peak, and the two
pictures differ in shape rather than in spectrum: Crunch keeps a tall leading
spike and sags towards the next edge, Lead is flat and ringing. The suite
drives both models with that same square at that same level and measures crest
factor -- Crunch 1.78, Lead 1.09.

That sag is not a fitted curve, it is C28 and R37: a 3.2 ms time constant
against a 2.5 ms half-cycle, so the stage's gain is still falling when the next
edge arrives. Take the shelf out and three checks fail, including the
distinctness one, which was confirmed by taking it out.

**Two bugs the suite found, and one hole it had.**

Lead divided by zero at the bottom of the Drive knob: a pot at zero shorts the
feedback resistor, which is a follower in the circuit and `1/0` in the model,
and the first Newton step was then `0 * inf`. **The suite did not catch it**,
because the finiteness check measured every model at one drive setting -- 0.7
-- and the failure was at 0.0. It now sweeps the whole of Drive and both ends
of Bias. That is the real fix; the model's floor is the wiper and track
resistance a real pot still has at its end stop.

The asymmetric diode pairs needed the `-1` of each Shockley term, which the
symmetric germanium pair had not: without it an unmatched pair carries current
at rest and the clipper's node sits off zero.

**Input reference.** The one fitted number in either channel, and it has to be
per channel: Crunch's single stage starts at a gain of 15.7 and needs a hot
input before its 1.6 V clipper does anything, while Lead has 430 times through
it before its own pot is touched. The pedal has a separate GAIN pot per mode;
this plugin has one Drive knob, so where each channel starts on it is set here
instead. 0.12 V and 0.02 V per unit, either side of the notes' own 20 mV test
signal.

**Levels.** Measured on Machine Running at 55 % drive as every other trim was:
1.9 dB and 2.5 dB under the set, for the same reason Germanium was -- their
gain legs leave the bottom of the band alone and the RMS matching measures a
curve rather than a filter. Trims 1.24 and 1.33; both now land within 0.1 dB.

**On the names.** "Crunch" and "Lead" are the pedal's own words for its two
modes, printed on its panel, and they describe what each one is -- which is
the rule the rest of this list follows. The maker's name is not on the panel
here, and is in the repository and the service notes as usual.

0.9.0 adds an eighth drive model, and it is the first one taken from a circuit
rather than from a book.

**Germanium** is the MXR Distortion+, modelled component by component from its
schematic and from ElectroSmash's component-level analysis of it. The pedal's
own name is not on the panel -- see the note at the end of this section.

It earns a place in a set that was deliberately capped at seven because it is
the only model here **whose gain is not the same at every frequency**, and on a
bass instrument that is the difference that matters most:

- C3, 47 nF, sits in the non-inverting stage's lower leg, so the stage has
  unity gain at DC and its full gain only above a corner. The DISTORTION pot
  sets both at once: 6 dB and 3 Hz at the bottom of the travel, 46 dB and
  720 Hz at the top. The harmonics get lifted forty-odd decibels and the
  fundamental does not, which is why the model stays tight down low where every
  other one in the set thickens.
- The 741's gain-bandwidth product is 1 MHz, so at full gain the stage cannot
  follow above 4.7 kHz. That is a datasheet number rather than a taste
  decision, and it is what turns the shelf into a hump.
- The clipper is a *shunt*: 10 k in series into two anti-parallel germanium
  1N270s and the output pot. It cannot be written as a transfer curve with a
  threshold -- the diodes load the signal rather than clamping it -- so the
  model solves the node equation with three Newton steps per sample instead.
- Bias is the diode-array modification the schematic itself suggests beside D1
  and D2: a second diode in series on one side, doubling that side's forward
  drop. Centred is the matched pair the pedal shipped with.

**The mid hump is a test, not a setting.** The published analysis measures a
hump at 1.5 kHz. Nothing in the model is a 1.5 kHz anything -- it is what the
shelf climbing from 720 Hz and the op-amp giving out at 4.7 kHz make between
them. The self-test sweeps the model small-signal and requires the peak to land
between 1 and 2.5 kHz with both sides at least 6 dB down; it measures 1.5 kHz,
-20 dB at 60 Hz and -10 dB at 9 kHz. Removing the bandwidth limit fails the
top-end check and removing the shelf fails three checks at once, both confirmed
by doing it.

**The distinctness test needed a new axis, and that is a finding rather than an
accommodation.** Germanium first measured 0.127 against Soft Clip, under the
0.15 bar -- because the test compared harmonic distributions from a single
1 kHz sine, and at one frequency a diode shunt clipper is just another soft
clipper. What it could not see is the only thing Germanium is *for*. The
measurement now also takes each model's THD at 80 Hz against its THD at 1 kHz
and puts that ratio in the distance. The bar is unchanged at 0.15, the previous
closest pair moved from 0.17 to 0.17, and Germanium now sits at 0.170.

**Levels.** Measured through the plugin on Machine Running at 55 % drive, as
every other model's trim was: Germanium came out 3.7 dB under the rest of the
set, because the RMS matching measures a curve and cannot see what the 47 nF
leg takes out of a line that is mostly fundamental. Its trim is 1.52 and it now
lands within 0.2 dB of the others.

**On the name.** The panel says "Germanium", not the pedal's. The existing
model names describe what a model *is* rather than whose it is, the manual
describes it without naming it, and the trademark belongs to somebody else --
but the schematic, the analysis and this file all name it outright, exactly as
the repository names Roland where the manual does not.

0.8.0 imports patterns written by another instrument.

**Reading the pattern formats** written by AudioRealism Bassline. The browser's
IMPORT menu grew a second entry beside "Other file...": "Pattern folder..."
asks for a directory rather than a file, and every `.pat` under it becomes a
preset. One shelf per source directory, named after it, so pointing at a
library of eighteen folders imports eighteen shelves in one go rather than one
file at a time.

Three shapes, all of them found in one library and all of them read:

| | |
|---|---|
| **ABL2 text** | four columns, `note gate slide accent`, the note spelled `c#3` |
| **ABL3 text** | six columns, `pitch down up slide accent gate`, the pitch a semitone offset |
| **Reason JukeboxPatch** | XML under the same extension, the same per-step values as `dpitch`/`ddown`/`dup`/`dslide`/`daccent`/`dgate` plus `dpatternlength` |

The format is picked per *file* from what is in it -- the XML by its first
character, not by its extension -- so one folder may hold a mixture.

A **`.param` sidecar** beside a `.pat` is that pattern's knobs on their own,
one `"Reso Trim" = 0.50000000` per line. It carries ABL's whole front panel
where the text header carries a subset, so it is applied after the header and
wins. It holds no pattern and is never imported on its own. One file in the
library has no knob header at all and only its sidecar, which is what the
pairing is for.

What comes across, and what does not:

- The notes, their octaves, the slides and the accents, exactly as the file has
  them. A step with its gate off is a rest.
- The header's knob settings -- Tune, Cutoff, Resonance, Env Mod, Decay,
  Accent, Waveform, Volume, and ABL3's Drive and Distort -- read as normalised
  positions through this plugin's own ranges. That puts an imported preset in
  the right area rather than on the same number: the two instruments model the
  same machine but do not share knob curves. Volume is the exception and is
  anchored rather than swept, so ABL's own default arrives as this plugin's.
- ABL's tempo, its high-pass and its distortion model are dropped. The first
  belongs to the host here, and neither of the others has a counterpart whose
  setting would mean the same thing.
- A file holding several patterns fills that many slots of the bank, up to the
  bank's sixty-four.
- A line written outside the +-2 octaves a step can carry is moved as a whole
  with Pattern Oct, chosen so the steps sit nearest their own middle. Across a
  library of 380 files not one step had to be pulled in.

ABL3's column order is the one thing here that could not be read off the
format: it is `pitch down up slide accent gate`, fixed by the JukeboxPatch,
which writes the same per-step values under those names in that order. The
self-test holds it in place by reading the same four steps in all three shapes
and requiring identical output -- confirmed for both the text and the XML path
by swapping two fields and watching the check fail.

Across the 381-file library every file now reads: 380 text patterns, one
JukeboxPatch, one sidecar applied, 444 patterns, and not one step whose octave
had to be pulled in.

0.7.1 fixes a crash in GEN, and it is the kind worth writing down because the
test suite watched it go past.

Widening the generator seed to the whole of a 32-bit word in 0.7.0 left
`nextGeneratorSeed()` computing `x % (maxSeed + 1)`. At the top of the word
that addition wraps to zero, so the modulo is a division by zero: pressing GEN
took the host down with SIGFPE, on the first press, every time.

**The suite had a test for GEN and it passed.** It ran sixty-four presses and
checked the range, the repeats and the determinism -- against
`const uint32_t kMaxSeed = 9999`, a literal that had been correct until the
range moved and was never touched again. It was testing a plugin that no
longer existed. The maximum now comes out of `paramTable()`, so it cannot
drift again, and two checks sit beside it for the top of the word
specifically. Both were confirmed to crash the suite with the fix backed out.

The lesson is the general one: a constant copied into a test is a second
source of truth, and the copy does not fail when the original changes -- it
keeps passing.

0.7.0 makes a long note a thing that can be drawn, adds a delay, and gives the
sequencer a mode meant for playing rather than for writing.

**Live mode.** A third value of Mode, beside MIDI and Sequencer. In Sequencer
mode a held key transposes the pattern, which is the machine's own behaviour
and is the wrong thing entirely for a set played off a pad controller. In Live
mode the keys belong to the bank instead:

- Any key the map does not know about simply runs the pattern, at the pitch it
  was written at.
- A mapped key selects its pattern, or steps the bank one either way. Stepping
  **clamps** at the first and the last rather than wrapping, which is what a
  pad wants: nothing happening at the end of the bank is better than landing on
  pattern 1 in the middle of a bar.
- **Pattern Oct** moves the whole running pattern by octaves, since the
  keyboard is no longer doing it. It is a parameter like any other, so it
  automates, and it works in the other two modes as well, where it adds to the
  held-key transpose.

The map is learned from the window: **MAP** turns the bank into the pad layout,
a click picks the target -- a pattern, or the PREV and NEXT buttons -- and the
next incoming note is bound to it. A right click clears one, and a right click
on MAP clears the lot. Each cell then shows the note that selects it.

**The map is in the state blob and deliberately not in presets.** A pad layout
belongs to the rig rather than to the sound, so browsing presets mid-set must
not silently remap the controller. State is at version 3 for it; a version 2
blob stops before the map and reads as one with nothing in it.

**The generator seed now spans the whole of a 32-bit word** -- 0 to
4,294,967,295, where it was 0 to 9999. A Unix timestamp fits in it, which is
what makes "seed it from the clock and never hear the same line twice" an
actual thing you can do here. Nothing had to be migrated: a stepped parameter
stores its real value, so a project holding seed 1234 still reads 1234. The
seed is typed as well as stepped now, because a range that wide cannot be
reached with two buttons.

One fix in `shared/` came out of it -- `paramValueToText` formatted a stepped
parameter with `%d` through an `int`, which overflowed above 2^31 and printed
"-2147483648". **That is a Verdalis bug too until proven otherwise**; see the
root CLAUDE.md.


**The delay.** The second stage on this instrument that the machine does not
have, built the way the drive stage was: out of the literature, with the source
named beside each part of it in `src/dsp/delay.h`. It is off by default, so no
preset that already existed sounds any different.

- Free or tempo-synced, 20 ms to two seconds or a thirty-second to a half note.
- Mono, Stereo (two lines at 3:2) or Ping-Pong, with a mid/side Width over the
  repeats and nothing else.
- Feedback to 130 %. Past unity the source's own stability condition is broken
  on purpose and the loop is held up by the suite's soft clipper instead, so it
  self-oscillates into a drone rather than overflowing. Thirty seconds at the
  top of the knob peaks at 1.000 and stays finite; the self-test asserts it.
- The plugin no longer tells the host it may sleep while the delay is ringing.

Two things the self-test caught before anything else did, both now regression
checks. The read head glides to a new delay time, which is what makes turning
Time bend the repeats -- but it was gliding up from zero on the *first* setting
too, so the first repeat landed early and smeared. And Ping-Pong: [Pirkle]'s
figure crosses the inputs as well as the feedback, which is right for a stereo
source and produces no ping-pong at all for a monophonic one, because both
lines get fed the same signal and both taps stay equal for ever.

**The window is 60 px wider**, 1440 rather than 1380, and row 1 rather than row
0 is now what sets that. Making room for the delay beside the generator took
three things: the sequencer's Mode and Rate stacked into one column, the
generator's Seed moved out to the step grid where its own -, + and GEN buttons
already are, and those 60 px. The seed is still a parameter and still
automatable; it is no longer a knob on a panel.

**The manual has screenshots.** Twenty-two of them -- every panel, the step
grid, a long note being drawn and then split, the pattern map being built, the
preset browser, and the whole window open and closed. They come from
`tools/make-screenshots.sh`, which drives the real editor and checks its own
work, and they are committed rather than built at release time because
`release.sh` cannot assume a display. The manual also gained the contents page
its stylesheet was written for and never had, and headings no longer end up
alone at the foot of a page: `page-break-after: avoid` is ignored by
wkhtmltopdf's WebKit, so `manual.py` now binds each heading to the block under
it in a `page-break-inside: avoid` box, which that build does honour.

**The manual is now an end-user document and only that.** The offline-rendering
chapter is gone, along with every reference to the repository, the source, the
self-test and `tools/analysis/README.md`; the cover carries the plugin's own
accent, its real name -- once, not twice -- and the right formats; and chapter 1
had been saying "Fifty-one parameters", "Linux. CLAP." and "sixteen-step
sequencer" for several releases. It also **names no brands** any more, on the
user's instruction, apart from the trademark notice, which stays verbatim.

**Long notes.**

A note held over several steps was always expressible -- the same note on each
of them, tied together with a slide, which is how the hardware does it -- but it
had to be entered a step at a time and it read as a row of separate boxes. It is
now one gesture and one shape:

- Dragging a note along a row of the pitch grid writes a long note across the
  steps it covers: every step takes the note, and every one but the last slides
  into the next. Dragging back over a run shortens it.
- A run of steps on the same pitch, each sliding into the next, is drawn as one
  continuous bar rather than as separate boxes.
- Turning a slide off in the SLIDE lane splits the run at that point; turning it
  back on joins it again. That needs no code of its own, because the slide flag
  *is* the tie -- `stepsTied()` in `src/pattern.cpp` is the whole rule, and the
  self-test holds it to same pitch and same octave so a glide is never drawn as
  one note.

Nothing about the pattern's storage, the preset format, the state blob or the
engine changed, so a 0.6.0 preset loads unchanged and a pattern written here
still opens in 0.6.0 -- as a row of tied steps, which is what it is.

0.6.0 rebuilds the drive stage out of the literature, and it exists because the
first attempt at it was wrong.

**What was wrong.** 0.5.0 shipped fourteen distortion types and most of them
were inaudible. Not subtly different -- the same. Three reasons, all of them
mine: every shape was given the same pre-gain, so they all crossed their knee at
the same setting and saturated at the same setting, and any clipper driven past
its knee is the same square wave whatever curve produced it; the level matching
then divided out the one thing that still differed, which is how much each curve
compresses; and none of them had any filtering of its own, so every model had an
identical spectral envelope. The self-test certified all fourteen as working
because it compared *sample buffers* -- "no two types render the same audio" --
and two signals can differ in every sample and be indistinguishable.

**What replaced it.** Seven models, each an equation out of a named source, and
built differently from each other rather than curved differently:

| Type | Source |
|---|---|
| Soft Clip | the stage this instrument always had |
| Overdrive | [DAFX] eq 4.14, after Schetzen |
| Tube | [DAFX] eq 4.13, after Bendiksen, with M-file 4.4's filters |
| Valve Stack | [Pirkle] 19.12/19.13: four class-A triodes and a tone stack |
| Fuzz | [DAFX] eq 4.15 with [Pirkle]'s FEXP1 asymmetry |
| Rectifier | [DAFX] 4.3.3: an octave up, not an edge |
| Crush | [Pirkle] eq 19.1 |

[DAFX] is Zoelzer (ed.), *DAFX: Digital Audio Effects*, 2nd edition, chapter 4.
[Pirkle] is *Designing Audio Effect Plugins in C++*, 2nd edition, chapter 19.
Neither book is in this repository and neither may be: they are copyrighted.
`Documents/` is gitignored for the same reason `!dev/` is.

Each model has its own gain staging, over the range its own source gives its own
parameter, so they do not all saturate together. Each runs at **twice the sample
rate**, which both books require of a nonlinearity and the first version did not
do at all. They are matched to each other in **loudness**, measured on a real
line rather than on a sine, so a comparison between two of them is a comparison
of character. And a new **Bias** control moves each model's operating point from
the one its source specifies, which is where even harmonics come from.

**And the test measures sound now.** Each model's Drive is searched for the
setting that gives it 25 % THD, and the harmonic distributions are compared
there -- at matched distortion, because any two clippers meet at the top of the
knob. The closest pair in the set measures 0.17 apart on that scale, and the
thing separating them is that one of them stops distorting as a note decays.
Nine checks cover the models; `demos/distortion/` holds an A/B set.

Soft Clip is the default and its code path is untouched, so **every factory
preset renders bit for bit what it did in 0.4.0**, checked at 44.1 and 48 kHz.

0.5.0 is three features and a button.

**Patterns are up to 128 steps long.** Steps ran 1 to 16, which is the machine;
it now runs 1 to 128, which is eight bars of sixteenths and long enough to hold
a melody rather than a riff. The generator fills exactly the length that is set
and its draws are per step and in order, so turning Steps up extends the line
the seed already described instead of replacing it. The grid draws sixteen
columns for a pattern of sixteen or fewer -- so a sixteen-step line looks
exactly as it did -- and thirty-two for anything longer, scrolling to the rest
with the wheel, a bar under the grid, or the playhead while it runs. The state
blob went to version 2 with it: a version 1 blob carries sixteen words per
pattern, is read as the first sixteen steps of a 128-step pattern, and a
project saved by 0.4.0 opens with its bank intact. Six checks cover the
generator, the preset text, the migration and the sequencer actually playing
past step sixteen.

**The drive stage has fourteen shapes.** It was one soft clipper; it is now
Soft Clip, Overdrive, Tube, Tape, Transistor, Germanium, Diode, Crunch,
Distortion, Metal, Fold, Thick, Crush and Destroy, with a Dist Mix control to
set how much of the result is heard against the clean signal. None of them is a
model of a particular circuit -- there is no schematic for any of this -- and
none is taken from another product: each is the shape its name has meant in
audio for decades, written from the mathematics. Soft Clip is the default and
its code path is untouched, so **every factory preset renders bit for bit what
it did before**, checked at 44.1 and 48 kHz. Level matching is measured per
shape rather than assumed, because the wavefolder's peak is in the middle of
its range rather than at the end of it.

**The preset library has folders and packs.** The browser groups the library by
folder -- Factory Presets, then your own, then Unfiled -- and a folder is made
by saving as "Folder/Name". A whole folder writes out as one preset pack, a
text file in the same format with a separator line between the presets, and
reads back in as a new folder without overwriting anything. Packs go to and
come from `<config>/SaeureKiste/packs` by default, or anywhere the desktop's
file chooser can reach.

And **DEL** in the pattern bank empties the selected slot, which previously
meant selecting the pattern and using CLEAR on the grid.

0.4.0 makes **GEN** generate. It regenerated from the seed that was already
set, so pressing it twice gave the same sixteen steps twice -- a button that did
nothing unless one of the densities had moved. It now picks a seed that is not
the current one and generates from that, so every press is a new line, which is
what every other generator does. The seed is still a parameter and still what
makes a pattern reproducible: the line GEN just made is a number, and the - and
+ buttons still walk through seeds one at a time.

0.3.3 makes two instances independent of each other. Every editor was reading
one window description, because the plugin handed `createWindow()` a
function-local `static` and the window kept a *reference* to it: opening a
second instance's editor redirected the first one's window at the second
plugin's pattern bank. Two tracks of SaeureKiste shared their patterns and their
pattern selection, and a pattern written in one appeared in -- and vanished
with -- the other. The window now copies what it is given, the spec is a local,
and the header ornament is one per window rather than one per binary. Nothing
mutable is left at file scope but the module-level preset discovery, which is
the same for every instance by definition.

0.3.2 fixes the sequencer restart in a stopped DAW. Releasing the key and
pressing it again carried on from the step the pattern had stopped on instead of
starting it over, because the restart was dropped whenever the key did not
arrive on a block's first sample, and because the free-run start path only
rewound the position when the host published no beats timeline -- a stopped DAW
publishes one. Four checks cover it, against a reference run of the same line.

0.3.1 makes the octave lane readable: up is the accent green and down is amber,
where both directions used to be the same colour and, at two octaves, nearly the
same shape as well.

0.3.0 adds four things to the sequencer: a **note output port**, so the line can
be recorded onto another track; **dragging a pattern into the host as a MIDI
file**; **copy and paste** between pattern slots; and an octave lane that reaches
**two octaves either way** instead of one.

0.2.1, folded into it, fixed three things about playing the sequencer from a
keyboard. It could end up playing on its own after browsing presets -- the Mode
parameter decides whether a MIDI note sounds or transposes, so changing it
between a note-on and its note-off left the key held on the side that no longer
receives the release. A key press now starts the pattern over instead of only
transposing it. And the tempo is read from a stopped host as well as a rolling
one, so a pattern auditioned off a key follows the project's tempo instead of
running at whatever the plugin last saw. `TODO.md` §4 has all three accounts.

## What works

**The circuit**, modelled from the February 1982 service notes and matching the
block diagram on page 3: VCO -> four-stage transistor ladder -> VCA, with the
decay envelope, the accent circuit and the slide lag around it, and a drive
stage added after.

**Two ways to play it.**

- *MIDI*: the host plays it. Velocity at or above a threshold makes an accent,
  overlapping notes make a slide, CC1 is the vibrato.
- *Sequencer*: sixteen steps of its own, locked to the host's beat timeline and
  re-read every block, so scrubbing, looping and tempo changes all land. A held
  MIDI note transposes the pattern instead of sounding, which is what the
  machine's own keyboard did. With no transport running, a key runs the pattern
  at the host's tempo and starts it again from step one; with one, the position
  belongs to the song and a key only transposes. Notes are sample-accurate: the audio block is
  split again at every step boundary. A step can sit two octaves either way of
  the pattern, and COPY and PASTE move a pattern to another slot.

**The line can leave the plugin as MIDI.** A note output port carries everything
the sequencer plays, transposition included, so it can be recorded onto another
track; and the MIDI button above the grid drags the selected pattern into the
host's arranger as a `.mid` file. Both write an accent as a velocity above the
accent threshold, a slide as an overlap and a vibrato as CC1 -- the three
conventions the plugin's own MIDI mode reads, so a recording played back into it
sounds like what it came from. The X11 half of the drag is verified against a
test drop target; the Windows half compiles and has never had a real drop.

**A bank of sixty-four patterns**, each with its own chain and repeat count:
Stay repeats it, Next goes on and wraps at the chain's length, First goes back
to pattern 1, Random picks inside the chain. Which pattern plays is a function
of the host's beat position, not a counter the sequencer advances, so looping
and scrubbing land on the right pattern for the same reason the steps do. The
whole bank, chains included, is in the preset file and in the state blob; the
selected pattern and the chain length are parameters and so are automatable.

**The Devil Fish.** Robin Whittle's modification of the same machine, from his
own manual: Overdrive into the filter, audio-rate Filter FM, the Muffler, Soft
Attack, the volume envelope as Amp Decay and Amp Sustain, the three accent Sweep
Speeds, Accent Hold, and the widened ranges on Cutoff, Decay, Slide Time, Res
Range and Tracking. **Every one defaults to the stock circuit**, and that is
checked the hard way: all twenty-seven presets render byte for byte what they
did before any of it existed. Whittle says a Devil Fish can still sound exactly
like a TB-303; so does this.

Not modelled: the jacks. External audio into the filter, the audio Filter FM
input, the Filter Out tap, CV and gate. Those need an audio input port and a
second output.

**The ten mods.** Every constant in the engine that the schematic does not give
is a control instead of a hidden guess -- the two Env Mod magnitudes, the three
accent numbers, the accent decay the hardware forces, the square's droop, the
ladder's saturation, the resonance limit and the oscillator's drift. Their
defaults are the values the engine shipped with, so nothing moved.

**The pattern generator.** A seed, a scale, a root and five densities. Same
settings, same sixteen steps, every time -- so a line worth keeping is a number
rather than a file.

**The window**, 1080 x 703, growing to 1080 x 975: the panels, the ladder's real
response curve animating across the header, a step grid with a twelve-row piano
roll, a per-step octave lane and three flag lanes, and the pattern bank beside
it. The ten mods and the three panels that go with them are behind the ADVANCED
button, which grows the window through `clap_host_gui::request_resize`; the
window remembers which way it was left in the plugin's state. There is no
activity meter -- what the instrument is doing is the header's filter curve and
the sequencer's playhead.

**No random state in the engine at all**, so a preset renders identically every
time. The self-test checks it across a reset with a different note in between.

## What is measured

The engine against figures in the service notes.
`tools/analysis/README.md` has the full table of where every number came from.

| Claim | Service notes | Measured out of the build |
|---|---|---|
| Resonant ring at centre cutoff, resonance full | 2 ms +/- 0.5 ms (400-667 Hz) | **556 Hz**, a 1.80 ms ring |
| The filter rings but does not sustain | the printed waveform is damped | maximum feedback 4.15 against a critical 4.25 |
| Ladder slope near the corner | C18 .018 against C19/C24/C26 .033 | **-14.2 dB/oct** 500->1000 Hz, **-19.5** 1000->2000, approaching -24 above |
| Square is quieter than saw | 3.0 V against 6.5 V p-p | 6.7 dB down, by construction |
| Accent is brighter | -- | +4.3 dB in the 900 Hz-8 kHz band at note onset |
| Accent is louder | -- | +2.7 dB peak at the default Accent setting |
| Envelope decay range | 200 ms to 2.5 s, to **10 %** | the knob's range, with the 10 % reading applied |

Oscillator aliasing, key 84 (1046.5 Hz), harmonics excluded by +/- 25 Hz: worst
non-harmonic component **-82 dBc**, and what is left at that level is window
skirt rather than a fold. Sawtooth and square both.

**A looping transport is a tested case, twice over.** Everything the sequencer
schedules is a position on the step timeline, so when the host loops or seeks,
all of it -- the pending note-offs and the record of what has already fired --
is rebased by the size of the jump rather than discarded or left stranded. Two
tests cover it, and both were verified by putting the bug back and watching them
fail:

- Four loop lengths (aligned to the pattern, not aligned, and one shorter than a
  single step), failing if the level drops more than 6 dB. Measured **-0.0 dB**;
  with the guards removed, **-47.6 dB**.
- A host loop exactly as long as the pattern, compared sample by sample against
  the same render with no loop at all. They are **bit-identical** -- the error
  sits 347 dB down. With the seam handled wrongly it measures 31 dB, which is
  what one note cut from 216 ms to 40 ms looks like.

Sequencer, rendered against a 130 BPM transport and read back note by note: all
sixteen steps land on the grid, every pitch is the one written, rests are
silent, and the two steps that read "wrong" are mid-slide, which is correct.
A slide glides 65.4 Hz to 121 Hz **with the amplitude envelope flat through the
join** -- it does not retrigger, which is the whole point.

Generator, over 400 seeds: the root is **44 %** of all generated notes, rests
are 19 %, octave jumps 21 % with a third of them downward. Raising the accent
density leaves every note, slide and vibrato exactly where it was.

## What is not done

**Nothing here has been listened to against a real TB-303.** Every number above
is the build agreeing with a schematic. That is the wrong half of the rule this
repository sets itself -- validate by ear first, by measurement second -- and it
is the top item in `TODO.md`. The four numbers most likely to be off are now
controls, though, so it is a preference rather than a defect.

**Four binaries, and the Windows ones have been run.** CLAP and VST3 for Linux
and Windows all come out of `./release.sh`. What each has been put through:

| | Checked |
|---|---|
| Linux CLAP | the full self-test, from inside the release archive |
| Linux VST3 | loads, exports `ModuleEntry` and `GetPluginFactory` |
| Windows CLAP | **the full self-test, 103 checks, 0 failures, under wine**; window opens and draws; the collapsible section resizes through `clap_host_gui::request_resize` |
| Windows VST3 | loads under wine, exports `InitDll` / `ExitDll` / `GetPluginFactory` |

The Windows renders were compared preset by preset against the Linux ones:
**the largest difference anywhere is 1 LSB of 32768**, and the worst RMS error is
95 dB below the signal -- which is mingw's libm rounding differently from
glibc's, not behaviour. `Oscillator` is the worst of them because it
self-oscillates, so a last-bit difference compounds in a resonant loop.

Both Windows binaries import nothing but system DLLs, so they need no MinGW
runtime beside them.

**Still not tested: a real Windows machine, and any DAW on either platform.**
wine is a good emulator of the API, not proof that Bitwig or Ableton will load
this.
