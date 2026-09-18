#include "midifile.h"

#include "pattern.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace saeurekiste {
namespace {

constexpr int kTicksPerQuarter = 960;

void push8(std::string &out, unsigned v) { out.push_back(static_cast<char>(v & 0xFF)); }

void push16(std::string &out, unsigned v) {
   push8(out, v >> 8);
   push8(out, v);
}

void push32(std::string &out, unsigned long v) {
   push8(out, static_cast<unsigned>(v >> 24));
   push8(out, static_cast<unsigned>(v >> 16));
   push8(out, static_cast<unsigned>(v >> 8));
   push8(out, static_cast<unsigned>(v));
}

// A MIDI variable-length quantity: seven bits at a time, high bit set on every
// byte but the last.
void pushVar(std::string &out, unsigned long v) {
   unsigned char buf[5];
   int n = 0;
   buf[n++] = static_cast<unsigned char>(v & 0x7F);
   while ((v >>= 7) != 0)
      buf[n++] = static_cast<unsigned char>((v & 0x7F) | 0x80);
   while (n-- > 0)
      out.push_back(static_cast<char>(buf[n]));
}

// One event, before the deltas are worked out.
struct Ev {
   long tick;
   int order; // at the same tick: note-off, then controller, then note-on
   std::string bytes;
};

void addNote(std::vector<Ev> &evs, long tick, int order, unsigned status, int data1, int data2) {
   Ev e;
   e.tick = tick < 0 ? 0 : tick;
   e.order = order;
   e.bytes.push_back(static_cast<char>(status));
   e.bytes.push_back(static_cast<char>(data1 & 0x7F));
   e.bytes.push_back(static_cast<char>(data2 & 0x7F));
   evs.push_back(e);
}

} // namespace

std::string patternToMidiFile(const MidiExport &spec) {
   if (!spec.steps)
      return std::string();

   const int length = spec.length < 1 ? 1 : (spec.length > kMaxSteps ? kMaxSteps : spec.length);
   const double perBeat = spec.stepsPerBeat > 0.0 ? spec.stepsPerBeat : 4.0;
   const double stepTicks = static_cast<double>(kTicksPerQuarter) / perBeat;
   const double swing = (spec.swingPercent * 0.01 - 0.5) * 2.0;
   const double gate = spec.gate < 0.05 ? 0.05 : (spec.gate > 1.0 ? 1.0 : spec.gate);

   // An odd step is late by the swing, exactly as the sequencer places it.
   auto onsetOf = [&](long k) {
      return (static_cast<double>(k) + ((k & 1) ? swing : 0.0)) * stepTicks;
   };

   const int accentVel = 127;
   // A plain note stays clear of the accent threshold even when that has been
   // moved down, so the accents survive a trip back through the plugin.
   int plainVel = static_cast<int>(spec.accentVelocity) - 10;
   if (plainVel > 80)
      plainVel = 80;
   if (plainVel < 1)
      plainVel = 1;

   std::vector<Ev> evs;
   bool any = false;
   for (int i = 0; i < length; ++i) {
      const Step st = Step::unpack(spec.steps[i]);
      if (st.note < 0)
         continue;
      any = true;
      const int key = std::max(0, std::min(127, 36 + st.note + 12 * st.octave));
      const double on = onsetOf(i);
      const double nextOn = onsetOf(i + 1);
      // A slid step holds past the next step's start: the overlap is what the
      // plugin reads as a slide, and what a DAW shows as two notes tied.
      const double off = st.slide ? nextOn + 0.02 * stepTicks : on + gate * (nextOn - on);

      addNote(evs, std::lround(on), 2, 0x90, key, st.accent ? accentVel : plainVel);
      addNote(evs, std::lround(off), 0, 0x80, key, 0);
      if (st.vibrato) {
         // CC1 for the length of the note, which is where the plugin's own MIDI
         // mode takes its vibrato from.
         addNote(evs, std::lround(on), 1, 0xB0, 1, 127);
         addNote(evs, std::lround(off), 1, 0xB0, 1, 0);
      }
   }
   if (!any)
      return std::string();

   std::stable_sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) {
      return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
   });

   std::string track;

   // The tempo and the time signature, so a host that reads them puts the
   // pattern at the speed it was written at.
   const double bpm = spec.tempoBpm > 1.0 ? spec.tempoBpm : 120.0;
   const unsigned long usPerQuarter = static_cast<unsigned long>(60000000.0 / bpm + 0.5);
   pushVar(track, 0);
   track += "\xFF\x51\x03";
   push8(track, static_cast<unsigned>(usPerQuarter >> 16));
   push8(track, static_cast<unsigned>(usPerQuarter >> 8));
   push8(track, static_cast<unsigned>(usPerQuarter));

   pushVar(track, 0);
   track += "\xFF\x58\x04";
   push8(track, 4);
   push8(track, 2); // 4/4
   push8(track, 24);
   push8(track, 8);

   char name[48];
   std::snprintf(name, sizeof(name), "SaeureKiste Pattern %d", spec.patternNumber);
   pushVar(track, 0);
   track += "\xFF\x03";
   pushVar(track, std::strlen(name));
   track += name;

   long last = 0;
   for (const Ev &e : evs) {
      pushVar(track, static_cast<unsigned long>(e.tick - last));
      last = e.tick;
      track += e.bytes;
   }
   pushVar(track, 0);
   track += "\xFF\x2F";
   push8(track, 0);

   std::string out;
   out += "MThd";
   push32(out, 6);
   push16(out, 0); // format 0
   push16(out, 1); // one track
   push16(out, kTicksPerQuarter);
   out += "MTrk";
   push32(out, static_cast<unsigned long>(track.size()));
   out += track;
   return out;
}

std::string midiExportPath(int patternNumber) {
   char file[64];
   std::snprintf(file, sizeof(file), "SaeureKiste-Pattern-%02d.mid", patternNumber);

#if defined(_WIN32)
   wchar_t tmp[MAX_PATH];
   const DWORD n = GetTempPathW(MAX_PATH, tmp);
   if (n == 0 || n >= MAX_PATH)
      return std::string();
   char narrow[MAX_PATH * 2];
   const int len =
      WideCharToMultiByte(CP_UTF8, 0, tmp, -1, narrow, sizeof(narrow), nullptr, nullptr);
   if (len <= 0)
      return std::string();
   std::string dir(narrow);
   dir += "SaeureKiste";
   CreateDirectoryA(dir.c_str(), nullptr); // already there is not an error
   return dir + "\\" + file;
#else
   const char *base = std::getenv("XDG_RUNTIME_DIR");
   if (!base || !*base)
      base = std::getenv("TMPDIR");
   if (!base || !*base)
      base = "/tmp";
   std::string dir = std::string(base) + "/saeurekiste";
   if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
      return std::string();
   return dir + "/" + file;
#endif
}

bool writeMidiFile(const std::string &path, const std::string &bytes, std::string &error) {
   if (path.empty()) {
      error = "no writable temporary directory";
      return false;
   }
   if (bytes.empty()) {
      error = "the pattern is empty";
      return false;
   }
   std::FILE *f = std::fopen(path.c_str(), "wb");
   if (!f) {
      error = "cannot write " + path;
      return false;
   }
   const size_t wrote = std::fwrite(bytes.data(), 1, bytes.size(), f);
   const bool ok = wrote == bytes.size();
   std::fclose(f);
   if (!ok)
      error = "short write to " + path;
   return ok;
}

} // namespace saeurekiste
