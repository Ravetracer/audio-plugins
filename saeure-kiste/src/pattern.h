#pragma once

// The sequencer's pattern: up to 128 steps of pitch, octave and three flags.
//
// It is deliberately *not* in the parameter table. Even sixteen steps times
// five fields is eighty values, and a host's parameter list is not the place
// for them -- nor is automating step 7's slide flag a thing anybody wants. So
// the pattern travels two ways of its own: packed into the plugin's state blob,
// and written into the preset file as five readable lines, which the plugin's
// own preset wrappers add on top of the shared format.
//
// Each step packs into sixteen bits so the audio thread can read one with a
// single relaxed atomic load:
//
//   bits 0-3   note + 1   0 = rest, 1..12 = C..B
//   bits 4-5   octave + 1 0 = -1, 1 = 0, 2 = +1  (the direction)
//   bit  6     accent
//   bit  7     slide
//   bit  8     vibrato
//   bit  9     wide       the octave is two rather than one, in that direction
//
// The octave is split across two fields rather than widened in place, and that
// is deliberate: a state blob saved by 0.2.x has bits 4-5 and nothing else, and
// a blob saved by this version is read by 0.2.x as the same step one octave
// less extreme rather than as nonsense. The alternative -- moving the field --
// would have made every old project's pattern unreadable.

#include <cstdint>
#include <string>

namespace saeurekiste {

// How long a pattern may be. The machine had sixteen steps and a pattern is
// still sixteen by default -- Steps runs 1 to kMaxSteps and defaults to
// kDefaultSteps -- but nothing in the sequencer, the storage or the file
// format cares which of the two numbers it is looking at. A long pattern is a
// melodic line rather than a bass figure, which is the point of it.
//
// 128 is where it stops because that is eight bars of sixteenths, the whole
// bank at that length is 16 kB of state, and a step narrower than the grid can
// draw is not an edit anybody can make.
constexpr int kMaxSteps = 128;

// What a fresh pattern, a fresh instance and the Steps parameter's default
// are: the machine's own sixteen. Everything that used to say kMaxSteps and
// meant "the length of a pattern on the hardware" says this instead.
constexpr int kDefaultSteps = 16;

// How far a step may be moved from the pattern's own octave, either way. The
// machine had one switch position up and one down; two is a sequencer feature
// rather than a hardware one, and it is what makes a line span a bass note and
// a lead in the same sixteen steps.
constexpr int kMaxOctave = 2;

// How many patterns the bank holds. The machine had far fewer and a mode
// switch to reach them; sixty-four is enough to write a whole track into and
// still fits an eight-by-eight grid beside the step editor.
constexpr int kMaxPatterns = 64;

enum Lane { kLaneSlide = 0, kLaneAccent, kLaneVibrato, kNumLanes };

struct Step {
   int note = -1;   // -1 rest, 0..11 = C..B
   int octave = 0;  // -2 .. +2
   bool slide = false;
   bool accent = false;
   bool vibrato = false;

   uint16_t pack() const;
   static Step unpack(uint16_t packed);

   bool flag(int lane) const {
      return lane == kLaneSlide ? slide : (lane == kLaneAccent ? accent : vibrato);
   }
   void setFlag(int lane, bool on) {
      if (lane == kLaneSlide)
         slide = on;
      else if (lane == kLaneAccent)
         accent = on;
      else
         vibrato = on;
   }
};

// Whether `held` is held *into* `next`, the step that follows it: the same
// pitch, and a slide out of it.
//
// That pair is what a tie is made of. A slid step's gate runs past the next
// step's onset, and the engine reads a note arriving while another is still
// held as a slide and does not retrigger -- so a run of tied steps is one long
// note, however many steps it covers. The window draws such a run as one bar
// and paints one when a note is dragged across several steps.
//
// The octave has to match as well as the note, because a slide between two
// different pitches is a glide, which is the other thing slide does and is not
// a tie.
bool stepsTied(const Step &held, const Step &next);

// The whole bank as it travels through a preset load. Flat rather than
// two-dimensional, because every path that touches it -- the preset text, the
// state blob, the plugin's own store -- walks it in one pass.
struct PatternData {
   bool present = false;
   uint16_t steps[kMaxPatterns * kMaxSteps] = {0};  // 16 kB, and it travels by
                                                    // reference everywhere

   // What each pattern does when it has played through -- a ChainKind -- and
   // how many times it plays first. Per pattern, so a chain can be a route
   // through the bank. `chainPresent` says the text carried them at all: a
   // preset older than this has one global Chain and Chain Repeat parameter
   // instead, which the plugin copies into every pattern.
   bool chainPresent = false;
   int chainMode[kMaxPatterns] = {0};
   int chainRepeat[kMaxPatterns];

   PatternData() {
      for (int &r : chainRepeat)
         r = 1;
   }

