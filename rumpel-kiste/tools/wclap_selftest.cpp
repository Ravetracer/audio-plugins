// Self-test for the WCLAP's plugin module: drives the plugin through
// clap_entry as a WCLAP host would (minus the function-table plumbing), built
// as a WASI command from the same sources as module.wasm and run with
//
//   node ../shared/tools/run-wasi.mjs build-wasm/rumpelkiste-wclap-selftest.wasm
//
// Checks the gui extension (webview only), a hit through the voices, and the
// page protocol of src/web/protocol.h end to end: polls, a gesture and the
// output events and undo step it makes, a pattern edit the sequencer plays
// (PLAY from the page, the host stopped), the non-parameter state the editor
// sends, the state the editor asks for, the state serial, and a state round
// trip.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include "midifile.h"
#include "params.h"
#include "pattern.h"
#include "web/protocol.h"

extern "C" const clap_plugin_entry_t clap_entry;

using namespace rumpelkiste;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok)
      ++failures;
}

// ------------------------------------------------------------- host side

std::vector<std::vector<uint8_t>> sentToPage;
bool hostWebviewSend(const clap_host_t *, const void *data, uint32_t size) {
   const auto *b = static_cast<const uint8_t *>(data);
   sentToPage.emplace_back(b, b + size);
   return true;
}
const clap_host_webview_t kHostWebview = {hostWebviewSend};
int dirtyMarks = 0;
void hostMarkDirty(const clap_host_t *) { ++dirtyMarks; }
const clap_host_state_t kHostState = {hostMarkDirty};
int rescans = 0;
void hostRescan(const clap_host_t *, clap_param_rescan_flags) { ++rescans; }
void hostClear(const clap_host_t *, clap_id, clap_param_clear_flags) {}
void hostRequestFlush(const clap_host_t *) {}
const clap_host_params_t kHostParams = {hostRescan, hostClear, hostRequestFlush};

const void *hostGetExtension(const clap_host_t *, const char *id) {
   if (!std::strcmp(id, CLAP_EXT_WEBVIEW))
      return &kHostWebview;
   if (!std::strcmp(id, CLAP_EXT_STATE))
      return &kHostState;
   if (!std::strcmp(id, CLAP_EXT_PARAMS))
      return &kHostParams;
   return nullptr;
}
void hostNoop(const clap_host_t *) {}

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "rumpelkiste-wclap-selftest", "Ravetracer", "",
                           "0",               hostGetExtension, hostNoop, hostNoop, hostNoop};

struct InEvents {
   std::vector<clap_event_note_t> notes;
   static uint32_t size(const clap_input_events_t *l) {
      return static_cast<uint32_t>(static_cast<const InEvents *>(l->ctx)->notes.size());
   }
   static const clap_event_header_t *get(const clap_input_events_t *l, uint32_t i) {
      return &static_cast<const InEvents *>(l->ctx)->notes[i].header;
   }
};

struct OutEvents {
   std::vector<uint16_t> types;
   std::vector<clap_id> params;
   std::vector<double> values;
   std::vector<int16_t> noteKeys;
   static bool push(const clap_output_events_t *l, const clap_event_header_t *h) {
      auto *self = static_cast<OutEvents *>(l->ctx);
      self->types.push_back(h->type);
      if (h->type == CLAP_EVENT_PARAM_VALUE) {
         const auto *ev = reinterpret_cast<const clap_event_param_value_t *>(h);
         self->params.push_back(ev->param_id);
         self->values.push_back(ev->value);
      } else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
         self->params.push_back(reinterpret_cast<const clap_event_param_gesture_t *>(h)->param_id);
         self->values.push_back(0.0);
      } else if (h->type == CLAP_EVENT_NOTE_ON) {
         self->noteKeys.push_back(reinterpret_cast<const clap_event_note_t *>(h)->key);
      }
      return true;
   }
   void clear() {
      types.clear();
      params.clear();
      values.clear();
      noteKeys.clear();
   }
};

