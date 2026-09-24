#pragma once

// The sequencer's pattern: up to 64 steps, each saying which of the eleven
// voices play, how hard, whether they flam, and whether the step carries the
// total accent.
//
// It is deliberately *not* in the parameter table, for the reason SäureKiste
// gives: twelve rows of sixty-four steps is not a thing a host's parameter list
// can hold sensibly, and nobody wants to automate step 7 of the rim shot. So the
// bank travels two ways of its own: packed into the plugin's state blob, and
// written into the preset file as readable lines, one per row.
//
// Each step packs into thirty-two bits so the audio thread can read one with a
// single relaxed atomic load:
//
//   bits 2v, 2v+1   voice v, 0..10: 0 = silent, 1 = plays, 2 = plays accented
//   bit  22         the total accent
//   bits 23 + v     voice v flams, for the five voices that can (BD, SD, toms)
//
// The two-bit field has a spare value, 3, which reads as "plays accented" so a
// future use of it cannot turn into silence in an older build.

#include <cstdint>
#include <string>

namespace rumpelkiste {

// How long a pattern may be. The machine had sixteen steps -- Last Step ran 1
// to 16 -- and a pattern is still sixteen by default. Sixty-four is four bars
// of sixteenths, the whole bank at that length is 16 kB of state, and the grid
// scrolls to anything past thirty-two.
constexpr int kMaxSteps = 64;
constexpr int kDefaultSteps = 16;

// How many patterns the bank holds. The machine held 96 in two banks of three
// groups; sixty-four fits an eight-by-eight grid beside the step editor, which
// is what SäureKiste settled on too.
constexpr int kMaxPatterns = 64;

// The grid's rows. The eleven voices are rows 0 to 10, in the order of the
// Voice enum in params.h; the total accent is a row of its own on top.
constexpr int kNumTracks = 12;
constexpr int kTrackAccent = 11;
constexpr int kNumVoiceTracks = 11;

// A step's level on one voice.
enum StepLevel { kLevelOff = 0, kLevelOn = 1, kLevelAccent = 2 };

// Which voices can flam. The owner's manual lists them: bass drum, snare and
// the three toms -- the voices with two keys on the machine.
inline bool voiceCanFlam(int voice) { return voice >= 0 && voice <= 4; }

inline int stepLevel(uint32_t w, int voice) {
   if (voice < 0 || voice >= kNumVoiceTracks)
      return kLevelOff;
   const int v = static_cast<int>((w >> (2 * voice)) & 3u);
   return v == 3 ? kLevelAccent : v;
}

inline uint32_t withLevel(uint32_t w, int voice, int level) {
   if (voice < 0 || voice >= kNumVoiceTracks)
      return w;
   const uint32_t l = static_cast<uint32_t>(level < 0 ? 0 : (level > 2 ? 2 : level));
   w &= ~(3u << (2 * voice));
   w |= l << (2 * voice);
   // A voice that stops playing keeps no flam: there is nothing left to flam,
   // and the grid would show a flam mark on an empty cell.
   if (l == 0 && voiceCanFlam(voice))
      w &= ~(1u << (23 + voice));
   return w;
}

inline bool stepAccent(uint32_t w) { return (w >> 22) & 1u; }
inline uint32_t withAccent(uint32_t w, bool on) {
   return on ? (w | (1u << 22)) : (w & ~(1u << 22));
}

inline bool stepFlam(uint32_t w, int voice) {
   return voiceCanFlam(voice) && ((w >> (23 + voice)) & 1u);
}
inline uint32_t withFlam(uint32_t w, int voice, bool on) {
   if (!voiceCanFlam(voice))
      return w;
   return on ? (w | (1u << (23 + voice))) : (w & ~(1u << (23 + voice)));
}

// Whether a track -- a voice row or the accent row -- is set on a step at all.
inline bool trackOn(uint32_t w, int track) {
   return track == kTrackAccent ? stepAccent(w) : stepLevel(w, track) != kLevelOff;
}

// The whole bank as it travels through a preset load. Flat rather than
// two-dimensional, because every path that touches it walks it in one pass.
struct PatternData {
   bool present = false;
   uint32_t steps[kMaxPatterns * kMaxSteps] = {0}; // 16 kB, travels by reference

   // What each pattern does when it has played through -- a ChainKind -- and
   // how many times it plays first.
   bool chainPresent = false;
   int chainMode[kMaxPatterns] = {0};
   int chainRepeat[kMaxPatterns];

   PatternData() {
      for (int &r : chainRepeat)
         r = 1;
   }

