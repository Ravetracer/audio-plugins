#include "params.h"

#include "plugincore/param_macros.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace saeurekiste {

namespace {

const char *const kWaveformNames[] = {"Sawtooth", "Square"};
const char *const kModeNames[] = {"MIDI", "Sequencer", "Live"};
const char *const kRateNames[] = {"1/32", "1/16T", "1/16", "1/8T", "1/8"};
const char *const kScaleNames[] = {"Minor",  "Major",    "Minor Pent", "Major Pent",
                                   "Dorian", "Phrygian", "Blues",      "Chromatic"};
const char *const kRootNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                  "F#", "G",  "G#", "A",  "A#", "B"};
const char *const kChainNames[] = {"Stay", "Next", "First", "Random"};
const char *const kTriggerNames[] = {"At End", "Instant", "Restart"};
const char *const kMufflerNames[] = {"Off", "Soft", "Hard"};
const char *const kSweepSpeedNames[] = {"Normal", "Fast", "Slow"};
// The drive stage's models, in the same order as DriveModel in dsp/drive.h.
// A preset stores the *name*, so reordering this table would silently change
// what an old preset loads as.
const char *const kDistNames[] = {"Soft Clip", "Overdrive", "Tube",      "Valve Stack",
                                  "Fuzz",      "Rectifier", "Crush",     "Germanium",
                                  "Crunch",    "Lead"};
const char *const kOnOffNames[] = {"Off", "On"};
const char *const kDelaySyncNames[] = {"Free", "Synced"};
// A preset stores the *name*, so this list may not be reordered.
const char *const kDelayDivNames[] = {"1/32", "1/16T", "1/16", "1/8T", "1/8",
                                      "1/8.", "1/4T", "1/4",  "1/2"};
const char *const kDelayModeNames[] = {"Mono", "Stereo", "Ping-Pong"};

