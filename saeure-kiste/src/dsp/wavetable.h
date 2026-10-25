#pragma once

// Wavetables for the oscillator's Wavetable waveform.
//
// Nothing here is the machine's. The oscillator on the main board makes a ramp
// and a square and nothing else; the wavetables are one of the additions that
// turn the instrument into something to experiment with, and they come from
// the ordinary practice of wavetable synthesis rather than from a source:
//
// - A table is a run of single cycles, its *frames*, and the Shape control
//   picks a position along them. Two neighbouring frames are crossfaded, so a
//   sweep of Shape morphs from one end of the table to the other.
// - Every frame is stored band-limited at several levels, one per octave of
//   pitch, each with half the harmonics of the one before. A note reads the
//   two levels whose harmonics fit under Nyquist at its pitch and crossfades
//   them, so nothing folds back however high it is played and a slide through
//   the levels does not step.
//
// One cycle of every frame is kept at kWtSourceLen samples, which is what the
// highest level needs: 511 harmonics. That is also the form a user's table is
// stored in, in a preset and in a project, so a table loaded back is built
// from exactly the numbers that were saved.

#include <cstdint>
#include <string>
#include <vector>

namespace saeurekiste {

// One cycle of the stored form.
constexpr int kWtSourceLen = 1024;
// The most frames a table may have. A user's file with more is thinned to this
// many, evenly across its length, which keeps what a preset has to carry under
// a quarter of a megabyte.
constexpr int kWtMaxFrames = 64;
// Band-limited levels per frame: 512, 256, ... 1 harmonics.
constexpr int kWtLevels = 10;

// The tables that come with the plugin, in the order of the Table parameter.
// A preset stores the *name*, so this list may not be reordered.
enum BuiltinTable {
   kTableAnalog = 0,
   kTablePwm,
   kTableVocal,
   kTableAdditive,
   kTableSync,
   kTableFm,
   kTableFold,
   kTableResonant,
   kNumBuiltinTables
};

// The Table parameter's last value: whatever the user has loaded.
constexpr int kTableUser = kNumBuiltinTables;

class Wavetable {
public:
   // From `frames` cycles of kWtSourceLen samples each. Each cycle is reduced
   // to its harmonics (DC is dropped), and the whole table is scaled so that
   // its loudest frame peaks at 1. Returns false for an empty table.
   bool build(const float *cycles, int frames);

   // The same from the stored form: what a preset or a project carries.
   bool build(const int16_t *cycles, int frames);

   int frames() const { return mFrames; }
   bool empty() const { return mFrames == 0; }

   // One sample. `phase` is 0..1 through the cycle, `position` 0..1 through
   // the frames, and `level` is fractional in 0..kWtLevels-1, from levelFor().
   // Audio-thread safe: it reads and never writes.
   float sample(float phase, float position, float level) const;

   // Which level a note at `hz` reads, so that its highest harmonic stays
   // below Nyquist. Fractional, so the two levels around it are crossfaded.
   static float levelFor(float hz, float sampleRate);

   // The table as it is stored: kWtSourceLen samples per frame, scaled to the
   // full range of an int16. Built for a user's table; empty for the built-in
   // ones, which are made rather than kept.
   const std::vector<int16_t> &stored() const { return mStored; }

   std::string name;

private:
   int mFrames = 0;
   int mFrameStride = 0;
   int mOffset[kWtLevels] = {0};
   int mSize[kWtLevels] = {0};
   std::vector<float> mData;
   std::vector<int16_t> mStored;
};

// The built-in tables. Made on first use -- which takes a fraction of a second
// and must therefore be on the main thread, which AcidEngine::prepare() sees
// to -- and shared by every instance in the process, because nothing in them
// ever changes.
const Wavetable &builtinTable(int index);

// One table, read out of a WAV file, in the stored form: kWtSourceLen samples
// per frame, band-limited, without DC and scaled so the loudest frame uses the
// whole of an int16. Wavetable::build() takes it from there.
//
// How long a frame is: the `clm ` chunk some wavetable editors write says so
// outright, and is believed. Without it a file that divides into 2048-sample
// frames is taken as a table of them, which is the common convention, and
// anything shorter than 4096 samples is taken as one single cycle of whatever
// length it is. Each frame is resampled to kWtSourceLen. Mono files and the
// left channel of anything else; 8, 16, 24 and 32-bit PCM and 32-bit float.
bool readWavetableWav(const std::string &path, std::vector<int16_t> &stored, int &frames,
                      std::string &error);

// One entry of the user's wavetable folder, in the order a tree shows it:
// each folder's subfolders first, then its WAV files, both alphabetical, and
// every folder straight before what is in it. Folders with no WAV file
// anywhere beneath them are left out, as are hidden files and folders.
struct TableFile {
   std::string path;   // relative to the folder, '/'-separated, ".wav" included
   std::string label;  // the file or folder name, a file without its extension
   int depth = 0;      // 0 for the folder's own top level
   bool folder = false;
};
std::vector<TableFile> listWavetableFolder(const std::string &root);

// The name a table read from the folder carries, which is also how the list
// finds it again: its relative path without the extension.
std::string tableNameFor(const std::string &relativePath);

// The stored form as text, for a preset line, and back. Base64 of the int16
// samples, little-endian.
std::string encodeTable(const std::vector<int16_t> &samples);
bool decodeTable(const std::string &text, std::vector<int16_t> &samples);

} // namespace saeurekiste
