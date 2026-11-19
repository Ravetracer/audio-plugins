#pragma once

// Messages between the WCLAP's two modules: module.wasm, the plugin, on the
// host's audio thread, and ui.wasm, the editor, in the plugin's page. Carried
// by clap.webview: the page posts, the plugin answers through
// clap_host_webview.send(). The plugin never sends unasked, so a closed page
// costs nothing.
//
// The editor works on a second instance of the plugin (the mirror, see
// plugin_web.inc) and sends what changed in it as the page messages below:
// gestures and values as they happen, everything else -- the pattern bank,
// the chains, the pattern map, the mutes, bulk value changes from a preset
// load -- as the difference to what it last sent. The plugin answers each poll
// with what the editor shows of the playing instrument (playhead, meters,
// parameter values when they moved) and with a state serial that moves when
// something other than the page changed the plugin (a project load, a learned
// note, the host's preset list); the page then asks for the whole state and
// loads it into the mirror.
//
// Binary, little endian, fixed offsets. Every message starts with a type byte
// and three bytes of padding.

#include <cstdint>
#include <cstring>
#include <vector>

#include "params.h"

namespace rumpelkiste::web {

enum PageMessage : uint8_t {
   // [u32 pageId][u32 opsSent][u32 valueSerial]: answered by PluginStatus.
   // A valueSerial the plugin does not have gets the values.
   kPagePoll = 1,
   // [u32 paramId]
   kPageBegin = 2,
   // [u32 paramId][f64 stored value]
   kPageSet = 3,
   // [u32 paramId]
   kPageEnd = 4,
   // Values moved without a gesture (a preset, an FX slot moved):
   // [u32 count] then count x [u32 paramId][f64 stored value].
   kPageValues = 5,
   // One pattern, whole: [u32 pattern][u32 x kMaxSteps]
   kPagePattern = 6,
   // Every pattern's chain: kMaxPatterns x [u8 mode][u8 0][u16 repeat]
   kPageChains = 7,
   // Live mode's map: 128 x i8
   kPageNoteMap = 8,
   // [u32 open]
   kPageAdvanced = 9,
   // [i32 target]
   kPageLearn = 10,
   // One byte per voice, its mute: kNumVoices x u8
   kPageMutes = 11,
   // Voices to hear once, a row label or the version label clicked: [u32 bits]
   kPageAudition = 12,
   // PLAY, the sequencer running with the host stopped: [u32 on]
   kPageFreeRun = 13,
   // What the editor keeps about its preset field, opaque to the plugin and
   // handed back with the state: [bytes...]
   kPageMemo = 14,
   // The end of an edit that was not a gesture: one undo step.
   kPageCommit = 15,
   // [u32 pageId][u32 opsSent]: answered by PluginState.
   kPageGetState = 16,
   // The editor's size and its design size, in CSS pixels:
   // [u32 width][u32 height][u32 designWidth][u32 designHeight]
   kPageResize = 17,
};

enum PluginMessage : uint8_t {
   // [u32 pageId][u32 opsSeen][u32 valueSerial][u32 stateSerial]
   // [f32 peakL][f32 peakR][u32 voices][u32 hitCounter][i32 playhead]
   // [i32 playingPattern][i32 activePattern][i32 learnTarget][f64 tempo]
   // [u8 freeRun][u8 hostPlaying][u8 0][u8 0][kNumVoices x u32 hits]
   // [u32 valueCount], then valueCount x f64 by parameter id when the page's
   // valueSerial was not the current one, else valueCount is 0.
   kPluginStatus = 1,
   // [u32 pageId][u32 opsSeen][u32 stateSerial][u32 memoLength][memo]
   // [u32 stateLength][the plugin's clap.state]
   kPluginState = 2,
};

// Up to the per-voice hit counters, and up to the values.
constexpr size_t kStatusFixed = 64;
constexpr size_t kStatusHeader = kStatusFixed + 4u * kNumVoices + 4u;

// Little helpers for both ends.
struct Writer {
   std::vector<uint8_t> bytes;
   explicit Writer(uint8_t type) { bytes.assign(4, 0); bytes[0] = type; }
   template <typename T> void put(T v) {
      const size_t at = bytes.size();
      bytes.resize(at + sizeof(T));
      std::memcpy(bytes.data() + at, &v, sizeof(T));
   }
   void putBytes(const void *data, size_t n) {
      const auto *p = static_cast<const uint8_t *>(data);
      bytes.insert(bytes.end(), p, p + n);
   }
};

struct Reader {
   const uint8_t *data;
   size_t size;
   size_t pos = 4;
   Reader(const void *d, size_t n) : data(static_cast<const uint8_t *>(d)), size(n) {}
   template <typename T> bool get(T &v) {
      if (pos + sizeof(T) > size)
         return false;
      std::memcpy(&v, data + pos, sizeof(T));
      pos += sizeof(T);
      return true;
   }
   bool getBytes(void *out, size_t n) {
      if (pos + n > size)
         return false;
      std::memcpy(out, data + pos, n);
      pos += n;
      return true;
   }
   const uint8_t *here() const { return data + pos; }
   size_t left() const { return size - pos; }
};

} // namespace rumpelkiste::web
