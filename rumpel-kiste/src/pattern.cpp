#include "pattern.h"

#include "params.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace rumpelkiste {

namespace {

const char *const kTrackKeys[kNumTracks] = {"bd", "sd", "lt", "mt", "ht", "rs",
                                            "cp", "ch", "oh", "cr", "rd", "ac"};
const char *const kTrackLabels[kNumTracks] = {"BD", "SD", "LT", "MT", "HT", "RS",
                                              "CP", "CH", "OH", "CR", "RD", "AC"};

const char *const kChainKeys[kNumChainModes] = {"Stay", "Next", "First", "Random"};

} // namespace

const char *trackKey(int track) {
   return track >= 0 && track < kNumTracks ? kTrackKeys[track] : "";
}

const char *trackLabel(int track) {
   return track >= 0 && track < kNumTracks ? kTrackLabels[track] : "";
}

bool patternEmpty(const uint32_t *steps) {
   for (int i = 0; i < kMaxSteps; ++i)
      if (steps[i] != 0)
         return false;
   return true;
}

int patternUsedLength(const uint32_t *steps) {
   for (int i = kMaxSteps; i > 0; --i)
      if (steps[i - 1] != 0)
         return i;
   return 0;
}

int steppedPattern(int current, int delta, int count) {
   const int n = count < 1 ? 1 : (count > kMaxPatterns ? kMaxPatterns : count);
   const int from = current < 0 ? 0 : (current > n - 1 ? n - 1 : current);
   const int to = from + delta;
   return to < 0 ? 0 : (to > n - 1 ? n - 1 : to);
}

// ------------------------------------------------------------------ generator
//
// A drum pattern generator that knows what a few kinds of groove look like.
// Every style is two tables of sixteen likelihoods per row, in tenths: `core`
// is what defines the style and is written whatever Busy says, `extra` is what
// Busy adds on top. A kick on every beat is core for House; a ghost snare on
// the sixteenth before the backbeat is extra for Breaks.
//
// The per-step draws are made in a fixed order and all of them are made on
// every step, so the pattern for a given seed is the same shape whatever the
// densities: Busy only decides how many of the drawn numbers fall under the
// line. That is what keeps turning a knob from reshuffling the whole bar.

