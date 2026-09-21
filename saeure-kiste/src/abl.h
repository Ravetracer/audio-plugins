#pragma once

// Reading the .pat text pattern format written by AudioRealism Bassline
// (ABL2 and ABL3), so a library of those patterns can be brought in here.
//
// The format is a line-oriented text file. Lines beginning with ';' are the
// header; everything else is one step, one line per step:
//
//   ; ABL2 Meta tag: 16
//   ; Tune: 0.500000 Cutoff: 0.664000 Resonance: 0.616000 ... Volume: 0.750000
//   c#3 1 0 0
//   ...
//
// The "Meta tag" line opens a pattern and its number is how many step lines
// follow; a file may hold several patterns one after another. The knob line
// carries whatever that version wrote, as "Name: value" pairs with every value
// normalised to 0..1.
//
// A step line is four fields in ABL2 and six in ABL3:
//
//   ABL2   note gate slide accent            note is "c-3", "d#4", ...
//   ABL3   pitch down up slide accent gate   pitch is a semitone offset
//
// The ABL3 field order is not guesswork: one file in the wild is a Reason
// JukeboxPatch holding the same per-step values under the names dpitch, ddown,
// dup, dslide, daccent, dgate, written in that order. ABL3's pitch counts
// semitones from the note ABL2 spells "c-3", and its down and up flags move a
// step an octave either way on top of that.
//
// Two other shapes turn up in the same libraries and are read too.
//
// A .pat may be a **Reason JukeboxPatch** -- XML, same instrument, the same
// per-step values written as named properties:
//
//   <Value property="dpitch0" type="number" >5.000000</Value>
//
// dpitch, ddown, dup, dslide, daccent and dgate with the step index appended,
// plus dpatternlength and the knobs under the names the text header uses. It
// is recognised by its first character, not by its extension, because it
// carries the same .pat extension as the text format.
//
// A **.param sidecar** beside a .pat is that pattern's knobs on their own, one
// `"Reso Trim" = 0.50000000` per line. It carries ABL's whole front panel
// where the text header carries a subset, so where both exist the sidecar
// wins. It holds no pattern, so it is never imported on its own.
//
// What is imported and what is not: the notes, their octaves, the slide and
// accent flags and the knob settings the header carries. ABL's tempo, its
// high-pass and its distortion model are dropped -- the first belongs to the
// host here, and the other two have no counterpart whose setting would mean
// the same thing.

#include <string>

#include "pattern.h"
#include "saeurekiste.h"

namespace saeurekiste {

// One .pat file, turned into something this plugin can write out as a preset.
struct AblImport {
   PresetData preset;  // the name, the description and the mapped knob values
   PatternData bank;   // the file's patterns, in bank order
   int patternCount = 0;
   int steps = 0;      // the longest pattern's length, and what Steps is set to
   int clipped = 0;    // steps whose octave did not fit and was pulled in
};

// Parses one file, in whichever of the shapes above it is written. `name` is
// what the preset will call itself, normally the file's stem. Returns false
// and fills `error` if the text holds no pattern.
bool parseAblPattern(const char *text, size_t length, const std::string &name, AblImport &out,
                     std::string &error);

// A .param sidecar's knob settings, applied over an already-parsed import.
// They win over the .pat header: the sidecar carries ABL's whole front panel
// under its own spelling, and the header carries a subset. Returns false if
// there was nothing in it to apply.
bool applyAblParams(const char *text, size_t length, AblImport &out);

// The preset file the import becomes.
std::string ablPresetText(const AblImport &import);

// Every .pat file under `root`, written into `presetDir` as presets: one
// subfolder per directory that holds any, named after that directory. So
// picking one pack of patterns imports one shelf and picking the folder they
// all live in imports every shelf at once, which is the reason to do this by
// folder rather than by file.
//
// Returns how many presets were written, 0 on failure with `error` filled, and
// names the first folder it made in `firstFolder` so the browser can open on
// it. An existing folder of the same name is never merged into: the new one
// gets a number, exactly as importing the same preset pack twice does.
int importAblFolder(const std::string &root, const std::string &presetDir,
                    std::string &firstFolder, std::string &error);

} // namespace saeurekiste
