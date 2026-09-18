#pragma once

// The sequencer's pattern: sixteen steps of pitch, octave and three flags.
//
// It is deliberately *not* in the parameter table. Sixteen steps times five
// fields is eighty values, and a host's parameter list is not the place for
// them -- nor is automating step 7's slide flag a thing anybody wants. So the
// pattern travels two ways of its own: packed into the plugin's state blob, and
// written into the preset file as five readable lines, which the plugin's own
// preset wrappers add on top of the shared format.
//
// Each step packs into sixteen bits so the audio thread can read one with a
// single relaxed atomic load:
//
//   bits 0-3   note + 1   0 = rest, 1..12 = C..B
//   bits 4-5   octave + 1 0 = -1, 1 = 0, 2 = +1
//   bit  6     accent
//   bit  7     slide
//   bit  8     vibrato

#include <cstdint>
#include <string>

namespace threeohthree {

constexpr int kMaxSteps = 16;

enum Lane { kLaneSlide = 0, kLaneAccent, kLaneVibrato, kNumLanes };

struct Step {
   int note = -1;   // -1 rest, 0..11 = C..B
   int octave = 0;  // -1, 0, +1
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

// A whole pattern as it travels through a preset load.
struct PatternData {
   bool present = false;
   uint16_t steps[kMaxSteps] = {0};
};

// ------------------------------------------------------------------ generator
//
// What the pattern generator is told. Everything about a generated pattern is
// in here: the same settings always produce the same sixteen steps, so a line
// somebody likes is a number they can write down rather than a file.
struct GenSettings {
   uint32_t seed = 1;
   int scale = 0; // ScaleKind, see params.h
   int root = 0;  // 0 = C
   double notes = 0.78;
   double accent = 0.3;
   double slide = 0.2;
   double octave = 0.22;
   double vibrato = 0.06;
};

// Fills `steps` (kMaxSteps of them) from the settings.
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

// The default pattern a fresh instance starts with. Sixteen steps that show
// what the instrument does rather than sixteen rests, because an empty
// sequencer looks broken.
void defaultPattern(uint16_t *steps);

// The five preset lines, ready to append to the shared format's output.
std::string formatPattern(const uint16_t *steps);

// How the window reaches the pattern.
//
// The window runs on the main thread; the sequencer reads the pattern from the
// audio thread. Every step is one packed 16-bit word behind a relaxed atomic,
// so a read never tears and an edit takes effect on the next step boundary.
class PatternAccess {
public:
   virtual ~PatternAccess() = default;

   virtual Step seqStep(int index) const = 0;
   virtual void seqSetStep(int index, const Step &step) = 0;
   // How many steps the pattern runs for, from the Steps parameter.
   virtual int seqLength() const = 0;
   // Which step is sounding, or -1 when the sequencer is not running.
   virtual int seqPlayhead() const = 0;
   // Whether Mode is set to Sequencer at all.
   virtual bool seqEnabled() const = 0;

   // The generator. The window does not know what a scale is or which
   // parameter holds the seed -- it only has three buttons, and the plugin
   // reads its own table.
   virtual int seqSeed() const = 0;
   // Writes the seed back as a proper parameter edit, so the host sees it, and
   // regenerates from it.
   virtual void seqSetSeed(int seed) = 0;
   virtual void seqGenerate() = 0;
};

// Feeds one "key = value" line to the pattern parser. Returns true if the key
// was one of the pattern's, whether or not the value made sense -- the caller
// uses that only to know the preset carried a pattern at all.
bool parsePatternLine(const std::string &key, const std::string &value, PatternData &out);

} // namespace threeohthree
