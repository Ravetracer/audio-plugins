#include "pattern.h"

#include "params.h"

#include "plugincore/dsp/rng.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace threeohthree {

namespace {

// Sharps rather than flats, because the machine's own panel is printed in
// sharps and so is every piano roll anybody writes a bass line in.
const char *const kNoteNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                    "F#", "G",  "G#", "A",  "A#", "B"};

std::vector<std::string> tokens(const std::string &value) {
   std::vector<std::string> out;
   size_t i = 0;
   while (i < value.size()) {
      while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i])))
         ++i;
      const size_t start = i;
      while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i])))
         ++i;
      if (i > start)
         out.push_back(value.substr(start, i - start));
   }
   return out;
}

int noteFromToken(const std::string &t) {
   if (t.empty() || t == "." || t == "-")
      return -1;
   for (int i = 11; i >= 0; --i) { // longest first, so "C#" beats "C"
      const size_t n = std::strlen(kNoteNames[i]);
      if (t.size() == n && strncasecmp(t.c_str(), kNoteNames[i], n) == 0)
         return i;
   }
   return -1;
}

bool flagFromToken(const std::string &t) {
   return !(t.empty() || t == "." || t == "0" || t == "-" || t == "_");
}

int octaveFromToken(const std::string &t) {
   if (t == "-" || t == "-1")
      return -1;
   if (t == "+" || t == "+1" || t == "1")
      return 1;
   return 0;
}

} // namespace

uint16_t Step::pack() const {
   const uint16_t n = note < 0 || note > 11 ? 0u : static_cast<uint16_t>(note + 1);
   const int o = octave < -1 ? -1 : (octave > 1 ? 1 : octave);
   uint16_t v = n;
   v |= static_cast<uint16_t>((o + 1) << 4);
   if (accent)
      v |= 1u << 6;
   if (slide)
      v |= 1u << 7;
   if (vibrato)
      v |= 1u << 8;
   return v;
}

Step Step::unpack(uint16_t packed) {
   Step s;
   const int n = packed & 0x0F;
   s.note = n == 0 ? -1 : n - 1;
   s.octave = static_cast<int>((packed >> 4) & 0x03) - 1;
   s.accent = (packed & (1u << 6)) != 0;
   s.slide = (packed & (1u << 7)) != 0;
   s.vibrato = (packed & (1u << 8)) != 0;
   return s;
}

void defaultPattern(uint16_t *steps) {
   // The same sixteen steps the offline renderer plays, so that opening the
   // plugin and pressing play gives the thing the README describes: a root, an
   // octave, a couple of rests, accents off the beat and two slides.
   struct Seed {
      int note;
      int octave;
      bool slide;
      bool accent;
      bool vibrato;
   };
   static const Seed kSeed[kMaxSteps] = {
      {0, 0, false, true, false},  {0, 0, true, false, false}, {0, 1, false, false, false},
      {-1, 0, false, false, false}, {0, 0, false, false, false}, {10, 0, false, true, false},
      {0, 0, true, false, false},  {3, 0, false, false, false}, {0, 0, false, true, false},
      {-1, 0, false, false, false}, {0, 1, true, false, true},  {10, 0, false, false, false},
      {0, 0, false, false, false}, {7, 0, false, true, false},  {0, 0, true, false, false},
      {-1, 0, false, false, false},
   };
   for (int i = 0; i < kMaxSteps; ++i) {
      Step s;
      s.note = kSeed[i].note;
      s.octave = kSeed[i].octave;
      s.slide = kSeed[i].slide;
      s.accent = kSeed[i].accent;
      s.vibrato = kSeed[i].vibrato;
      steps[i] = s.pack();
   }
}

