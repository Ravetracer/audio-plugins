# SäureKiste — manual

*A monophonic acid bass synthesiser for Linux. CLAP.*

Version 0.3.0

---

> This manual's prose is written out in full rather than generated, unlike the
> rest of this repository, which builds its parameter reference and preset
> library from the plugin itself. The consequence is that the parameter table
> below can drift from `src/params.cpp`; it is maintained by hand and should be
> re-read against the source when a parameter changes.

---

## 1. What it is

SaeureKiste models the main board of a Roland TB-303 as the February 1982
service notes draw it: a relaxation-oscillator VCO, a four-stage transistor
ladder filter, a VCA, a decay-only envelope, an accent circuit, and a slide lag
on the pitch CV. One stage is added that the machine does not have — an
overdrive after the filter — and it is marked as such everywhere it appears.

It is **monophonic** and it tells the host so.

It plays two ways: from the host, or from its own sixteen-step sequencer, which
draws on a bank of sixty-four patterns.

It also models **Robin Whittle's Devil Fish** modification of the same machine,
from his own manual — see §11. Every one of those controls defaults to the stock
circuit, so the plugin is a TB-303 until you ask it not to be.

Fifty-one parameters, twenty-seven presets.

Not affiliated with or endorsed by Roland Corporation. *TB-303* is their
trademark, used here only to name what was modelled. Not affiliated with or
endorsed by Robin Whittle; *Devil Fish* is his, and is used only to name the
modification that was modelled.

## 2. Playing it

**Mode** decides where the notes come from.

In **MIDI** the host plays it, and the two things the machine's sequencer used
to say per step are recovered from what a piano roll already has. In
**Sequencer** the plugin plays its own sixteen steps and a held MIDI note
transposes them — see §6.

Everything below applies to both.

### Accent

In MIDI mode, a note whose velocity is **at or above the Accent At threshold**
(100 by default) is accented; in Sequencer mode it is the step's accent bit. Everything below it is not. It is a switch and not a
curve, because the hardware's accent was one bit per step.

An accented note is three things at once:

- **louder**, by the amount the Accent knob sets;
- **brighter**, because the accent circuit adds to the filter's cutoff CV;
- **shorter**, because the ACCENT line gates an analog switch onto the envelope
  node and bypasses the Decay knob entirely. On the hardware this is not
  optional; a later modification made it switchable, which tells you how
  strongly people felt about it.

The brightness does not stop when the note does. The accent charges a capacitor
which then discharges over **68 ms** — long enough that a note landing a
sixteenth later is still sitting in the tail of it. That is where a 303 line
gets its breathing quality, and **Sweep Time** is that time constant.

### Slide

**Overlap two notes** and the second slides from the first. Extend a note in the
piano roll so it runs past the start of the next one; in Sequencer mode, tick
the step's Slide box and the sequencer does the overlapping for you.

What matters is what does *not* happen: nothing retriggers. The filter envelope
carries straight on through the join and the amplifier does not start again,
because on the machine a slid step holds the gate high and never fires a new
trigger. A slide is therefore audibly different from two legato notes.

Releasing the upper of two held notes slides back down to the lower one, for the
same reason.

## 3. The controls

Eight are the machine's front panel. Six are not, and are marked **(added)**.

### VCO

| Control | Range | Default | |
|---|---|---|---|
| **Waveform** | Sawtooth / Square | Sawtooth | Switch S1. The sawtooth falls rather than rises — the oscillator is an integrator that ramps down and is snapped back up. The square arrives at the filter **6.7 dB down**, because the schematic prints both swings: 12 V to 5.5 V for the saw and 8 V to 5 V for the square. That difference is reproduced rather than normalised away. |
| **Tuning** | ±700 cents | 0 | VR2. The service notes give its travel as approximately a perfect fifth either way. |

### VCF

