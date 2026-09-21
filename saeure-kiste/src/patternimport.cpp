#include "patternimport.h"

#include "midifile.h"
#include "params.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace saeurekiste {

namespace {

// Where ABL's pitch zero lands here: the key the sequencer plays for note C at
// octave 0, which is what plugin.cpp's `36 + note + 12 * octave` starts from.
// ABL2 spells the same note "c-3", so its octave digit and this agree without
// an offset of their own.
constexpr int kAblBaseKey = 36;

int noteOfLetter(char c) {
   switch (c) {
   case 'c': return 0;
   case 'd': return 2;
   case 'e': return 4;
   case 'f': return 5;
   case 'g': return 7;
   case 'a': return 9;
   case 'b': return 11;
   default: return -1;
   }
}

// "c-3", "d#4". Returns the MIDI key, or -1 if the token is not a note at all
// -- which happens: one file in the corpus this was written against has two
// stray bytes where a note should be, and a step nobody can read is a rest
// rather than a reason to refuse the file.
int keyFromAblNote(const std::string &t) {
   if (t.size() < 3)
      return -1;
   const int note = noteOfLetter(static_cast<char>(std::tolower(t[0])));
   if (note < 0 || (t[1] != '-' && t[1] != '#'))
      return -1;
   char *end = nullptr;
   const long octave = std::strtol(t.c_str() + 2, &end, 10);
   if (end == t.c_str() + 2 || octave < -1 || octave > 9)
      return -1;
   return static_cast<int>(octave) * 12 + note + (t[1] == '#' ? 1 : 0);
}

std::vector<std::string> split(const std::string &line) {
   std::vector<std::string> out;
   size_t i = 0;
   while (i < line.size()) {
      while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
         ++i;
      const size_t start = i;
      while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
         ++i;
      if (i > start)
         out.push_back(line.substr(start, i - start));
   }
   return out;
}

// One step as it came out of the file, before it is fitted to this plugin's
// narrower octave range.
struct RawStep {
   int key = -1; // MIDI key, or -1 for a rest
   bool slide = false;
   bool accent = false;
   bool vibrato = false; // only the MIDI reader sets this; ABL has no such flag

