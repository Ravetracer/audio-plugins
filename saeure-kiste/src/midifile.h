#pragma once

// A pattern, written out as a standard MIDI file.
//
// What comes out is what the sequencer would play: the step's key, an accent as
// a velocity above the accent threshold, a slid step held past the start of the
// next one, and a vibrato step bracketed by CC1. Those are the same three
// conventions the plugin's own MIDI mode reads back, so a file dropped into a
// DAW and played into this plugin again sounds like the pattern it came from.
//
// Format 0, one track, 960 ticks to the quarter note -- which divides by three
// for the triplet rates and by three again for a triplet swing, so no onset
// lands between two ticks.

#include <cstdint>
#include <string>

namespace saeurekiste {

struct MidiExport {
   const uint16_t *steps = nullptr; // one pattern, kMaxSteps packed words
   int length = 16;                 // how many of them play
   double stepsPerBeat = 4.0;       // the Rate, as steps to the beat
   double gate = 0.5;               // how much of its step a note holds
   double swingPercent = 50.0;      // 50 is straight, 66.7 is triplet swing
   double tempoBpm = 120.0;         // what the file says its tempo is
   double accentVelocity = 100.0;   // the plugin's accent threshold, 1..127
   int patternNumber = 1;           // for the track name
};

// The file's bytes. Empty if there is nothing in the pattern to write.
std::string patternToMidiFile(const MidiExport &spec);

// Where the file goes and what it is called: a temp directory of our own, one
// file per pattern, overwritten each time. Returns an empty string if the
// directory could not be made.
std::string midiExportPath(int patternNumber);

// Writes `bytes` to `path`. Returns false and fills `error` if it could not.
bool writeMidiFile(const std::string &path, const std::string &bytes, std::string &error);

} // namespace saeurekiste