| Control | Range | Default | |
|---|---|---|---|
| **Cutoff** | 100 Hz – 2.5 kHz | 500 Hz | VR3. The centre is 500 Hz because the factory alignment procedure puts it there: with cutoff centred and resonance full, TM3 is trimmed until the filter rings at 2 ms ± 0.5 ms, and 2 ms is 500 Hz. |
| **Resonance** | 0 – 100 % | 35 % | VR4. It stops just short of oscillation, because the machine does — the ringing waveform printed for that alignment check dies away. A 303 that sustains a tone is a modified 303. |
| **Env Mod** | 0 – 100 % | 50 % | VR5, and the one control that does two things at once. See §4. |
| **Decay** | 200 ms – 2.5 s | 600 ms | VR6. The range is printed on the schematic. The envelope has no attack worth the name and no sustain at all — it is triggered and it falls. An accented note ignores this knob. |
| **Tracking** *(added)* | 0 – 100 % | 0 % | Filter key follow. The machine has **none**: its pitch CV reaches the oscillator and stops, so a note two octaves up meets the same filter as the root. 0 % is the machine. |

### Accent

| Control | Range | Default | |
|---|---|---|---|
| **Accent** | 0 – 100 % | 60 % | VR7. How much louder, brighter and shorter an accented note is. |
| **Accent At** *(added)* | 1 – 127 | 100 | Which velocities count as an accent. |
| **Sweep Time** *(added)* | 10 – 500 ms | 68 ms | The accent's time constant. 68 ms is the circuit's own: C62 is 1 µF and discharges through R138, 68 k. Shorten it and each accent stands alone; lengthen it and a run of them builds. |

### Slide

| Control | Range | Default | |
|---|---|---|---|
| **Slide Time** *(added)* | 10 – 300 ms | 60 ms | Fixed on the hardware by C35 (0.22 µF) and its resistor network; a control here because the notes now come from a host. 60 ms is where a 303 sits. |

### Drive *(the whole panel is added)*

| Control | Range | Default | |
|---|---|---|---|
| **Drive** | 0 – 100 % | 20 % | A soft clipper after the filter, level-matched so that turning it up thickens rather than simply getting louder. At zero the signal path is the machine's. |
| **Tone** | 800 Hz – 18 kHz | 8 kHz | A lowpass after the drive. At the top of its range it does nothing. |

### Output

| Control | Range | Default | |
|---|---|---|---|
| **Volume** | −60 – 0 dB | −6 dB | VR8. It stops at unity rather than offering makeup gain, because the service notes give the output stage's gain as unity and there is nothing above it. |

### Mods *(the whole panel is added)*

Ten numbers that the schematic does not give. They used to be constants in the
engine, which meant shipping one particular guess; they are controls instead,
with those guesses as their defaults. Nothing moved — a preset that does not
mention them sounds exactly as it did.

They are also close to the list of things people soldered into their own
machines, and there is a reason that list is short and specific: no two 303s
agreed anyway. Matched transistor pairs, a posistor and twenty years of drift
saw to that, so “the” 303 sound was never one sound. This is where you pick
yours.

| Control | Range | Default | |
|---|---|---|---|
| **Env Bias** | 0 – 3 oct | 1.5 | How far Env Mod drops the resting cutoff as it deepens the sweep — the size of Q9's trick (§4). At **zero the gimmick is off** and Env Mod becomes an ordinary envelope-amount knob, which is worth hearing once. |
| **Env Depth** | 1 – 8 oct | 5.0 | How deep a full sweep is. |
| **Acc Sweep** | 0 – 6 oct | 3.5 | How far a full accent opens the filter. This is the control that decides whether an accent reads as one at all: an accented note is also running a much shorter envelope, so too little here and the shortening wins and the accent comes out *darker*. It did, at two octaves. |
| **Acc Build** | 0 – 100 % | 75 % | How much of C62 one accent fills. Below 100 % an accent lands on what the last one left, so a run of them builds. |
| **Acc Gain** | 0 – 200 % | 90 % | How much louder an accent is. |
| **Acc Decay** | 20 ms – 2.5 s | 200 ms | The decay an accented note is forced onto by the 4066. **Set it equal to Decay and accents stop being shorter** — the modification the Devil Fish put a front-panel switch on, and the most-requested change to this circuit there has ever been. |
| **Droop** | 1 – 400 Hz | 25 Hz | The tilt on the square. Down for a clean square, up to thin it towards a pulse. Does nothing on the sawtooth. |
| **Ladder** | 0 – 200 % | 100 % | How hard the feedback is driven into its own saturation. Down and the filter is cleaner and rings harder; up and it fights back. |
| **Res Range** | 50 – 130 % | 100 % | How much feedback Resonance can ask for. 100 % is the machine — just below oscillation. **Above it the filter sings**, which a stock 303 cannot do. |
| **Drift** | 0 – 100 % | 0 % | Oscillator instability. Deterministic: the wander is seeded at reset, so a render is still repeatable to the sample. |