   // Compared step by step to find a repeat, which is how a four-bar file
   // that says the same thing four times becomes one bar.
   bool operator==(const RawStep &o) const {
      return key == o.key && slide == o.slide && accent == o.accent && vibrato == o.vibrato;
   }
};

// What this plugin's own MIDI export calls an accent, read back the same way.
// It is the Accent At parameter's default rather than a number of its own, so
// the two halves of the convention cannot drift apart.
constexpr int kMidiAccentVelocity = 100;

using KnobList = std::vector<std::pair<std::string, double>>;

std::string lowered(const std::string &s) {
   std::string out = s;
   for (char &c : out)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return out;
}

// "Tune: 0.5 Cutoff: 0.664 ..." -- every "Name: number" pair on a header line.
// Written as a scan rather than a fixed field list because the three versions
// of the header seen in the wild carry different sets of keys, and a version
// that carries one more should not fail to parse.
void scanKnobsColon(const std::string &line, KnobList &out) {
   const std::vector<std::string> t = split(line);
   for (size_t i = 0; i + 1 < t.size(); ++i) {
      if (t[i].size() < 2 || t[i].back() != ':')
         continue;
      char *end = nullptr;
      const double v = std::strtod(t[i + 1].c_str(), &end);
      if (end == t[i + 1].c_str())
         continue;
      out.emplace_back(lowered(t[i].substr(0, t[i].size() - 1)), v);
   }
}

// A .param sidecar: one `"Reso Trim" = 0.50000000` per line. Same knobs, a
// different spelling, and a few this instrument has no counterpart for.
void scanKnobsQuoted(const std::string &line, KnobList &out) {
   const size_t open = line.find('"');
   if (open == std::string::npos)
      return;
   const size_t close = line.find('"', open + 1);
   const size_t eq = line.find('=', close == std::string::npos ? open : close);
   if (close == std::string::npos || eq == std::string::npos)
      return;
   char *end = nullptr;
   const double v = std::strtod(line.c_str() + eq + 1, &end);
   if (end == line.c_str() + eq + 1)
      return;
   out.emplace_back(lowered(line.substr(open + 1, close - open - 1)), v);
}

// ABL's knobs are all normalised 0..1 and so are most of this plugin's, so the
// mapping is the same number through this plugin's own taper. That taper was
// fitted to the machine both of them model, which is the whole reason it is a
// defensible thing to do -- but it is still an approximation, and the two
// instruments do not have the same knob curves.
double rawFromNormalized(const ParamDesc &d, double v) {
   v = std::min(1.0, std::max(0.0, v));
   switch (d.kind) {
   case ParamKind::Percent:
   case ParamKind::Log:
      return v;
   case ParamKind::Enum:
   case ParamKind::Stepped:
      return std::floor(d.min + v * (d.max - d.min) + 0.5);
   default:
      return d.min + v * (d.max - d.min);
   }
}

// Replaces rather than appends. formatPreset takes the *first* entry it finds
// for a parameter, so a second one would be silently ignored -- and a .param
// sidecar applied after its .pat has to win.
void setParam(PresetData &preset, uint32_t id, double raw) {
   for (auto &kv : preset.values)
      if (kv.first == id) {
         kv.second = raw;
         return;
      }
   preset.values.emplace_back(id, raw);
}

void mapNormalized(PresetData &preset, uint32_t id, double v) {
   setParam(preset, id, rawFromNormalized(paramTable()[id], v));
}

// Which of this plugin's knobs each of ABL's turns into. Tempo, HighPass and
// DistType are deliberately absent -- see abl.h.
void mapKnobs(const std::vector<std::pair<std::string, double>> &knobs, PresetData &preset) {
   for (const auto &kv : knobs) {
      const std::string &k = kv.first;
      const double v = kv.second;
      if (k == "tune" || k == "tuning")
         mapNormalized(preset, kParamTuning, v);
      else if (k == "cutoff")
         mapNormalized(preset, kParamCutoff, v);
      else if (k == "resonance")
         mapNormalized(preset, kParamResonance, v);
      else if (k == "envmod")
         mapNormalized(preset, kParamEnvMod, v);
      else if (k == "decay")
         mapNormalized(preset, kParamDecay, v);
      else if (k == "accent")
         mapNormalized(preset, kParamAccent, v);
      else if (k == "waveform")
         mapNormalized(preset, kParamWaveform, v);
      else if (k == "drive")
         mapNormalized(preset, kParamDrive, v);
      else if (k == "distort")
         mapNormalized(preset, kParamDistMix, v);
      else if (k == "volume") {
         // Volume is the one knob that is not a normalised sweep of this
         // plugin's range. ABL writes 0.75 for its own default, so that is
         // pinned to this plugin's default of -6 dB and everything else is
         // read as an amplitude ratio against it -- which keeps an imported
         // library at the level it was mixed at instead of 9 dB under it.
         const ParamDesc &d = paramTable()[kParamVolume];
         const double db = v <= 0.0 ? d.min : -6.0 + 20.0 * std::log10(v / 0.75);
         setParam(preset, kParamVolume, std::min(d.max, std::max(d.min, db)));
      }
   }
}

int floorDiv12(int v) { return v >= 0 ? v / 12 : -((-v + 11) / 12); }

// ------------------------------------------------------------------- the XML
//
// A .pat may be a Reason JukeboxPatch rather than the text format: same
// instrument, same fields, written as named properties.
//
//   <Value property="dpitch0" type="number" >5.000000</Value>
//
// The per-step names are dpitch, ddown, dup, dslide, daccent and dgate with
// the step index appended, which is where the six text columns get their order
// from -- see abl.h. dpatternlength is the length, and everything else is a
// knob under the same name the text header uses.
const char *const kStepFields[6] = {"dpitch", "ddown", "dup", "dslide", "daccent", "dgate"};

// Splits "dpitch12" into field 0 and index 12. Returns false for a name that
// is not one of the six, which includes "decay" and "dpatternlength".
bool splitStepProperty(const std::string &key, int &field, int &index) {
   for (int f = 0; f < 6; ++f) {
      const size_t n = std::strlen(kStepFields[f]);
      if (key.size() <= n || key.compare(0, n, kStepFields[f]) != 0)
         continue;
      index = 0;
      for (size_t i = n; i < key.size(); ++i) {
         if (key[i] < '0' || key[i] > '9')
            return false;
         index = index * 10 + (key[i] - '0');
         if (index >= kMaxSteps)
            return false;
      }
      field = f;
      return true;
   }
   return false;
}

bool parseXmlPatch(const std::string &all, KnobList &knobs,
                   std::vector<std::vector<RawStep>> &patterns, std::string &error) {
   // field -> step -> value. Absent stays absent: a patch that writes no
   // dgate at all is sixteen notes rather than sixteen rests.
   double step[6][kMaxSteps];
   bool have[6][kMaxSteps];
   for (int f = 0; f < 6; ++f)
      for (int i = 0; i < kMaxSteps; ++i) {
         step[f][i] = 0.0;
         have[f][i] = false;
      }
   int highest = -1;
   int declared = 0;

   size_t pos = 0;
   while ((pos = all.find("property=\"", pos)) != std::string::npos) {
      pos += 10;
      const size_t nameEnd = all.find('"', pos);
      if (nameEnd == std::string::npos)
         break;
      const std::string key = lowered(all.substr(pos, nameEnd - pos));
      pos = nameEnd + 1;
      // The value is the text of the element, so skip to the end of the open
      // tag and read up to the closing one.
      const size_t tagEnd = all.find('>', pos);
      if (tagEnd == std::string::npos)
         break;
      const size_t close = all.find('<', tagEnd);
      if (close == std::string::npos)
         break;
      char *end = nullptr;
      const double v = std::strtod(all.substr(tagEnd + 1, close - tagEnd - 1).c_str(), &end);
      pos = close;
      if (!end || end == all.c_str())
         continue;

      int field = 0;
      int index = 0;
      if (splitStepProperty(key, field, index)) {
         step[field][index] = v;
         have[field][index] = true;
         if (index > highest)
            highest = index;
      } else if (key == "dpatternlength") {
         declared = static_cast<int>(v + 0.5);
      } else {
         knobs.emplace_back(key, v);
      }
   }

   int count = declared > 0 ? declared : highest + 1;
   if (count > kMaxSteps)
      count = kMaxSteps;
   if (count <= 0) {
      error = "That XML patch has no pattern in it.";
      return false;
   }

   patterns.emplace_back();
   for (int i = 0; i < count; ++i) {
      RawStep r;
      const bool gate = have[5][i] ? step[5][i] != 0.0 : true;
      if (gate)
         r.key = kAblBaseKey + static_cast<int>(step[0][i] + (step[0][i] < 0 ? -0.5 : 0.5)) +
                 12 * ((step[2][i] != 0.0 ? 1 : 0) - (step[1][i] != 0.0 ? 1 : 0));
      r.slide = step[3][i] != 0.0;
      r.accent = step[4][i] != 0.0;
      patterns.back().push_back(r);
   }
   return true;
}

// Everything a parsed file has in common once it is a list of patterns: fit
// the octaves, pack the steps, and fill in the parameters that describe the
// pattern rather than the sound. Shared by every format this file reads,
// because none of them differ after this point.
bool buildImport(std::vector<std::vector<RawStep>> &patterns, const std::string &name,
                 AblImport &out, std::string &error) {
   while (!patterns.empty() && patterns.back().empty())
      patterns.pop_back();
   if (patterns.empty()) {
      error = "No pattern in " + name + ".";
      return false;
   }
   if (static_cast<int>(patterns.size()) > kMaxPatterns)
      patterns.resize(kMaxPatterns);

   // How far the whole file has to move so that as many steps as possible land
   // inside the +-2 octaves a step can carry. The rest is Pattern Oct, which
   // shifts the bank as a whole -- so a line written two octaves up is still
   // that line rather than a flattened copy of it.
   //
   // Fewest clipped steps wins; between two shifts that clip the same number,
   // the one that leaves the steps nearest their own middle wins, so a line
   // written three octaves up becomes Pattern Oct 3 and sixteen steps at zero
   // rather than Pattern Oct 1 and sixteen steps pinned at the top of their
   // range with no room left to edit them.
   int bestShift = 0;
   int bestClipped = -1;
   int bestSpread = 0;
   for (int shift = -4; shift <= 4; ++shift) {
      int clipped = 0;
      int spread = 0;
      for (const auto &p : patterns)
         for (const RawStep &s : p) {
            if (s.key < 0)
               continue;
            const int octave = floorDiv12(s.key - kAblBaseKey) - shift;
            if (octave < -kMaxOctave || octave > kMaxOctave)
               ++clipped;
            spread += std::abs(std::min(kMaxOctave, std::max(-kMaxOctave, octave)));
         }
      if (bestClipped < 0 || clipped < bestClipped ||
          (clipped == bestClipped && spread < bestSpread)) {
         bestClipped = clipped;
         bestSpread = spread;
         bestShift = shift;
      }
   }

   for (int p = 0; p < kMaxPatterns; ++p)
      clearPattern(out.bank.pattern(p));
   out.bank.present = true;
   for (size_t p = 0; p < patterns.size(); ++p) {
      uint16_t *bank = out.bank.pattern(static_cast<int>(p));
      for (size_t i = 0; i < patterns[p].size(); ++i) {
         const RawStep &r = patterns[p][i];
         Step s;
         if (r.key >= 0) {
            const int rel = r.key - kAblBaseKey;
            s.note = ((rel % 12) + 12) % 12;
            s.octave = std::min(kMaxOctave, std::max(-kMaxOctave, floorDiv12(rel) - bestShift));
         }
         s.slide = r.slide;
         s.accent = r.accent;
         s.vibrato = r.vibrato;
         bank[i] = s.pack();
      }
      out.steps = std::max(out.steps, static_cast<int>(patterns[p].size()));
   }
   out.patternCount = static_cast<int>(patterns.size());
   out.clipped = bestClipped < 0 ? 0 : bestClipped;

   out.preset.name = name;
   out.preset.author = "Imported";

   setParam(out.preset, kParamMode, 1.0); // Sequencer: the pattern is the point
   setParam(out.preset, kParamSeqSteps, static_cast<double>(std::max(1, out.steps)));
   setParam(out.preset, kParamPatternOctave, static_cast<double>(bestShift));
   return true;
}

} // namespace

