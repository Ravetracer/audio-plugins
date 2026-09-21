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


// ------------------------------------------------------------------ reading

namespace {

// A standard MIDI file is big-endian throughout, and its delta times are the
// variable-length quantity the spec calls a "variable-length value": seven
// bits per byte, high bit set on every byte but the last.
struct Reader {
   const unsigned char *p;
   size_t left;

   bool need(size_t n) const { return left >= n; }
   unsigned char byte() {
      --left;
      return *p++;
   }
   uint32_t be(int n) {
      uint32_t v = 0;
      for (int i = 0; i < n; ++i)
         v = (v << 8) | byte();
      return v;
   }
   // Returns false on a quantity that never terminates, which is what a
   // truncated file looks like from in here.
   bool vlq(uint32_t &out) {
      out = 0;
      for (int i = 0; i < 4; ++i) {
         if (!need(1))
            return false;
         const unsigned char c = byte();
         out = (out << 7) | (c & 0x7Fu);
         if (!(c & 0x80u))
            return true;
      }
      return false;
   }
};

} // namespace

bool parseMidiFile(const char *bytes, size_t length, MidiRead &out, std::string &error) {
   out = MidiRead();
   if (!bytes || length < 14 || std::string(bytes, 4) != "MThd") {
      error = "Not a MIDI file.";
      return false;
   }
   Reader h{reinterpret_cast<const unsigned char *>(bytes) + 8, 6};
   const uint32_t headerLen = static_cast<uint32_t>(
      (static_cast<unsigned char>(bytes[4]) << 24) | (static_cast<unsigned char>(bytes[5]) << 16) |
      (static_cast<unsigned char>(bytes[6]) << 8) | static_cast<unsigned char>(bytes[7]));
   const int format = static_cast<int>(h.be(2));
   const int tracks = static_cast<int>(h.be(2));
   const int division = static_cast<int>(static_cast<int16_t>(h.be(2)));
   if (format > 1) {
      error = "Only format 0 and format 1 MIDI files are read.";
      return false;
   }
   if (division <= 0) {
      error = "That MIDI file is timed in SMPTE frames rather than in beats.";
      return false;
   }
   out.division = division;

   // Note-ons waiting for their note-off, per key. A list rather than one
   // entry, because a file may legitimately restart a key before releasing it.
   std::vector<std::pair<long, int>> pending[128];

   size_t pos = 8 + headerLen;
   for (int t = 0; t < tracks && pos + 8 <= length; ++t) {
      if (std::string(bytes + pos, 4) != "MTrk")
         break;
      const uint32_t trackLen = static_cast<uint32_t>(
         (static_cast<unsigned char>(bytes[pos + 4]) << 24) |
         (static_cast<unsigned char>(bytes[pos + 5]) << 16) |
         (static_cast<unsigned char>(bytes[pos + 6]) << 8) |
         static_cast<unsigned char>(bytes[pos + 7]));
      pos += 8;
      const size_t end = trackLen > length - pos ? length : pos + trackLen;
      Reader r{reinterpret_cast<const unsigned char *>(bytes) + pos, end - pos};
      long now = 0;
      unsigned char running = 0;
      while (r.need(1)) {
         uint32_t delta = 0;
         if (!r.vlq(delta))
            break;
         now += static_cast<long>(delta);
         if (!r.need(1))
            break;
         unsigned char status = *r.p;
         if (status & 0x80u) {
            r.byte();
            if (status < 0xF0u)
               running = status;
         } else {
            // Running status: the last channel status byte still applies.
            status = running;
            if (!status)
               break;
         }

         if (status == 0xFFu) { // meta
            if (!r.need(1))
               break;
            r.byte(); // the type, which nothing here needs
            uint32_t len = 0;
            if (!r.vlq(len) || !r.need(len))
               break;
            r.p += len;
            r.left -= len;
            continue;
         }
         if (status == 0xF0u || status == 0xF7u) { // sysex
            uint32_t len = 0;
            if (!r.vlq(len) || !r.need(len))
               break;
            r.p += len;
            r.left -= len;
            continue;
         }

         const unsigned char high = status & 0xF0u;
         const int operands = (high == 0xC0u || high == 0xD0u) ? 1 : 2;
         if (!r.need(static_cast<size_t>(operands)))
            break;
         const int d1 = r.byte();
         const int d2 = operands == 2 ? r.byte() : 0;

         if (high == 0x90u && d2 > 0) {
            pending[d1 & 127].emplace_back(now, d2);
         } else if (high == 0x80u || (high == 0x90u && d2 == 0)) {
            auto &q = pending[d1 & 127];
            if (!q.empty()) {
               MidiNote n;
               n.onset = q.front().first;
               n.velocity = q.front().second;
               n.release = now;
               n.key = d1 & 127;
               out.notes.push_back(n);
               q.erase(q.begin());
            }
         } else if (high == 0xB0u && d1 == 1) {
            out.mod.emplace_back(now, d2 > 0);
         }
      }
      pos = end;
   }

   // A note still held at the end of the file is released there rather than
   // dropped: a one-bar loop exported without a final note-off is still a
   // pattern, and throwing its last note away would be the wrong answer.
   long last = 0;
   for (const MidiNote &n : out.notes)
      last = std::max(last, n.release);
   for (int k = 0; k < 128; ++k)
      for (const auto &held : pending[k]) {
         MidiNote n;
         n.onset = held.first;
         n.velocity = held.second;
         n.release = std::max(last, held.first + 1);
         n.key = k;
         out.notes.push_back(n);
      }

   if (out.notes.empty()) {
      error = "That MIDI file has no notes in it.";
      return false;
   }
   std::sort(out.notes.begin(), out.notes.end(), [](const MidiNote &a, const MidiNote &b) {
      return a.onset != b.onset ? a.onset < b.onset : a.key < b.key;
   });
   std::sort(out.mod.begin(), out.mod.end());
   return true;
}

} // namespace saeurekiste