## 4. Env Mod, and why it is strange

Page 8 of the service notes is a short essay headed *VCF ENVELOPE MODULATION*,
and it is the most useful page in them.

The complaint it opens with is this. In an ordinary synthesiser the envelope
only ever *opens* the filter, from a resting point the cutoff knob sets. So
asking for a deeper sweep means asking the filter to travel further up — into a
range where, as Roland put it, *significant aural characteristic changes do not
occur*. You get a brighter note and not much more movement.

The TB-303 does something about it. Q9 sets the bias for the antilog pair Q10
and Q11, and the Env Mod control is wired so that turning it up feeds more
envelope to Q10 **and** shifts that bias, which lowers the filter's resting
cutoff. Roland's own word for the arrangement is *a gimmick*.

The effect, on the knob:

- at low settings, a shallow sweep starting near where Cutoff points;
- at high settings, a deep sweep starting well *below* where Cutoff points.

So turning Env Mod up darkens the end of the note and brightens the beginning,
and the whole sweep stays inside the range where the ear can hear it move. This
is why the control feels unlike an envelope-amount knob on anything else, and it
is modelled here rather than approximated by a plain envelope depth.

## 5. The filter, and the 18 dB argument

The ladder has four stages. Read their capacitors off the schematic, bottom to
top: **C18 = 0.018 µF**, then C19, C24 and C26 = **0.033 µF** each.

They are driven by one current, so each pole sits at 1/(2πRC) and the odd one
out is the first stage the signal meets — at 0.033/0.018 = **1.83 times** the
others. A 303's ladder is therefore three coincident poles plus a fourth nearly
an octave above them.

That is not a detail. It is why the filter measures closer to 18 dB per octave
than to 24 near its corner and only reaches the full four-pole slope an octave
up. Measured out of this build, resonance at zero, cutoff knob at 500 Hz:

| Band | Slope |
|---|---|
| 500 → 1000 Hz | −14.2 dB/oct |
| 1000 → 2000 Hz | −19.5 dB/oct |
| above 2 kHz | approaching −24 |

The feedback is soft-clipped, because the ladder is transistors and transistors
run out of headroom. That is where the growl at high resonance comes from, and
it is also what stops the resonance running away.

## 6. The sequencer

Set **Mode** to Sequencer and the plugin plays itself.

It runs off the **host's beat timeline**, re-read at the top of every block, so
scrubbing, looping and tempo changes all land where they should. Inside the
block the audio is split again at every step boundary, so a note starts on the
sample it is due on and not on the next buffer. If the host is stopped, or has
no transport at all, a held MIDI note runs the pattern anyway — at the project's
tempo, which a stopped host still reports, or at 120 BPM if there is no
transport at all — so it can be auditioned without putting the song into play.

A held MIDI note **transposes** the pattern rather than sounding. **C2 is the
pattern as written**; releasing everything puts it back.

When the pattern is running off a key rather than off the host's transport,
**pressing a key starts it again from step one**. That is the only way to place
the first step where you want it without a transport, and it is what the
machine's own keyboard did. With the host playing, a key only transposes: the
position belongs to the song, and a pattern that jumped back to step one in the
middle of a bar would be out of step with everything else in the project.

### The grid

Sixteen steps across, below the panels.