bool parseAblPattern(const char *text, size_t length, const std::string &name, AblImport &out,
                     std::string &error) {
   out = AblImport();
   if (!text) {
      error = "Empty file.";
      return false;
   }

   KnobList knobs;
   std::vector<std::vector<RawStep>> patterns;

   const std::string all(text, length);
   const size_t firstReal = all.find_first_not_of(" \t\r\n");
   const bool xml = firstReal != std::string::npos && all[firstReal] == '<';
   if (xml && !parseXmlPatch(all, knobs, patterns, error))
      return false;

   size_t pos = xml ? all.size() + 1 : 0;
   while (pos <= all.size()) {
      size_t end = all.find('\n', pos);
      if (end == std::string::npos)
         end = all.size();
      std::string line = all.substr(pos, end - pos);
      pos = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
         line.pop_back();
      if (line.empty())
         continue;

      if (line[0] == ';') {
         // "; ABL2 Meta tag: 16" opens a pattern; anything else on a comment
         // line is knobs, and the knobs belong to the file rather than to the
         // pattern -- a multi-pattern file writes them once, before the first.
         if (line.find("Meta tag") != std::string::npos)
            patterns.emplace_back();
         else
            scanKnobsColon(line, knobs);
         continue;
      }

      const std::vector<std::string> f = split(line);
      RawStep s;
      bool gate = false;
      if (f.size() == 4) {
         s.key = keyFromAblNote(f[0]);
         gate = f[1] != "0";
         s.slide = f[2] != "0";
         s.accent = f[3] != "0";
      } else if (f.size() == 6) {
         char *end2 = nullptr;
         const long pitch = std::strtol(f[0].c_str(), &end2, 10);
         if (end2 == f[0].c_str())
            continue;
         s.key = kAblBaseKey + static_cast<int>(pitch) +
                 12 * ((f[2] != "0" ? 1 : 0) - (f[1] != "0" ? 1 : 0));
         s.slide = f[3] != "0";
         s.accent = f[4] != "0";
         gate = f[5] != "0";
      } else {
         continue; // not a step line; nothing else in the format looks like one
      }
      if (!gate)
         s.key = -1;
      if (patterns.empty())
         patterns.emplace_back(); // a file that starts straight in on the steps
      if (static_cast<int>(patterns.back().size()) < kMaxSteps)
         patterns.back().push_back(s);
   }
   if (!buildImport(patterns, name, out, error))
      return false;

   out.preset.description =
      "Imported from an ABL text pattern file. The notes, slides and accents are exactly what "
      "the file held; the knob settings are that file's own, read through this plugin's ranges, "
      "which puts them in the right area rather than on the same number.";

   mapKnobs(knobs, out.preset);
   return true;
}

