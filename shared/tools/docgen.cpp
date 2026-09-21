// Generates the reference sections of a plugin's manual from the plugin itself.
//
// The parameter reference is the plugin's own ParamDesc table -- the same table
// the host, the window and the preset format are built from -- so a manual
// generated here cannot drift from the build it documents. The preset library
// is read from the preset files, whose headers already carry a name, a
// description and a feature list.
//
// Built by shared/tools/make-manual.sh, which compiles it directly against one
// plugin's params.cpp rather than through that plugin's CMake project: release
// builds switch the offline tools off, and the manual has to be buildable
// anyway.
//
//   docgen --params                  the parameter reference, as Markdown
//   docgen --params-brief            the same table without the explanations
//   docgen --presets <presets-dir>   the preset library, as Markdown
//
// PLUGINCORE_DOC_NS names the plugin namespace to document, e.g. -DPLUGINCORE_DOC_NS=rainyday.

#include "params.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#ifndef PLUGINCORE_DOC_NS
#error "define PLUGINCORE_DOC_NS with the plugin namespace to document"
#endif

namespace plugin = PLUGINCORE_DOC_NS;
using plugincore::ParamDesc;
using plugincore::ParamKind;

namespace {

std::string display(const ParamDesc &d, double raw) {
   char buf[128];
   if (!plugincore::paramValueToText(d, raw, buf, sizeof(buf)))
      return "?";
   return buf;
}

// Markdown table cells cannot contain an unescaped pipe, and the tips are
// written as prose with "--" for a dash, which the Markdown step turns into an
// en dash by itself.
//
// A tip written across several source lines carries real newlines, and a
// Markdown table row ends at the first one -- which silently truncated the
// row and left the rest of the tip as a stray paragraph. Every run of
// whitespace therefore collapses to a single space.
std::string cell(const char *text) {
   std::string out;
   bool space = false;
   for (const char *p = text; *p; ++p) {
      const bool isSpace = *p == ' ' || *p == '\n' || *p == '\t' || *p == '\r';
      if (isSpace) {
         space = true;
         continue;
      }
      if (space && !out.empty())
         out += ' ';
      space = false;
      if (*p == '|')
         out += "\\|";
      else
         out += *p;
   }
   return out;
}

// What the parameter can be set to, as one string. Enums list their choices;
// everything else is its two ends, formatted exactly as the plugin displays
// them.
std::string range(const ParamDesc &d) {
   if (d.kind == ParamKind::Enum) {
      std::string out;
      for (uint32_t i = 0; i < d.enumCount; ++i) {
         if (i)
            out += ", ";
         out += d.enumNames[i];
      }
      return out;
   }
   return display(d, d.min) + " … " + display(d, d.max);
}

// `brief` leaves out the "What it does" column.
//
// The explanations come from each parameter's `tip`, which is written for the
// plugin's own help line and names the hardware it models outright. A manual
// that may not print those names therefore cannot take the tips -- but it can
// still take the part that actually goes stale, which is the ranges and the
// defaults. A brief table is a reference a manual can carry beside its own
// hand-written prose without either repeating it or contradicting it.
void emitParams(bool brief = false) {
   const ParamDesc *table = plugin::paramTable();
   const uint32_t count = plugin::kNumParams;

   // Modules in the order they first appear in the table, which is the order
   // the plugin window lays its panels out in.
   std::vector<std::string> modules;
   for (uint32_t i = 0; i < count; ++i) {
      const std::string m = table[i].module;
      if (std::find(modules.begin(), modules.end(), m) == modules.end())
         modules.push_back(m);
   }

   if (brief)
      std::printf("Every parameter, its range and where it starts, grouped the way the\n"
                  "plugin window groups them. This table is generated from the\n"
                  "instrument itself, so it cannot disagree with the build. What each\n"
                  "control is for is in the chapters above. Every one is automatable and\n"
                  "modulatable from the host, and every one can be typed in directly by\n"
                  "clicking its value in the window; the name in `code` is the key used\n"
                  "in preset files.\n\n");
   else
      std::printf("There are %u parameters, grouped the way the plugin window groups\n"
                  "them. Every one is automatable and modulatable from the host, and every\n"
                  "one can be typed in directly by clicking its value in the window. The\n"
                  "name in `code` is the key used in preset files.\n\n",
                  count);

   for (const std::string &m : modules) {
      std::printf("### %s\n\n", m.c_str());
      std::printf(brief ? "| Parameter | Range | Default |\n"
                        : "| Parameter | Range | Default | What it does |\n");
      std::printf(brief ? "|---|---|---|\n" : "|---|---|---|---|\n");
      for (uint32_t i = 0; i < count; ++i) {
         const ParamDesc &d = table[i];
         if (m != d.module)
            continue;
         if (brief)
            std::printf("| **%s**<br>`%s` | %s | %s |\n", d.name, d.key, range(d).c_str(),
                        display(d, d.def).c_str());
         else
            std::printf("| **%s**<br>`%s` | %s | %s | %s |\n", d.name, d.key, range(d).c_str(),
                        display(d, d.def).c_str(), cell(d.tip).c_str());
      }
      std::printf("\n");
   }
}

struct Preset {
   std::string file;
   std::string name;
   std::string description;
   std::string features;
};

// The preset header, which is the first few "key = value" lines of the file.
// Enough of the format to read a name and a description out of it; the values
// themselves are the plugin's business, not the manual's.
bool readHeader(const std::string &path, Preset &out) {
   FILE *f = std::fopen(path.c_str(), "rb");
   if (!f)
      return false;
   char line[2048];
   while (std::fgets(line, sizeof(line), f)) {
      std::string s(line);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
         s.pop_back();
      const size_t eq = s.find('=');
      if (s.empty() || s[0] == '#' || eq == std::string::npos)
         continue;
      std::string key = s.substr(0, eq);
      std::string val = s.substr(eq + 1);
      const auto trim = [](std::string &t) {
         while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t.erase(t.begin());
         while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
            t.pop_back();
      };
      trim(key);
      trim(val);
      if (key == "name")
         out.name = val;
      else if (key == "description")
         out.description = val;
      else if (key == "features")
         out.features = val;
   }
   std::fclose(f);
   return !out.name.empty();
}

void emitPresets(const char *dir) {
   DIR *d = opendir(dir);
   if (!d) {
      std::fprintf(stderr, "docgen: cannot read %s\n", dir);
      return;
   }
   std::vector<Preset> presets;
   while (const dirent *e = readdir(d)) {
      const std::string file = e->d_name;
      if (file.size() < 2 || file[0] == '.')
         continue;
      Preset p;
      p.file = file;
      if (readHeader(std::string(dir) + "/" + file, p))
         presets.push_back(p);
   }
   closedir(d);

   std::sort(presets.begin(), presets.end(),
             [](const Preset &a, const Preset &b) { return a.name < b.name; });

   std::printf("%zu factory presets ship with the plugin. They are installed beside\n"
               "the binary and exposed through CLAP preset discovery, so they appear in\n"
               "the host's own browser as well as in the plugin window's.\n\n",
               presets.size());

   for (const Preset &p : presets) {
      std::printf("**%s**\n: %s", p.name.c_str(), p.description.c_str());
      if (!p.features.empty())
         std::printf(" *(%s)*", p.features.c_str());
      std::printf("\n\n");
   }
}

} // namespace

int main(int argc, char **argv) {
   const std::string mode = argc > 1 ? argv[1] : "";
   if (mode == "--params-brief") {
      emitParams(true);
      return 0;
   }
   if (mode == "--params") {
      emitParams();
      return 0;
   }
   if (mode == "--presets" && argc > 2) {
      emitPresets(argv[2]);
      return 0;
   }
   std::fprintf(stderr, "usage: docgen --params | --params-brief | --presets <dir>\n");
   return 2;
}