- The **piano roll** is twelve semitones with C at the bottom and the black keys
  on darker lanes. Click a cell to put a note there, click it again (or
  right-click anywhere in the column) to clear the step, and **drag to paint**.
  An accented step is drawn bright — an accent is the first thing you look for
  when reading somebody else's pattern.
- The **OCT** row above it moves a step up to **two octaves either way**. Click
  the top half to step up, the bottom half to step down, one octave a click, as
  far as +2 and −2; the right button puts a step straight back to the middle.
  One octave fills its half of the box, two reach across the seam, so the row
  can be read at a glance. The machine had one switch position each way; two is
  a sequencer feature, and it is what lets a line hold a bass note and a lead in
  the same sixteen steps.
- **SLIDE**, **ACCENT** and **VIB** below. Click or drag.
- Steps past the pattern's length are greyed; the playing step is lit.

**CLEAR** empties it. The two **arrow** buttons walk the whole pattern one step
sideways under the bar, which is the quickest way to find out that a line you
liked was starting in the wrong place. **MIDI** drags the pattern out of the
plugin — see *Taking the pattern with you* below. The rest is the generator.

All of it applies to whichever pattern the bank has selected — which is not
necessarily the one sounding, because a running chain moves on without the
editor following it. The playhead is only drawn when the two are the same.

### The pattern bank

Beside the grid, eight by eight: **sixty-four patterns**. Click one to edit it,
or roll the wheel over the grid to step through them. A pattern with something
written in it is filled, the selected one is outlined in the accent, the one
sounding is ringed, and the ones the chain will reach are lit.

The machine had sixty-four too, and a mode switch to reach them.

**COPY** and **PASTE**, in the bank's own title row, are how a pattern becomes a
variation of another one: copy it, click an empty slot, paste, and change the
two steps you wanted to change. PASTE stays greyed until something has been
copied. The clipboard is the editor's — it lasts as long as the window is open,
and it is not in the preset, the state or the parameter list.

**CHAIN** is what happens when a pattern has played through.

| Chain | After each time round |
|---|---|
| **Stay** | repeat the selected pattern. The hardware's behaviour, and the default. |
| **Next** | step to the following pattern, wrapping round at **Length**. Four patterns make a sixty-four step line. |
| **First** | play the selected pattern once, then stay on pattern 1. |
| **Random** | pick one from inside the chain each time round. |

**LENGTH** is how many patterns the chain covers, counting from pattern 1. Stay
and First ignore it. A pattern selected from outside the chain is where the
chain starts, and after that it runs inside it.

Which pattern plays is worked out from the host's beat position — not counted up
as the sequencer goes — for the same reason the steps are. A loop, a seek or a
scrub therefore lands on exactly the pattern it should, Random included: the same
bar of the song always picks the same pattern, however it was reached.

The selected pattern, the chain mode and the chain length are ordinary
parameters, so a host can automate a pattern change like any other knob. The
sixteen steps inside a pattern are not, and deliberately so — eighty values per
pattern would make a mess of any host's parameter list. The consequence is that a
pattern edit is not automatable and the host's undo does not see it, which is the
same deal a hardware sequencer offers.

The whole bank travels in the preset file and in the plugin's state. Only the
patterns with something in them are written out.

### Taking the pattern with you

A line worth keeping is often a line you want to *edit* — in the arranger, as
notes, with the rest of the track around it. There are two ways out, and they
carry the same thing.

**The note output port.** The plugin has a note output as well as an input, and
in Sequencer mode everything the sequencer plays goes out of it: route it to
another track and record the line as it plays. What comes out is what you hear,
including the transposition a held key applies.

**Drag the MIDI button.** Press **MIDI** above the grid and drag into the host's
arranger. The selected pattern is written to a temporary `.mid` file and dropped
where you let go — which is how a DAW takes MIDI from anything else. A click
that does not move drops nothing.

Both use the same three conventions, and they are the ones this plugin reads
back in MIDI mode, so a recording played into it again sounds like what it came
from:

| In the pattern | In the MIDI |
|---|---|
| **Accent** | a velocity above the **Acc Thresh** parameter — 127, against 80 for a plain step |
| **Slide** | the note is still held when the next one starts. The overlap *is* the slide |
| **Vibrato** | CC1 up for the length of the note and back down after it |
| **Octave**, **Gate**, **Rate**, **Swing** | the key, the length and where the note falls |

