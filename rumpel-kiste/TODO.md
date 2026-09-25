# RumpelKiste -- to do

## 1. Listen to it

Every voice has now been measured against recordings and fitted to them
(`tools/analysis/README.md`, "Measured"). It has not been compared with a
machine by ear. The things a pair of ears should judge first:

- the hat and cymbal stand-ins: fitted by band levels, which is not timbre
- the kick's weight against the rest, and the drive presets, which now get a
  kick 8 dB quieter than they were written for
- the late part of the open hat, which the recording darkens more than the
  stand-in does

## 2. Measured constants that could become mods

The rule is that a number the schematic does not give is a control. The
recordings have since given many of them, and they are constants in
`drums.cpp`, marked as measured -- measurements of one machine, not guesses.
Worth deciding which deserve a knob:

- the kick's 45 ms hold
- the hats' and cymbals' ROM clocks, and the open hat's run length
- the audible decay curves behind every Decay/Tone knob (their tables)
- the rim shot's resonator tolerances and its clamp asymmetry

## 3. Features

- **Individual outputs.** The machine has one per voice. CLAP can offer
  eleven mono ports beside the main pair; the engine already computes each
  voice separately for the drive bus.
- **Per-voice drive amount** rather than a switch, if the switch turns out
  too coarse.
- **The triplet scales could set Last Step to 12**, as the machine does, as an
  option.
- **Pattern map shortcuts** -- the same gap SäureKiste's TODO lists: sixteen
  pads onto sixteen patterns is sixteen learns.
- **Demo renders**: a `presets/demo-descriptions.txt` and a
  `shared/tools/make-demos.sh` run.

## 4. Porting

`src/gui/seqwindow.cpp` is a fork of a fork. A fix to the shared window or to
SäureKiste's has to be ported here by hand, and the Windows `long` fix above is
worth checking for in both (SäureKiste's blink timer uses `auto`, so it is
safe; the shared window has no timer).