// Where the numbers come from.
//
// Every default below is either a value printed in the TB-303 service notes
// (Feb 19 1982, First Edition) or a component value read off the main-board
// schematic on page 5 of them. tools/analysis/README.md lists each one and
// what it was read from; the short version is in the tips.
//
// The two that are neither -- Drive and Tone -- belong to a stage the machine
// does not have, and say so.
const ParamDesc kParams[kNumParams] = {
   // ------------------------------------------------------------------- vco
   ENUM(kParamWaveform, "waveform", "Waveform", "VCO", 0.0, kWaveformNames,
        "Switch S1 on the main board. The sawtooth falls rather than rises -- the "
        "oscillator is an integrator that ramps down and is snapped back up by Q8 -- "
        "and the square is the quieter of the two by a factor the schematic states "
        "outright: the saw swings 12 V to 5.5 V and the square 8 V to 5 V, so the "
        "square arrives at the filter 6.7 dB down. That level difference is part of "
        "how the two settings sound, and it is reproduced rather than normalised "
        "away."),
   LIN(kParamTuning, "tuning", "Tuning", "VCO", -700.0, 700.0, 0.0, "cents",
       "VR2, the front-panel tuning control. The service notes give its travel as "
       "approximately plus or minus 700 cents -- a perfect fifth either way -- which "
       "is the range here. Zero is concert pitch."),

   // ------------------------------------------------------------------- vcf
   LOG(kParamCutoff, "cutoff", "Cutoff", "VCF", 0.5499243591, 30.0, 5000.0, "Hz",
       "VR3. The range is pinned by the service notes' own alignment procedure: with "
       "cutoff centred and resonance full clockwise, TM3 is trimmed until the filter "
       "rings with a period of 2 ms plus or minus 0.5 ms. A 2 ms ring is 500 Hz, so "
       "the centre of this knob is 500 Hz -- and 100 Hz to 2.5 kHz is the decade and "
       "a bit around it. The filter reaches far higher in use; that is what Env Mod and "
       "Accent are for. The travel either side of that decade is the Devil Fish's: "
       "Whittle widened the pot so the filter goes both higher and lower, and 5 kHz is "
       "about an octave above where a stock machine's resonant peak stops. The middle "
       "of the knob is still 500 Hz."),
   PCT(kParamResonance, "resonance", "Resonance", "VCF", 0.35,
       "VR4, the feedback around the ladder. It stops just short of oscillation on "
       "purpose, because the machine does: the waveform the service notes print for "
       "the TM3 check, taken at full resonance, is a ring that dies away rather than "
       "a tone that sustains. A 303 that self-oscillates is a modified 303."),
   PCT(kParamEnvMod, "envmod", "Env Mod", "VCF", 0.5,
       "VR5, and the one control on the machine that does two things at once. Page 8 "
       "of the service notes explains why: raising Env Mod feeds more envelope to the "
       "base of Q10, and the same movement shifts the bias that Q9 sets, which lowers "
       "the filter's resting cutoff. Roland's word for it is a gimmick. The effect is "
       "that a deeper sweep also starts from further down, so the sweep stays inside "
       "the range where it is audible instead of running off the top. Turning this up "
       "therefore darkens the note it is not sweeping and brightens the one it is."),
   LOG(kParamDecay, "decay", "Decay", "VCF", 0.6505149978, 30.0, 3000.0, "ms",
       "VR6, the filter envelope's decay. The range is printed on the schematic beside "
       "the envelope curve: DECAY VR MAX T = 2.5 sec, MIN T = 200 ms. The envelope has "
       "no attack worth the name and no sustain at all -- it is triggered and it falls, "
       "which is why every unaccented note on a 303 drops the same way. An accented "
       "note ignores this knob; see Accent. The range runs past the schematic's at both "
       "ends, to the 30 ms to 3 s of the Devil Fish's Normal Decay pot; the printed "
       "200 ms to 2.5 s is the middle of the travel."),
   // Written out rather than through PCT, which fixes the maximum at 100 %.
   // The kind stays Percent, so the stored value still means what it always
   // meant and a preset or a project written before this loads unchanged --
   // only the top of the travel is new.
   {kParamTracking, "tracking", "Tracking", "VCF", 0.0, 2.0, 0.0, ParamKind::Percent, 0, 0, "%",
    nullptr, 0,
       "How much the cutoff follows the note. The machine has none: the pitch CV goes "
       "to the VCO and nowhere else, so a bass note and a note two octaves up meet the "
       "same filter. Zero is therefore the hardware, and it is the default. It is here "
       "because a line written across three octaves in a piano roll is not a line "
       "anybody wrote on the machine, and at 100 % the filter follows the key exactly. "
       "Past 100 % is the Devil Fish's over-tracking, where the filter climbs faster "
       "than the note does. Whittle's use for it is the other end: a low note drags the "
    "filter below the oscillator's own first harmonic and the note stops sounding "
    "almost entirely."},

   // ---------------------------------------------------------------- accent
   PCT(kParamAccent, "accent", "Accent", "Accent", 0.6,
       "VR7. An accented note is louder, brighter and shorter all at once, and this "
       "sets how much of all three. It is not a velocity control: the accent circuit "
       "has its own envelope and its own time constant, and this scales that."),
   STEP(kParamAccentThreshold, "accent_threshold", "Accent At", "Accent", 1.0, 127.0, 100.0, "",
        "Which velocities count as an accent. On the machine accent was one bit per "
        "step, written on the sequencer; played from a host it has to come from "
        "somewhere, and velocity is the obvious place. Notes at or above this "
        "velocity are accented and notes below it are not -- it is a switch, not a "
        "curve, because the hardware's accent is a switch."),
   LOG(kParamAccentDecay, "accent_decay", "Sweep Time", "Accent", 0.4900, 10.0, 500.0, "ms",
       "How long an accent takes to fade out of the filter, and the reason a 303 line "
       "breathes. C62 on the main board is 1 uF and discharges through R138, 68 k, so "
       "the accent circuit's time constant is 68 ms -- which is the default here. That "
       "is shorter than a bar and longer than a sixteenth, so accents landing close "
       "together stack on top of each other and the filter pumps. Shorten this and "
       "each accent stands alone; lengthen it and a run of them builds."),

   // ----------------------------------------------------------------- slide
   LOG(kParamSlideTime, "slide_time", "Slide Time", "Slide", 0.5, 10.0, 360.0, "ms",
       "How long a slide takes. Overlap two notes in the host and the second slides "
       "from the first instead of restarting it -- which is exactly what the machine "
       "does, because a slid step holds the gate high and never fires a new trigger, "
       "so the filter envelope carries straight on through. The circuit's own time is "
       "fixed by C35, 0.22 uF, and the resistor network around it; the default here "
       "is 60 ms, which is where a 303 sits."),

   // ----------------------------------------------------------------- drive
   PCT(kParamDrive, "drive", "Drive", "Drive", 0.2,
       "The one stage that is not in the schematic. A 303 into a mixer is a clean, "
       "fairly quiet instrument; everything anybody recognises as acid went through "
       "something else first. This is that something: how hard the signal is pushed "
       "into whatever Type is set to, compensated so that turning it up thickens "
       "rather than simply raising the level. At zero the signal path is the "
       "machine's. Crush and Destroy read it as how much damage to do rather than as "
       "a level, because neither of those is a clipper."),
   LOG(kParamTone, "tone", "Tone", "Drive", 0.739536, 800.0, 18000.0, "Hz",
       "A gentle lowpass after the drive, for taking the top off what the clipper "
       "adds. Also not in the schematic. At the top of its range it is doing nothing."),
   // ---------------------------------------------------------------- output
   LIN(kParamVolume, "volume", "Volume", "Output", -60.0, 0.0, -6.0, "dB",
       "VR8, the master. It stops at unity rather than offering makeup gain, because "
       "the machine does -- the service notes give the output stage's gain as unity "
       "and there is nothing above it. Fully down is silence."),

   // ------------------------------------------------------------------ mods
   //
   // Ten constants that used to be hard-coded, all of them numbers the
   // schematic does not give. Their defaults are the values the engine shipped
   // with, so a preset that does not mention them sounds exactly as it did.
   LIN(kParamEnvBias, "env_bias", "Env Bias", "Mods", 0.0, 3.0, 1.5, "oct",
       "How far Env Mod drops the filter's resting cutoff as it deepens the sweep -- "
       "the size of Q9's bias shift. Page 8 of the service notes describes the "
       "arrangement and says why it exists, but gives no figure for it, and it is the "
       "single number that most decides how Env Mod feels. At zero this control is "
       "gone and Env Mod becomes an ordinary envelope-amount knob: the sweep gets "
       "deeper without the start of it getting darker, which is what almost every "
       "other filter in the world does and what the TB-303 deliberately does not."),
   LIN(kParamEnvDepth, "env_depth", "Env Depth", "Mods", 1.0, 8.0, 5.0, "oct",
       "How deep a full Env Mod sweep is, in octaves. Also not a figure the schematic "
       "gives. Five octaves is enough to cross the whole audible range from a resting "
       "point an octave and a half down; more than that is a machine somebody has "
       "been inside."),
   LIN(kParamAccSweep, "acc_sweep", "Acc Sweep", "Mods", 0.0, 6.0, 3.5, "oct",
       "How far a full accent opens the filter. This is the control that decides "
       "whether an accent reads as an accent at all, because an accented note is also "
       "running a much shorter envelope: too little here and the shortened decay wins, "
       "and the accent comes out *darker* than the note it was supposed to be "
       "accenting. That is not hypothetical -- it is what this instrument did at two "
       "octaves, and how the default of 3.5 was arrived at."),
   PCT(kParamAccBuild, "acc_build", "Acc Build", "Mods", 0.75,
       "How much of the accent capacitor each accent fills. C62 discharges through "
       "R138 in 68 ms, which the schematic gives, but how much charge a single accent "
       "pulse puts in depends on the pulse width against the charging path and "
       "neither is printed. Below 100 % an accent lands on whatever the last one left "
       "behind, so a run of them builds instead of repeating -- which is the "
       "behaviour the machine is described as having. At 100 % every accent is "
       "identical and nothing accumulates."),
   LIN(kParamAccGain, "acc_gain", "Acc Gain", "Mods", 0.0, 200.0, 90.0, "%",
       "How much louder an accented note is. The accent reaches the amplifier as well "
       "as the filter; this is the amplifier half of it."),
   LOG(kParamAccDecay, "acc_decay_time", "Acc Decay", "Mods", 0.476896, 20.0, 2500.0, "ms",
       "The envelope decay an accented note is forced to use. On the machine this is "
       "not a choice: the ACCENT line gates IC12, a 4066 analog switch, onto the "
       "envelope node, and the Decay knob is bypassed for as long as it is high. The "
       "fixed value it lands on is the short end of the knob's own range, 200 ms, "
       "which is the default here. Set this to the same value as Decay and accented "
       "notes stop being shorter -- which is precisely the modification the Devil "
       "Fish added a switch for, and the most-requested change to the circuit there "
       "has ever been."),
   LOG(kParamDroop, "droop", "Droop", "Mods", 0.537248, 1.0, 400.0, "Hz",
       "The tilt on the square wave. The square the service notes print is not flat: "
       "its top slopes down across the half period at 110 Hz by roughly a third, and "
       "a first-order highpass at 25 Hz draws that. Which resistor and capacitor in "
       "the circuit actually produce it could not be traced from the scan, so this is "
       "the one control here fitted to a *drawing*. Turn it down for a clean square; "
       "turn it up and the square thins towards a pulse. It does nothing at all on "
       "the sawtooth."),
   LIN(kParamLadder, "ladder", "Ladder", "Mods", 0.0, 200.0, 100.0, "%",
       "How hard the ladder's feedback is driven into its own saturation. The filter "
       "is transistors, and transistors run out of headroom -- that is where a 303 "
       "gets its growl at high resonance rather than a clean whistle, and it is also "
       "what stops the resonance running away. Down, and the filter is cleaner and "
       "rings harder. Up, and it fights back."),
   LIN(kParamResRange, "res_range", "Res Range", "Mods", 50.0, 200.0, 100.0, "%",
       "How much feedback the Resonance knob can ask for. 100 % is the machine: the "
       "top of the knob sits just below the point where the loop would oscillate, "
       "which is why a stock 303 rings and dies rather than singing -- the damped "
       "waveform printed for the TM3 alignment is the proof. Above 100 % it crosses "
       "that line and the filter becomes a sine oscillator with the keyboard doing "
       "nothing to it. That is a modification, not a machine, and it is here because "
       "people made it."),
   PCT(kParamDrift, "drift", "Drift", "Mods", 0.0,
       "Oscillator instability. The exponential converter is a matched transistor "
       "pair with a posistor compensating its temperature coefficient, and it is only "
       "ever approximately right: no two machines were in tune with each other and "
       "none of them held still. Zero is the arithmetic. Up from there the pitch "
       "wanders slowly and each note starts a shade off. Deterministic -- the wander "
       "is seeded at reset, so a render is still repeatable to the sample."),

   // ------------------------------------------------------------- sequencer
   ENUM(kParamMode, "mode", "Mode", "Sequencer", 0.0, kModeNames,
        "Where the notes come from. In MIDI the host plays it: velocity makes an "
        "accent, overlapping notes make a slide, and the mod wheel is the vibrato. In "
        "Sequencer the plugin plays its own sixteen steps, locked to the host's "
        "transport, and a held MIDI note transposes the pattern instead of sounding -- "
        "C2 plays it as written. With no transport running a key runs the pattern and "
        "starts it again from step one. The machine only ever worked the second way."),
   ENUM(kParamSeqRate, "seq_rate", "Rate", "Sequencer", 2.0, kRateNames,
        "How long one step lasts, as a fraction of a beat. 1/16 is the grid a bass "
        "line is written on and is the default; the triplet settings are not something "
        "the hardware could do at all."),
   STEP(kParamSeqSteps, "seq_steps", "Steps", "Sequencer", 1.0, 128.0, 16.0, "",
        "How many steps the pattern runs before it repeats. The machine took 1 to 16 "
        "and this takes 1 to 128, which is two different instruments in one control. "
        "Below sixteen is the machine's own trick: fifteen steps against a four-four "
        "bar walks the pattern around the beat. Above it the sequencer stops being a "
        "bass figure that repeats every bar and becomes a line long enough to have a "
        "melody in it -- 64 is four bars of sixteenths, 128 is eight. The grid scrolls "
        "to whatever does not fit, and the generator fills exactly this many steps."),
   PCT(kParamGate, "gate", "Gate", "Sequencer", 0.5,
       "How much of its own step a note holds for. It does not apply to a step marked "
       "Slide -- that one holds past the start of the next step on purpose, because "
       "overlapping the notes is what produces a slide."),
   LIN(kParamSwing, "swing", "Swing", "Sequencer", 50.0, 75.0, 50.0, "%",
       "Delays every second step. 50 % is straight, 66.7 % is triplet swing. The "
       "hardware had none of this; its steps were exactly even."),

   // --------------------------------------------------------------- vibrato
   LIN(kParamVibDepth, "vib_depth", "Vib Depth", "Vibrato", 0.0, 200.0, 25.0, "cents",
       "How far a vibrato step bends. Nothing on the machine does this -- a vibrato "
       "per step is a modification, and a common one, because a line of identical "
       "notes is a line of identical notes. The top of the range is a whole tone "
       "either way, which is past vibrato and into something the note is doing on "
       "purpose; the useful part is still the bottom quarter of the travel."),
   LOG(kParamVibRate, "vib_rate", "Vib Rate", "Vibrato", 0.673617, 0.5, 20.0, "Hz",
       "How fast it bends."),
   LIN(kParamVibDelay, "vib_delay", "Vib Delay", "Vibrato", 0.0, 400.0, 60.0, "ms",
       "How long the note waits before the vibrato comes in, after which it ramps up "
       "over 80 ms. At zero the note is already wobbling when it starts, which sounds "
       "like a mistake rather than like playing. A slide does not restart the wait: a "
       "slid note is a continuation, and dropping the vibrato in the middle of a held "
       "phrase would be wrong."),

   // ------------------------------------------------------- pattern generator
   //
   // A seed plus six densities. Together they describe a pattern completely:
   // the GEN button in the grid turns them into sixteen steps, and the same
   // settings always give the same sixteen. So a line you like is a number you
   // can write on a piece of paper.
   //
   // Each step draws all six of its random values whether or not it uses them,
   // which is the property that makes the thing usable: nudging Accents changes
   // only which steps are accented and leaves the notes exactly where they
   // were. A generator that reshuffled everything on every tweak would be a
   // slot machine rather than an instrument.
   STEP(kParamRandSeed, "rand_seed", "Seed", "Generator", 0.0, 4294967295.0, 1.0, "",
        "Which pattern. Step it with the - and + buttons beside the grid and the "
        "pattern regenerates as you go, which is how this is meant to be used: hold "
        "the settings still and walk through seeds until one of them is the one."),
   ENUM(kParamRandScale, "rand_scale", "Scale", "Generator", 0.0, kScaleNames,
        "Which notes the generator may use. Minor is where nearly every acid line "
        "lives; Chromatic is the setting for when it should not make sense. The root "
        "itself comes up more often than the rest, because a bass line that does not "
        "keep returning to its root is not a bass line."),
   ENUM(kParamRandRoot, "rand_root", "Root", "Generator", 0.0, kRootNames,
        "What the scale is built on, and the note the pattern keeps coming back to. "
        "It moves the notes inside the octave rather than transposing the result -- to "
        "move the whole line, hold a MIDI note."),
   PCT(kParamRandNotes, "rand_notes", "Notes", "Generator", 0.78,
       "How many of the sixteen steps get a note at all. The rest are rests, and they "
       "matter more than they look: a pattern with a note on every step has no shape."),
   PCT(kParamRandAccent, "rand_accent", "Accents", "Generator", 0.3,
       "How often a note is accented."),
   PCT(kParamRandSlide, "rand_slide", "Slides", "Generator", 0.2,
       "How often a note slides into the next. Worth keeping low -- a slide only reads "
       "as a slide when the notes around it do not."),
   PCT(kParamRandOctave, "rand_octave", "Octaves", "Generator", 0.22,
       "How often a note jumps an octave. Mostly up; one jump in three is down."),
   PCT(kParamRandVibrato, "rand_vibrato", "Vibrato", "Generator", 0.06,
       "How often a note gets a vibrato. Nothing on the machine does this at all, so "
       "the default is sparing."),

   // ------------------------------------------------------------ the bank
   //
   // Sixty-four patterns, and the three controls that say which of them is
   // being edited and what order they play in. The steps inside a pattern are
   // still not parameters; these are, because a pattern change is exactly the
   // kind of thing an arrangement automates.
   STEP(kParamPattern, "pattern", "Pattern", "Sequencer", 1.0, 64.0, 1.0, "",
        "Which of the sixty-four patterns the grid edits, and the one the chain "
        "starts from. The machine had far fewer and a mode switch to reach them; this "
        "is the same idea with the switch replaced by a grid you can click."),
   ENUM(kParamChainMode, "chain_mode", "Chain", "Sequencer", 0.0, kChainNames,
        "What happens when the pattern on screen has played through. Every pattern has "
        "its own. Stay repeats it, which is the hardware's behaviour and the default. "
        "Next steps to the following pattern and wraps round at Chain Length. First goes "
        "back to pattern 1. Random picks one from inside the chain. The chain is worked "
        "out from the host's beat position, so looping and scrubbing land on the pattern "
        "they should."),
   STEP(kParamChainLength, "chain_length", "Chain Length", "Sequencer", 1.0, 64.0, 4.0, "",
        "How many patterns the chain covers, counting from pattern 1: where Next wraps "
        "round and what Random picks from. One setting for the whole bank."),

   // -------------------------------------------------------- the Devil Fish
   //
   // Robin Whittle's modification, from his own manual. Every default here is
   // the setting his "Limiting the Devil Fish to TB-303 sounds" table gives, so
   // a preset that says nothing about any of them is the stock machine and
   // renders exactly as it did before these existed.
   LIN(kParamOverdrive, "overdrive", "Overdrive", "VCF", -60.0, 36.5, 0.0, "dB",
       "How hard the oscillator is driven into the filter, which is not the same knob "
       "as Drive: this one is in front of the ladder and Drive is behind it. The "
       "machine has no control here at all -- the level is fixed -- and 0 dB is that "
       "level. Up from there the ladder's input pair stops being linear and starts "
       "switching, which is Whittle's \"the filter operates under duress\"; the top of "
       "the range is his 66.6 times normal. Down at the bottom the oscillator is gone "
       "altogether, which is only interesting with Res Range past 100 %: the filter "
       "sings on its own and this knob reintroduces the oscillator by hand."),
   PCT(kParamFilterFM, "filter_fm", "Filter FM", "VCF", 0.0,
       "The amplifier's own output fed back into the filter's frequency, at audio rate. "
       "Nothing on the machine does this. It is loudest where the signal is loudest, so "
       "it bites hardest on accented notes and wherever Overdrive is up, and it needs "
       "resonance to have anything to work with. A little is edge. A lot is what "
       "Whittle calls a spluttering chaotic mess, and he is right. The filter "
       "coefficients are recomputed every sample while this is up, rather than every "
       "eighth, because that is what audio-rate modulation costs."),
   ENUM(kParamMuffler, "muffler", "Muffler", "Drive", 0.0, kMufflerNames,
        "A clipper on the amplifier's output, after everything else. It is not a fuzz: "
        "it only touches signals that are already loud -- an accent, a high Overdrive, "
        "a hot external level -- and it leaves the bottom of the spectrum alone, so "
        "what it takes off is the top of the loudest peaks rather than the weight of "
        "the note. Two kinds, because the modification offers two. Off is the machine."),
   LOG(kParamSoftAttack, "soft_attack", "Soft Attack", "Amp", 0.4326507131, 0.3, 30.0, "ms",
       "How fast the amplifier opens on an unaccented note. The machine's is fixed by "
       "C41 and R134 at 2.2 ms, which is the default and is as good as instant; an "
       "accented note always uses it whatever this says. Turned up, the note swells "
       "instead of starting, which is the one thing a 303 cannot do and the reason "
       "Whittle put a pot on it."),
   LOG(kParamAmpDecay, "amp_decay", "Amp Decay", "Amp", 0.8647887373, 16.0, 8000.0, "ms",
       "How long the amplifier takes to fall away under a held note, to a tenth. On the "
       "machine this is not a control: R123 and C42 fix it at 1.5 s, which reaches a "
       "tenth in about 3.45 s, and that is the default. It is long enough that over a "
       "sixteenth note nothing happens -- the 303's amplifier holds while the filter "
       "falls, and that is why the instrument sounds the way it does. Shorten it and "
       "the notes start closing on their own."),
   PCT(kParamAmpSustain, "amp_sustain", "Amp Sustain", "Amp", 0.0,
       "Where Amp Decay falls to instead of silence. At zero -- the machine -- a held "
       "note dies away on its own. Turned up it stops falling partway and holds there "
       "for as long as the gate is open, so a note can run indefinitely. There is "
       "nothing in the schematic that does this."),
   ENUM(kParamSweepSpeed, "sweep_speed", "Sweep Speed", "Accent", 0.0, kSweepSpeedNames,
        "How the accent circuit answers accents in quick succession. Normal is the "
        "machine: charge left in C62 from one accent makes the next one bigger, which "
        "is the thing people mean when they say a 303 gets worked up. Fast is the "
        "opposite -- the first accent is the strongest and the ones behind it are "
        "smaller, because each pulse is what is added rather than what has "
        "accumulated. Slow takes longer to rise, rises about twice as far, and takes "
        "longer to cool, so it is still settling through the notes after it."),
   ENUM(kParamAccentHold, "accent_hold", "Accent Hold", "Accent", 0.0, kOnOffNames,
        "Accents every note, whatever the step's accent bit or the note's velocity "
        "says. Whittle's front panel has a pushbutton for it. Useful for hearing what "
        "the accent circuit is actually doing, and for a bar that has to lean on "
        "everything at once."),

   // The drive stage's shape and its dry/wet, at the end of the table because
   // the table is indexed by id and these two were appended to the enum. They
   // belong to the Drive module, which is what the "Drive" in each row says
   // and what puts them in the right chapter of the manual.
   ENUM(kParamDistType, "dist_type", "Type", "Drive", 0.0, kDistNames,
        "Which model the drive stage is. Each one is an equation out of the "
        "literature rather than a name on a switch, and they are built differently "
        "from each other rather than merely curved differently -- which is the only "
        "way a set of these tells itself apart. Soft Clip is the stage this "
        "instrument has always had and is the default. Overdrive keeps a linear "
        "region below a third of full scale, so it follows the playing. Tube sits at "
        "a work point off centre and is far harder on one half of the wave than the "
        "other, which is where its even harmonics come from. Valve Stack is three "
        "gain stages in series, each with its own DC blocker and a shelf where the "
        "cathode capacitor would be. Fuzz is exponential from the first volt and has "
        "no linear region at all. Hard Clip is a flat top. Rectifier folds the "
        "negative half onto the positive one, which doubles the fundamental -- an "
        "octave up over the note rather than an edge on it. Crush quantises to fewer "
        "bits. Germanium is the odd one out and the only one taken from a circuit "
        "rather than from a book: the 1970s stompbox whose gain stage lifts the "
        "harmonics forty-odd decibels and leaves the fundamental alone, into a pair of "
        "germanium diodes wired across the path. It is the one model here that "
        "distorts part of the spectrum and not the rest, which is why it stays tight "
        "on a bass line where the others thicken. See the manual for which source each "
        "one comes from."),
   PCT(kParamDistMix, "dist_mix", "Dist Mix", "Drive", 1.0,
       "How much of the driven signal is heard against the clean one. At 100 % the "
       "stage is in the path, which is where it has always been and what every preset "
       "written before this control assumes. Below that, the driven signal is mixed "
       "back with the filter's own output -- which is how a hard model is made usable: "
       "Fuzz or Rectifier at 25 % adds something to a line that is otherwise still the "
       "machine, and a rectifier mixed in quietly is an octave under the note rather "
       "than a fuzz box. At zero the stage is bypassed however Drive and Type are set, "
       "and the only thing after the filter is Tone."),
   BIPCT(kParamDistBias, "dist_bias", "Bias", "Drive", 0.0,
         "Where the model sits on its own curve. At the centre each one is at the "
         "operating point its source specifies -- the work point of the tube model, "
         "the matched pair of the hard clipper, half-wave and full-wave in equal "
         "measure for the rectifier. Away from the centre the two halves of the wave "
         "are treated differently, which is what puts even harmonics into a sound "
         "that otherwise has only odd ones, and what makes a distortion sound like a "
         "circuit rather than like arithmetic. It does nothing at all to Soft Clip, "
         "which is symmetric by construction and stays as it was."),

   // ----------------------------------------------------------------- delay
   //
   // The last stage in the box, and the second one the service notes know
   // nothing about. Every part of it is an equation out of a named source --
   // see dsp/delay.h for which, and for the one place it leaves them, which is
   // the feedback going past unity.
   ENUM(kParamDelayOn, "delay_on", "Enable", "Delay", 0.0, kOnOffNames,
        "Whether the delay is in the path at all. Off is the default and off is "
        "what every preset written before this stage existed loads as, so nothing "
        "that already sounded a particular way has moved. Off is a true bypass: the "
        "lines stop being read and the repeats stop being mixed, but what is already "
        "in them is kept, so switching back on carries on from where it was rather "
        "than from silence."),
   ENUM(kParamDelaySync, "delay_sync", "Sync", "Delay", 1.0, kDelaySyncNames,
        "Where the delay time comes from. Synced takes it from the host's tempo and "
        "the Division chip, which is what keeps the repeats in time with the "
        "sequencer through a tempo change. Free takes it from the Time knob and "
        "ignores the tempo, which is the setting for the delays that are not "
        "supposed to be in time -- a slapback under 100 ms, or a length deliberately "
        "against the grid."),
   LOG(kParamDelayTime, "delay_time", "Time", "Delay", 0.5880456, 20.0, 2000.0, "ms",
       "The delay time when Sync is Free, from a slapback to two seconds. It does "
       "nothing when Sync is Synced. The read head glides to a new setting over "
       "about 50 ms rather than jumping, so turning this knob bends the repeats the "
       "way a tape delay does instead of clicking -- which is a sound in its own "
       "right and is the reason the smoothing is that slow."),
   ENUM(kParamDelayDivision, "delay_div", "Division", "Delay", 5.0, kDelayDivNames,
        "The delay time as a note value, when Sync is Synced. A dotted eighth is the "
        "one this instrument is usually reached for with: it puts a repeat between "
        "every pair of sixteenths and turns a plain line into a rolling one. The "
        "triplet values do the same against a straight pattern and the straight ones "
        "reinforce it. Half a note at a slow tempo is longer than the buffer holds "
        "and is clamped to three seconds."),
   LIN(kParamDelayFeedback, "delay_feedback", "Feedback", "Delay", 0.0, 130.0, 35.0, "%",
       "How much of each repeat is fed back in to make the next one. Up to 100 % the "
       "stage is the IIR comb filter its source prints, and the repeats die away at "
       "a rate this sets. Past 100 % they do not: the source's own stability "
       "condition is broken on purpose and the loop is held up by the soft clipper "
       "in it instead, so the repeats grow, saturate and then sit there as a "
       "self-oscillating drone under whatever is played over them. That is what the "
       "top thirty per cent of this knob is for. It cannot run away -- the clipper "
       "bounds it -- but it will not stop on its own either, and Delay off or Mix at "
       "zero is how it is stopped."),
   PCT(kParamDelayMix, "delay_mix", "Mix", "Delay", 0.25,
       "How much of the repeats is heard against the dry instrument. A crossfade, "
       "the same one Dist Mix is, so at 100 % only the delay is heard and the "
       "instrument itself is gone -- which is worth knowing before turning it all "
       "the way up. At zero the stage is bypassed however everything else is set."),
   ENUM(kParamDelayMode, "delay_mode", "Routing", "Delay", 1.0, kDelayModeNames,
        "How the two delay lines are wired. Mono is one line heard in both channels: "
        "the repeats sit exactly where the instrument does. Stereo is two lines, the "
        "right one running at two thirds of the left, so the repeats interleave and "
        "spread. Ping-Pong crosses the input and the feedback, which walks each "
        "repeat from one side to the other and back. Width opens or closes whichever "
        "of the two stereo modes is set, and has nothing to do in Mono."),
   LIN(kParamDelayWidth, "delay_width", "Width", "Delay", 0.0, 200.0, 100.0, "%",
       "How wide the repeats are spread, as a mid/side matrix over the delay's own "
       "output and nothing else -- the dry instrument stays where it is. 100 % is "
       "the two lines as they come out, 0 % folds them to the middle, and above that "
       "they are pushed outwards past where a pair of speakers puts them. It does "
       "nothing in Mono, where the two sides are the same signal and there is no "
       "side to widen."),

   // ----------------------------------------------------- live mode's octave
   STEP(kParamPatternOctave, "pattern_octave", "Pattern Oct", "Sequencer", -4.0, 4.0, 0.0, "oct",
        "Moves the whole running pattern by octaves, without touching a single "
        "step. It is what replaces the keyboard in Live mode, where the keys are "
        "busy selecting patterns -- but it is not limited to Live mode and adds to "
        "the held-key transpose in the other two, so a line written low can be "
        "played an octave up without rewriting it or holding a key. A step that "
        "would land outside MIDI's own range is clamped rather than wrapped, so a "
        "pattern pushed four octaves up flattens at the top instead of folding "
        "back into the bass."),
   ENUM(kParamPatternTrigger, "pattern_trigger", "Trigger", "Sequencer", 0.0, kTriggerNames,
        "When a pattern selected while the sequencer runs takes over. At End lets the "
        "playing one reach its last step first, which is the hardware's behaviour and "
        "the default. Instant switches on the next step and carries on from the same "
        "place in the new pattern, for cutting between patterns mid-bar. Restart "
        "switches on the next step too, but plays the new pattern from its first step."),
   STEP(kParamChainRepeat, "chain_repeat", "Chain Repeat", "Sequencer", 1.0, 256.0, 1.0, "",
        "How many times the pattern on screen plays before its chain moves on. Every "
        "pattern has its own, so a chain can play one pattern four times and the next "
        "once. Stay ignores it."),
};

