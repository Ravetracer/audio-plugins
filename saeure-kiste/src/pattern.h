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

// The whole bank as it travels through a preset load. Flat rather than
// two-dimensional, because every path that touches it -- the preset text, the
// state blob, the plugin's own store -- walks it in one pass.
struct PatternData {
   bool present = false;
   uint16_t steps[kMaxPatterns * kMaxSteps] = {0};  // 16 kB, and it travels by
                                                    // reference everywhere

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
std::string formatPattern(const uint16_t *steps);

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
   // Whether a pattern has any notes in it, for the bank grid's shading.
   virtual bool seqPatternEmpty(int pattern) const = 0;

   // The generator. The window does not know what a scale is or which
   // parameter holds the seed -- it only has three buttons, and the plugin
   // reads its own table.
   virtual int seqSeed() const = 0;
   // Writes the seed back as a proper parameter edit, so the host sees it, and
   // regenerates from it.
   virtual void seqSetSeed(int seed) = 0;
   // Regenerates from the seed that is set. Deterministic, and what the - and
   // + buttons use.
   virtual void seqGenerate() = 0;
   // Picks a new seed and generates from that: what GEN does, and what a
   // player expects from a button called GEN -- a new pattern every press.
   virtual void seqGenerateNew() = 0;

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
