// The WCLAP's editor (ui.wasm): Shapeau's own window -- the fork in
// gui/window.cpp, the shape editor, the panels and the preset browser --
// running in the plugin's page and drawn into a canvas (shared/web/wclap-ui.js).
// The host runs the plugin (module.wasm) in its audio thread, where nothing
// can be drawn, so the two are separate modules and talk only through the
// webview messages in web/protocol.h.
//
// The plugin is represented by Remote: a CurveModel and a GuiDelegate like the
// plugin itself, holding a mirror of the plugin's parameters, shapes, chain
// and view, and the scope's bins as the plugin last sent them. The editing
// rules are CurveModel's, the same code the plugin runs for its native
// window; what Remote adds is that every change is also sent to the plugin.
// User presets live in the page's own file system, which the page keeps.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gui/curvemodel.h"
#include "gui/gui.h"
#include "params.h"
#include "presetlibrary.h"
#include "shapeau.h"
#include "web/protocol.h"

extern "C" {
// Posts bytes to the plugin (window.parent.postMessage in the page).
__attribute__((import_module("wclap"), import_name("send"))) void wclapJsSend(const void *data,
                                                                            int size);
}

namespace shapeau::web {

namespace {

double nowMs() {
   static const auto start = std::chrono::steady_clock::now();
   return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

bool samePoints(const Curve &a, const Curve &b) {
   if (a.count() != b.count())
      return false;
   for (int i = 0; i < a.count(); ++i) {
      const CurvePoint &p = a.point(i), &q = b.point(i);
      if (p.x != q.x || p.y != q.y || p.weight != q.weight || p.bend != q.bend)
         return false;
   }
   return true;
}

bool sameChain(const CurveSet &a, const CurveSet &b) {
   if (a.chainCount != b.chainCount)
      return false;
   for (int i = 0; i < a.chainCount; ++i)
      if (a.chain[i] != b.chain[i])
         return false;
   return true;
}

class Remote final : public GuiDelegate, public CurveModel {
public:
   Remote() {
      for (uint32_t i = 0; i < kNumParams; ++i)
         mValues[i] = paramTable()[i].def;
      resetCurves(mCurves);
      mSent = mCurves;
      mPresets = presetLibrary();
   }

   // ------------------------------------------------------------ GuiDelegate

   double guiParamValue(uint32_t id) const override { return id < kNumParams ? mValues[id] : 0.0; }

   void guiBeginEdit(uint32_t id) override {
      if (id < kNumParams)
         sendId(PageBegin, id);
   }

   void guiSetParam(uint32_t id, double value) override {
      const ParamDesc *d = paramById(id);
      if (!d)
         return;
      value = std::min(d->max, std::max(d->min, value));
      mValues[id] = value;
      mPresetEdited = true;
      std::vector<uint8_t> m = header(PageSet);
      put32(m, id);
      putF64(m, value);
      send(m);
   }

   void guiEndEdit(uint32_t id) override {
      if (id < kNumParams)
         sendId(PageEnd, id);
   }

   void guiOutputPeaks(float &left, float &right) const override { left = right = 0.0f; }
   uint32_t guiVoiceCount() const override { return 0; }
   uint32_t guiVoiceLimit() const override { return 0; }

   const std::vector<GuiPreset> &guiPresets() const override { return mPresets; }
   int guiCurrentPreset() const override { return mCurrentPreset; }
   bool guiPresetEdited() const override { return mPresetEdited; }

   std::string guiSuggestedPresetName() const override {
      if (mCurrentPreset >= 0 && mCurrentPreset < static_cast<int>(mPresets.size()))
         return mPresets[static_cast<size_t>(mCurrentPreset)].name;
      return "New Shape";
   }

   // As the plugin does it, into the page's file system.
   bool guiSavePreset(const std::string &input, std::string &error) override {
      std::string folder, name;
      const size_t cut = input.find_last_of("/\\");
      folder = cut == std::string::npos ? std::string() : input.substr(0, cut);
      name = cut == std::string::npos ? input : input.substr(cut + 1);
      auto trim = [](std::string &s) {
         while (!s.empty() && s.front() == ' ')
            s.erase(s.begin());
         while (!s.empty() && s.back() == ' ')
            s.pop_back();
      };
      trim(folder);
      trim(name);
      if (name.empty()) {
         error = "Give the preset a name after the folder.";
         return false;
      }
      const std::string path = plugincore::userPresetPathIn(presetContext(), folder, name);
      if (path.empty()) {
         error = "No user preset directory.";
         return false;
      }
      PresetData data;
      data.name = name;
      for (uint32_t i = 0; i < kNumParams; ++i)
         data.values.emplace_back(paramTable()[i].id, mValues[i]);
      if (!writePresetFile(path, formatPreset(data, mCurves), error))
         return false;
      mPresets = presetLibrary();
      mCurrentPreset = -1;
      for (size_t i = 0; i < mPresets.size(); ++i)
         if (mPresets[i].path == path)
            mCurrentPreset = static_cast<int>(i);
      mPresetEdited = false;
      return true;
   }

   // The preset is read here, applied to the mirror at once, and its text sent
   // to the plugin, which parses and applies the same file.
   void guiLoadPreset(int index) override {
      if (index < 0 || index >= static_cast<int>(mPresets.size()))
         return;
      const GuiPreset &entry = mPresets[static_cast<size_t>(index)];
      std::string text;
      if (!entry.loadKey.empty()) {
         if (const char *factory = factoryPresetText(entry.loadKey))
            text = factory;
      } else {
         std::ifstream file(entry.path, std::ios::binary);
         std::ostringstream buf;
         buf << file.rdbuf();
         text = buf.str();
      }
      PresetData data;
      CurveSet curves;
      std::string error;
      if (text.empty() || !parsePreset(text.c_str(), text.size(), data, curves, error))
         return;
      flushCurves();
      for (uint32_t i = 0; i < kNumParams; ++i)
         mValues[i] = paramTable()[i].def;
      for (const auto &kv : data.values)
         if (const ParamDesc *d = paramById(kv.first))
            mValues[kv.first] = std::min(d->max, std::max(d->min, kv.second));
      mCurves = curves;
      mSent = curves;
      std::vector<uint8_t> m = header(PagePreset);
      put32(m, static_cast<uint32_t>(text.size()));
      m.insert(m.end(), text.begin(), text.end());
      send(m);
      mCurrentPreset = index;
      mPresetEdited = false;
   }

   // ------------------------------------------------------------- CurveModel

   double modelRealValue(uint32_t id) const override {
      return id < kNumParams ? paramToReal(paramTable()[id], mValues[id]) : 0.0;
   }
   void modelBeginEdit(uint32_t id) override { guiBeginEdit(id); }
   void modelSetParam(uint32_t id, double raw) override { guiSetParam(id, raw); }
   void modelEndEdit(uint32_t id) override { guiEndEdit(id); }
   void modelCurvesChanged() override {
      mCurvesDirty = true;
      mPresetEdited = true;
   }
   // The view is sent only once the plugin has told the editor its own: the
   // plugin keeps it in its state, and a page that has just opened must not
   // overwrite it with the defaults.
   void modelViewChanged() override {
      if (!mHeard)
         return;
      std::vector<uint8_t> m;
      put8(m, PageView);
      put8(m, static_cast<uint8_t>(mCurveModule.load()));
      put8(m, static_cast<uint8_t>(mCurveBand.load()));
      put8(m, viewFlags());
      send(m);
   }

   const Scope *curveScope() const override { return &mScope; }

   // --------------------------------------------------------------- traffic

   // The shapes and the chain that differ from what the plugin was last sent
   // or last sent us. Once per frame while something is dragged, and at the
   // end of every gesture.
   void flushCurves() {
      if (!mCurvesDirty)
         return;
      mCurvesDirty = false;
      std::vector<uint8_t> m = header(PageCurves);
      int count = 0;
      for (int mod = 0; mod < kNumModules; ++mod)
         for (int b = 0; b < kNumBands; ++b)
            if (!samePoints(mCurves.curve[mod][b], mSent.curve[mod][b])) {
               putCurve(m, mod, b, mCurves.curve[mod][b]);
               ++count;
            }
      if (count > 0) {
         m[1] = static_cast<uint8_t>(count);
         send(m);
      }
      if (!sameChain(mCurves, mSent)) {
         std::vector<uint8_t> c = header(PageChain);
         putChain(c, mCurves);
         send(c);
      }
      mSent = mCurves;
   }

   // The end of a gesture: one undo step in the host for what it changed.
   void commit() {
      flushCurves();
      if (mSinceCommit)
         send(header(PageCommit));
      mSinceCommit = false;
   }

   void poll() {
      flushCurves();
      std::vector<uint8_t> m = header(PagePoll);
      put32(m, mValueSerial);
      put32(m, mCurveSerial);
      put32(m, mScopeGen);
      put32(m, mEditCount);
      wclapJsSend(m.data(), static_cast<int>(m.size()));
   }

   // The plugin's answer to a poll.
   void status(const uint8_t *data, size_t size) {
      if (size < kStatusHeader || data[0] != PluginStatus)
         return;
      Reader in{data, size};
      in.pos = 1;
      const uint8_t flags = in.u8();
      const uint16_t count = in.u16();
      const uint32_t valueSerial = in.u32();
      const uint32_t curveSerial = in.u32();
      const uint32_t editCount = in.u32();
      const uint32_t scopeGen = in.u32();
      const uint32_t blocks = in.u32();
      const uint32_t triggers = in.u32();
      const float playhead = in.f32();
      const uint32_t runs = in.u32();
      // Written before the page's latest edits reached the plugin: the mirror
      // is newer, and the values and shapes come again with the next poll.
      const bool current = editCount == mEditCount && !mCurvesDirty;

      if (flags & kStatusValues) {
         if (!in.need(8u * count))
            return;
         if (current && count == kNumParams) {
            for (uint32_t i = 0; i < kNumParams; ++i)
               mValues[i] = in.f64();
            mValueSerial = valueSerial;
         } else {
            in.pos += 8u * count;
         }
      }
      if (flags & kStatusCurves) {
         CurveSet set;
         resetCurves(set);
         const int module = in.u8();
         const int band = in.u8();
         const uint8_t view = in.u8();
         getChain(in, set);
         for (int i = 0; i < kNumModules * kNumBands; ++i)
            if (!getCurve(in, set))
               return;
         if (!in.ok)
            return;
         if (current) {
            mCurves = set;
            mSent = set;
            mCurveModule.store(std::clamp(module, 0, kNumModules - 1));
            mCurveBand.store(std::clamp(band, 0, kNumBands - 1));
            mEditAllBands.store((view & kViewAllBands) != 0);
            mSnap.store((view & kViewSnap) != 0);
            mStepMode.store((view & kViewStepMode) != 0);
            mCurveSerial = curveSerial;
            mHeard = true;
         }
      }
      for (uint32_t r = 0; r < runs; ++r) {
         const int first = in.u16();
         const int bins = in.u16();
         if (!in.need(2u * kScopeFields * static_cast<size_t>(bins)) || first + bins > Scope::kBins)
            return;
         for (int k = first; k < first + bins; ++k) {
            int16_t v[kScopeFields];
            for (int f = 0; f < kScopeFields; ++f)
               v[f] = static_cast<int16_t>(in.u16());
            mScope.inLo[k].store(scopeDecode(v[0], false), std::memory_order_relaxed);
            mScope.inHi[k].store(scopeDecode(v[1], false), std::memory_order_relaxed);
            mScope.outLo[k].store(scopeDecode(v[2], false), std::memory_order_relaxed);
            mScope.outHi[k].store(scopeDecode(v[3], false), std::memory_order_relaxed);
            mScope.sideLo[k].store(scopeDecode(v[4], false), std::memory_order_relaxed);
            mScope.sideHi[k].store(scopeDecode(v[5], false), std::memory_order_relaxed);
            mScope.trace[k].store(scopeDecode(v[6], true), std::memory_order_relaxed);
         }
      }
      mScopeGen = scopeGen;
      mScope.playhead.store(playhead, std::memory_order_relaxed);
      mScope.triggers.store(triggers, std::memory_order_relaxed);
      // The window repaints when this moves; new bins move it too.
      mScope.blocks.store(blocks + mScopeGen, std::memory_order_relaxed);
   }

private:
   uint8_t viewFlags() const {
      return static_cast<uint8_t>((mEditAllBands.load() ? kViewAllBands : 0) |
                                  (mSnap.load() ? kViewSnap : 0) |
                                  (mStepMode.load() ? kViewStepMode : 0));
   }

   static std::vector<uint8_t> header(uint8_t type) { return {type, 0, 0, 0}; }

   void sendId(uint8_t type, uint32_t id) {
      std::vector<uint8_t> m = header(type);
      put32(m, id);
      send(m);
   }

   // Every message but a poll is an edit: counted, so the answers written
   // before it reached the plugin can be told apart.
   void send(const std::vector<uint8_t> &m) {
      ++mEditCount;
      mSinceCommit = true;
      wclapJsSend(m.data(), static_cast<int>(m.size()));
   }

   double mValues[kNumParams] = {};
   CurveSet mSent;          // the shapes and chain as the plugin has them
   bool mCurvesDirty = false;
   bool mSinceCommit = false;
   bool mHeard = false;
   uint32_t mValueSerial = 0, mCurveSerial = 0, mScopeGen = 0, mEditCount = 0;
   Scope mScope;

   std::vector<GuiPreset> mPresets;
   int mCurrentPreset = -1;
   bool mPresetEdited = false;
};

struct Ui {
   Remote remote;
   WebGui *window = nullptr;
   int cssW = 0, cssH = 0;
   double dpr = 1.0;
   double lastPoll = -1.0e9;
   uint32_t frameSerial = 0;
};

Ui *ui = nullptr;
std::vector<uint8_t> scratch;

// The editor fills the page at the largest zoom that shows all of it.
void layout() {
   const double fit = std::min(ui->cssW / static_cast<double>(kDesignW),
                               ui->cssH / static_cast<double>(kDesignH));
   ui->window->webSetFrame(static_cast<int>(std::lround(ui->cssW * ui->dpr)),
                           static_cast<int>(std::lround(ui->cssH * ui->dpr)), fit * ui->dpr);
}

} // namespace

} // namespace shapeau::web

using namespace shapeau;
using shapeau::web::ui;

extern "C" {

// A buffer the page writes into before a call (messages, key text).
__attribute__((export_name("wclap_ui_buffer"))) uint8_t *wclapUiBuffer(int size) {
   web::scratch.resize(static_cast<size_t>(std::max(size, 1)));
   return web::scratch.data();
}

__attribute__((export_name("wclap_ui_start"))) void wclapUiStart(int cssW, int cssH, double dpr) {
   if (ui)
      return;
   ui = new web::Ui;
   ui->cssW = cssW;
   ui->cssH = cssH;
   ui->dpr = dpr > 0.25 ? dpr : 1.0;
   ui->window = createWebGui(ui->remote, ui->remote);
   ui->window->open();
   {
      uint32_t w = 0, h = 0;
      ui->window->designSize(&w, &h);
      if (w != web::kDesignW || h != web::kDesignH)
         std::fprintf(stderr, "Shapeau: the window is %ux%u, web/protocol.h says %ux%u\n", w, h,
                      web::kDesignW, web::kDesignH);
   }
   web::layout();
   ui->remote.poll();
}

__attribute__((export_name("wclap_ui_resize"))) void wclapUiResize(int cssW, int cssH, double dpr) {
   if (!ui)
      return;
   ui->cssW = cssW;
   ui->cssH = cssH;
   ui->dpr = dpr > 0.25 ? dpr : 1.0;
   web::layout();
}

// Once per animation frame: polls the plugin at 30 Hz, sends what a drag
// changed, and paints. Returns the serial of the newest frame.
__attribute__((export_name("wclap_ui_tick"))) uint32_t wclapUiTick() {
   if (!ui)
      return 0;
   const double now = web::nowMs();
   if (now - ui->lastPoll >= 33.0) {
      ui->lastPoll = now;
      ui->remote.poll();
   }
   ui->remote.flushCurves();
   if (ui->window->webRender())
      ++ui->frameSerial;
   return ui->frameSerial;
}

__attribute__((export_name("wclap_ui_frame"))) const uint8_t *wclapUiFrame() {
   return ui ? ui->window->webFrame() : nullptr;
}
__attribute__((export_name("wclap_ui_frame_width"))) int wclapUiFrameWidth() {
   return ui ? ui->window->webFrameWidth() : 0;
}
__attribute__((export_name("wclap_ui_frame_height"))) int wclapUiFrameHeight() {
   return ui ? ui->window->webFrameHeight() : 0;
}

// Pointer input in canvas pixels; buttons 1 left, 2 middle, 3 right.
__attribute__((export_name("wclap_ui_pointer_down"))) void
wclapUiPointerDown(float x, float y, int button, unsigned mods, int) {
   if (ui && button > 0)
      ui->window->webPointerDown(x, y, static_cast<unsigned>(button), mods, web::nowMs());
}
__attribute__((export_name("wclap_ui_pointer_up"))) void wclapUiPointerUp(float x, float y,
                                                                           int button,
                                                                           unsigned mods) {
   if (!ui)
      return;
   ui->window->webPointerUp(x, y, static_cast<unsigned>(button), mods);
   ui->remote.commit();
}
__attribute__((export_name("wclap_ui_pointer_move"))) void wclapUiPointerMove(float x, float y,
                                                                               unsigned mods) {
   if (ui)
      ui->window->webPointerMove(x, y, mods);
}
__attribute__((export_name("wclap_ui_wheel"))) void wclapUiWheel(float x, float y, float notches,
                                                                 unsigned mods) {
   if (!ui)
      return;
   ui->window->webWheel(x, y, notches, mods, web::nowMs());
   ui->remote.commit();
}
__attribute__((export_name("wclap_ui_leave"))) void wclapUiLeave() {
   if (ui)
      ui->window->webLeave();
}
// The key's text (UTF-8) is in the buffer, `textLen` bytes. Returns 1 when used.
__attribute__((export_name("wclap_ui_key"))) int wclapUiKey(unsigned keysym, int textLen,
                                                            unsigned mods) {
   if (!ui)
      return 0;
   const std::string text(reinterpret_cast<const char *>(web::scratch.data()),
                          static_cast<size_t>(std::clamp<int>(
                             textLen, 0, static_cast<int>(web::scratch.size()))));
   const bool used = ui->window->webKey(keysym, text.c_str(), mods);
   ui->remote.commit();
   return used ? 1 : 0;
}

// A message from the plugin, `size` bytes in the buffer.
__attribute__((export_name("wclap_ui_message"))) void wclapUiMessage(int size) {
   if (!ui || size <= 0 || static_cast<size_t>(size) > web::scratch.size())
      return;
   ui->remote.status(web::scratch.data(), static_cast<size_t>(size));
}

} // extern "C"
