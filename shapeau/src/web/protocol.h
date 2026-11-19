#pragma once

// Messages between the WCLAP's plugin (module.wasm, running in the host's
// audio thread) and its editor (ui.wasm, running in the page), carried by
// clap.webview: the page posts to the plugin, the plugin answers through
// clap_host_webview.send(). Binary, little endian, fixed offsets.
//
// The page asks and the plugin answers; it never sends unasked, so a closed
// page costs nothing. What the editor needs is mirrored in the page: the
// parameter values, the shapes and the chain (CurveSet), the editor's view
// (effect, band, ALL, snap, step mode, which the plugin keeps in its state),
// and the scope's bins.
//
// Every answer to a poll echoes the page's edit count from that poll. Messages
// arrive in order, so an answer whose count is behind the page's own was
// written before the page's latest edits reached the plugin, and the page
// keeps what it has instead of jumping back for a frame. The data is then sent
// again with the next poll, because the page did not take its serial.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "dsp/curve.h"
#include "params.h"

namespace shapeau::web {

enum PageMessage : uint8_t {
   // [type][0 0 0][u32 value serial][u32 curve serial][u32 scope generation]
   // [u32 edit count]: answered by Status. Serials of 0 ask for everything
   // (the page just opened).
   PagePoll = 1,
   // A parameter edit in stored (raw) values, as GuiDelegate makes them.
   // [type][0 0 0][u32 id]
   PageBegin = 2,
   // [type][0 0 0][u32 id][f64 value]
   PageSet = 3,
   // [type][0 0 0][u32 id]
   PageEnd = 4,
   // Shapes that changed: [type][u8 count][0 0], then `count` curve records
   // (putCurve / getCurve).
   PageCurves = 5,
   // The chain: [type][0 0 0][u8 count][u8 module x kNumModules] (putChain)
   PageChain = 6,
   // The editor's view: [type][u8 module][u8 band][u8 flags] (kView*).
   PageView = 7,
   // A preset to load, as the text of its file: the plugin parses and applies
   // it, shapes and all. [type][0 0 0][u32 length][bytes]
   PagePreset = 8,
   // The end of a pointer gesture or a key: the plugin marks its state dirty
   // once if anything changed since the last one, so a drag of a shape point
   // is one undo step in the host. [type][0 0 0]
   PageCommit = 9,
   // The editor wants another window size (logical pixels).
   // [type][0 0 0][u32 width][u32 height]
   PageResize = 10,
};

enum PluginMessage : uint8_t {
   // [0]  type
   // [1]  u8 flags (kStatus*)
   // [2]  u16 parameter count
   // [4]  u32 value serial
   // [8]  u32 curve serial
   // [12] u32 the edit count of the poll answered
   // [16] u32 scope generation
   // [20] u32 scope blocks (moves while audio does)
   // [24] u32 trigger count
   // [28] f32 playhead, < 0 for none
   // [32] u16 scope runs, u16 0
   // then, with kStatusValues: f64 x parameter count
   // then, with kStatusCurves: [u8 module][u8 band][u8 view flags][u8 chain
   //   count][u8 chain x 8], then kNumModules x kNumBands curve records
   // then the scope runs: [u16 first bin][u16 bins], and per bin seven i16
   //   (in lo, in hi, out lo, out hi, side lo, side hi, trace; see scopeEncode)
   PluginStatus = 1,
};

constexpr uint8_t kStatusValues = 1;
constexpr uint8_t kStatusCurves = 2;
constexpr uint32_t kStatusHeader = 36;

constexpr uint8_t kViewAllBands = 1;
constexpr uint8_t kViewSnap = 2;
constexpr uint8_t kViewStepMode = 4;

constexpr int kScopeFields = 7;
constexpr size_t kPointBytes = 13; // f32 x, f32 y, f32 bend, u8 weight

// The window at scale 1, the size the host is asked for before the editor has
// said anything. The editor checks it against its own layout (web/uimain.cpp).
constexpr uint32_t kDesignW = 1432;
constexpr uint32_t kDesignH = 864;

// ------------------------------------------------------------------- writing

inline void put8(std::vector<uint8_t> &m, uint8_t v) { m.push_back(v); }
inline void put16(std::vector<uint8_t> &m, uint16_t v) {
   const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
   m.insert(m.end(), b, b + 2);
}
inline void put32(std::vector<uint8_t> &m, uint32_t v) {
   uint8_t b[4];
   std::memcpy(b, &v, 4);
   m.insert(m.end(), b, b + 4);
}
inline void putF32(std::vector<uint8_t> &m, float v) {
   uint8_t b[4];
   std::memcpy(b, &v, 4);
   m.insert(m.end(), b, b + 4);
}
inline void putF64(std::vector<uint8_t> &m, double v) {
   uint8_t b[8];
   std::memcpy(b, &v, 8);
   m.insert(m.end(), b, b + 8);
}
inline void at32(std::vector<uint8_t> &m, size_t at, uint32_t v) { std::memcpy(m.data() + at, &v, 4); }

// [u8 module][u8 band][u8 count][u8 0], then the points.
inline void putCurve(std::vector<uint8_t> &m, int module, int band, const Curve &c) {
   put8(m, static_cast<uint8_t>(module));
   put8(m, static_cast<uint8_t>(band));
   put8(m, static_cast<uint8_t>(c.count()));
   put8(m, 0);
   for (int i = 0; i < c.count(); ++i) {
      const CurvePoint &p = c.point(i);
      putF32(m, p.x);
      putF32(m, p.y);
      putF32(m, p.bend);
      put8(m, p.weight);
   }
}

// ------------------------------------------------------------------- reading

struct Reader {
   const uint8_t *data;
   size_t size;
   size_t pos = 0;
   bool ok = true;

