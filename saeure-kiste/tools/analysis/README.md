# What was read off the schematics, and what was not

Every number in `src/dsp/acid_engine.cpp` comes from one of three places, and
this file says which for each of them. The sources are the two PDFs in `!dev/`:

- **`TB-303.pdf`** -- *TB-303 Service Notes, First Edition, Feb. 19 1982*
  (3rd printing Sep. 1986), ten pages: specifications, IC pinouts, the block
  diagram, the switch board, the main board at A3, the PCB foil side with the
  alignment procedure, the VCF envelope-modulation note, the parts list and a
  correction sheet.
- **`Roland TB-303.pdf`** -- three pages, the same main-board drawing at a
  different scale.

Neither is ours to redistribute, which is why `!dev/` is gitignored.

There is no reference **audio** here at all, and that is a real gap: the whole
instrument is fitted to a circuit diagram and an alignment procedure rather than
to a recording of the machine. See *What has not been checked* at the bottom.

## Read directly off the schematic

| Quantity | Value | Where |
|---|---|---|
| Ladder capacitors | C18 = .018 uF; C19, C24, C26 = .033 uF | main board, VCF block |
| Ladder pole ratio | 0.033 / 0.018 = **1.8333** | derived from the above |
| Accent sweep RC | C62 = 1 uF, R138 = 68 k -> **68 ms** | main board, ENV block |
| Accent decay switch | ACCENT -> IC12 (4066) pins 5/3-4, then R117 22 k and D26/D27/D28 onto the envelope node | main board, ENV block |
| VCA envelope rise | C41 = .1 uF, R134 = 22 k -> **2.2 ms** | main board, VCA block |
| VCA envelope sag | C42 = 1 uF, R123 = 1.5 M -> **1.5 s** | main board, VCA block |
| Slide network | C35 = .22 uF switched by IC12 (4066), with R91 1 M and R92/R93 100 k | main board, VCO block |
| VCO saw swing | 12 V down to 5.5 V | waveform inset, VCO block |
| VCO square swing | 8 V down to 5 V | waveform inset, beside "9 ms KEY A" |
| Square / saw level | 3.0 / 6.5 = **0.462**, i.e. 6.7 dB down | derived from the above two |
| VCO core | IC11a integrator with C34 .001, reset by Q8, comparator Q24/Q25/Q27; antilog pair Q26 (2SC1583) with R100 a 560 ohm posistor | main board, VCO block |
| Filter input coupling | C17 1 uF into R62 220 k -> 0.72 Hz, i.e. nothing | main board, VCF block |
| A second accent RC | C13 1 uF with R46 47 k -> 47 ms, on the VCF side | main board, near VR4 |

## Printed as text or as a curve

| Quantity | Value | Where |
|---|---|---|
| Envelope decay range | "DECAY VR MAX T = 2.5 sec, MIN T = 200 ms" | main board, beside the envelope curve |
| What T means | the curve's vertical axis is marked **100 %** and **10 %**, so T is the time to a tenth -- -20 dB, not -60 | same curve |
| Filter alignment | cutoff centred, waveform sawtooth, resonance full clockwise, Env Mod / Decay / Accent full counter-clockwise; trim TM3 until the ring is **2 ms +/- 0.5 ms** | page 7, "VCF" |
| The filter does not oscillate | the waveform printed for that check is a **damped** ring | page 7 |
| Tuning range | "TUNING Control: approx. +/- 700 cents (perfect fifth)" | page 1, specifications |
| Output stage gain | "Gain: Unity" | page 1, specifications |
| CV scaling | 1 V/oct, C = 1.000 V, C = 4.000 V, A key = 2.75 V | main board, CV block |
| The Env Mod trick | page 8, *VCF ENVELOPE MODULATION*: raising VR5 feeds more envelope to the base of Q10 **and** moves the bias Q9 sets, lowering the resting cutoff. Roland's own word for it is a gimmick, and the reason given is that a modulation which only ever opens the filter spends most of its travel where nothing audible happens. | page 8 |

## Derived, and how

- **Cutoff knob range, 100 Hz to 2.5 kHz.** The alignment procedure puts a 2 ms
  ring at the centre of the knob. 2 ms is 500 Hz, and 100 Hz to 2.5 kHz has its
  geometric centre at exactly 500. Measured back out of the built engine: with
  the knob centred and resonance at maximum the resonant peak lands at
  **556 Hz**, a 1.80 ms ring, inside the service note's 2 +/- 0.5 ms window.
