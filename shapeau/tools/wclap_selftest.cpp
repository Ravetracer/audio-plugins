// Self-test of the WCLAP build: drives the plugin through clap_entry as a
// WCLAP host would, built as a WASI command from the same objects as
// module.wasm and run with
//
//   node ../shared/tools/run-wasi.mjs build-wasm/shapeau-wclap-selftest.wasm
//
// Checks the factory, the webview API, audio through a factory preset, and the
// page protocol (web/protocol.h): a first poll with every value, every shape
// and the view; a parameter gesture as one mark_dirty; a shape edit and a chain
// edit from the page reaching the audio, committed as one mark_dirty; the view;
// a preset from the page; the scope's bins sent once and then only when new;
// stale answers recognised by their edit count; and a state round trip that
// keeps the parameters and the shapes.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include "params.h"
#include "presetlibrary.h"
#include "shapeau.h"
#include "web/protocol.h"

extern "C" const clap_plugin_entry_t clap_entry;

using namespace shapeau;
using namespace shapeau::web;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
   std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok)
      ++failures;
}

// ----------------------------------------------------------------- host side

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

const void *hostGetExtension(const clap_host_t *, const char *id) {
   if (!std::strcmp(id, CLAP_EXT_WEBVIEW))
      return &kHostWebview;
   if (!std::strcmp(id, CLAP_EXT_STATE))
      return &kHostState;
   return nullptr;
}
void hostNoop(const clap_host_t *) {}

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "shapeau-wclap-selftest", "Ravetracer", "", "0",
                           hostGetExtension,  hostNoop, hostNoop, hostNoop};

uint32_t emptySize(const clap_input_events_t *) { return 0; }
const clap_event_header_t *emptyGet(const clap_input_events_t *, uint32_t) { return nullptr; }

std::vector<clap_event_param_value_t> paramOut;
bool pushOut(const clap_output_events_t *, const clap_event_header_t *ev) {
   if (ev->type == CLAP_EVENT_PARAM_VALUE)
      paramOut.push_back(*reinterpret_cast<const clap_event_param_value_t *>(ev));
   return true;
}

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
constexpr uint32_t kBlock = 128; // what openDAW hands a plugin at most

// A 200 Hz tone at 0.5 through the main input, silence in the sidechain; the
// peak of each block out.
struct Run {
   const clap_plugin_t *plugin;
   std::vector<float> inL, inR, sideL, sideR, outL, outR;
   uint64_t frame = 0;
   bool finite = true;

   explicit Run(const clap_plugin_t *p)
      : plugin(p), inL(kBlock), inR(kBlock), sideL(kBlock), sideR(kBlock), outL(kBlock),
        outR(kBlock) {}

   float block() {
      for (uint32_t i = 0; i < kBlock; ++i)
         inL[i] = inR[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 200.0 * (frame + i) / kRate));
      float *in[2] = {inL.data(), inR.data()};
      float *side[2] = {sideL.data(), sideR.data()};
      float *out[2] = {outL.data(), outR.data()};
      clap_audio_buffer_t ain[2] = {{in, nullptr, 2, 0, 0}, {side, nullptr, 2, 0, 0}};
      clap_audio_buffer_t aout{out, nullptr, 2, 0, 0};
      clap_input_events_t ie{nullptr, emptySize, emptyGet};
      clap_output_events_t oe{nullptr, pushOut};
      clap_event_transport_t transport{};
      transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
      transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                        CLAP_TRANSPORT_IS_PLAYING;
      transport.tempo = 120.0;
      transport.song_pos_beats =
         static_cast<clap_beattime>(std::llround(frame / kRate * 2.0 * CLAP_BEATTIME_FACTOR));
      clap_process_t pr{};
      pr.steady_time = static_cast<int64_t>(frame);
      pr.frames_count = kBlock;
      pr.transport = &transport;
      pr.audio_inputs = ain;
      pr.audio_outputs = &aout;
      pr.audio_inputs_count = 2;
      pr.audio_outputs_count = 1;
      pr.in_events = &ie;
      pr.out_events = &oe;
      plugin->process(plugin, &pr);
      frame += kBlock;
      float peak = 0.0f;
      for (uint32_t i = 0; i < kBlock; ++i) {
         if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]))
            finite = false;
         peak = std::max({peak, std::fabs(outL[i]), std::fabs(outR[i])});
      }
      return peak;
   }

   // The peak over `seconds`. With `poll`, called every 33 ms of audio, as
   // the page polls.
   template <typename F> float seconds(double s, F poll) {
      float peak = 0.0f;
      const int every = static_cast<int>(0.033 * kRate / kBlock);
      for (int b = 0; b < static_cast<int>(s * kRate / kBlock); ++b) {
         peak = std::max(peak, block());
         if (b % every == every - 1)
            poll();
      }
      return peak;
   }
   float seconds(double s) {
      return seconds(s, [] {});
   }
};