struct Buffer {
   std::vector<uint8_t> data;
   size_t readPos = 0;
   static int64_t write(const clap_ostream_t *s, const void *buf, uint64_t n) {
      auto *b = static_cast<Buffer *>(s->ctx);
      const auto *p = static_cast<const uint8_t *>(buf);
      b->data.insert(b->data.end(), p, p + n);
      return static_cast<int64_t>(n);
   }
   static int64_t read(const clap_istream_t *s, void *buf, uint64_t n) {
      auto *b = static_cast<Buffer *>(s->ctx);
      const size_t k = std::min<size_t>(n, b->data.size() - b->readPos);
      std::memcpy(buf, b->data.data() + b->readPos, k);
      b->readPos += k;
      return static_cast<int64_t>(k);
   }
};

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 128;

struct Run {
   const clap_plugin_t *plugin;
   std::vector<float> outL, outR;
   InEvents in;
   OutEvents out;
   int64_t steady = 0;

   explicit Run(const clap_plugin_t *p) : plugin(p), outL(kBlock), outR(kBlock) {}

   void note(uint16_t type, int16_t key) {
      clap_event_note_t ev{};
      ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
      ev.note_id = -1;
      ev.port_index = 0;
      ev.channel = 0;
      ev.key = key;
      ev.velocity = 0.8;
      in.notes.push_back(ev);
   }

   // One block; returns its peak, and flags non-finite output.
   float block(bool &finite) {
      float *outs[2] = {outL.data(), outR.data()};
      clap_audio_buffer_t aout{outs, nullptr, 2, 0, 0};
      clap_input_events_t ie{&in, InEvents::size, InEvents::get};
      clap_output_events_t oe{&out, OutEvents::push};
      clap_process_t pr{};
      pr.steady_time = steady;
      pr.frames_count = kBlock;
      pr.audio_outputs = &aout;
      pr.audio_outputs_count = 1;
      pr.in_events = &ie;
      pr.out_events = &oe;
      plugin->process(plugin, &pr);
      in.notes.clear();
      steady += kBlock;
      float peak = 0.0f;
      for (uint32_t i = 0; i < kBlock; ++i) {
         if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]))
            finite = false;
         peak = std::max({peak, std::fabs(outL[i]), std::fabs(outR[i])});
      }
      return peak;
   }

   float run(double seconds, bool &finite) {
      float peak = 0.0f;
      for (int b = 0; b < static_cast<int>(seconds * kRate / kBlock); ++b)
         peak = std::max(peak, block(finite));
      return peak;
   }
};

const clap_plugin_webview_t *webview = nullptr;
const clap_plugin_t *plugin = nullptr;

void toPlugin(const web::Writer &w) {
   webview->receive(plugin, w.bytes.data(), static_cast<uint32_t>(w.bytes.size()));
}

std::vector<uint8_t> lastAnswer() { return sentToPage.empty() ? std::vector<uint8_t>{} : sentToPage.back(); }

std::vector<uint8_t> poll(uint32_t ops, uint32_t serial) {
   web::Writer w(web::kPagePoll);
   w.put(uint32_t{77});
   w.put(ops);
   w.put(serial);
   sentToPage.clear();
   toPlugin(w);
   return lastAnswer();
}

template <typename T> T at(const std::vector<uint8_t> &b, size_t offset) {
   T v{};
   if (offset + sizeof(T) <= b.size())
      std::memcpy(&v, b.data() + offset, sizeof(T));
   return v;
}

std::vector<uint8_t> saveState(const clap_plugin_state_t *state, const clap_plugin_t *p) {
   Buffer b;
   clap_ostream_t os{&b, Buffer::write};
   state->save(p, &os);
   return b.data;
}

// Where pattern `p`'s steps start in a state blob: the header, then one
// (id, value) pair per parameter.
size_t patternOffset(int p) { return 12 + 12u * kNumParams + 4u * kMaxSteps * static_cast<size_t>(p); }