void generatePattern(const GenSettings &settings, uint16_t *out) {

   const uint32_t seed = settings.seed;
   const double pNote = settings.notes;
   const double pAccent = settings.accent;
   const double pSlide = settings.slide;
   const double pOct = settings.octave;
   const double pVib = settings.vibrato;
   const int root = settings.root;

   int scale[12];
   const int scaleCount = scaleNotes(settings.scale, scale);

   // Seeded from the seed alone, so the pattern is the number and nothing
   // else. Mixed first because small consecutive seeds must give unrelated
   // patterns -- walking 1, 2, 3 through near-identical lines would make the
   // buttons useless.
   RngLite rng;
   rng.seed(seed * 2654435761u + 0x9E3779B9u);
   for (int i = 0; i < 8; ++i)
      rng.next();

   Step steps[kMaxSteps];
   for (int i = 0; i < kMaxSteps; ++i) {
      // All six draws, every step, unconditionally. See the note above.
      const double rNote = rng.uniformPositive();
      const double rDegree = rng.uniformPositive();
      const double rRoot = rng.uniformPositive();
      const double rAccent = rng.uniformPositive();
      const double rSlide = rng.uniformPositive();
      const double rOct = rng.uniformPositive();
      const double rVib = rng.uniformPositive();

      Step st;
      const bool downbeat = (i % 4) == 0;
      // The first step of a beat is likelier to sound, and step one always
      // does: a pattern that starts with a rest reads as a mistake.
      const double noteChance = i == 0 ? 1.0 : (downbeat ? pNote * 0.6 + 0.4 : pNote);
      if (rNote >= noteChance) {
         steps[i] = st; // a rest
         continue;
      }

      // The root, weighted: almost always on the first step, heavily on the
      // other downbeats, ordinarily elsewhere. A line that does not start on
      // its own root sounds like it started in the wrong place, and that is
      // worth more than the variety it costs.
      const double rootChance = i == 0 ? 0.85 : (downbeat ? 0.62 : 0.34);
      int degree = 0;
      if (rRoot >= rootChance && scaleCount > 1) {
         int pick = static_cast<int>(rDegree * static_cast<double>(scaleCount - 1)) + 1;
         if (pick >= scaleCount)
            pick = scaleCount - 1;
         degree = scale[pick];
      }
      st.note = (root + degree) % 12;

      st.accent = rAccent < pAccent;
      st.slide = rSlide < pSlide;
      st.vibrato = rVib < pVib;

      // Octave jumps prefer accented notes -- accent plus an octave up is
      // the gesture everybody recognises -- and one jump in three goes down.
      const double octChance = st.accent ? pOct * 1.6 : pOct * 0.7;
      if (rOct < octChance)
         st.octave = rOct < octChance * 0.34 ? -1 : 1;

      steps[i] = st;
   }

   // A slide into a rest slides into nothing, so it is not a slide. Checked
   // afterwards because it is the only rule that needs a neighbour.
   for (int i = 0; i < kMaxSteps; ++i) {
      const int next = (i + 1) % kMaxSteps;
      if (steps[i].slide && steps[next].note < 0)
         steps[i].slide = false;
   }

   for (int i = 0; i < kMaxSteps; ++i)
      out[i] = steps[i].pack();

}

std::string formatPattern(const uint16_t *steps) {
   // Column-aligned so the five lines read as a grid in a text editor, which is
   // the only reason to write a pattern as text rather than as a number.
   auto row = [&](const char *key, int which) {
      std::string line = key;
      while (line.size() < 12)
         line += ' ';
      line += "=";
      for (int i = 0; i < kMaxSteps; ++i) {
         const Step s = Step::unpack(steps[i]);
         const char *tok = ".";
         switch (which) {
         case 0:
            tok = s.note < 0 ? "." : kNoteNames[s.note];
            break;
         case 1:
            tok = s.octave < 0 ? "-" : (s.octave > 0 ? "+" : ".");
            break;
         default:
            tok = s.flag(which - 2) ? "x" : ".";
            break;
         }
         char cell[8];
         std::snprintf(cell, sizeof(cell), " %-3s", tok);
         line += cell;
      }
      while (!line.empty() && line.back() == ' ')
         line.pop_back();
      line += "\n";
      return line;
   };

   std::string out = "\n# Sequence\n";
   out += row("seq_pitch", 0);
   out += row("seq_octave", 1);
   out += row("seq_slide", 2 + kLaneSlide);
   out += row("seq_accent", 2 + kLaneAccent);
   out += row("seq_vibrato", 2 + kLaneVibrato);
   return out;
}

bool parsePatternLine(const std::string &key, const std::string &value, PatternData &out) {
   int which = -1;
   if (key == "seq_pitch")
      which = 0;
   else if (key == "seq_octave")
      which = 1;
   else if (key == "seq_slide")
      which = 2 + kLaneSlide;
   else if (key == "seq_accent")
      which = 2 + kLaneAccent;
   else if (key == "seq_vibrato")
      which = 2 + kLaneVibrato;
   else
      return false;

   // The first pattern line seen starts from an empty pattern rather than from
   // whatever was there, so a preset that gives only some of the five lines
   // still describes exactly what it means.
   if (!out.present) {
      for (int i = 0; i < kMaxSteps; ++i)
         out.steps[i] = Step().pack();
      out.present = true;
   }

   const std::vector<std::string> t = tokens(value);
   for (int i = 0; i < kMaxSteps && i < static_cast<int>(t.size()); ++i) {
      Step s = Step::unpack(out.steps[i]);
      switch (which) {
      case 0:
         s.note = noteFromToken(t[i]);
         break;
      case 1:
         s.octave = octaveFromToken(t[i]);
         break;
      default:
         s.setFlag(which - 2, flagFromToken(t[i]));
         break;
      }
      out.steps[i] = s.pack();
   }
   return true;
}

} // namespace threeohthree