The dragged file carries the project's tempo and a 4/4 time signature, at 960
ticks to the quarter note — which divides exactly by three, so a triplet rate and
a triplet swing both land on whole ticks rather than between two of them.

| Control | Range | Default | |
|---|---|---|---|
| **Mode** | MIDI / Sequencer | MIDI | Where the notes come from. |
| **Rate** | 1/32 – 1/8 | 1/16 | How long one step lasts. The triplet settings are not something the hardware could do. |
| **Steps** | 1 – 16 | 16 | How many steps before it repeats. The machine took the same range, and the interesting part of it is the bit that is not 16: fifteen sixteenths against a four-four bar walks the pattern round the beat and comes back after fifteen bars. |
| **Gate** | 0 – 100 % | 50 % | How much of its step a note holds. It does not apply to a step marked Slide — that one holds past the next step's start on purpose. |
| **Swing** | 50 – 75 % | 50 % | Delays every second step. 66.7 % is triplet swing. The hardware's steps were exactly even. |
| **Pattern** | 1 – 64 | 1 | Which pattern the grid edits, and the one the chain starts from. |
| **Chain** | Stay / Next / First / Random | Stay | What happens when a pattern has played through. |
| **Chain Length** | 1 – 64 | 4 | How many patterns the chain covers. Only Next and Random use it. |

### Vibrato *(added)*

| Control | Range | Default | |
|---|---|---|---|
| **Vib Depth** | 0 – 200 cents | 25 | How far a vibrato step bends. In MIDI mode the mod wheel (CC1) scales it, since there is no step to carry the bit. The top of the range is a whole tone either way — past vibrato, and into something the note is doing on purpose. |
| **Vib Rate** | 0.5 – 20 Hz | 6 Hz | How fast. |
| **Vib Delay** | 0 – 400 ms | 60 ms | How long the note waits first, after which it ramps in over 80 ms. At zero it is already wobbling when the note starts, which sounds like a mistake rather than like playing. A slide does not restart the wait. |

## 7. The pattern generator

A seed, a scale, a root and five densities. **The same settings always give the
same sixteen steps**, so a line worth keeping is a number you can write down
rather than a file you have to find.

Step the seed with the **−** and **+** buttons beside the grid and the pattern
regenerates as you go. That is how it is meant to be used: hold the settings
still and walk through seeds until one of them is the one. **GEN** regenerates
with the current seed, for when the densities have moved.

Two things make it a generator rather than a random number generator wired to a
piano roll.

**It changes one thing at a time.** Every step draws all of its random values
whether or not it uses them, so turning Accents up changes which steps are
accented and leaves the notes exactly where they were. A generator that
reshuffled the line on every tweak would be a slot machine. Octaves are the one
deliberate exception: a jump is far likelier on an accented note, because
accent-plus-octave is *the* gesture, so moving the accents does move some
octaves.

**It knows some things about bass lines.** The root comes up far more often than
any other degree and almost always starts the pattern; the first step of each
beat is likelier to sound and likelier to be the root; octave jumps prefer
accented notes; and a slide into a rest is taken out afterwards, because it
slides into nothing. Uniform noise over twelve semitones does not sound like a
bass line and never did.

Measured over 400 seeds: the root is **44 %** of all generated notes, rests are
19 %, and octave jumps 21 % with a third of them downward.

| Control | Default | |
|---|---|---|
| **Seed** | 1 | Which pattern. 0 – 9999. |
| **Scale** | Minor | Which notes it may use. Minor is where nearly every acid line lives; Chromatic is for when it should not make sense. |
| **Root** | C | What the scale is built on. It moves the notes inside the octave rather than transposing the result — to move the line, hold a MIDI note. |
| **Notes** | 78 % | How many steps get a note at all. The rests matter more than they look: a pattern with a note on every step has no shape. |
| **Accents** | 30 % | |
| **Slides** | 20 % | Worth keeping low. A slide only reads as one when the notes around it do not. |
| **Octaves** | 22 % | Mostly up; one jump in three is down. |
| **Vibrato** | 6 % | Nothing on the machine does this, so the default is sparing. |