   uint16_t *pattern(int index) { return steps + index * kMaxSteps; }
   const uint16_t *pattern(int index) const { return steps + index * kMaxSteps; }
};

// kMaxSteps rests. Not the same as that many zero words: a zero unpacks to an
// octave of -1, which is invisible on a rest but writes a "-" into a preset.
void clearPattern(uint16_t *steps);

// Whether a pattern has anything in it at all. What the bank grid shades and
// what decides whether a preset bothers to write the pattern out.
bool patternEmpty(const uint16_t *steps);

// One past the last step that is not a rest, and 0 for an empty pattern.
//
// What a preset writes out. A pattern is always kMaxSteps words in memory, but
// writing all 128 columns for a sixteen-step line would be unreadable and
// would change every file in the factory library for no reason -- so the text
// carries what the pattern actually uses, rounded up to the machine's sixteen,
// and the reader clears whatever a shorter line does not mention.
int patternUsedLength(const uint16_t *steps);

// ------------------------------------------------------------------ live map
//
// What one MIDI note does in Live mode. A note carries exactly one of these,
// so the map is 128 of them and inverts trivially; several notes may do the
// same thing, which is what lets two pads either side of a controller both
// step forward.
enum NoteAction {
   kNoteNone = -1,        // this note does nothing but run the pattern
   kNotePrevPattern = -2, // step back one, and stop at the first
   kNoteNextPattern = -3, // step on one, and stop at the last
   // Anything from 0 upwards selects that pattern outright.
};

// Where stepping lands. Clamped rather than wrapped, at both ends: a set
// played off pads wants the button to stop doing anything at the end of the
// bank, not to jump back to pattern 1 in the middle of a bar.
int steppedPattern(int current, int delta, int count);

// ------------------------------------------------------------------ generator
//
// What the pattern generator is told. Everything about a generated pattern is
// in here: the same settings always produce the same sixteen steps, so a line
// somebody likes is a number they can write down rather than a file.
struct GenSettings {
   uint32_t seed = 1;
   int scale = 0; // ScaleKind, see params.h
   int root = 0;  // 0 = C
   // How many steps to fill. Everything past it is cleared, so the pattern
   // holds nothing the sequencer will not reach.
   //
   // The draws are per step and in order, so a longer pattern begins with
   // exactly the steps a shorter one had: turning Steps up extends the line
   // rather than replacing it, and the seed still describes the pattern
   // completely.
   int length = kDefaultSteps;
   double notes = 0.78;
   double accent = 0.3;
   double slide = 0.2;
   double octave = 0.22;
   double vibrato = 0.06;
};

// Fills `steps` (kMaxSteps of them, `settings.length` of them with notes) from
// the settings.
//
// Two properties matter, and the tests check both:
//
//  1. Every step draws all of its random values whether or not it uses them, so
//     turning Accents up changes only which steps are accented and leaves the
//     notes exactly where they were. A generator that reshuffled everything on
//     every tweak would be a slot machine rather than an instrument.
//
//     Octaves are the one deliberate exception: a jump is far likelier on an
//     accented note, because accent plus an octave up is *the* gesture, so
//     changing the accents does move some octaves. Notes, slides and vibratos
//     never move.
//
//  2. It knows some things about bass lines -- see the implementation.
void generatePattern(const GenSettings &settings, uint16_t *steps);

// A seed to generate from next, for the GEN button.
//
// GEN used to regenerate from the seed that was already set, which made it a
// no-op unless one of the densities had moved -- press it twice and you got
// the same sixteen steps twice. What a player reaches for it for is a new
// line, so it picks a new seed instead: `salt` is whatever the caller has that
// changes between presses, and the result is in 0..maxSeed and never equal to
// `current`, so every press is a pattern that was not there before.
//
// The seed stays visible and stays a parameter, which is the point of it --
// the line GEN just made is a number that can be written down, nudged with the
// - and + buttons, and automated.
uint32_t nextGeneratorSeed(uint32_t current, uint32_t salt, uint32_t maxSeed);

// The default pattern a fresh instance starts with. Sixteen steps that show
// what the instrument does rather than sixteen rests, because an empty
// sequencer looks broken. The rest of the 128 are rests, so a fresh instance
// with Steps turned up plays the sixteen and then silence rather than junk.
void defaultPattern(uint16_t *steps);

// The bank's preset lines, ready to append to the shared format's output.
//
// Pattern 1 keeps the original five keys -- seq_pitch and friends -- so a
// preset written before the bank existed still loads and one written now still
// opens in an older build. Patterns 2 upwards use seq2_pitch, seq3_pitch and so
// on, and an empty pattern is left out rather than written as sixteen dots.
//
// Each pattern's chain is written as seq_chain and seq_repeat beside its steps.
// Pattern 1 always writes both, which is what tells a reader the preset has
// per-pattern chains at all; the others only when they are not Stay and 1.
std::string formatPattern(const PatternData &bank);

// ------------------------------------------------------------------ transpose
//
// What lets a pattern be transposed into a wall and back out of it with its
// shape intact. A step's pitch runs from C two octaves down to B two octaves
// up; a transpose that would take a step past either end leaves it at the end,
// and the memory holds the pitch it *would* have had, so transposing back puts
// every step where it was rather than where the wall left it.
//
// The memory is only good while the pitches are the ones it wrote. Anything
// else that moves a note -- an edit, GEN, a paste -- makes the next transpose
// start again from what is there. Flags do not count; toggling an accent keeps
// the memory.
struct TransposeMemory {
   bool valid = false;
   int offset = 0;
   int8_t base[kMaxSteps];    // each step's pitch when the memory began
   int8_t written[kMaxSteps]; // each step's pitch as last transposed
};

// Moves every note in `steps` (kMaxSteps of them) by `semitones`.
void transposePattern(uint16_t *steps, int semitones, TransposeMemory &memory);

// How the window reaches the pattern.
//
// The window runs on the main thread; the sequencer reads the pattern from the
// audio thread. Every step is one packed 16-bit word behind a relaxed atomic,
// so a read never tears and an edit takes effect on the next step boundary.
class PatternAccess {
public:
   virtual ~PatternAccess() = default;