   bool need(size_t n) {
      if (!ok || pos + n > size)
         ok = false;
      return ok;
   }
   uint8_t u8() { return need(1) ? data[pos++] : 0; }
   uint16_t u16() {
      uint16_t v = 0;
      if (need(2))
         std::memcpy(&v, data + pos, 2), pos += 2;
      return v;
   }
   uint32_t u32() {
      uint32_t v = 0;
      if (need(4))
         std::memcpy(&v, data + pos, 4), pos += 4;
      return v;
   }
   float f32() {
      float v = 0.0f;
      if (need(4))
         std::memcpy(&v, data + pos, 4), pos += 4;
      return v;
   }
   double f64() {
      double v = 0.0;
      if (need(8))
         std::memcpy(&v, data + pos, 8), pos += 8;
      return v;
   }
};

// One curve record into `set`. False when the record is malformed; a record
// for a module or band that does not exist is skipped.
inline bool getCurve(Reader &in, CurveSet &set) {
   const int module = in.u8();
   const int band = in.u8();
   const int count = in.u8();
   in.u8();
   if (!in.need(kPointBytes * static_cast<size_t>(count)))
      return false;
   CurvePoint points[kMaxCurvePoints];
   for (int i = 0; i < count; ++i) {
      CurvePoint p;
      p.x = in.f32();
      p.y = in.f32();
      p.bend = in.f32();
      p.weight = in.u8();
      if (i < kMaxCurvePoints)
         points[i] = p;
   }
   if (module >= kNumModules || band >= kNumBands)
      return true;
   return set.curve[module][band].assign(points, count);
}

// The chain as eight bytes: the count, then the modules (unused ones zero).
inline void putChain(std::vector<uint8_t> &m, const CurveSet &set) {
   put8(m, static_cast<uint8_t>(set.chainCount));
   for (int i = 0; i < kNumModules; ++i)
      put8(m, i < set.chainCount ? static_cast<uint8_t>(set.chain[i]) : 0);
}

inline void getChain(Reader &in, CurveSet &set) {
   const int count = in.u8();
   uint8_t modules[kNumModules];
   for (int i = 0; i < kNumModules; ++i)
      modules[i] = in.u8();
   if (!in.ok)
      return;
   set.chainCount = 0;
   for (int i = 0; i < count && i < kNumModules; ++i)
      chainAdd(set, modules[i]);
}

// ----------------------------------------------------------------- the scope
//
// A bin is seven values; the editor clamps the audio to +-1 and the trace to
// 0..1 when it draws them, so sixteen bits each lose nothing it shows. A
// trace below zero (none) is -32768.

inline int16_t scopeEncode(float v, bool trace) {
   if (trace && v < 0.0f)
      return -32768;
   v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
   if (!(v == v))
      v = 0.0f;
   return static_cast<int16_t>(v * 32767.0f + (v < 0.0f ? -0.5f : 0.5f));
}

inline float scopeDecode(int16_t v, bool trace) {
   if (trace && v == -32768)
      return -1.0f;
   return static_cast<float>(v) / 32767.0f;
}

} // namespace shapeau::web