## 8. Preset library

| Preset | |
|---|---|
| **Factory Reset** | The machine's own middle position, and the reference the others were built against. Leaves the pattern bank alone. |
| **Blank Slate** | The same, and the sequencer with it: the bank emptied down to one bar of C in pattern 1, no octaves, slides, accents or vibrato anywhere. Where you start when you want to write a line rather than edit one. |
| **Dry Reference** | Every added stage off: no drive, tone fully open, master at unity. The signal path the schematic draws and nothing else. Start here when comparing against a recording. |
| **Classic Squelch** | Resonance high, envelope sweeping nearly its whole range, decay short enough that the peak falls back before the next sixteenth. |
| **Dark Engine** | The square wave, low and almost closed. |
| **Open Sweep** | Decay at 1.6 s, so the filter never finishes closing between notes and the line reads as one movement. |
| **Long Fall** | Decay at the knob's maximum, 2.5 s. |
| **Sub Roll** | Env Mod at zero: no sweep, no bias shift, just a filtered oscillator holding the bottom. |
| **Accent Pump** | Sweep Time at 180 ms, well past the circuit's 68. Accents build instead of repeating. |
| **Every Accent Alone** | Sweep Time at 20 ms. Nothing carries over. The same instrument with the hardest-to-hear half removed. |
| **Screamer** | Resonance near the top with the drive hard into its clipper. |
| **Tin Whistle** | The square up where it is thin rather than hollow. |
| **Rubber Band** | Slide Time at 150 ms — longer than a sixteenth, so slid notes arrive between the two pitches they were written as. |
| **Wide Range** | Filter tracking at 60 %, for a line written across three octaves. |
| **Detuned** | Fourteen cents flat, against anything else in the track that is exactly in tune. |
| **Normal Decay** | Acc Decay matched to Decay: accents stay louder and brighter but stop being shorter. |
| **Oscillator** | Res Range at 120 %, so the ladder sings instead of ringing. |
| **Tired Machine** | Drift at 55 %. Nothing quite holds still. |
| **Plain Envelope** | Env Bias at zero — Roland's gimmick switched off, for comparison. |

### Driving their own sequencer

| Preset | |
|---|---|
| **Machine Running** | The plain case: sixteen steps, three slides, four accents. |
| **Fifteen Steps** | The same with Steps at 15, so the pattern walks around the bar. |
| **Swung Line** | Swing at 66.7 %, which the hardware could not do at all. |
| **Vibrato Steps** | Eighth notes with a vibrato on four of them. |
| **Daylight** | *Happy.* Major pentatonic, short and bright, a little swing, both slides moving upward and not a minor third anywhere in it. |
| **Homesick** | *Sad.* Natural minor at half speed, long decay, a gate that nearly fills its step so notes lean into each other, and a delayed vibrato on two of them. Ends on a D that does not resolve. |
| **Teeth** | *Beasty.* 92 % resonance, Acc Sweep at 4.5 octaves, the drive at 85 % and the ladder driven half again as hard as the circuit drives it. Almost every step accented, so with Acc Build at 85 % the accent circuit never empties and the whole bar climbs. |
| **Basement** | *Dark.* The square an octave down, cutoff almost shut, a decay long enough that the filter never finishes closing. Minor seconds and tritones, seven notes in sixteen steps, and Acc Decay at 600 ms so an accent swells instead of stabbing. |

## 9. The window

One row of knobs across a wide, shallow panel, which is the shape of the machine
it models — and a second row of them behind a button, because ten of the
controls are for tuning the engine rather than for playing it.

- **Drag** a knob to edit it; **double-click** to reset it; **shift-drag** for
  fine control; **click a value** to type one.
- **Click a chip's arrows** to step an enumerated control, or the chip itself
  for its list.
- The **preset bar** browses the factory library and anything in your own preset
  directory; **SAVE** writes a new one there.