- **Maximum feedback, 4.15.** With poles at w, w, w and 1.8333 w the loop phase
  reaches 180 degrees at 1.16 w, where the loop gain is 4.25. (Four *equal*
  poles would need 4.0 at 1.0 w.) The maximum stops below 4.25 because the
  machine does not oscillate.
- **The rolloff.** Measured out of the built filter, resonance at zero, cutoff
  knob at 500 Hz: **-14.2 dB/octave** from 500 to 1000 Hz and **-19.5** from
  1000 to 2000, approaching -24 above that. That shallow corner is the whole
  content of the "a 303 is an 18 dB filter" argument, and it is one capacitor.

## Fitted, guessed, or added -- the honest list

These are the numbers that are **not** in the service notes.

**They are all controls now.** Every one of them used to be a constant in
`acid_engine.cpp`, which meant shipping one particular guess and asking everyone
to live with it. They are on the MODS panel instead, with these values as their
defaults -- so the table below is a list of defaults rather than a list of
decisions made on somebody else's behalf. That is also the honest position: no
two of these machines agreed with each other, and "the" 303 sound was never one
sound.

| Constant | Value | Why that value |
|---|---|---|
| **Droop** | 25 Hz | The printed square is not flat: its top slopes down across the half period at 110 Hz by roughly a third. A first-order highpass at 25 Hz draws that. **Which RC in the circuit does it could not be traced from the scan**, so this is fitted to a drawing rather than read off a component. |
| **Acc Build** | 0.75 | How much of C62 each accent fills. It depends on the accent pulse's width against the charging path and neither is printed. 0.75 puts a lone accent at three quarters and lets a run of them build, which is how the machine is described as behaving. |
| **Acc Sweep** | 3.5 | How far a full accent opens the filter. Not a circuit value; chosen so that an accented note measures brighter than an unaccented one **over its whole length** and not only at its onset -- which at two octaves it did not, because an accent also shortens the decay and the shortening won. |
| **Acc Gain** | 0.9 | How much louder an accent is. |
| **Env Bias** | 1.5 | The size of Q9's bias shift. The *shape* is the schematic's; the magnitude is chosen so the sweep stays inside the audible range at every setting of the knob, which is what page 8 says the circuit achieves. |
| **Env Depth** | 5.0 | How deep a full Env Mod sweep is. Same reasoning. |
| `kVcaReleaseSec` | 6 ms | The gate falling. Audible as the clipped end of a staccato note. Not printed anywhere, and the one on this list that is still a constant. |
| **Acc Decay** | 200 ms | Which fixed value the 4066 puts the envelope on. 200 ms is the Decay knob's own minimum, which is where the circuit lands, but the schematic does not state it. |
| **Ladder** | 100 % | How hard the feedback is driven into saturation. A single soft clip stands in for a transistor model. |
| **Res Range** | 100 % | Where the Resonance knob's top sits relative to the oscillation threshold. |
| **Drift** | 0 % | Off by default, because the arithmetic is exact and the machine was not. |
| `kControlBlock` | 8 samples | How often the filter coefficients are recomputed. 6 kHz at 48 k, far above anything the envelopes do. |
| Drive and Tone | -- | Not in the schematic at all, and the parameter tips say so. A 303 into a mixer is clean; everything anybody recognises as acid went through something else first. |
| Vibrato, the sequencer, the generator | -- | Not in the machine either. The sequencer is the machine's *idea* with a piano roll instead of a row of buttons; the per-step vibrato and the pattern generator are both things other people added. |

## What has not been checked

- **Nothing here has been compared against a recording of a real TB-303.** Every
  measurement above is the built engine against the service notes -- a number
  agreeing with a number. The suite's own rule is to validate by ear first and
  by measurement second, and only the second half of that has been done. An A/B
  against reference audio, starting from the `Dry Reference` preset, is the
  outstanding work; see `../../TODO.md`.
- The **VCO waveform shapes** are ideal: a linear falling ramp and a hard
  square. The real integrator's ramp has some curvature and its reset takes a
  finite time, neither of which is modelled.
- The **exponential converter** is exact. The real one is a matched transistor
  pair with a posistor compensating its tempco, and it drifts.
- The **ladder saturation** is a single soft clip in the feedback path, not a
  transistor model.