#undef LIN
#undef PCT
#undef BIPCT
#undef LOG
#undef STEP
#undef ENUM

// The table is indexed by id everywhere -- paramByIdIn() returns the entry only
// when table[id].id == id -- so a row out of order shows up as a parameter the
// host cannot read rather than as a wrong one.

} // namespace

// How many beats each delay Division is. A quarter note is one beat, a triplet
// is two thirds of the straight value it is named after, and a dotted one is
// one and a half of it.
double delayDivisionBeats(int division) {
   switch (division) {
   case kDelayDiv32:
      return 0.125;
   case kDelayDiv16T:
      return 1.0 / 6.0;
   case kDelayDiv16:
      return 0.25;
   case kDelayDiv8T:
      return 1.0 / 3.0;
   case kDelayDiv8Dot:
      return 0.75;
   case kDelayDiv4T:
      return 2.0 / 3.0;
   case kDelayDiv4:
      return 1.0;
   case kDelayDiv2:
      return 2.0;
   case kDelayDiv8:
   default:
      return 0.5;
   }
}

// Steps per beat for each Rate setting. 1/16 is four to the beat.
double stepsPerBeat(int rate) {
   switch (rate) {
   case kRate32:
      return 8.0;
   case kRate16T:
      return 6.0;
   case kRate8T:
      return 3.0;
   case kRate8:
      return 2.0;
   case kRate16:
   default:
      return 4.0;
   }
}

