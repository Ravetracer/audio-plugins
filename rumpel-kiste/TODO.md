# RumpelKiste -- to do

## 1. Listen to it

Against a real machine or good recordings of one, voice by voice. The mods'
defaults (BD Pitch, BD Sweep, SD Pitch, Tom Pitch, Tom Sweep, Rim Gate, Clap
Spread, Local Accent) are where a reference would move numbers, and the
voices' relative gains in `drums.cpp` are balanced by rendering, not by ear.

## 2. Hidden guesses that should become mods

The rule is that a number the schematic does not give is a control. Four are
still constants:

- C135's value, which sets both hat decay ranges (fitted to the p.9 traces).
- The cymbal ROM envelopes' depth: 44 dB over the crash ROM, the ride's
  14 dB ping plus 30 dB.
- The clap tail's 12 ms rise.
- The rim shot's clamp drive (`tanh(6x)`): how hard the resonators hit D91/D92.

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