// ------------------------------------------------------------------ the page

struct Status {
   bool ok = false;
   uint8_t flags = 0;
   uint32_t valueSerial = 0, curveSerial = 0, editCount = 0, scopeGen = 0, blocks = 0, runs = 0;
   float playhead = -1.0f;
   std::vector<double> values;
   int module = -1, band = -1;
   uint8_t view = 0;
   CurveSet curves;
   int bins = 0;           // scope bins carried
   float inPeak = 0.0f;    // the loudest input bin among them
   size_t bytes = 0;
};

Status parseStatus(const std::vector<uint8_t> &m) {
   Status s;
   s.bytes = m.size();
   if (m.size() < kStatusHeader || m[0] != PluginStatus)
      return s;
   Reader in{m.data(), m.size()};
   in.pos = 1;
   s.flags = in.u8();
   const uint16_t count = in.u16();
   s.valueSerial = in.u32();
   s.curveSerial = in.u32();
   s.editCount = in.u32();
   s.scopeGen = in.u32();
   s.blocks = in.u32();
   in.u32(); // triggers
   s.playhead = in.f32();
   s.runs = in.u32();
   if (s.flags & kStatusValues)
      for (uint16_t i = 0; i < count; ++i)
         s.values.push_back(in.f64());
   if (s.flags & kStatusCurves) {
      resetCurves(s.curves);
      s.module = in.u8();
      s.band = in.u8();
      s.view = in.u8();
      getChain(in, s.curves);
      for (int i = 0; i < kNumModules * kNumBands; ++i)
         if (!getCurve(in, s.curves))
            return s;
   }
   for (uint32_t r = 0; r < s.runs; ++r) {
      const int first = in.u16();
      const int bins = in.u16();
      (void)first;
      for (int k = 0; k < bins; ++k) {
         int16_t v[kScopeFields];
         for (int f = 0; f < kScopeFields; ++f)
            v[f] = static_cast<int16_t>(in.u16());
         s.inPeak = std::max({s.inPeak, std::fabs(scopeDecode(v[0], false)),
                              std::fabs(scopeDecode(v[1], false))});
      }
      s.bins += bins;
   }
   s.ok = in.ok && in.pos == m.size();
   return s;
}

struct Page {
   const clap_plugin_t *plugin;
   const clap_plugin_webview_t *webview;
   uint32_t valueSerial = 0, curveSerial = 0, scopeGen = 0, editCount = 0;

   void send(const std::vector<uint8_t> &m, bool edit = true) {
      if (edit)
         ++editCount;
      webview->receive(plugin, m.data(), static_cast<uint32_t>(m.size()));
   }

   // A poll; the serials are taken from the answer like the editor does.
   Status poll(bool take = true) {
      std::vector<uint8_t> m = {PagePoll, 0, 0, 0};
      put32(m, valueSerial);
      put32(m, curveSerial);
      put32(m, scopeGen);
      put32(m, editCount);
      sentToPage.clear();
      send(m, false);
      Status s = sentToPage.size() == 1 ? parseStatus(sentToPage.back()) : Status{};
      if (take && s.ok && s.editCount == editCount) {
         valueSerial = s.valueSerial;
         curveSerial = s.curveSerial;
         scopeGen = s.scopeGen;
      }
      return s;
   }

   void param(uint8_t type, uint32_t id, double value = 0.0) {
      std::vector<uint8_t> m = {type, 0, 0, 0};
      put32(m, id);
      if (type == PageSet)
         putF64(m, value);
      send(m);
   }
};

bool sameCurve(const Curve &a, const Curve &b) {
   if (a.count() != b.count())
      return false;
   for (int i = 0; i < a.count(); ++i) {
      const CurvePoint &p = a.point(i), &q = b.point(i);
      if (std::fabs(p.x - q.x) > 1e-4f || std::fabs(p.y - q.y) > 1e-4f || p.weight != q.weight ||
          std::fabs(p.bend - q.bend) > 1e-3f)
         return false;
   }
   return true;
}

} // namespace