namespace {

// Where a pattern in `mode` hands over to on the `cycle`-th time round.
int chainNext(int mode, int from, int span, long cycle) {
   switch (mode) {
   case kChainFirst:
      return 0;
   case kChainRandom: {
      // A hash of the cycle rather than a running generator: the same bar of
      // the song always picks the same pattern, however it was reached.
      uint32_t x = static_cast<uint32_t>(cycle) * 2654435761u + 0x9E3779B9u;
      x ^= x >> 16;
      x *= 0x7FEB352Du;
      x ^= x >> 15;
      return static_cast<int>(x % static_cast<uint32_t>(span));
   }
   case kChainNext:
   default:
      // A pattern outside the chain feeds into its first pattern.
      return from + 1 < span ? from + 1 : 0;
   }
}

} // namespace

int chainPatternAt(ChainWalk &walk, const int *modes, const int *repeats, int start,
                   int chainLength, long cycle) {
   const int span = chainLength < 1 ? 1 : (chainLength > 64 ? 64 : chainLength);
   const int from = start < 0 ? 0 : (start > 63 ? 63 : start);
   if (walk.start != from || cycle < walk.cycle)
      walk = ChainWalk{from, 0, from, 1};
   while (walk.cycle < cycle) {
      const int mode = modes[walk.pattern];
      if (mode == kChainStay) {
         // Nothing leaves a Stay, so there is nothing to walk through.
         walk.cycle = cycle;
         break;
      }
      ++walk.cycle;
      const int rep = repeats[walk.pattern] < 1 ? 1 : repeats[walk.pattern];
      if (walk.plays >= rep) {
         walk.pattern = chainNext(mode, walk.pattern, span, walk.cycle);
         walk.plays = 1;
      } else {
         ++walk.plays;
      }
   }
   return cycle <= 0 ? from : walk.pattern;
}

