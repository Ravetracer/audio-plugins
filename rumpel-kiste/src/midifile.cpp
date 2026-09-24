#include "midifile.h"

#include "params.h"
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

namespace rumpelkiste {

int voiceForKey(int key) {
   switch (key) {
   case 35:
   case 36:
      return kVoiceBD;
   case 37:
      return kVoiceRS;
   case 38:
   case 40:
      return kVoiceSD;
   case 39:
      return kVoiceCP;
   case 41:
   case 43:
      return kVoiceLT;
   case 42:
   case 44:
      return kVoiceCH;
   case 45:
   case 47:
      return kVoiceMT;
   case 46:
      return kVoiceOH;
   case 48:
   case 50:
      return kVoiceHT;
   case 49:
   case 57:
      return kVoiceCR;
   case 51:
   case 53:
   case 59:
      return kVoiceRD;
   default:
      return -1;
   }
}

int keyForVoice(int voice) {
   static const int kKeys[kNumVoices] = {36, 38, 41, 45, 48, 37, 39, 42, 46, 49, 51};
   return voice >= 0 && voice < kNumVoices ? kKeys[voice] : 36;
}

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

void pushVar(std::string &out, unsigned long v) {
   unsigned char buf[5];
   int n = 0;
   buf[n++] = static_cast<unsigned char>(v & 0x7F);
   while ((v >>= 7) != 0)
      buf[n++] = static_cast<unsigned char>((v & 0x7F) | 0x80);
   while (n-- > 0)
      out.push_back(static_cast<char>(buf[n]));
}

struct Ev {
   long tick;
   int order; // at the same tick: note-off before note-on
   std::string bytes;
};

void addEvent(std::vector<Ev> &evs, long tick, int order, unsigned status, int data1, int data2) {
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
   const double bpm = spec.tempoBpm > 1.0 ? spec.tempoBpm : 120.0;
   const double flamTicks = spec.flamSeconds * bpm / 60.0 * kTicksPerQuarter;

   auto onsetOf = [&](long k) {
      return (static_cast<double>(k) + ((k & 1) ? spec.shuffle : 0.0)) * stepTicks;
   };

   // A plain hit stays clear of the accent threshold even when that has been
   // moved down, so the accents survive a trip back through the plugin.
   int plainVel = static_cast<int>(spec.accentVelocity) - 10;
   plainVel = plainVel > 90 ? 90 : (plainVel < 1 ? 1 : plainVel);
   const int accentVel = 127;
   // A drum hit has no length worth the name; a thirty-second is short enough
   // never to overlap the next hit on the same key at any scale.
   const long gate = std::max(1L, std::lround(stepTicks * 0.25));

   std::vector<Ev> evs;
   bool any = false;
   for (int i = 0; i < length; ++i) {
      const uint32_t w = spec.steps[i];
      const bool total = stepAccent(w);
      for (int v = 0; v < kNumVoices; ++v) {
         const int level = stepLevel(w, v);
         if (level == kLevelOff)
            continue;
         any = true;
         const int key = keyForVoice(v);
         const int vel = (total || level == kLevelAccent) ? accentVel : plainVel;
         const long on = std::lround(onsetOf(i));
         if (stepFlam(w, v)) {
            // The first stroke on the step, lighter, and the second after the
            // flam interval -- the owner's manual's order.
            const long second = on + std::max(1L, std::lround(flamTicks));
            addEvent(evs, on, 1, 0x99, key, std::max(1, plainVel * 2 / 3));
            addEvent(evs, std::min(on + gate, second), 0, 0x89, key, 0);
            addEvent(evs, second, 1, 0x99, key, vel);
            addEvent(evs, second + gate, 0, 0x89, key, 0);
         } else {
            addEvent(evs, on, 1, 0x99, key, vel);
            addEvent(evs, on + gate, 0, 0x89, key, 0);
         }
      }
   }
   if (!any)
      return std::string();

   std::stable_sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) {
      return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
   });

   std::string track;
   const unsigned long usPerQuarter = static_cast<unsigned long>(60000000.0 / bpm + 0.5);
   pushVar(track, 0);
   track += "\xFF\x51\x03";
   push8(track, static_cast<unsigned>(usPerQuarter >> 16));
   push8(track, static_cast<unsigned>(usPerQuarter >> 8));
   push8(track, static_cast<unsigned>(usPerQuarter));

   pushVar(track, 0);
   track += "\xFF\x58\x04";
   push8(track, 4);
   push8(track, 2);
   push8(track, 24);
   push8(track, 8);

   char name[48];
   std::snprintf(name, sizeof(name), "RumpelKiste Pattern %d", spec.patternNumber);
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
   push16(out, 0);
   push16(out, 1);
   push16(out, kTicksPerQuarter);
   out += "MTrk";
   push32(out, static_cast<unsigned long>(track.size()));
   out += track;
   return out;
}

std::string midiExportPath(int patternNumber) {
   char file[64];
   std::snprintf(file, sizeof(file), "RumpelKiste-Pattern-%02d.mid", patternNumber);

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
   dir += "RumpelKiste";
   CreateDirectoryA(dir.c_str(), nullptr); // already there is not an error
   return dir + "\\" + file;
#else
   const char *base = std::getenv("XDG_RUNTIME_DIR");
   if (!base || !*base)
      base = std::getenv("TMPDIR");
   if (!base || !*base)
      base = "/tmp";
   std::string dir = std::string(base) + "/rumpelkiste";
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

} // namespace rumpelkiste
