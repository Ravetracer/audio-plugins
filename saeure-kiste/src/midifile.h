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
#include <utility>
#include <vector>

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

// ------------------------------------------------------------------ reading
//
// The other direction, for importing somebody else's pattern. Only what a
// pattern needs comes back: the notes, and the CC1 the vibrato convention
// uses. Tempo, key, port and the rest of a MIDI file are dropped, because a
// pattern here has none of them -- the tempo belongs to the host and the
// pattern is sixteen steps of pitch rather than a timeline.

struct MidiNote {
   long onset = 0;   // in ticks from the start of the file
   long release = 0; // its note-off, which is what says whether it slides
   int key = 0;
   int velocity = 0;
};

struct MidiRead {
   int division = 96;              // ticks to the quarter note
   std::vector<MidiNote> notes;    // sorted by onset, then by key
   std::vector<std::pair<long, bool>> mod; // CC1 on and off, in ticks
};

// Parses format 0 and format 1, merging every track: a pattern is one voice
// and which track its notes were written on is not something this plugin has
// anywhere to put. Returns false for anything that is not a MIDI file, or that
// is one with no notes in it.
//
// SMPTE timecode divisions are refused rather than guessed at. A pattern is
// counted in beats and an SMPTE file is counted in seconds, so there is no
// conversion that does not need a tempo this plugin does not have.
bool parseMidiFile(const char *bytes, size_t length, MidiRead &out, std::string &error);

} // namespace saeurekiste