int main() {
   check(clap_entry.init(""), "clap_entry.init");
   const auto *factory =
      static_cast<const clap_plugin_factory_t *>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
   check(factory && factory->get_plugin_count(factory) == 1, "plugin factory with one plugin");
   if (!factory)
      return 1;
   const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
   std::printf("      %s %s (%s)\n", desc->name, desc->version, desc->id);

   const clap_plugin_t *plugin = factory->create_plugin(factory, &kHost, desc->id);
   check(plugin && plugin->init(plugin), "create + init");
   if (!plugin)
      return 1;
   const auto *gui = static_cast<const clap_plugin_gui_t *>(plugin->get_extension(plugin, CLAP_EXT_GUI));
   const auto *webview =
      static_cast<const clap_plugin_webview_t *>(plugin->get_extension(plugin, CLAP_EXT_WEBVIEW));
   const auto *params =
      static_cast<const clap_plugin_params_t *>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   const auto *state =
      static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
   const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(
      plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
   check(gui && gui->is_api_supported(plugin, CLAP_WINDOW_API_WEBVIEW, false) &&
            !gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false),
         "gui: the webview API only");
   check(webview && params && state && ports, "webview, params, state, audio-ports");
   if (!gui || !webview || !params || !state || !ports)
      return 1;
   {
      char uri[64] = {};
      check(webview->get_uri(plugin, uri, sizeof(uri)) > 0 && !std::strcmp(uri, "/index.html"),
            "page /index.html");
      uint32_t w = 0, h = 0;
      gui->get_size(plugin, &w, &h);
      uint32_t aw = 3000, ah = 1000;
      gui->adjust_size(plugin, &aw, &ah);
      std::printf("      window %ux%u; 3000x1000 adjusts to %ux%u\n", w, h, aw, ah);
      check(w == kDesignW && h == kDesignH && ah == 1000 && std::abs(int(aw) - 1657) <= 1,
            "window size and aspect");
      check(ports->count(plugin, true) == 2 && params->count(plugin) == kNumParams,
            "two inputs (main, sidechain), every parameter");
   }

   check(plugin->activate(plugin, kRate, 1, kBlock), "activate 48 kHz");
   check(plugin->start_processing(plugin), "start_processing");
   Run run(plugin);
   Page page{plugin, webview};

   // ------------------------------------------------------- the first poll
   Status first = page.poll();
   std::printf("      first answer: %zu bytes\n", first.bytes);
   check(first.ok && (first.flags & kStatusValues) && (first.flags & kStatusCurves) &&
            first.values.size() == kNumParams,
         "page: the first poll carries values, shapes and view");
   check(first.ok && first.module == 0 && first.band == 1 &&
            first.view == (kViewAllBands | kViewSnap),
         "page: the default view (volume, mid, ALL, snap)");
   bool neutral = first.ok;
   for (int m = 0; m < kNumModules; ++m)
      for (int b = 0; b < kNumBands; ++b)
         neutral = neutral && first.curves.curve[m][b].count() == 2 &&
                   std::fabs(first.curves.curve[m][b].point(0).y - neutralValue(m)) < 1e-6f;
   check(neutral, "page: every shape neutral");
   Status idle = page.poll();
   check(idle.ok && idle.flags == 0 && idle.bytes == kStatusHeader,
         "page: nothing new, a 36 byte answer");

   const float dry = run.seconds(0.5);
   std::printf("      dry peak %.3f\n", dry);
   check(run.finite && std::fabs(dry - 0.5f) < 0.02f, "a fresh plugin passes the tone");

   // ------------------------------------------------- a parameter gesture
   int marks = dirtyMarks;
   paramOut.clear();
   page.param(PageBegin, kParamVolTrim);
   page.param(PageSet, kParamVolTrim, -6.0);
   page.param(PageSet, kParamVolTrim, -12.0);
   check(dirtyMarks == marks, "page: no mark_dirty inside a gesture");
   page.param(PageEnd, kParamVolTrim);
   double trim = 0.0;
   params->get_value(plugin, kParamVolTrim, &trim);
   check(trim == -12.0, "page: an edit reaches the parameter");
   check(dirtyMarks == marks + 1, "page: a gesture is one mark_dirty");
   run.block();
   check(paramOut.size() == 2 && paramOut.back().value == -12.0,
         "page: the edits go to the host as parameter events");
   Status afterTrim = page.poll();
   check(afterTrim.ok && (afterTrim.flags & kStatusValues) && afterTrim.values[kParamVolTrim] == -12.0,
         "page: the next answer has the edit");

   // ------------------------------------------------------- a shape edit
   // A volume shape that is silent for the first half of the cycle, drawn in
   // ALL (three curve records) with the stage switched on, as the editor's
   // CurveModel would send it, then the end of the gesture.
   marks = dirtyMarks;
   Curve duck;
   duck.setFlat(1.0f);
   duck.movePoint(0, 0.0f, 0.0f);
   duck.addPoint(0.5f, 0.0f, 0);
   duck.addPoint(0.505f, 1.0f, 0);
   std::vector<uint8_t> edit = {PageCurves, 3, 0, 0};
   for (int b = 0; b < kNumBands; ++b)
      putCurve(edit, kModuleVolume, b, duck);
   page.send(edit);
   page.param(PageSet, kParamVolOn, 1.0);
   page.param(PageSet, kParamVolTrim, 0.0);
   Status stale = page.poll(false);
   check(stale.ok && stale.editCount == page.editCount, "page: the answer echoes the edit count");
   check(dirtyMarks == marks, "page: no mark_dirty before the commit");
   page.send({PageCommit, 0, 0, 0});
   check(dirtyMarks == marks + 1, "page: a shape drag is one mark_dirty at its commit");
   page.send({PageCommit, 0, 0, 0});
   check(dirtyMarks == marks + 1, "page: a commit with nothing new marks nothing");

   // At 120 BPM a quarter note is 0.5 s: 24000 samples silent, 24000 open.
   float quiet = 0.0f, loud = 0.0f;
   run.seconds(0.5); // let Smooth settle
   for (int b = 0; b < static_cast<int>(2.0 * kRate / kBlock); ++b) {
      const double phase = std::fmod((run.frame + kBlock / 2) / (0.5 * kRate), 1.0);
      const float peak = run.block();
      if (phase > 0.1 && phase < 0.4)
         quiet = std::max(quiet, peak);
      if (phase > 0.6 && phase < 0.9)
         loud = std::max(loud, peak);
   }
   std::printf("      first half %.4f, second half %.3f\n", quiet, loud);
   check(quiet < 0.01f && loud > 0.4f, "page: the drawn shape reaches the audio");
   Status afterShape = page.poll();
   check(afterShape.ok && (afterShape.flags & kStatusCurves) &&
            sameCurve(afterShape.curves.curve[kModuleVolume][0], duck) &&
            sameCurve(afterShape.curves.curve[kModuleVolume][2], duck),
         "page: the shape comes back exactly in the next answer");

   // ----------------------------------------------------------- the scope
   // Bins are written only while a page polls (the second above stopped it):
   // a page that has seen none is sent every bin that holds audio, and after
   // that only the ones that changed.
   page.poll();
   run.seconds(0.6, [&] { page.poll(); });
   page.scopeGen = 0;
   Status scope1 = page.poll();
   std::printf("      scope: %d bins in %u runs, %zu bytes; input peak %.3f\n", scope1.bins,
               scope1.runs, scope1.bytes, scope1.inPeak);
   check(scope1.ok && scope1.bins > 400 && scope1.inPeak > 0.45f && scope1.inPeak < 0.55f,
         "page: the scope shows the input");
   Status scope2 = page.poll();
   check(scope2.ok && scope2.bins == 0, "page: no new audio, no bins");
   run.seconds(0.05);
   Status scope3 = page.poll();
   std::printf("      after 50 ms of audio: %d bins, %zu bytes\n", scope3.bins, scope3.bytes);
   check(scope3.ok && scope3.bins > 0 && scope3.bins < 60, "page: then only the bins that moved");
   check(scope3.playhead >= 0.0f && scope3.playhead <= 1.0f, "page: the playhead");

   // ------------------------------------------------- chain and the view
   std::vector<uint8_t> chain = {PageChain, 0, 0, 0};
   {
      CurveSet set;
      chainAdd(set, kModuleFilter);
      chainAdd(set, kModuleVolume);
      putChain(chain, set);
   }
   page.send(chain);
   page.send({PageView, kModuleFilter, kBandHigh, kViewStepMode});
   Status afterView = page.poll();
   check(afterView.ok && afterView.curves.chainCount == 2 &&
            afterView.curves.chain[0] == kModuleFilter && afterView.curves.chain[1] == kModuleVolume,
         "page: the chain");
   check(afterView.module == kModuleFilter && afterView.band == kBandHigh &&
            afterView.view == kViewStepMode,
         "page: the view");

   // ------------------------------------------------ state round trip
   Buffer saved;
   clap_ostream_t os{&saved, Buffer::write};
   check(state->save(plugin, &os) && !saved.data.empty(), "state save");
   const clap_plugin_t *second = factory->create_plugin(factory, &kHost, desc->id);
   second->init(second);
   const auto *state2 =
      static_cast<const clap_plugin_state_t *>(second->get_extension(second, CLAP_EXT_STATE));
   const auto *webview2 =
      static_cast<const clap_plugin_webview_t *>(second->get_extension(second, CLAP_EXT_WEBVIEW));
   clap_istream_t is{&saved, Buffer::read};
   check(state2->load(second, &is), "state load into a second instance");
   Page page2{second, webview2};
   Status restored = page2.poll();
   Status original = Page{plugin, webview}.poll();
   bool sameValues = restored.values == original.values && restored.values.size() == kNumParams;
   bool sameShapes = restored.ok;
   for (int m = 0; m < kNumModules; ++m)
      for (int b = 0; b < kNumBands; ++b)
         sameShapes = sameShapes && sameCurve(restored.curves.curve[m][b], original.curves.curve[m][b]);
   check(sameValues, "state: every parameter survives");
   check(sameShapes && restored.curves.chainCount == 2, "state: the shapes and the chain survive");
   check(restored.module == kModuleFilter && restored.band == kBandHigh &&
            restored.view == kViewStepMode,
         "state: the view survives");
   second->destroy(second);

   // ---------------------------------------------- a preset from the page
   const char *text = factoryPresetText("kick_duck");
   check(text != nullptr, "the Kick Duck factory preset");
   if (text) {
      marks = dirtyMarks;
      std::vector<uint8_t> m = {PagePreset, 0, 0, 0};
      put32(m, static_cast<uint32_t>(std::strlen(text)));
      m.insert(m.end(), text, text + std::strlen(text));
      page.send(m);
      Status afterPreset = page.poll();
      PresetData data;
      CurveSet curves;
      std::string error;
      parsePreset(text, std::strlen(text), data, curves, error);
      bool matches = afterPreset.ok && afterPreset.values.size() == kNumParams;
      for (const auto &kv : data.values)
         matches = matches && std::fabs(afterPreset.values[kv.first] - kv.second) < 1e-9;
      for (int mod = 0; mod < kNumModules; ++mod)
         for (int b = 0; b < kNumBands; ++b)
            matches = matches && sameCurve(afterPreset.curves.curve[mod][b], curves.curve[mod][b]);
      check(matches, "page: a preset's values and shapes arrive");
      check(dirtyMarks == marks + 1, "page: a preset is one mark_dirty");
      const float out = run.seconds(1.0);
      check(run.finite && out > 0.0f, "Kick Duck plays, finite");
   }

   // ------------------------------------------------------------- timing
   // Every factory preset, loaded from the page, 3 s of the tone each.
   {
      double worst = 0.0, total = 0.0;
      int presets = 0;
      for (const GuiPreset &entry : presetLibrary()) {
         const char *factory = factoryPresetText(entry.loadKey);
         if (!factory)
            continue;
         std::vector<uint8_t> m = {PagePreset, 0, 0, 0};
         put32(m, static_cast<uint32_t>(std::strlen(factory)));
         m.insert(m.end(), factory, factory + std::strlen(factory));
         page.send(m);
         run.seconds(0.2);
         const auto t0 = std::chrono::steady_clock::now();
         const int blocks = static_cast<int>(3.0 * kRate / kBlock);
         for (int b = 0; b < blocks; ++b)
            run.block();
         const double us = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() /
                           blocks * 1e6;
         std::printf("      %-16s %5.1f us per %u-frame block\n", entry.name.c_str(), us, kBlock);
         worst = std::max(worst, us);
         total += us;
         ++presets;
      }
      std::printf("      mean %.1f us, worst %.1f us (a block is %.0f us of audio)\n",
                  presets ? total / presets : 0.0, worst, kBlock / kRate * 1e6);
      check(presets == 10 && run.finite, "every factory preset plays, finite");
   }

   plugin->stop_processing(plugin);
   plugin->deactivate(plugin);
   plugin->destroy(plugin);
   clap_entry.deinit();

   std::printf("%s\n", failures ? "FAILED" : "all passed");
   return failures ? 1 : 0;
}