void sendPattern0(uint32_t step0) {
   uint32_t steps[kMaxSteps] = {};
   steps[0] = step0;
   web::Writer p(web::kPagePattern);
   p.put(uint32_t{0});
   p.putBytes(steps, sizeof(steps));
   toPlugin(p);
}

} // namespace

int main() {
   check(clap_entry.init(""), "clap_entry.init");
   const auto *factory = static_cast<const clap_plugin_factory_t *>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
   check(factory && factory->get_plugin_count(factory) == 1, "plugin factory with one plugin");
   if (!factory)
      return 1;
   const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
   std::printf("      %s %s (%s)\n", desc->name, desc->version, desc->id);
   plugin = factory->create_plugin(factory, &kHost, desc->id);
   check(plugin && plugin->init(plugin), "create + init");
   if (!plugin)
      return 1;

   const auto *gui = static_cast<const clap_plugin_gui_t *>(plugin->get_extension(plugin, CLAP_EXT_GUI));
   webview = static_cast<const clap_plugin_webview_t *>(plugin->get_extension(plugin, CLAP_EXT_WEBVIEW));
   check(gui && gui->is_api_supported(plugin, CLAP_WINDOW_API_WEBVIEW, false) &&
            !gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false),
         "gui: the webview API only");
   check(webview != nullptr, "clap.webview offered");
   if (!webview || !gui)
      return 1;
   char uri[64] = {};
   check(webview->get_uri(plugin, uri, sizeof(uri)) > 0 && !std::strcmp(uri, "/index.html"), "page /index.html");
   uint32_t gw = 0, gh = 0;
   check(gui->create(plugin, CLAP_WINDOW_API_WEBVIEW, false) && gui->get_size(plugin, &gw, &gh) && gw == 1572 &&
            gh == 898,
         "gui: 1572 x 898 at first");
   uint32_t aw = 1000, ah = 1000;
   gui->adjust_size(plugin, &aw, &ah);
   check(aw == 1000 && ah == 571, "gui: adjust_size keeps the layout's aspect");

   const auto *params = static_cast<const clap_plugin_params_t *>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   const auto *state = static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
   check(params && state && params->count(plugin) == kNumParams, "params and state");
   if (!params || !state)
      return 1;

   check(plugin->activate(plugin, kRate, 1, kBlock), "activate 48 kHz");
   check(plugin->start_processing(plugin), "start_processing");
   Run run(plugin);
   bool finite = true;

   // A kick from a MIDI key; keys always play.
   const auto t0 = std::chrono::steady_clock::now();
   run.note(CLAP_EVENT_NOTE_ON, static_cast<int16_t>(keyForVoice(kVoiceBD)));
   const float played = run.run(1.0, finite);
   run.note(CLAP_EVENT_NOTE_OFF, static_cast<int16_t>(keyForVoice(kVoiceBD)));
   run.run(0.3, finite);
   const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   check(finite && played > 1e-3f, "a kick sounds, finite");
   std::printf("      peak %.3f, %.1f us per %u-frame block (%.0fx realtime)\n", played,
               secs * 1e6 / (1.3 * kRate / kBlock), kBlock, 1.3 / secs);

   // ------------------------------------------------------------ polls
   auto first = poll(0, 0);
   const uint32_t serial = at<uint32_t>(first, 12);
   const size_t countAt = web::kStatusHeader - 4;
   check(first.size() == web::kStatusHeader + 8u * kNumParams && first[0] == web::kPluginStatus &&
            at<uint32_t>(first, 4) == 77 && at<uint32_t>(first, countAt) == kNumParams,
         "page: the first poll carries every value");
   auto second = poll(0, serial);
   check(second.size() == web::kStatusHeader && at<uint32_t>(second, countAt) == 0,
         "page: nothing new, header only");
   const uint32_t stateSerial = at<uint32_t>(first, 16);

   // ------------------------------------------------------------ a gesture
   const int marks = dirtyMarks;
   const double tuneValue = paramTable()[kParamBdTune].min +
                            0.25 * (paramTable()[kParamBdTune].max - paramTable()[kParamBdTune].min);
   run.out.clear();
   {
      web::Writer b(web::kPageBegin), s(web::kPageSet), e(web::kPageEnd);
      b.put(static_cast<uint32_t>(kParamBdTune));
      s.put(static_cast<uint32_t>(kParamBdTune));
      s.put(tuneValue);
      e.put(static_cast<uint32_t>(kParamBdTune));
      toPlugin(b);
      toPlugin(s);
      toPlugin(e);
   }
   double now = 0.0;
   params->get_value(plugin, kParamBdTune, &now);
   check(now == tuneValue, "page: an edit reaches the parameter");
   check(dirtyMarks == marks + 1, "page: a gesture is one mark_dirty");
   run.block(finite);
   const bool gestureOut = run.out.types.size() >= 3 && run.out.types[0] == CLAP_EVENT_PARAM_GESTURE_BEGIN &&
                           run.out.types[1] == CLAP_EVENT_PARAM_VALUE && run.out.values[1] == tuneValue &&
                           run.out.types[2] == CLAP_EVENT_PARAM_GESTURE_END &&
                           run.out.params[1] == kParamBdTune;
   check(gestureOut, "page: the gesture reaches the host as begin, value, end");
   auto third = poll(3, serial);
   check(at<uint32_t>(third, 8) == 3 && at<uint32_t>(third, countAt) == kNumParams &&
            at<double>(third, web::kStatusHeader + 8u * kParamBdTune) == tuneValue,
         "page: the next answer has the edit and the ops it saw");
   check(at<uint32_t>(third, 16) == stateSerial, "page: the page's own edit does not move the state serial");

   // ------------------------------------------------------------ the sequencer
   // Mode Sequencer, Pattern 1 a kick on step 1 and nothing else, PLAY from
   // the page with the host stopped: the note port says what it played. Then
   // the step becomes a snare, and the snare is what plays.
   {
      web::Writer w(web::kPageValues);
      w.put(uint32_t{1});
      w.put(static_cast<uint32_t>(kParamMode));
      w.put(static_cast<double>(kModeSequencer));
      toPlugin(w);
      sendPattern0(withLevel(0u, kVoiceBD, kLevelOn));
      toPlugin(web::Writer(web::kPageCommit));
      web::Writer play(web::kPageFreeRun);
      play.put(uint32_t{1});
      toPlugin(play);
   }
   check(dirtyMarks == marks + 2, "page: a commit is one mark_dirty");
   run.out.clear();
   const float seqPeak = run.run(0.5, finite);
   const bool kick = !run.out.noteKeys.empty() && run.out.noteKeys[0] == keyForVoice(kVoiceBD);
   auto playing = poll(12, 0);
   check(playing[60] == 1, "page: the status says PLAY is on");
   sendPattern0(withLevel(0u, kVoiceSD, kLevelOn));
   run.out.clear();
   run.run(2.5, finite);
   const bool snare = !run.out.noteKeys.empty() && run.out.noteKeys[0] == keyForVoice(kVoiceSD);
   {
      web::Writer stop(web::kPageFreeRun);
      stop.put(uint32_t{0});
      toPlugin(stop);
      run.run(0.2, finite);
   }
   check(kick && snare && seqPeak > 1e-3f, "page: a pattern edit is what the sequencer plays");
   std::printf("      sequencer played key %d, then key %d, peak %.3f\n",
               kick ? keyForVoice(kVoiceBD) : -1, snare ? keyForVoice(kVoiceSD) : -1, seqPeak);
   check(at<uint32_t>(poll(12, 0), web::kStatusFixed + 4u * kVoiceSD) > 0, "page: the snare's hits are counted");

   // ------------------------------------------------------------ the rest of the state
   {
      web::Writer chains(web::kPageChains);
      for (int p = 0; p < kMaxPatterns; ++p) {
         chains.put(static_cast<uint8_t>(p == 3 ? kChainNext : kChainStay));
         chains.put(uint8_t{0});
         chains.put(static_cast<uint16_t>(p == 3 ? 4 : 1));
      }
      toPlugin(chains);
      web::Writer map(web::kPageNoteMap);
      for (int n = 0; n < 128; ++n)
         map.put(static_cast<int8_t>(n == 60 ? 7 : kNoteNone));
      toPlugin(map);
      web::Writer adv(web::kPageAdvanced);
      adv.put(uint32_t{1});
      toPlugin(adv);
      web::Writer mutes(web::kPageMutes);
      for (int v = 0; v < kNumVoices; ++v)
         mutes.put(static_cast<uint8_t>(v == kVoiceSD ? 1 : 0));
      toPlugin(mutes);
      web::Writer memo(web::kPageMemo);
      memo.putBytes("memo!", 5);
      toPlugin(memo);
   }
   const std::vector<uint8_t> blob = saveState(state, plugin);
   const size_t mapAt = patternOffset(kMaxPatterns) + 1;
   const size_t chainsAt = mapAt + 128;
   const size_t mutesAt = chainsAt + 3 * kMaxPatterns;
   check(blob.size() >= mutesAt + kNumVoices &&
            stepLevel(at<uint32_t>(blob, patternOffset(0)), kVoiceSD) == kLevelOn,
         "state: the pattern edit is in it");
   check(blob[patternOffset(kMaxPatterns)] == 1, "state: the section is open");
   check(static_cast<int8_t>(blob[mapAt + 60]) == 7, "state: the pattern map");
   check(blob[chainsAt + 9] == kChainNext && at<uint16_t>(blob, chainsAt + 10) == 4, "state: the chains");
   check(blob[mutesAt + kVoiceSD] == 1 && blob[mutesAt + kVoiceBD] == 0, "state: the mutes");
   gui->destroy(plugin);
   check(gui->create(plugin, CLAP_WINDOW_API_WEBVIEW, false) && gui->get_size(plugin, &gw, &gh) && gh == 1034,
         "gui: with the section open the window opens at its taller size");

   // The editor's request for the whole state: the memo and the blob.
   {
      web::Writer w(web::kPageGetState);
      w.put(uint32_t{77});
      w.put(uint32_t{12});
      sentToPage.clear();
      toPlugin(w);
      const auto answer = lastAnswer();
      const uint32_t memoLen = at<uint32_t>(answer, 16);
      const uint32_t stateLen = at<uint32_t>(answer, 20 + memoLen);
      check(answer.size() > 24 && answer[0] == web::kPluginState && at<uint32_t>(answer, 8) == 12 && memoLen == 5 &&
               !std::memcmp(answer.data() + 20, "memo!", 5) && stateLen == blob.size() &&
               !std::memcmp(answer.data() + 24 + memoLen, blob.data(), blob.size()),
            "page: the state it asks for is the plugin's state and its memo");
   }

   // ------------------------------------------------------------ round trip
   const clap_plugin_t *other = factory->create_plugin(factory, &kHost, desc->id);
   other->init(other);
   const auto *otherState = static_cast<const clap_plugin_state_t *>(other->get_extension(other, CLAP_EXT_STATE));
   {
      Buffer b;
      b.data = blob;
      clap_istream_t is{&b, Buffer::read};
      check(otherState->load(other, &is), "state: loads into a second instance");
   }
   check(saveState(otherState, other) == blob, "state: the second instance saves the same bytes");
   other->destroy(other);

   // A load from the host is not the page's: the serial moves, and the page
   // then asks for the state.
   {
      Buffer b;
      b.data = blob;
      clap_istream_t is{&b, Buffer::read};
      state->load(plugin, &is);
      auto after = poll(12, 0);
      check(at<uint32_t>(after, 16) != stateSerial, "page: a state load from the host moves the state serial");
   }

   plugin->stop_processing(plugin);
   plugin->deactivate(plugin);
   plugin->destroy(plugin);
   clap_entry.deinit();

   std::printf("%s\n", failures ? "FAILED" : "all passed");
   return failures ? 1 : 0;
}