- **ADVANCED**, beside SAVE, opens the collapsible half of the window: the ten
  mods, and the ACCENT, SLIDE and VIBRATO panels. The window grows to make room
  and shrinks again when you close it, and it remembers which way you left it.
  A host that will not resize a plugin editor on request will leave it clipped;
  everything in it is reachable from the host's own parameter list either way.
- **Clicking the version label** plays one low accented note, so a preset can be
  auditioned without reaching for a keyboard.
- The curve across the **header** is the ladder's actual response — the same
  transfer function the DSP uses, unequal capacitor included — sweeping down
  after every note the way the envelope does.

## 10. Offline rendering

```sh
./build/saeurekiste-render --list
./build/saeurekiste-render --preset dark_engine --out acid.wav --seconds 16
./build/saeurekiste-render --all --outdir /tmp/acid --bpm 138
./build/saeurekiste-render --hold --key 40 --seconds 4
./build/saeurekiste-render --all --demo-moves --outdir demos --seconds 16
./build/saeurekiste-render --selftest
```

The renderer plays a sixteen-step acid line by default, because the accent tail
and the non-retriggering slide are both inaudible in a single held note.
`--hold` gives the held note back.

A preset in **Sequencer** mode is rendered differently and the renderer works
that out for itself: a running transport and no notes at all, with the length
rounded up to a whole number of times round the pattern and never fewer than
three, so a demo ends where the loop does rather than halfway through a bar.

### Moving knobs while it renders

A demo that holds every control still for sixteen seconds is a photograph of a
preset. `--demo-moves` makes it a recording of somebody playing one:

```sh
./build/saeurekiste-render --all --demo-moves --outdir demos \
   --seconds 16 --tail 3 --rate 48000
```

Three controls move, all three **relative to whatever the preset sets**, so a
demo shows the instrument's range without losing what makes it that preset:

| | Over the take |
|---|---|
| **Cutoff** | down about an octave and a half, up to an octave above where it started, then settling a little under it |
| **Resonance** | a quarter of its travel, so the sweep gets more vocal as it goes |
| **Drive** | a third of its travel, starting halfway in, so the end leans into the clipper |

Resonance and Drive **reflect rather than clamp**: a preset already near the top
travels the same distance downward instead, because a knob sitting against the
ceiling for sixteen seconds is the thing this exists to avoid. The moves run
over the held seconds and not the tail, so the last note decays wherever the
sweep left the filter.

For anything else, `--move` writes one out by hand, in real units, and can be
given more than once:

```sh
--move "Cutoff=200 Hz..2 kHz"          # a straight ramp over the take
--move "Cutoff=800..2400..600"         # there and back
--move "Drive=20%..90%@8:16"           # only the second half
```

Interpolation happens in the parameter's own domain, so a Log control like
Cutoff sweeps evenly **in octaves** rather than crawling through its top one.

Without either flag nothing moves and a render is byte-for-byte what it always
was.

`--seed 1 --seeds 8` prints what the generator makes, as the grid draws it.

The engine holds no random state, so the same preset at the same sample rate
renders the same samples every time.

## 11. The Devil Fish

Robin Whittle has modified TB-303s as the **Devil Fish** since the early
nineties. He publishes a manual for it, and it is a good source of a different
kind from the service notes: it documents what each addition *does*, in numbers,
rather than printing a schematic. Everything in this chapter comes from it.

**Every control here defaults to the stock circuit.** Whittle's manual has a
section called *Limiting the Devil Fish to TB-303 sounds*, a table of where to
leave each control so the machine behaves like an unmodified one; those are the
defaults. It is checked rather than asserted: all twenty-seven presets render
byte for byte what they did before any of this existed.

### The controls

**Overdrive** *(VCF)* is the oscillator's level into the filter. It is not
`Drive` — this one is in front of the ladder and `Drive` is behind it. 0 dB is
the fixed level the machine has. Above that the ladder's input pair stops being
linear and starts switching, which is Whittle's "the filter operates under
duress"; the top of the range is his 66.6 times normal. At the bottom the
oscillator is gone altogether, and that setting is only interesting with
`Res Range` past 100 %: the filter sings on its own and Overdrive reintroduces
the oscillator by hand.

