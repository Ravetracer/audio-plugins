# SäureKiste -- status

Version 0.3.1. Linux and Windows, CLAP and VST3. 51 parameters, 27 presets,
builds clean, self-test passes with no failures across 143 checks.

Six of seven fixes were confirmed by backing the bug out and watching the suite
fail; the seventh has no contract to assert. See `TODO.md` §3.

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

**A bank of sixty-four patterns**, with a chain: Stay repeats the selected one,
Next runs the chain and wraps at its length, First comes home after one time
round, Random picks inside it. Which pattern plays is a function of the host's
beat position, not a counter the sequencer advances, so looping and scrubbing
land on the right pattern for the same reason the steps do. The whole bank is in
the preset file and in the state blob; the selected pattern, the chain mode and
the chain length are parameters and so are automatable.

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
