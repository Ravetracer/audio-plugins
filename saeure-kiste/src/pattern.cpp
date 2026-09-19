#include "pattern.h"

#include "params.h"

#include "plugincore/dsp/rng.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

namespace saeurekiste {

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
   if (t == "--" || t == "-2")
      return -2;
   if (t == "-" || t == "-1")
      return -1;
   if (t == "+" || t == "+1" || t == "1")
      return 1;
   if (t == "++" || t == "+2" || t == "2")
      return 2;
   return 0;
}

} // namespace

uint16_t Step::pack() const {
   const uint16_t n = note < 0 || note > 11 ? 0u : static_cast<uint16_t>(note + 1);
   const int o = octave < -kMaxOctave ? -kMaxOctave : (octave > kMaxOctave ? kMaxOctave : octave);
   // The direction in bits 4-5, the distance in bit 9: see the note on the bit
   // layout in pattern.h for why it is split that way.
   const int dir = o < 0 ? -1 : (o > 0 ? 1 : 0);
   uint16_t v = n;
   v |= static_cast<uint16_t>((dir + 1) << 4);
   if (accent)
      v |= 1u << 6;
   if (slide)
      v |= 1u << 7;
   if (vibrato)
      v |= 1u << 8;
   if (o == -2 || o == 2)
      v |= 1u << 9;
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
   if (packed & (1u << 9))
      s.octave *= 2;
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

void clearPattern(uint16_t *steps) {
   const uint16_t rest = Step().pack();
   for (int i = 0; i < kMaxSteps; ++i)
      steps[i] = rest;
}

bool patternEmpty(const uint16_t *steps) {
   const uint16_t rest = Step().pack();
   for (int i = 0; i < kMaxSteps; ++i)
      if (steps[i] != rest && steps[i] != 0)
         return false;
   return true;
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
   auto row = [](const std::string &key, int which, const uint16_t *pat) {
      std::string line = key;
      while (line.size() < 13)
         line += ' ';
      line += "=";
      for (int i = 0; i < kMaxSteps; ++i) {
         const Step s = Step::unpack(pat[i]);
         const char *tok = ".";
         switch (which) {
         case 0:
            tok = s.note < 0 ? "." : kNoteNames[s.note];
            break;
         case 1:
            // One character per octave, so the column still reads as a shape.
            // The single-octave tokens are what they always were, which keeps
            // every preset written before this byte for byte what it was.
            tok = s.octave <= -2 ? "--"
                                 : (s.octave == -1 ? "-"
                                                   : (s.octave >= 2 ? "++"
                                                                    : (s.octave == 1 ? "+" : ".")));
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

   static const char *const kFields[5] = {"pitch", "octave", "slide", "accent", "vibrato"};
   static const int kWhich[5] = {0, 1, 2 + kLaneSlide, 2 + kLaneAccent, 2 + kLaneVibrato};

   std::string out = "\n# Sequence\n";
   for (int p = 0; p < kMaxPatterns; ++p) {
      const uint16_t *pat = steps + p * kMaxSteps;
      // Pattern 1 is always written: a preset with no sequence at all would
      // silently keep whatever the previous one left behind. The other
      // sixty-three earn their lines by having something in them.
      if (p > 0 && patternEmpty(pat))
         continue;
      char stem[16];
      if (p == 0)
         std::snprintf(stem, sizeof(stem), "seq_");
      else
         std::snprintf(stem, sizeof(stem), "seq%d_", p + 1);
      if (p > 0) {
         char header[32];
         std::snprintf(header, sizeof(header), "# Pattern %d\n", p + 1);
         out += header;
      }
      for (int f = 0; f < 5; ++f)
         out += row(std::string(stem) + kFields[f], kWhich[f], pat);
   }
   return out;
}

bool parsePatternLine(const std::string &key, const std::string &value, PatternData &out) {
   // "seq_pitch" is pattern 1; "seq7_pitch" is pattern 7. The number is where a
   // key stops being the old format and starts being the bank's.
   if (key.compare(0, 3, "seq") != 0)
      return false;
   size_t i = 3;
   int index = 0;
   while (i < key.size() && key[i] >= '0' && key[i] <= '9') {
      index = index * 10 + (key[i] - '0');
      ++i;
      if (index > kMaxPatterns)
         return false;
   }
   if (i >= key.size() || key[i] != '_')
      return false;
   // A bare "seq_" is pattern 1; "seq1_" says the same thing out loud.
   const int pattern = index == 0 ? 0 : index - 1;
   const std::string field = key.substr(i + 1);

   int which = -1;
   if (field == "pitch")
      which = 0;
   else if (field == "octave")
      which = 1;
   else if (field == "slide")
      which = 2 + kLaneSlide;
   else if (field == "accent")
      which = 2 + kLaneAccent;
   else if (field == "vibrato")
      which = 2 + kLaneVibrato;
   else
      return false;

   // The first pattern line seen empties the whole bank rather than only the
   // pattern it names, so a preset that gives three patterns describes exactly
   // three and does not inherit the other sixty-one from whatever was loaded
   // before it.
   if (!out.present) {
      for (int p = 0; p < kMaxPatterns; ++p)
         clearPattern(out.pattern(p));
      out.present = true;
   }

   uint16_t *pat = out.pattern(pattern);
   const std::vector<std::string> t = tokens(value);
   for (int n = 0; n < kMaxSteps && n < static_cast<int>(t.size()); ++n) {
      Step s = Step::unpack(pat[n]);
      switch (which) {
      case 0:
         s.note = noteFromToken(t[n]);
         break;
      case 1:
         s.octave = octaveFromToken(t[n]);
         break;
      default:
         s.setFlag(which - 2, flagFromToken(t[n]));
         break;
      }
      pat[n] = s.pack();
   }
   return true;
}

uint32_t nextGeneratorSeed(uint32_t current, uint32_t salt, uint32_t maxSeed) {
   if (maxSeed == 0)
      return 0;
   // Two rounds of an avalanche mix, so consecutive salts -- which is what a
   // counter gives -- land nowhere near each other.
   uint32_t x = salt * 0x9E3779B9u + current * 0x85EBCA6Bu + 0x165667B1u;
   for (int guard = 0; guard < 8; ++guard) {
      x ^= x >> 16;
      x *= 0x7FEB352Du;
      x ^= x >> 15;
      x *= 0x846CA68Bu;
      x ^= x >> 16;
      const uint32_t candidate = x % (maxSeed + 1);
      if (candidate != current)
         return candidate;
      // The one in ten thousand that came back with the seed already set. Mix
      // again rather than return it: GEN has to change something.
      x += 0x9E3779B9u;
   }
   return current + 1 > maxSeed ? 0 : current + 1;
}

} // namespace saeurekiste