// ------------------------------------------------------------------- MIDI

bool parseMidiPattern(const char *bytes, size_t length, const std::string &name, AblImport &out,
                      std::string &error) {
   out = AblImport();
   MidiRead midi;
   if (!parseMidiFile(bytes, length, midi, error))
      return false;

   // The grid. A step here is a sixteenth, which is this sequencer's own
   // default Rate and what every file in the library this was written against
   // uses. A file written in triplets or thirty-seconds quantises to the
   // nearest sixteenth and loses something; there is no way to know it was
   // meant differently from the file alone, and guessing a grid per file would
   // turn a wrong import into a mysterious one.
   const long ticks = midi.division / 4;
   if (ticks <= 0) {
      error = "That MIDI file's resolution is too coarse to read as steps.";
      return false;
   }

   long lastOnset = 0;
   for (const MidiNote &n : midi.notes)
      lastOnset = std::max(lastOnset, n.onset);
   int used = static_cast<int>((lastOnset + ticks / 2) / ticks) + 1;
   if (used > kMaxSteps * 8)
      used = kMaxSteps * 8; // a long file, reduced below and then truncated
   std::vector<RawStep> grid(static_cast<size_t>(used));
   std::vector<long> release(static_cast<size_t>(used), -1);

   for (const MidiNote &n : midi.notes) {
      const int step = static_cast<int>((n.onset + ticks / 2) / ticks);
      if (step < 0 || step >= used)
         continue;
      // One note per step: this instrument is monophonic and a step holds one
      // pitch. The earliest onset wins, and the lowest key breaks a tie --
      // which on a bass part is the note somebody meant.
      if (grid[static_cast<size_t>(step)].key >= 0)
         continue;
      grid[static_cast<size_t>(step)].key = n.key;
      // The same three conventions this plugin's own MIDI *export* uses, read
      // back: a velocity at or above the accent threshold is an accent, and a
      // note still held when the next step starts is a slide. See midifile.h
      // -- the two halves have to agree or a pattern will not survive being
      // dragged out and imported again, which the self-test checks.
      grid[static_cast<size_t>(step)].accent = n.velocity >= kMidiAccentVelocity;
      release[static_cast<size_t>(step)] = n.release;
   }
   for (int i = 0; i < used; ++i) {
      if (grid[static_cast<size_t>(i)].key < 0)
         continue;
      // Held past the start of the next step, with a tick of slack for a file
      // that was quantised by something else.
      const long nextOnset = static_cast<long>(i + 1) * ticks;
      grid[static_cast<size_t>(i)].slide = release[static_cast<size_t>(i)] > nextOnset + 1;
   }
   // CC1 brackets a vibrato step, which is the third of the export's
   // conventions: the controller goes up at the note's onset and down at its
   // release. So the rule is the same one the notes get -- quantise the *up*
   // to its nearest step and mark that step -- rather than asking what the
   // controller was doing at some point inside the step. Sampling a moment
   // instead is what the first version did, and with the gate at a half the
   // release landed exactly on the point being sampled and won.
   for (const auto &m : midi.mod) {
      if (!m.second)
         continue;
      const int step = static_cast<int>((m.first + ticks / 2) / ticks);
      if (step >= 0 && step < used)
         grid[static_cast<size_t>(step)].vibrato = true;
   }

   // The loop, taken out. These files are written as four bars of a one bar
   // figure as often as not, and a pattern that says the same thing four times
   // is harder to edit and no different to listen to. The smallest period that
   // divides the length and repeats it exactly is the pattern; anything that
   // does not repeat exactly is left at its full length.
   int period = used;
   for (int p = 1; p < used; ++p) {
      if (used % p != 0)
         continue;
      bool repeats = true;
      for (int i = p; i < used && repeats; ++i)
         repeats = grid[static_cast<size_t>(i)] == grid[static_cast<size_t>(i % p)];
      if (repeats) {
         period = p;
         break;
      }
   }
   grid.resize(static_cast<size_t>(period));
   if (static_cast<int>(grid.size()) > kMaxSteps)
      grid.resize(kMaxSteps);

   std::vector<std::vector<RawStep>> patterns;
   patterns.push_back(grid);
   if (!buildImport(patterns, name, out, error))
      return false;

   out.preset.description =
      "Imported from a MIDI file. The notes, their octaves and the accents are what the file "
      "held; a note still sounding when the next step began became a slide, and a velocity at "
      "or above the accent threshold became an accent. Nothing else in the file is a thing this "
      "pattern has anywhere to put.";
   return true;
}