namespace {

struct StyleRow {
   const char *core;
   const char *extra;
};

// Rows in track order: BD SD LT MT HT RS CP CH OH CR RD AC. The accent row's
// numbers are a weight for where the total accent goes rather than a hit.
struct Style {
   StyleRow rows[kNumTracks];
};

// clang-format off
const Style kStyles[kNumGenStyles] = {
   // House: four on the floor, claps on two and four, the open hat on the
   // off-beat and the closed one filling the sixteenths between.
   {{
      {"9000900090009000", "0000001000000021"}, // BD
      {"0000000000000000", "0000100000001003"}, // SD
      {"0000000000000000", "0000000000000012"}, // LT
      {"0000000000000000", "0000000000000102"}, // MT
      {"0000000000000000", "0000000000001002"}, // HT
      {"0000000000000000", "0002000100200010"}, // RS
      {"0000900000009000", "0000000000000100"}, // CP
      {"0303030303030303", "5050505050505050"}, // CH
      {"0090009000900090", "0000000000000000"}, // OH
      {"3000000000000000", "0000000000000000"}, // CR
      {"0000000000000000", "3000300030003000"}, // RD
      {"7000700070007000", "0000000000000000"}, // AC
   }},
   // Techno: the same floor, sixteenth hats, a sparser clap and the rim shot
   // doing the syncopation.
   {{
      {"9000900090009000", "0000000000100010"},
      {"0000000000000000", "0000000000000102"},
      {"0000000000000000", "0000000001000020"},
      {"0000000000000000", "0000001000000100"},
      {"0000000000000000", "0000000000010000"},
      {"0000000000000000", "0020000200200002"},
      {"0000600000006000", "0000300000003000"},
      {"5555555555555555", "3333333333333333"},
      {"0070007000700070", "0000000000000000"},
      {"3000000000000000", "0000000000000000"},
      {"0000000000000000", "4040404040404040"},
      {"7020702070207020", "0000000000000000"},
   }},
   // Electro: a broken kick, a hard snare on two and four, eighth-note hats and
   // the toms answering at the end of the bar.
   {{
      {"9000009000900000", "0000000000010010"},
      {"0000900000009000", "0000000100000101"},
      {"0000000000000000", "0000000000000205"},
      {"0000000000000000", "0000000000020500"},
      {"0000000000000000", "0000000002050000"},
      {"0000000000000000", "0000000200020000"},
      {"0000500000005000", "0000000000000000"},
      {"7070707070707070", "0303030303030303"},
      {"0000000000000000", "0000000000000030"},
      {"3000000000000000", "0000000000000000"},
      {"0000000000000000", "0000000000000000"},
      {"7000007000700000", "0000000000000000"},
   }},
   // Breaks: the kick on one and the and-of-three, a backbeat snare with ghost
   // notes round it, eighth hats.
   {{
      {"9000000000900000", "0050000200050000"},
      {"0000900000009000", "0000000403000304"},
      {"0000000000000000", "0000000000000030"},
      {"0000000000000000", "0000000000000300"},
      {"0000000000000000", "0000000000003000"},
      {"0000000000000000", "0000000100000000"},
      {"0000000000000000", "0000200000002000"},
      {"7070707070707070", "0202020202020202"},
      {"0000000000000000", "0000000000000050"},
      {"3000000000000000", "0000000000000000"},
      {"0000000000000000", "0000000000000000"},
      {"6000600060006000", "0000000000000000"},
   }},
   // Garage: a skipping two-step kick, rim shots in the gaps, hats on the
   // sixteenths between the eighths. Wants Shuffle turned up.
   {{
      {"9000000000900000", "0000000100000020"},
      {"0000900000009000", "0000000000000000"},
      {"0000000000000000", "0000000000000010"},
      {"0000000000000000", "0000000000000100"},
      {"0000000000000000", "0000000000000000"},
      {"0000000000000000", "0020000200020020"},
      {"0000500000005000", "0000000000000000"},
      {"0707070707070707", "4040404040404040"},
      {"0000000000000000", "0000000000300000"},
      {"2000000000000000", "0000000000000000"},
      {"0000000000000000", "0000000000000000"},
      {"6000600060006000", "0000000000000000"},
   }},
   // Wild: a kick on one and nothing else anybody would call a rule.
   {{
      {"9000000000000000", "4444444444444444"},
      {"1111111111111111", "4444444444444444"},
      {"1111111111111111", "3333333333333333"},
      {"1111111111111111", "3333333333333333"},
      {"1111111111111111", "3333333333333333"},
      {"1111111111111111", "3333333333333333"},
      {"1111111111111111", "3333333333333333"},
      {"2222222222222222", "5555555555555555"},
      {"1111111111111111", "3333333333333333"},
      {"1000000000000000", "1000000000000000"},
      {"1111111111111111", "3333333333333333"},
      {"3333333333333333", "0000000000000000"},
   }},
};
// clang-format on

double digit(const char *row, int i) { return static_cast<double>(row[i] - '0') / 9.0; }

// xorshift64*, seeded through splitmix64 so seeds 1 and 2 do not start out
// correlated. Deterministic by construction, which is the whole point: the
// seed is the pattern.
struct Rng {
   uint64_t s;
   explicit Rng(uint32_t seed) {
      uint64_t z = static_cast<uint64_t>(seed) + 0x9E3779B97F4A7C15ull;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      s = (z ^ (z >> 31)) | 1ull;
   }
   double next() {
      s ^= s >> 12;
      s ^= s << 25;
      s ^= s >> 27;
      return static_cast<double>((s * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
   }
};

} // namespace

void generatePattern(const GenSettings &g, uint32_t *steps) {
   for (int i = 0; i < kMaxSteps; ++i)
      steps[i] = 0;
   const int style = g.style < 0 || g.style >= kNumGenStyles ? 0 : g.style;
   const int length = g.length < 1 ? 1 : (g.length > kMaxSteps ? kMaxSteps : g.length);
   const double busy = g.busy < 0.0 ? 0.0 : (g.busy > 1.0 ? 1.0 : g.busy);
   const double acc = g.accent < 0.0 ? 0.0 : (g.accent > 1.0 ? 1.0 : g.accent);
   const Style &st = kStyles[style];
   const int lastBar = (length - 1) / 16;
   Rng rng(g.seed);

   for (int i = 0; i < length; ++i) {
      const int pos = i % 16;
      const int bar = i / 16;
      // The last bar of a pattern longer than one is where a fill goes.
      const bool fillBar = length > 16 && bar == lastBar;
      uint32_t w = 0;
      for (int track = 0; track < kNumTracks; ++track) {
         // Three draws per row per step, always, in this order.
         const double rHit = rng.next();
         const double rAcc = rng.next();
         const double rFlam = rng.next();

         const StyleRow &row = st.rows[track];
         double core = digit(row.core, pos);
         double extra = digit(row.extra, pos);

         if (track == kTrackAccent) {
            if (rAcc < acc * 1.3 * core)
               w = withAccent(w, true);
            continue;
         }
         // The crash marks the top of a pattern and nothing else.
         if (track == kVoiceCR && i != 0) {
            core = 0.0;
            extra = 0.0;
         }
         if (fillBar && pos >= 8 &&
             (track == kVoiceLT || track == kVoiceMT || track == kVoiceHT || track == kVoiceSD))
            extra = extra * 2.0 + 0.12;
         const double p = core + 2.0 * busy * extra;
         if (!(rHit < p))
            continue;
         const bool accented = rAcc < acc * 0.45;
         w = withLevel(w, track, accented ? kLevelAccent : kLevelOn);
         const double flamChance = track == kVoiceSD ? 0.12 : (track == kVoiceBD ? 0.02 : 0.08);
         if (voiceCanFlam(track) && rFlam < flamChance * busy)
            w = withFlam(w, track, true);
      }
      // One hi-hat voice: an open and a closed hat on the same step is the
      // closed one cutting the open one off before it is heard, so the closed
      // one goes.
      if (stepLevel(w, kVoiceOH) != kLevelOff)
         w = withLevel(w, kVoiceCH, kLevelOff);
      steps[i] = w;
   }
}

uint32_t nextGeneratorSeed(uint32_t current, uint32_t salt, uint32_t maxSeed) {
   if (maxSeed == 0)
      return 0;
   uint32_t x = salt * 0x9E3779B9u + current * 0x85EBCA6Bu + 0x165667B1u;
   for (int guard = 0; guard < 8; ++guard) {
      x ^= x >> 16;
      x *= 0x7FEB352Du;
      x ^= x >> 15;
      x *= 0x846CA68Bu;
      x ^= x >> 16;
      // maxSeed + 1 wraps to zero at the top of the word, and a modulo by it
      // would be a division by zero -- the bug that crashed SäureKiste's GEN
      // the day its seed range was widened. Spelled out here from the start.
      const uint32_t span = maxSeed + 1;
      const uint32_t candidate = span == 0 ? x : x % span;
      if (candidate != current)
         return candidate;
      x += 0x9E3779B9u;
   }
   return current >= maxSeed ? 0 : current + 1;
}

// ------------------------------------------------------------ preset lines

namespace {

// A row as text: '.' silent, 'x' plays, 'X' accented, 'f' and 'F' the same two
// with a flam. Grouped in fours, which is how a drum pattern is read.
std::string rowText(const uint32_t *steps, int track, int length) {
   std::string s;
   for (int i = 0; i < length; ++i) {
      if (i > 0 && i % 4 == 0)
         s.push_back(' ');
      const uint32_t w = steps[i];
      char c = '.';
      if (track == kTrackAccent) {
         c = stepAccent(w) ? 'x' : '.';
      } else {
         const int level = stepLevel(w, track);
         const bool flam = stepFlam(w, track);
         if (level == kLevelOn)
            c = flam ? 'f' : 'x';
         else if (level == kLevelAccent)
            c = flam ? 'F' : 'X';
      }
      s.push_back(c);
   }
   return s;
}

bool rowUsed(const uint32_t *steps, int track) {
   for (int i = 0; i < kMaxSteps; ++i)
      if (trackOn(steps[i], track))
         return true;
   return false;
}

} // namespace

std::string formatPattern(const PatternData &bank) {
   std::string out;
   for (int p = 0; p < kMaxPatterns; ++p) {
      const uint32_t *steps = bank.pattern(p);
      const bool empty = patternEmpty(steps);
      const int mode = bank.chainMode[p];
      const int repeat = bank.chainRepeat[p];
      const bool chained = mode != kChainStay || repeat != 1;
      if (empty && !chained)
         continue;
      char prefix[16];
      std::snprintf(prefix, sizeof(prefix), "p%d_", p + 1);
      if (!empty) {
         // Rounded up to the bar, so a pattern that ends on step 13 is still
         // written as sixteen columns and the files line up.
         int length = patternUsedLength(steps);
         length = ((length + 15) / 16) * 16;
         for (int track = 0; track < kNumTracks; ++track) {
            if (!rowUsed(steps, track))
               continue;
            out += prefix;
            out += trackKey(track);
            out += " = ";
            out += rowText(steps, track, length);
            out += "\n";
         }
      }
      if (chained) {
         char line[64];
         std::snprintf(line, sizeof(line), "%schain = %s\n", prefix,
                       kChainKeys[mode < 0 || mode >= kNumChainModes ? 0 : mode]);
         out += line;
         std::snprintf(line, sizeof(line), "%srepeat = %d\n", prefix, repeat);
         out += line;
      }
   }
   return out;
}

bool parsePatternLine(const std::string &key, const std::string &value, PatternData &out) {
   // p<N>_<row>
   if (key.size() < 4 || key[0] != 'p' || !std::isdigit(static_cast<unsigned char>(key[1])))
      return false;
   size_t i = 1;
   int n = 0;
   while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i])) && n < 1000)
      n = n * 10 + (key[i++] - '0');
   if (i >= key.size() || key[i] != '_' || n < 1 || n > kMaxPatterns)
      return false;
   const std::string row = key.substr(i + 1);
   const int pat = n - 1;

   if (row == "chain") {
      out.chainPresent = true;
      for (int m = 0; m < kNumChainModes; ++m)
         if (strcasecmp(value.c_str(), kChainKeys[m]) == 0)
            out.chainMode[pat] = m;
      return true;
   }
   if (row == "repeat") {
      out.chainPresent = true;
      const int r = std::atoi(value.c_str());
      out.chainRepeat[pat] = r < 1 ? 1 : (r > kMaxChainRepeat ? kMaxChainRepeat : r);
      return true;
   }

   int track = -1;
   for (int t = 0; t < kNumTracks; ++t)
      if (row == kTrackKeys[t])
         track = t;
   if (track < 0)
      return false;

   out.present = true;
   uint32_t *steps = out.pattern(pat);
   int step = 0;
   for (char c : value) {
      if (c == ' ' || c == '|' || c == '\t')
         continue;
      if (step >= kMaxSteps)
         break;
      uint32_t w = steps[step];
      if (track == kTrackAccent) {
         w = withAccent(w, !(c == '.' || c == '-' || c == '0'));
      } else {
         int level = kLevelOff;
         bool flam = false;
         switch (c) {
         case 'x':
         case 'o':
         case '1':
            level = kLevelOn;
            break;
         case 'X':
         case 'O':
         case '2':
            level = kLevelAccent;
            break;
         case 'f':
            level = kLevelOn;
            flam = true;
            break;
         case 'F':
            level = kLevelAccent;
            flam = true;
            break;
         default:
            break;
         }
         w = withLevel(w, track, level);
         w = withFlam(w, track, flam && level != kLevelOff);
      }
      steps[step++] = w;
   }
   return true;
}

void defaultPattern(uint32_t *steps) {
   PatternData tmp;
   parsePatternLine("p1_bd", "x... x... x... x...", tmp);
   parsePatternLine("p1_cp", ".... x... .... x...", tmp);
   parsePatternLine("p1_ch", "x..x x..x x..x x.xx", tmp);
   parsePatternLine("p1_oh", "..x. ..x. ..x. ..x.", tmp);
   parsePatternLine("p1_ac", "..x. ..x. ..x. ..x.", tmp);
   for (int i = 0; i < kMaxSteps; ++i)
      steps[i] = tmp.pattern(0)[i];
}

} // namespace rumpelkiste
