#include "shapeau.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "dsp/curve.h"
#include "params.h"

namespace shapeau {

// The preset format itself lives in shared/src/preset.cpp. All this plugin
// contributes is which name, extension and parameter table to use, and the
// wrappers that bind them.
const PresetContext &presetContext() {
   // kPluginDirName, not kPluginName: this context locates the preset
   // directories, and those are named after the binary rather than the label
   // the host shows.
   static const PresetContext ctx{kPluginDirName, kPresetExtension, paramTable(), kNumParams};
   return ctx;
}

std::string factoryPresetDir() { return plugincore::factoryPresetDir(presetContext()); }
std::string userPresetDir() { return plugincore::userPresetDir(presetContext()); }

bool parsePreset(const char *text, size_t length, PresetData &out, std::string &error) {
   return plugincore::parsePreset(presetContext(), text, length, out, error);
}

bool parsePresetFile(const std::string &path, PresetData &out, std::string &error) {
   return plugincore::parsePresetFile(presetContext(), path, out, error);
}

std::string formatPreset(const PresetData &preset) {
   return plugincore::formatPreset(presetContext(), preset);
}

std::string userPresetPath(const std::string &name) {
   return plugincore::userPresetPath(presetContext(), name);
}

// ------------------------------------------------------------------- shapes
//
// The shared parser drops any key it does not recognise, which is exactly the
// hook the shapes need: they are not parameters, they do not belong in the
// parameter table, and the shared format already tolerates lines it does not
// understand. One line per module and band, 21 in all:
//
//   curve vol 0 = 0.0000 1.0000 0 0.000, 0.2500 0.0000 2 0.000, 1.0000 1.0000 0 0.000
//
// A point is time, value, corner weight and segment bend. Files written before
// bend existed have three numbers and still load with a straight segment.
// Band numbers are the engine's: 0 low, 1 mid, 2 high.

namespace {

void trimEnds(std::string &s) {
   size_t b = 0;
   while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
      ++b;
   size_t e = s.size();
   while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
      --e;
   s = s.substr(b, e - b);
}

// Parses one `curve` line into `set`. Returns false when the line is a curve
// line that cannot be read -- a malformed shape is worth reporting rather than
// silently leaving the default in its place.
bool parseCurveLine(const std::string &line, CurveSet &set) {
   std::istringstream in(line);
   std::string keyword, module, bandText, equals;
   if (!(in >> keyword >> module >> bandText >> equals))
      return false;
   if (keyword != "curve" || equals != "=")
      return false;
   const int m = moduleFromKey(module);
   if (m < 0)
      return false;
   int band = -1;
   try {
      band = std::stoi(bandText);
   } catch (...) {
      return false;
   }
   if (band < 0 || band >= kNumBands)
      return false;

   std::string body;
   std::getline(in, body);
   trimEnds(body);
   Curve parsed = set.curve[m][band];
   if (!parsed.parse(body))
      return false;
   set.curve[m][band] = parsed;
   return true;
}

void scanCurveLines(const char *text, size_t length, CurveSet &out, std::string *error) {
   std::string all(text, length);
   std::istringstream in(all);
   std::string line;
   while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r')
         line.pop_back();
      std::string trimmed = line;
      trimEnds(trimmed);
      // The chain: "chain = vol flt drv". A preset without the line has an
      // empty list, which runs whatever it switches on in the old order.
      if (trimmed.compare(0, 5, "chain") == 0) {
         const size_t eq = trimmed.find('=');
         if (eq != std::string::npos && !parseChain(trimmed.substr(eq + 1), out) && error &&
             error->empty())
            *error = "the effect chain line could not be read";
         continue;
      }
      if (trimmed.empty() || trimmed[0] == '#' || trimmed.compare(0, 6, "curve ") != 0)
         continue;
      if (!parseCurveLine(trimmed, out) && error && error->empty())
         *error = "a shape curve line could not be read";
   }
}

} // namespace

bool parsePreset(const char *text, size_t length, PresetData &out, CurveSet &curves,
                 std::string &error) {
   if (!plugincore::parsePreset(presetContext(), text, length, out, error))
      return false;
   // A preset describes the whole sound, so the shapes start from neutral and
   // the file fills in what it has. A preset written before shapes existed
   // therefore loads with every module flat, which is what it meant.
   resetCurves(curves);
   std::string curveError;
   if (text)
      scanCurveLines(text, length, curves, &curveError);
   if (!curveError.empty()) {
      error = curveError;
      return false;
   }
   return true;
}

bool parsePresetFileWithCurves(const std::string &path, PresetData &out, CurveSet &curves,
                               std::string &error) {
   std::ifstream file(path, std::ios::binary);
   if (!file) {
      error = "cannot open '" + path + "'";
      return false;
   }
   std::ostringstream buf;
   buf << file.rdbuf();
   const std::string text = buf.str();
   if (!parsePreset(text.c_str(), text.size(), out, curves, error)) {
      error = path + ": " + error;
      return false;
   }
   return true;
}

std::string formatPreset(const PresetData &preset, const CurveSet &curves) {
   std::string out = plugincore::formatPreset(presetContext(), preset);
   if (curves.chainCount > 0) {
      out += "\n# The effects in the chain, in the order the audio runs through them.\n";
      out += "chain = " + formatChain(curves) + "\n";
   }
   out += "\n# Shape curves. One line per module and band, in normalised time\n";
   out += "# and value, with each point's corner weight (0 hard, 1 medium, 2\n";
   out += "# soft) and the bend of the segment leaving it (-1 .. +1).\n";
   out += "# Bands are 0 low, 1 mid, 2 high.\n";
   for (int m = 0; m < kNumModules; ++m)
      for (int b = 0; b < kNumBands; ++b) {
         out += "curve ";
         out += moduleKey(m);
         out += " ";
         out += std::to_string(b);
         out += " = ";
         out += curves.curve[m][b].format();
         out += "\n";
      }
   return out;
}

} // namespace shapeau
