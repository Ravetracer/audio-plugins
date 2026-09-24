#pragma once

// MIDI conventions, and a pattern written out as a standard MIDI file.
//
// The key numbers are the machine's own: the MIDI implementation chart in the
// service notes (p.15) gives 35/36 bass drum, 37 rim shot, 38/40 snare, 39 hand
// clap, 41/43 low tom, 42/44 closed hat, 45/47 mid tom, 46 open hat, 48/50 hi
// tom, 49 crash and 51 ride -- which is also, not by accident, where the
// General MIDI drum map put them a few years later. The plugin reads these
// keys, sends them out of its note port while the sequencer runs, and writes
// them into the files the window drags out, so all three agree.
//
// Format 0, one track, channel 10, 960 ticks to the quarter note.

#include <cstdint>
#include <string>

namespace rumpelkiste {

// The voice a MIDI key plays, or -1. Both keys of each pair, plus the General
// MIDI neighbours a drum map in a host is likely to send: 57 crash 2 and 59
// ride 2 onto the two cymbals, 53 ride bell onto the ride.
int voiceForKey(int key);

// The key a voice is written as: the first of its pair.
int keyForVoice(int voice);

struct MidiExport {
   const uint32_t *steps = nullptr; // one pattern, kMaxSteps packed words
   int length = 16;                 // how many of them play
   double stepsPerBeat = 4.0;       // the Scale, as steps to the beat
   double shuffle = 0.0;            // how late the second step of a pair is, in steps
   double flamSeconds = 0.012;      // the flam interval
   double tempoBpm = 120.0;
   double accentVelocity = 100.0;   // the plugin's accent threshold, 1..127
   int patternNumber = 1;
};

// The file's bytes. Empty if there is nothing in the pattern to write.
std::string patternToMidiFile(const MidiExport &spec);

// Where the file goes: a temp directory of our own, one file per pattern,
// overwritten each time. Empty if the directory could not be made.
std::string midiExportPath(int patternNumber);

bool writeMidiFile(const std::string &path, const std::string &bytes, std::string &error);

} // namespace rumpelkiste