int scaleNotes(int scale, int *out) {
   // The root is always first, which is what lets the generator weight it.
   switch (scale) {
   case kScaleMajor: {
      static const int v[] = {0, 2, 4, 5, 7, 9, 11};
      for (int i = 0; i < 7; ++i)
         out[i] = v[i];
      return 7;
   }
   case kScaleMinorPent: {
      static const int v[] = {0, 3, 5, 7, 10};
      for (int i = 0; i < 5; ++i)
         out[i] = v[i];
      return 5;
   }
   case kScaleMajorPent: {
      static const int v[] = {0, 2, 4, 7, 9};
      for (int i = 0; i < 5; ++i)
         out[i] = v[i];
      return 5;
   }
   case kScaleDorian: {
      static const int v[] = {0, 2, 3, 5, 7, 9, 10};
      for (int i = 0; i < 7; ++i)
         out[i] = v[i];
      return 7;
   }
   case kScalePhrygian: {
      static const int v[] = {0, 1, 3, 5, 7, 8, 10};
      for (int i = 0; i < 7; ++i)
         out[i] = v[i];
      return 7;
   }
   case kScaleBlues: {
      static const int v[] = {0, 3, 5, 6, 7, 10};
      for (int i = 0; i < 6; ++i)
         out[i] = v[i];
      return 6;
   }
   case kScaleChromatic: {
      for (int i = 0; i < 12; ++i)
         out[i] = i;
      return 12;
   }
   case kScaleMinor:
   default: {
      static const int v[] = {0, 2, 3, 5, 7, 8, 10};
      for (int i = 0; i < 7; ++i)
         out[i] = v[i];
      return 7;
   }
   }
}

const ParamDesc *paramTable() { return kParams; }

const ParamDesc *paramById(uint32_t id) { return paramByIdIn(kParams, kNumParams, id); }

const ParamDesc *paramByKey(const char *key) { return paramByKeyIn(kParams, kNumParams, key); }

} // namespace saeurekiste