   uint32_t *pattern(int index) { return steps + index * kMaxSteps; }
   const uint32_t *pattern(int index) const { return steps + index * kMaxSteps; }
};

// Whether a pattern has anything in it at all. What the bank grid shades and
// what decides whether a preset bothers to write the pattern out.
bool patternEmpty(const uint32_t *steps);

// One past the last step that has anything on it, and 0 for an empty pattern.
int patternUsedLength(const uint32_t *steps);

// The row's name in a preset file and on the grid: "bd", "sd" ... "ac".
const char *trackKey(int track);
// And as the grid labels it: "BD", "SD" ... "AC".
const char *trackLabel(int track);

// ------------------------------------------------------------------ live map
//
// What one MIDI note does to the bank, when it is mapped. SäureKiste's pattern
// map, kept: a note can select a pattern outright or step the bank, and a
// mapped note is consumed -- it does not also play a voice.
enum NoteAction {
   kNoteNone = -1,        // this note is not mapped: it plays its voice, if any
   kNotePrevPattern = -2, // step back one, and stop at the first
   kNoteNextPattern = -3, // step on one, and stop at the last
   // Anything from 0 upwards selects that pattern outright.
};

// Where stepping lands. Clamped rather than wrapped at both ends, so a pad
// pressed at the end of the bank does nothing rather than jumping to 1.
int steppedPattern(int current, int delta, int count);

// ------------------------------------------------------------------ generator
//
// What the pattern generator is told. The same settings always produce the
// same pattern, so a groove somebody likes is a number they can write down.
struct GenSettings {
   uint32_t seed = 1;
   int style = 0; // GenStyle, see params.h
   int length = kDefaultSteps;
   double busy = 0.5;
   double accent = 0.35;
};

// Fills `steps` (kMaxSteps of them, `settings.length` of them written) from
// the settings. Every step draws all of its random numbers whether or not it
// uses them, so turning Busy up adds hits to the pattern that was there and
// turning Accents up moves accents and nothing else.
void generatePattern(const GenSettings &settings, uint32_t *steps);

// A seed to generate from next, for the GEN button: never equal to `current`,
// and in 0..maxSeed. `maxSeed` may be the top of a 32-bit word, which is why
// this does not add one to it.
uint32_t nextGeneratorSeed(uint32_t current, uint32_t salt, uint32_t maxSeed);

// The pattern a fresh instance starts with, so the sequencer shows what the
// instrument does rather than sixteen empty columns.
void defaultPattern(uint32_t *steps);

// The bank's preset lines, ready to append to the shared format's output. An
// empty pattern is left out, and so is an empty row inside one.
std::string formatPattern(const PatternData &bank);

// Feeds one "key = value" line to the pattern parser. Returns true if the key
// was one of the bank's, whether or not the value made sense.
bool parsePatternLine(const std::string &key, const std::string &value, PatternData &out);

// How the window reaches the pattern.
//
// The window runs on the main thread; the sequencer reads the pattern from the
// audio thread. Every step is one packed word behind a relaxed atomic, so a
// read never tears and an edit takes effect on the next step.
class PatternAccess {
public:
   virtual ~PatternAccess() = default;

   virtual uint32_t seqStep(int pattern, int index) const = 0;
   virtual void seqSetStep(int pattern, int index, uint32_t step) = 0;
   // How many steps the pattern runs for, from Last Step.
   virtual int seqLength() const = 0;
   // Which step is sounding, or -1 when the sequencer is not running.
   virtual int seqPlayhead() const = 0;
   // Whether Mode is Sequencer.
   virtual bool seqEnabled() const = 0;

   virtual int seqPattern() const = 0;
   virtual int seqPlayingPattern() const = 0;
   virtual int seqShownPattern() const = 0;
   virtual int seqPendingPattern() const = 0;
   virtual bool seqPatternEmpty(int pattern) const = 0;

   // The generator. The window has three buttons and a seed; the plugin reads
   // its own table for the rest.
   virtual uint32_t seqSeed() const = 0;
   virtual uint32_t seqSeedMax() const = 0;
   virtual void seqSetSeed(uint32_t seed) = 0;
   virtual void seqGenerate() = 0;
   virtual void seqGenerateNew() = 0;

   // The pattern map. Only offered in Sequencer mode, where a pattern plays.
   virtual int seqLearnTarget() const = 0;
   virtual void seqSetLearnTarget(int target) = 0;
   virtual int seqMappedNote(int target) const = 0;
   virtual void seqClearMap(int target) = 0;
   virtual void seqStepPattern(int delta) = 0;

   // Mutes, per voice row. Part of the plugin's state, not of a preset: a mute
   // is a thing done while playing, and a preset that silenced a voice would be
   // a broken preset.
   virtual bool seqMuted(int voice) const = 0;
   virtual void seqSetMuted(int voice, bool muted) = 0;

   // Plays one voice once, for the window's row labels.
   virtual void seqAudition(int voice) = 0;
   // How many times each voice has been triggered, by anything. Monotonic; the
   // window lights a row's label when its count moves.
   virtual uint32_t seqVoiceHits(int voice) const = 0;

   // Free running: the sequencer plays at the host's tempo while the host's
   // transport is stopped. What the PLAY button does.
   virtual bool seqFreeRunning() const = 0;
   virtual void seqSetFreeRunning(bool on) = 0;
   // Whether the host's transport is rolling, which overrides free running.
   virtual bool seqHostPlaying() const = 0;

   // Writes the shown pattern to a temporary .mid file and returns the path,
   // or an empty string if there was nothing to write.
   virtual std::string seqExportMidi() = 0;
};

} // namespace rumpelkiste