bool applyAblParams(const char *text, size_t length, AblImport &out) {
   if (!text)
      return false;
   KnobList knobs;
   const std::string all(text, length);
   size_t pos = 0;
   while (pos <= all.size()) {
      size_t end = all.find('\n', pos);
      if (end == std::string::npos)
         end = all.size();
      scanKnobsQuoted(all.substr(pos, end - pos), knobs);
      pos = end + 1;
   }
   if (knobs.empty())
      return false;
   mapKnobs(knobs, out.preset);
   return true;
}

std::string ablPresetText(const AblImport &import) {
   return formatPreset(import.preset, &import.bank);
}

int importPatternFiles(const std::string &root, const std::string &presetDir,
                    std::string &firstFolder, std::string &error) {
   if (presetDir.empty()) {
      error = "No user preset directory: neither XDG_CONFIG_HOME nor HOME is set.";
      return 0;
   }

   std::error_code ec;
   std::map<std::string, std::vector<std::filesystem::path>> byFolder;

   // A single file is a folder of one. The shelf is then named after the
   // directory the file sits in, exactly as it would be if the whole directory
   // had been picked -- so importing one pattern and later importing the rest
   // of its folder puts them together rather than in two places.
   if (std::filesystem::is_regular_file(root, ec)) {
      const std::filesystem::path one(root);
      byFolder[one.parent_path().filename().string()].push_back(one);
   } else
   for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
        it.increment(ec)) {
      if (ec)
         break;
      if (!it->is_regular_file(ec))
         continue;
      std::string ext = it->path().extension().string();
      for (char &c : ext)
         c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (ext == ".pat" || ext == ".mid" || ext == ".midi")
         byFolder[it->path().parent_path().filename().string()].push_back(it->path());
   }
   if (byFolder.empty()) {
      error = "No pattern files in that folder: nothing ending .pat, .mid or .midi.";
      return 0;
   }

   int written = 0;
   for (auto &entry : byFolder) {
      std::string stem = presetFileStem(entry.first);
      if (stem.empty())
         stem = "patterns";
      std::string unique = stem;
      for (int n = 2; n < 100 && std::filesystem::exists(std::filesystem::path(presetDir) / unique,
                                                        ec);
           ++n)
         unique = stem + "_" + std::to_string(n);

      std::sort(entry.second.begin(), entry.second.end());
      int here = 0;
      for (const auto &file : entry.second) {
         std::ifstream in(file, std::ios::binary);
         if (!in)
            continue;
         std::ostringstream buf;
         buf << in.rdbuf();
         const std::string text = buf.str();

         // Which reader, decided by the extension: a MIDI file and a text
         // pattern have nothing in common to sniff for, unlike the three
         // shapes of .pat, which do.
         std::string fext = file.extension().string();
         for (char &c : fext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
         const bool isMidi = fext == ".mid" || fext == ".midi";

         AblImport import;
         std::string perr;
         const bool read =
            isMidi ? parseMidiPattern(text.c_str(), text.size(), file.stem().string(), import, perr)
                   : parseAblPattern(text.c_str(), text.size(), file.stem().string(), import, perr);
         if (!read) {
            error = perr; // reported only if nothing at all could be read
            continue;
         }

         // A .param sidecar beside the .pat is the same pattern's knobs, in
         // full and under their own spelling. It is applied after the header
         // and wins, because it is the more complete of the two.
         std::filesystem::path sidecar = file;
         sidecar.replace_extension(".param");
         std::ifstream side(isMidi ? std::filesystem::path() : sidecar, std::ios::binary);
         if (side) {
            std::ostringstream sbuf;
            sbuf << side.rdbuf();
            const std::string stext = sbuf.str();
            applyAblParams(stext.c_str(), stext.size(), import);
         }
         std::string name = presetFileStem(import.preset.name);
         if (name.empty())
            name = "pattern";
         const std::string out =
            presetDir + "/" + unique + "/" + name + "." + kPresetExtension;
         std::string werr;
         if (writePresetFile(out, ablPresetText(import), werr))
            ++here;
         else
            error = werr;
      }
      if (here > 0) {
         written += here;
         if (firstFolder.empty())
            firstFolder = unique;
      }
   }

   if (written == 0 && error.empty())
      error = "Nothing in that folder could be read as a pattern.";
   return written;
}

} // namespace saeurekiste