   // Both take a pattern index as well as a step, because the window edits
   // whichever of the sixty-four the Pattern parameter has selected while the
   // sequencer may well be playing another one.
   virtual Step seqStep(int pattern, int index) const = 0;
   virtual void seqSetStep(int pattern, int index, const Step &step) = 0;
   // How many steps the pattern runs for, from the Steps parameter.
   virtual int seqLength() const = 0;
   // Which step is sounding, or -1 when the sequencer is not running.
   virtual int seqPlayhead() const = 0;
   // Whether Mode is set to Sequencer at all.
   virtual bool seqEnabled() const = 0;

   // Which pattern the grid edits, from the Pattern parameter, zero based.
   virtual int seqPattern() const = 0;
   // Which pattern is sounding, or -1 when the sequencer is not running. Only
   // differs from seqPattern() while a chain is running.
   virtual int seqPlayingPattern() const = 0;
   // The pattern the window shows and edits: the playing one while a chain is
   // running, the selected one otherwise.
   virtual int seqShownPattern() const = 0;
   // The selected pattern while it is waiting for the playing one to reach its
   // end, or -1 when nothing is waiting.
   virtual int seqPendingPattern() const = 0;
   // Whether a pattern has any notes in it, for the bank grid's shading.
   virtual bool seqPatternEmpty(int pattern) const = 0;

   // The generator. The window does not know what a scale is or which
   // parameter holds the seed -- it only has three buttons, and the plugin
   // reads its own table.
   //
   // The seed is unsigned and spans the whole of a 32-bit word, so it is wide
   // enough to take a Unix timestamp. That is more than a knob can be dragged
   // across, which is why the window types it as well as stepping it, and it
   // is why this is uint32_t rather than int: the top half of the range does
   // not fit in a signed one.
   virtual uint32_t seqSeed() const = 0;
   virtual uint32_t seqSeedMax() const = 0;
   // Writes the seed back as a proper parameter edit, so the host sees it, and
   // regenerates from it.
   virtual void seqSetSeed(uint32_t seed) = 0;
   // Regenerates from the seed that is set. Deterministic, and what the - and
   // + buttons use.
   virtual void seqGenerate() = 0;
   // Picks a new seed and generates from that: what GEN does, and what a
   // player expects from a button called GEN -- a new pattern every press.
   virtual void seqGenerateNew() = 0;

   // ------------------------------------------------------------- live mode
   //
   // The window draws the map and arms the learning; the plugin owns it and is
   // the only thing that ever sees a MIDI note.

   // Whether Mode is Live. What decides whether the map is drawn at all.
   virtual bool seqLive() const = 0;

   // Which target the next incoming note will be bound to -- a NoteAction, or
   // a pattern index, or kNoteNone for "not waiting for one".
   virtual int seqLearnTarget() const = 0;
   virtual void seqSetLearnTarget(int target) = 0;

   // Which note is bound to a target, or -1. Takes the same target values as
   // seqSetLearnTarget, so the window asks about a bank cell and the two step
   // buttons the same way.
   virtual int seqMappedNote(int target) const = 0;
   // Unbinds every note that points at a target, and, with kNoteNone, the lot.
   virtual void seqClearMap(int target) = 0;

   // Prev and next by mouse. The same call the mapped notes go through, so the
   // buttons and the pads cannot drift apart.
   virtual void seqStepPattern(int delta) = 0;

   // Transposes a pattern by semitones, keeping its shape across the edges of
   // the grid. See transposePattern().
   virtual void seqTranspose(int pattern, int semitones) = 0;

   // The whole pattern's transpose, in octaves.
   virtual int seqPatternOctave() const = 0;
   virtual void seqSetPatternOctave(int octaves) = 0;

   // Writes the selected pattern to a temporary .mid file and returns the path,
   // or an empty string if there was nothing to write or nowhere to write it.
   // The window drags that file into the host; what goes in it -- the rate, the
   // gate, the swing and the tempo -- is the plugin's to know, not the
   // window's, so the window only asks for a path.
   virtual std::string seqExportMidi() = 0;
};

// Feeds one "key = value" line to the pattern parser. Returns true if the key
// was one of the pattern's, whether or not the value made sense -- the caller
// uses that only to know the preset carried a pattern at all.
bool parsePatternLine(const std::string &key, const std::string &value, PatternData &out);

} // namespace saeurekiste