**Filter FM** *(VCF)* feeds the amplifier's own output back into the filter
frequency, at audio rate. It is loudest where the signal is loudest, so it bites
hardest on accented notes and wherever Overdrive is up, and it needs resonance
to have anything to work with. A little is edge. A lot is what Whittle calls a
spluttering chaotic mess, and he is right. While it is up the filter
coefficients are recomputed every sample instead of every eighth, which is what
audio-rate modulation costs and why it is off by default.

**Muffler** *(DRIVE)* is a clipper on the output — Off, Soft, Hard. It only
touches signals that are already loud, and it leaves the bottom of the spectrum
alone, so what it takes off is the top of the loudest peaks rather than the
weight of the note. Measured on a stock line: Soft raises the spectral centroid
10 %, Hard 34 %, both within 1.5 dB of the same level. That is the buzz, not a
volume control.

**Soft Attack** *(AMP)* is how fast the amplifier opens on an unaccented note,
0.3 to 30 ms. The machine's is fixed by C41 and R134 at 2.2 ms, which is the
default and is as good as instant; an accented note always uses it whatever this
says. Turned up, the note swells instead of starting — the one thing a 303
cannot do.

**Amp Decay** and **Amp Sustain** *(AMP)* are the volume envelope, which the
machine gives you no way to reach: R123 and C42 fix its decay at 1.5 s, reaching
a tenth in about 3.45 s, and that is the default. It is long enough that over a
sixteenth note nothing happens, which is why the 303's amplifier holds while the
filter falls. Shorten it and the notes start closing on their own. Amp Sustain
is where the decay falls to instead of silence, so a held note can run
indefinitely.

**Sweep Speed** *(ACCENT)* is how the accent circuit answers accents in quick
succession.

| | |
|---|---|
| **Normal** | The machine. Charge left in C62 from one accent makes the next one bigger — you poke it and it squeals, you poke it again and it squeals more. |
| **Fast** | The opposite. The output is the pulse that was just added rather than what has accumulated, so a residue makes the next one *smaller* and the first accent of a run is the strongest. |
| **Slow** | Rises more gently to about twice as far, and takes longer to cool, so it is still settling through the notes that follow. |

Whittle describes what these three do without giving component values — there is
no Devil Fish schematic — so Normal is the machine and the other two are fitted
to his description. `Sweep Time` and `Acc Build` remain the controls.

**Accent Hold** *(ACCENT)* accents every note whatever its step or velocity says.
His front panel has a pushbutton for it.

### The widened ranges

Four controls kept their defaults and grew their travel:

| | Was | Now |
|---|---|---|
| **Cutoff** | 100 Hz – 2.5 kHz | 30 Hz – 5 kHz |
| **Decay** | 200 ms – 2.5 s | 30 ms – 3 s |
| **Slide Time** | 10 – 300 ms | 10 – 360 ms |
| **Res Range** | 50 – 130 % | 50 – 200 % |
| **Tracking** | 0 – 100 % | 0 – 200 % |

A project saved before this converts on load: for a logarithmic control the
saved value is a position on the curve, not a frequency, so widening the curve
would otherwise move it. Preset files were never at risk — they are written in
real units.

### What is not modelled

The part of the modification that is jacks: the external audio input into the
filter, the audio Filter FM input, the Filter Out tap, and the CV and gate
sockets. Those want an audio input port and a second output, which is a
different shape of plugin. Whittle's MIDI retrofits need nothing at all here.

## 12. Where the numbers came from

`tools/analysis/README.md` lists every number in the engine and says whether it
was read off the schematic, printed in the service notes, derived, or fitted.

The ones that were guesses are all **controls** now, on the Mods panel, with
those guesses as their defaults — so that file is a list of defaults rather than
a list of decisions made on your behalf.

The honest summary: this instrument is fitted to a circuit diagram and an
alignment procedure. It has never been listened to against a real TB-303.
