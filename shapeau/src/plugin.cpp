#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "plugincore/dsp/denormals.h"

#include "dsp/engine.h"
#include "entry.h"
#include "factories.h"
#include "gui/curvemodel.h"
#include "params.h"
#include "presetlibrary.h"
#include "presets_generated.h"
#include "shapeau.h"

// The WCLAP (a WebAssembly build) has no native window. Its editor runs in the
// host's webview page as a module of its own (web/uimain.cpp) and talks to the
// plugin through clap.webview messages (web/protocol.h).
#if defined(__wasm__) && !defined(SHAPEAU_WITH_GUI)
#   define SHAPEAU_WEBVIEW 1
#   include <clap/ext/draft/webview.h>
#   include "web/protocol.h"
#else
#   define SHAPEAU_WEBVIEW 0
#endif

#ifdef SHAPEAU_WITH_GUI
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "gui/gui.h"

#if defined(__APPLE__)
#   include <CoreFoundation/CoreFoundation.h>
#endif
#endif

namespace shapeau {

namespace {

const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO,
                                 "effect", "multiband", "filter", "distortion", nullptr};

const clap_plugin_descriptor_t kDescriptor = {
   CLAP_VERSION_INIT,  kPluginId,          kPluginName, kPluginVendor,
   kPluginUrl,         kPluginUrl,         kPluginUrl,  kPluginVersion,
   kPluginDescription, kFeatures,
};

// 'SHAP' little-endian.
constexpr uint32_t kStateMagic = 0x50414853u;
// Version 1: header, one (id, value) pair per parameter, then twelve shapes as
// length-prefixed text, then the editor's selection and snap setting.
// Version 2 appends the modal-stage shapes (crush, pan, width) after the first
// four modules, and the editor's all-bands and step-mode flags after the snap
// setting. A version 1 blob is read with its twelve shapes mapped onto the
// first four modules; anything shorter than the blob stops where it stops.
// Version 3 appends the chain, as length-prefixed text ("vol flt drv"). A blob
// without it has an empty list, which runs the effects it switches on in the
// order they always ran in.
constexpr uint32_t kStateVersion = 3;
constexpr int kStateV1Modules = 4;

double clampv(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

class ShapeauPlugin final : public GuiDelegate, public CurveModel {
public:
   explicit ShapeauPlugin(const clap_host_t *host) : mHost(host) {
      for (uint32_t i = 0; i < kNumParams; ++i)
         mValues[i].store(paramTable()[i].def, std::memory_order_relaxed);
      resetCurves(mCurves);
#if SHAPEAU_WEBVIEW
      for (auto &bin : mScopeShadow)
         bin[web::kScopeFields - 1] = web::scopeEncode(-1.0f, true); // a cleared trace
#endif
      mPlugin.desc = &kDescriptor;
      mPlugin.plugin_data = this;
      mPlugin.init = [](const clap_plugin_t *) { return true; };
      mPlugin.destroy = [](const clap_plugin_t *p) { delete self(p); };
      mPlugin.activate = [](const clap_plugin_t *p, double sr, uint32_t minF, uint32_t maxF) {
         return self(p)->activate(sr, minF, maxF);
      };
      mPlugin.deactivate = [](const clap_plugin_t *p) { self(p)->deactivate(); };
      mPlugin.start_processing = [](const clap_plugin_t *) { return true; };
      mPlugin.stop_processing = [](const clap_plugin_t *) {};
      mPlugin.reset = [](const clap_plugin_t *p) {
         self(p)->mEngine.reset();
         self(p)->mParamsDirty.store(true, std::memory_order_release);
      };
      mPlugin.process = [](const clap_plugin_t *p, const clap_process_t *pr) {
         return self(p)->process(pr);
      };
      mPlugin.get_extension = [](const clap_plugin_t *p, const char *id) {
         return self(p)->getExtension(id);
      };
      mPlugin.on_main_thread = [](const clap_plugin_t *) {};
   }

#ifdef SHAPEAU_WITH_GUI
   ~ShapeauPlugin() override {
      stopGuiClock();
      delete mGui;
   }
#endif

private:
   static ShapeauPlugin *self(const clap_plugin_t *p) {
      return static_cast<ShapeauPlugin *>(p->plugin_data);
   }

   // --------------------------------------------------------------- lifecycle

   bool activate(double sampleRate, uint32_t /*minFrames*/, uint32_t /*maxFrames*/) {
      mSampleRate = sampleRate;
      mEngine.prepare(sampleRate, 1024);
      mEngine.publishCurves(mCurves);
      mParamsDirty.store(true, std::memory_order_release);
      return true;
   }

   void deactivate() {}

   // ------------------------------------------------------------------ params

   double rawValue(uint32_t id) const {
      return id < kNumParams ? mValues[id].load(std::memory_order_relaxed) : 0.0;
   }

   double realValue(uint32_t id) const { return paramToReal(paramTable()[id], rawValue(id)); }

   bool switched(uint32_t id) const { return static_cast<int>(std::floor(realValue(id) + 0.5)) != 0; }

   int enumValue(uint32_t id) const {
      return static_cast<int>(std::floor(realValue(id) + 0.5));
   }

   void syncEngine() {
      EngineParams p;
      p.inputGain = static_cast<float>(std::pow(10.0, realValue(kParamInput) / 20.0));
      p.outputGain = static_cast<float>(std::pow(10.0, realValue(kParamOutput) / 20.0));
      p.mix = static_cast<float>(realValue(kParamMix));
      p.splitLowHz = static_cast<float>(realValue(kParamSplitLow));
      p.splitHighHz = static_cast<float>(realValue(kParamSplitHigh));
      for (int b = 0; b < kNumBands; ++b)
         p.bandOn[b] = switched(kParamBandLow + b);

      p.volOn = switched(kParamVolOn);
      p.volTrimGain = static_cast<float>(std::pow(10.0, realValue(kParamVolTrim) / 20.0));
      p.volRate = enumValue(kParamVolRate);
      p.volPhase = static_cast<float>(realValue(kParamVolPhase));
      p.volSmooth = static_cast<float>(realValue(kParamVolSmooth));

      p.fltOn = switched(kParamFltOn);
      p.fltCutHz = static_cast<float>(realValue(kParamFltCutoff));
      p.fltRes = static_cast<float>(realValue(kParamFltRes));
      p.fltDepthOct = enumValue(kParamFltDepth);
      p.fltDrive = static_cast<float>(realValue(kParamFltDrive));
      p.fltType[kBandLow] = enumValue(kParamFltTypeLow);
      p.fltType[kBandMid] = enumValue(kParamFltTypeMid);
      p.fltType[kBandHigh] = enumValue(kParamFltTypeHigh);
      p.fltRate = enumValue(kParamFltRate);
      p.fltPhase = static_cast<float>(realValue(kParamFltPhase));
      p.fltSmooth = static_cast<float>(realValue(kParamFltSmooth));

      p.drvOn = switched(kParamDrvOn);
      p.drvStyle = enumValue(kParamDrvStyle);
      p.drvDrive = static_cast<float>(realValue(kParamDrvDrive));
      p.drvInputGain = static_cast<float>(std::pow(10.0, realValue(kParamDrvInput) / 20.0));
      p.drvOutputGain = static_cast<float>(std::pow(10.0, realValue(kParamDrvOutput) / 20.0));
      p.drvTone = static_cast<float>(realValue(kParamDrvTone));
      p.drvMix = static_cast<float>(realValue(kParamDrvMix));
      p.drvRate = enumValue(kParamDrvRate);
      p.drvPhase = static_cast<float>(realValue(kParamDrvPhase));
      p.drvSmooth = static_cast<float>(realValue(kParamDrvSmooth));

      p.timOn = switched(kParamTimOn);
      p.timRangeSec = static_cast<float>(realValue(kParamTimRange) * 0.001);
      p.timStep = static_cast<float>(realValue(kParamTimStep));
      p.timRate = enumValue(kParamTimRate);
      p.timPhase = static_cast<float>(realValue(kParamTimPhase));
      p.timSmooth = static_cast<float>(realValue(kParamTimSmooth));

      p.crsOn = switched(kParamCrsOn);
      p.crsAmount = static_cast<float>(realValue(kParamCrsAmount));
      p.crsBits = enumValue(kParamCrsBits);
      p.crsDown = enumValue(kParamCrsDown);
      p.crsMix = static_cast<float>(realValue(kParamCrsMix));
      p.crsRate = enumValue(kParamCrsRate);
      p.crsPhase = static_cast<float>(realValue(kParamCrsPhase));
      p.crsSmooth = static_cast<float>(realValue(kParamCrsSmooth));

      p.panOn = switched(kParamPanOn);
      p.panRate = enumValue(kParamPanRate);
      p.panPhase = static_cast<float>(realValue(kParamPanPhase));
      p.panSmooth = static_cast<float>(realValue(kParamPanSmooth));

      p.widOn = switched(kParamWidOn);
      p.widRate = enumValue(kParamWidRate);
      p.widPhase = static_cast<float>(realValue(kParamWidPhase));
      p.widSmooth = static_cast<float>(realValue(kParamWidSmooth));

      for (int m = 0; m < kNumModules; ++m) {
         p.trigMode[m] = std::clamp(enumValue(triggerParam(m)), 0, kNumTriggerModes - 1);
         p.trigLoop[m] = switched(loopParam(m));
         p.trigEnd[m] = static_cast<float>(realValue(endParam(m)));
      }
      p.trigFromInput = enumValue(kParamTrigSource) == 1;
      p.trigThreshold = static_cast<float>(std::pow(10.0, realValue(kParamTrigThreshold) / 20.0));
      p.trigFilterHz = static_cast<float>(realValue(kParamTrigFilter));
      p.trigHoldSec = static_cast<float>(realValue(kParamTrigHold) * 0.001);

      mEngine.setParams(p);
   }

   static uint32_t paramsCount(const clap_plugin_t *) { return kNumParams; }

   static bool paramsGetInfo(const clap_plugin_t *, uint32_t index, clap_param_info_t *info) {
      if (index >= kNumParams)
         return false;
      const ParamDesc &d = paramTable()[index];
      std::memset(info, 0, sizeof(*info));
      info->id = d.id;
      info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
      if (d.kind == ParamKind::Stepped || d.kind == ParamKind::Enum)
         info->flags |= CLAP_PARAM_IS_STEPPED;
      if (d.kind == ParamKind::Enum)
         info->flags |= CLAP_PARAM_IS_ENUM;
      info->min_value = d.min;
      info->max_value = d.max;
      info->default_value = d.def;
      std::snprintf(info->name, sizeof(info->name), "%s", d.name);
      std::snprintf(info->module, sizeof(info->module), "%s", d.module);
      return true;
   }

   static bool paramsGetValue(const clap_plugin_t *p, clap_id id, double *out) {
      if (!out || id >= kNumParams)
         return false;
      *out = self(p)->mValues[id].load(std::memory_order_relaxed);
      return true;
   }

   static bool paramsValueToText(const clap_plugin_t *, clap_id id, double value, char *out,
                                 uint32_t size) {
      const ParamDesc *d = paramById(id);
      return d && paramValueToText(*d, value, out, size);
   }

   static bool paramsTextToValue(const clap_plugin_t *, clap_id id, const char *text, double *out) {
      const ParamDesc *d = paramById(id);
      return d && paramTextToValue(*d, text, out);
   }

   static void paramsFlush(const clap_plugin_t *p, const clap_input_events_t *in,
                           const clap_output_events_t *out) {
      ShapeauPlugin *plug = self(p);
      const uint32_t n = in ? in->size(in) : 0;
      for (uint32_t i = 0; i < n; ++i)
         plug->handleEvent(in->get(in, i));
      plug->drainGuiEdits(out, 0);
   }

   // The key of a note-on, in either dialect, or -1 for anything else. A MIDI
   // note-on at velocity zero is a note-off, as MIDI defines it.
   static int noteOnKey(const clap_event_header_t *ev) {
      if (!ev || ev->space_id != CLAP_CORE_EVENT_SPACE_ID)
         return -1;
      if (ev->type == CLAP_EVENT_NOTE_ON)
         return reinterpret_cast<const clap_event_note_t *>(ev)->key;
      if (ev->type == CLAP_EVENT_MIDI) {
         const uint8_t *d = reinterpret_cast<const clap_event_midi_t *>(ev)->data;
         if ((d[0] & 0xF0) == 0x90 && d[2] > 0)
            return d[1];
      }
      return -1;
   }

   void handleEvent(const clap_event_header_t *ev) {
      if (!ev)
         return;
      switch (ev->type) {
      case CLAP_EVENT_PARAM_VALUE: {
         const auto *p = reinterpret_cast<const clap_event_param_value_t *>(ev);
         if (p->param_id >= kNumParams)
            return;
         const ParamDesc &d = paramTable()[p->param_id];
         mValues[p->param_id].store(clampv(p->value, d.min, d.max), std::memory_order_relaxed);
         mParamsDirty.store(true, std::memory_order_release);
         mPresetEdited.store(true);
         break;
      }
      default:
         break;
      }
   }

   // ---------------------------------------------------------------- gui edits
   //
   // The window runs on the main thread and must not write parameters behind
   // the host's back, or automation recording would never see a knob move. So
   // it posts into this single-producer queue and the audio side turns each
   // entry into a real CLAP event on its next process() or flush().

   enum class EditKind : uint8_t { GestureBegin, Value, GestureEnd };

   struct ParamEdit {
      uint32_t id;
      double value;
      EditKind kind;
   };

   static constexpr uint32_t kEditQueueSize = 512;

   void pushGuiEdit(uint32_t id, double value, EditKind kind) {
      const uint32_t write = mEditWrite.load(std::memory_order_relaxed);
      const uint32_t read = mEditRead.load(std::memory_order_acquire);
      if (write - read >= kEditQueueSize)
         return;
      mEditQueue[write & (kEditQueueSize - 1)] = {id, value, kind};
      mEditWrite.store(write + 1, std::memory_order_release);
      requestFlush();
   }

   void requestFlush() {
      if (!mHost)
         return;
      auto *hostParams =
         static_cast<const clap_host_params_t *>(mHost->get_extension(mHost, CLAP_EXT_PARAMS));
      if (hostParams && hostParams->request_flush)
         hostParams->request_flush(mHost);
   }

   void drainGuiEdits(const clap_output_events_t *out, uint32_t /*frames*/) {
      if (!out)
         return;
      uint32_t read = mEditRead.load(std::memory_order_relaxed);
      const uint32_t write = mEditWrite.load(std::memory_order_acquire);
      while (read != write) {
         const ParamEdit &e = mEditQueue[read & (kEditQueueSize - 1)];
         if (e.kind == EditKind::GestureBegin) {
            clap_event_param_gesture_t g{};
            g.header.size = sizeof(g);
            g.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
            g.header.time = 0;
            g.param_id = e.id;
            out->try_push(out, &g.header);
         } else if (e.kind == EditKind::GestureEnd) {
            clap_event_param_gesture_t g{};
            g.header.size = sizeof(g);
            g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
            g.header.time = 0;
            g.param_id = e.id;
            out->try_push(out, &g.header);
         } else {
            const ParamDesc &d = paramTable()[e.id];
            clap_event_param_value_t v{};
            v.header.size = sizeof(v);
            v.header.type = CLAP_EVENT_PARAM_VALUE;
            v.header.time = 0;
            v.param_id = e.id;
            v.value = clampv(e.value, d.min, d.max);
            out->try_push(out, &v.header);
         }
         ++read;
      }
      mEditRead.store(read, std::memory_order_release);
   }

   void setRaw(uint32_t id, double value) {
      const ParamDesc *d = paramById(id);
      if (!d)
         return;
      mValues[id].store(clampv(value, d->min, d->max), std::memory_order_relaxed);
      mParamsDirty.store(true, std::memory_order_release);
      mPresetEdited.store(true);
   }

   // ------------------------------------------------------------------- state

   static void put(std::string &blob, const void *data, size_t bytes) {
      blob.append(static_cast<const char *>(data), bytes);
   }

   static bool stateSave(const clap_plugin_t *p, const clap_ostream_t *stream) {
      ShapeauPlugin *plug = self(p);
      std::string blob;
      const uint32_t header[3] = {kStateMagic, kStateVersion, kNumParams};
      put(blob, header, sizeof(header));
      for (uint32_t i = 0; i < kNumParams; ++i) {
         const uint32_t id = paramTable()[i].id;
         const double v = plug->mValues[i].load(std::memory_order_relaxed);
         put(blob, &id, sizeof(id));
         put(blob, &v, sizeof(v));
      }
      for (int m = 0; m < kNumModules; ++m)
         for (int b = 0; b < kNumBands; ++b) {
            const std::string text = plug->mCurves.curve[m][b].format();
            const uint32_t size = static_cast<uint32_t>(text.size());
            put(blob, &size, sizeof(size));
            blob.append(text);
         }
      const int8_t selection[2] = {static_cast<int8_t>(plug->mCurveModule.load()),
                                   static_cast<int8_t>(plug->mCurveBand.load())};
      put(blob, selection, sizeof(selection));
      const uint8_t flags[3] = {static_cast<uint8_t>(plug->mSnap.load() ? 1u : 0u),
                                static_cast<uint8_t>(plug->mEditAllBands.load() ? 1u : 0u),
                                static_cast<uint8_t>(plug->mStepMode.load() ? 1u : 0u)};
      put(blob, flags, sizeof(flags));
      {
         const std::string chain = formatChain(plug->mCurves);
         const uint32_t size = static_cast<uint32_t>(chain.size());
         put(blob, &size, sizeof(size));
         blob.append(chain);
      }

      size_t written = 0;
      while (written < blob.size()) {
         const int64_t n = stream->write(stream, blob.data() + written, blob.size() - written);
         if (n <= 0)
            return false;
         written += static_cast<size_t>(n);
      }
      return true;
   }

   static bool readExactly(const clap_istream_t *stream, void *dst, size_t bytes) {
      size_t got = 0;
      char *out = static_cast<char *>(dst);
      while (got < bytes) {
         const int64_t n = stream->read(stream, out + got, bytes - got);
         if (n <= 0)
            return false;
         got += static_cast<size_t>(n);
      }
      return true;
   }

   static bool stateLoad(const clap_plugin_t *p, const clap_istream_t *stream) {
      ShapeauPlugin *plug = self(p);
      uint32_t header[3] = {0, 0, 0};
      if (!readExactly(stream, header, sizeof(header)))
         return false;
      if (header[0] != kStateMagic || header[1] > kStateVersion)
         return false;
      if (header[2] > 4096)
         return false;
      // A blob from before a parameter existed does not mention it, and it must
      // come back at its default rather than at whatever this instance held.
      for (uint32_t i = 0; i < kNumParams; ++i)
         plug->mValues[i].store(paramTable()[i].def, std::memory_order_relaxed);
      for (uint32_t i = 0; i < header[2]; ++i) {
         uint32_t id = 0;
         double value = 0.0;
         if (!readExactly(stream, &id, sizeof(id)) || !readExactly(stream, &value, sizeof(value)))
            return false;
         const ParamDesc *d = paramById(id);
         if (!d)
            continue;
         plug->mValues[id].store(clampv(value, d->min, d->max), std::memory_order_relaxed);
      }
      resetCurves(plug->mCurves);
      const int shapeModules = header[1] == 1 ? kStateV1Modules : kNumModules;
      bool haveShapes = true;
      for (int i = 0; i < shapeModules * kNumBands && haveShapes; ++i) {
         const int m = i / kNumBands;
         const int b = i % kNumBands;
         uint32_t size = 0;
         if (!readExactly(stream, &size, sizeof(size))) {
            haveShapes = false; // an older blob: the shapes stay neutral
            break;
         }
         if (size > 65536)
            return false;
         std::string text(size, '\0');
         if (!readExactly(stream, &text[0], size)) {
            haveShapes = false;
            break;
         }
         // Version 1 stored volume, filter, drive and time in that order, and
         // those are still indices 0-3, so no mapping is needed -- only the
         // count differs.
         plug->mCurves.curve[m][b].parse(text);
      }
      if (haveShapes) {
         int8_t selection[2] = {0, 0};
         if (readExactly(stream, selection, sizeof(selection))) {
            plug->mCurveModule.store(std::clamp<int>(selection[0], 0, kNumModules - 1));
            plug->mCurveBand.store(std::clamp<int>(selection[1], 0, kNumBands - 1));
         }
         uint8_t snap = 0;
         if (readExactly(stream, &snap, sizeof(snap)))
            plug->mSnap.store(snap != 0);
         if (header[1] >= 2) {
            uint8_t modes[2] = {0, 0};
            if (readExactly(stream, modes, sizeof(modes))) {
               plug->mEditAllBands.store(modes[0] != 0);
               plug->mStepMode.store(modes[1] != 0);
            }
         }
         uint32_t size = 0;
         if (header[1] >= 3 && readExactly(stream, &size, sizeof(size)) && size <= 256) {
            std::string chain(size, '\0');
            if (size == 0 || readExactly(stream, &chain[0], size))
               parseChain(chain, plug->mCurves);
         }
      }

      plug->mEngine.publishCurves(plug->mCurves);
      plug->curvesShown();
      plug->mParamsDirty.store(true, std::memory_order_release);
      plug->notifyParamValuesChanged();
      return true;
   }

   // -------------------------------------------------------------- preset load

   void applyPreset(const PresetData &preset, const CurveSet &curves) {
      for (uint32_t i = 0; i < kNumParams; ++i)
         mValues[i].store(paramTable()[i].def, std::memory_order_relaxed);
      for (const auto &kv : preset.values) {
         const ParamDesc *d = paramById(kv.first);
         if (!d)
            continue;
         mValues[kv.first].store(clampv(kv.second, d->min, d->max), std::memory_order_relaxed);
      }
      mCurves = curves;
      mEngine.publishCurves(mCurves);
      curvesShown();
      mParamsDirty.store(true, std::memory_order_release);
      notifyParamValuesChanged();
   }

   void notifyParamValuesChanged() {
      if (!mHost)
         return;
      auto *hostParams =
         static_cast<const clap_host_params_t *>(mHost->get_extension(mHost, CLAP_EXT_PARAMS));
      if (hostParams && hostParams->rescan)
         hostParams->rescan(mHost, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
   }

   void reportPresetError(uint32_t locationKind, const char *location, const char *loadKey,
                          const std::string &msg) {
      if (!mHost)
         return;
      auto *hostPreset = static_cast<const clap_host_preset_load_t *>(
         mHost->get_extension(mHost, CLAP_EXT_PRESET_LOAD));
      if (!hostPreset)
         hostPreset = static_cast<const clap_host_preset_load_t *>(
            mHost->get_extension(mHost, CLAP_EXT_PRESET_LOAD_COMPAT));
      if (hostPreset && hostPreset->on_error)
         hostPreset->on_error(mHost, locationKind, location, loadKey, 0, msg.c_str());
      auto *log = static_cast<const clap_host_log_t *>(mHost->get_extension(mHost, CLAP_EXT_LOG));
      if (log && log->log)
         log->log(mHost, CLAP_LOG_WARNING, msg.c_str());
   }

   static bool presetLoadFromLocation(const clap_plugin_t *p, uint32_t locationKind,
                                      const char *location, const char *loadKey) {
      ShapeauPlugin *plug = self(p);
      PresetData preset;
      CurveSet curves;
      resetCurves(curves);
      std::string error;

      if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE) {
         if (!location || !location[0]) {
            plug->reportPresetError(locationKind, location, loadKey, "missing preset path");
            return false;
         }
         if (!parsePresetFileWithCurves(location, preset, curves, error)) {
            plug->reportPresetError(locationKind, location, loadKey, error);
            return false;
         }
      } else if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
         const BuiltinPreset *found = nullptr;
         for (unsigned i = 0; i < kNumBuiltinPresets; ++i)
            if (loadKey && std::strcmp(kBuiltinPresets[i].loadKey, loadKey) == 0) {
               found = &kBuiltinPresets[i];
               break;
            }
         if (!found) {
            plug->reportPresetError(locationKind, location, loadKey,
                                    std::string("unknown built-in preset '") +
                                       (loadKey ? loadKey : "") + "'");
            return false;
         }
         if (!parsePreset(found->text, std::strlen(found->text), preset, curves, error)) {
            plug->reportPresetError(locationKind, location, loadKey, error);
            return false;
         }
      } else {
         plug->reportPresetError(locationKind, location, loadKey, "unsupported location kind");
         return false;
      }

      plug->applyPreset(preset, curves);
      plug->notePresetLoaded(locationKind, loadKey, location);

      if (plug->mHost) {
         auto *hostPreset = static_cast<const clap_host_preset_load_t *>(
            plug->mHost->get_extension(plug->mHost, CLAP_EXT_PRESET_LOAD));
         if (!hostPreset)
            hostPreset = static_cast<const clap_host_preset_load_t *>(
               plug->mHost->get_extension(plug->mHost, CLAP_EXT_PRESET_LOAD_COMPAT));
         if (hostPreset && hostPreset->loaded)
            hostPreset->loaded(plug->mHost, locationKind, location, loadKey);
      }
      return true;
   }

   // -------------------------------------------------------------------- ports

   // The main stereo pair, and on the input side a second one for the
   // sidechain: never heard, only listened to by the audio trigger and drawn
   // behind the shape. It is not main, so a host that does not route anything
   // to it leaves it silent or unconnected, and both are handled.
   static uint32_t audioPortsCount(const clap_plugin_t *, bool isInput) { return isInput ? 2 : 1; }

   static bool audioPortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                             clap_audio_port_info_t *info) {
      if (isInput && index == 1) {
         std::memset(info, 0, sizeof(*info));
         info->id = 2u;
         std::snprintf(info->name, sizeof(info->name), "%s", "Sidechain");
         info->flags = 0;
         info->channel_count = 2;
         info->port_type = CLAP_PORT_STEREO;
         info->in_place_pair = CLAP_INVALID_ID;
         return true;
      }
      if (index != 0)
         return false;
      std::memset(info, 0, sizeof(*info));
      info->id = isInput ? 0u : 1u;
      std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "In" : "Out");
      info->flags = CLAP_AUDIO_PORT_IS_MAIN;
      info->channel_count = 2;
      info->port_type = CLAP_PORT_STEREO;
      info->in_place_pair = isInput ? 1u : 0u;
      return true;
   }

   // One note input, for the MIDI trigger. Nothing is played; a note only
   // restarts the effects set to MIDI.
   static uint32_t notePortsCount(const clap_plugin_t *, bool isInput) { return isInput ? 1 : 0; }

   static bool notePortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                            clap_note_port_info_t *info) {
      if (!isInput || index != 0)
         return false;
      std::memset(info, 0, sizeof(*info));
      info->id = 0;
      info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
      info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
      std::snprintf(info->name, sizeof(info->name), "%s", "Trigger");
      return true;
   }

   // Whether a note answers the Note and Octave the Trigger panel sets.
   bool noteTriggers(int key) const {
      if (key < 0 || key > 127)
         return false;
      const int note = enumValue(kParamTrigNote);     // 0 any, 1..12 C..B
      const int octave = enumValue(kParamTrigOctave); // 0 any, 1..11 is -1..9
      return (note == 0 || key % 12 == note - 1) && (octave == 0 || key / 12 == octave - 1);
   }

   static uint32_t tailGet(const clap_plugin_t *p) {
      ShapeauPlugin *plug = self(p);
      const double tail = plug->mEngine.tailSeconds() * plug->mSampleRate;
      return static_cast<uint32_t>(clampv(tail, 0.0, 2.0e9));
   }

   // ------------------------------------------------------------------ process

   clap_process_status process(const clap_process_t *pr) {
      if (!pr)
         return CLAP_PROCESS_ERROR;
      const uint32_t frames = pr->frames_count;

      // Host events first: a parameter change applies to the whole block. The
      // event's time offset is ignored on purpose -- see TODO.md, where making
      // the block split at every event is the next piece of work. A trigger
      // note is the exception: it keeps its frame, which is where the effects
      // set to MIDI restart.
      const uint32_t count = pr->in_events ? pr->in_events->size(pr->in_events) : 0;
      uint32_t notes[64];
      uint32_t noteCount = 0;
      for (uint32_t i = 0; i < count; ++i) {
         const clap_event_header_t *ev = pr->in_events->get(pr->in_events, i);
         const int key = noteOnKey(ev);
         if (key >= 0) {
            if (noteTriggers(key) && noteCount < 64)
               notes[noteCount++] = ev->time;
            continue;
         }
         handleEvent(ev);
      }
      drainGuiEdits(pr->out_events, frames);

      if (frames == 0)
         return CLAP_PROCESS_CONTINUE;

      const clap_audio_buffer_t *in = pr->audio_inputs_count ? &pr->audio_inputs[0] : nullptr;
      clap_audio_buffer_t *out = pr->audio_outputs_count ? &pr->audio_outputs[0] : nullptr;
      if (!in || !out || !out->data32 || out->channel_count < 2)
         return CLAP_PROCESS_ERROR;

      float *l = out->data32[0];
      float *r = out->data32[1];
      if (!l || !r)
         return CLAP_PROCESS_ERROR;

      const float *inL = in->data32 && in->data32[0] ? in->data32[0] : l;
      const float *inR = in->data32 && in->data32[1] ? in->data32[1] : r;
      if (inL != l)
         std::memcpy(l, inL, frames * sizeof(float));
      if (inR != r)
         std::memcpy(r, inR, frames * sizeof(float));

      if (mParamsDirty.exchange(false, std::memory_order_acq_rel))
         syncEngine();

      // The sidechain, when the host connected one. A mono one is used for both
      // sides; anything missing is no sidechain at all.
      const float *sideL = nullptr, *sideR = nullptr;
      if (pr->audio_inputs_count > 1) {
         const clap_audio_buffer_t &side = pr->audio_inputs[1];
         if (side.data32 && side.channel_count > 0 && side.data32[0]) {
            sideL = side.data32[0];
            sideR = side.channel_count > 1 && side.data32[1] ? side.data32[1] : sideL;
         }
      }

      double beats = -1.0;
      double bpm = 0.0;
      if (pr->transport) {
         bpm = pr->transport->tempo;
         if ((pr->transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) &&
             (pr->transport->flags & CLAP_TRANSPORT_IS_PLAYING))
            beats = static_cast<double>(pr->transport->song_pos_beats) /
                    static_cast<double>(CLAP_BEATTIME_FACTOR);
      }

#if SHAPEAU_WEBVIEW
      webScopeAlive(frames);
#endif
      const plugincore::ScopedNoDenormals denormals;
      mEngine.process(l, r, frames, beats, bpm, sideL, sideR, notes, noteCount);
      publishOutputPeaks(l, r, frames);
      return CLAP_PROCESS_CONTINUE;
   }

   void publishOutputPeaks(const float *l, const float *r, uint32_t frames) {
      if (frames == 0)
         return;
      float peakL = 0.0f, peakR = 0.0f;
      for (uint32_t i = 0; i < frames; ++i) {
         peakL = std::max(peakL, std::fabs(l[i]));
         peakR = std::max(peakR, std::fabs(r[i]));
      }
      const double seconds = static_cast<double>(frames) / mSampleRate;
      const float fall = static_cast<float>(std::exp(-seconds / 0.35));
      mFallingPeakL = std::max(peakL, mFallingPeakL * fall);
      mFallingPeakR = std::max(peakR, mFallingPeakR * fall);
      mPeakL.store(mFallingPeakL, std::memory_order_relaxed);
      mPeakR.store(mFallingPeakR, std::memory_order_relaxed);
   }

   // ------------------------------------------------------------- preset list

   void ensurePresetList() {
      if (mPresetsScanned)
         return;
      mPresetsScanned = true;
#ifdef SHAPEAU_WITH_GUI
      mPresets = presetLibrary();
#endif
   }

   void notePresetLoaded(uint32_t locationKind, const char *loadKey, const char *location) {
#ifdef SHAPEAU_WITH_GUI
      ensurePresetList();
      mCurrentPreset = -1;
      for (size_t i = 0; i < mPresets.size(); ++i) {
         const bool match =
            locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
               ? (loadKey && mPresets[i].loadKey == loadKey)
               : (location && !mPresets[i].path.empty() && mPresets[i].path == location);
         if (match) {
            mCurrentPreset = static_cast<int>(i);
            break;
         }
      }
#else
      (void)locationKind;
      (void)loadKey;
      (void)location;
#endif
      mPresetEdited.store(false);
   }

   // ------------------------------------------------------------- GuiDelegate

   double guiParamValue(uint32_t id) const override { return rawValue(id); }

   void guiBeginEdit(uint32_t id) override {
      if (id < kNumParams)
         pushGuiEdit(id, 0.0, EditKind::GestureBegin);
   }

   void guiSetParam(uint32_t id, double value) override {
      const ParamDesc *d = paramById(id);
      if (!d)
         return;
      const double clamped = clampv(value, d->min, d->max);
      mValues[id].store(clamped, std::memory_order_relaxed);
      mParamsDirty.store(true, std::memory_order_release);
      mPresetEdited.store(true);
      pushGuiEdit(id, clamped, EditKind::Value);
   }

   void guiEndEdit(uint32_t id) override {
      if (id < kNumParams)
         pushGuiEdit(id, 0.0, EditKind::GestureEnd);
   }

   void guiOutputPeaks(float &left, float &right) const override {
      left = mPeakL.load(std::memory_order_relaxed);
      right = mPeakR.load(std::memory_order_relaxed);
   }

   uint32_t guiVoiceCount() const override { return 0; }
   uint32_t guiVoiceLimit() const override { return 0; }

   const std::vector<GuiPreset> &guiPresets() const override { return mPresets; }
   int guiCurrentPreset() const override { return mCurrentPreset; }
   bool guiPresetEdited() const override { return mPresetEdited.load(); }

   std::string guiSuggestedPresetName() const override {
      if (mCurrentPreset >= 0 && mCurrentPreset < static_cast<int>(mPresets.size()))
         return mPresets[static_cast<size_t>(mCurrentPreset)].name;
      return "New Shape";
   }

   static void splitFolder(const std::string &input, std::string &folder, std::string &leaf) {
      const size_t cut = input.find_last_of("/\\");
      if (cut == std::string::npos) {
         folder.clear();
         leaf = input;
      } else {
         folder = input.substr(0, cut);
         leaf = input.substr(cut + 1);
      }
      auto trim = [](std::string &s) {
         while (!s.empty() && s.front() == ' ')
            s.erase(s.begin());
         while (!s.empty() && s.back() == ' ')
            s.pop_back();
      };
      trim(folder);
      trim(leaf);
   }

   bool guiSavePreset(const std::string &input, std::string &error) override {
      std::string folder, name;
      splitFolder(input, folder, name);
      if (name.empty()) {
         error = "Give the preset a name after the folder.";
         return false;
      }
      const std::string path = plugincore::userPresetPathIn(presetContext(), folder, name);
      if (path.empty()) {
         error = "No user preset directory: neither XDG_CONFIG_HOME nor HOME is set.";
         return false;
      }
      PresetData data;
      data.name = name;
      for (uint32_t i = 0; i < kNumParams; ++i)
         data.values.emplace_back(paramTable()[i].id, rawValue(i));
      if (!writePresetFile(path, formatPreset(data, mCurves), error))
         return false;
      mPresets.clear();
      mPresetsScanned = false;
      ensurePresetList();
      mCurrentPreset = -1;
      for (size_t i = 0; i < mPresets.size(); ++i)
         if (mPresets[i].path == path) {
            mCurrentPreset = static_cast<int>(i);
            break;
         }
      mPresetEdited.store(false);
      return true;
   }

   void guiLoadPreset(int index) override {
      if (index < 0 || index >= static_cast<int>(mPresets.size()))
         return;
      const GuiPreset entry = mPresets[static_cast<size_t>(index)];
      if (!entry.loadKey.empty())
         presetLoadFromLocation(&mPlugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr,
                                entry.loadKey.c_str());
      else
         presetLoadFromLocation(&mPlugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, entry.path.c_str(),
                                nullptr);
   }

   // -------------------------------------------------------------- CurveModel
   //
   // The editing rules are CurveModel's; what the plugin adds is where the
   // parameters live, the publishing of the shapes to the audio thread, and
   // the scope.

   double modelRealValue(uint32_t id) const override { return realValue(id); }
   void modelBeginEdit(uint32_t id) override { guiBeginEdit(id); }
   void modelSetParam(uint32_t id, double raw) override { guiSetParam(id, raw); }
   void modelEndEdit(uint32_t id) override { guiEndEdit(id); }
   void modelCurvesChanged() override { curveChanged(); }
   void modelViewChanged() override {
      updateScopeTarget();
      curvesShown();
   }

   // The scope follows what the editor shows, and runs only while a window is
   // open to show it.
   void updateScopeTarget() {
      mEngine.setScopeTarget(mCurveModule.load(), mCurveBand.load(), mEditAllBands.load(),
                             mGuiOpen.load());
   }

   const Scope *curveScope() const override { return &mEngine.scope(); }

   void curveChanged() {
      mEngine.publishCurves(mCurves);
      mPresetEdited.store(true);
      curvesShown();
   }

   // The shapes, the chain or the editor's view changed, from wherever: the
   // web editor gets them again with its next poll.
   void curvesShown() {
#if SHAPEAU_WEBVIEW
      if (++mWebCurveSerial == 0)
         mWebCurveSerial = 1;
#endif
   }

   // ----------------------------------------------------------- gui extension

#ifdef SHAPEAU_WITH_GUI
   static constexpr const char *kWindowApi =
#if defined(_WIN32)
      CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
      CLAP_WINDOW_API_COCOA;
#else
      CLAP_WINDOW_API_X11;
#endif

   static uintptr_t nativeHandle(const clap_window_t &w) {
#if defined(_WIN32)
      return reinterpret_cast<uintptr_t>(w.win32);
#elif defined(__APPLE__)
      return reinterpret_cast<uintptr_t>(w.cocoa);
#else
      return static_cast<uintptr_t>(w.x11);
#endif
   }

   static bool guiIsApiSupported(const clap_plugin_t *, const char *api, bool isFloating) {
      return !isFloating && api && std::strcmp(api, kWindowApi) == 0;
   }

   static bool guiGetPreferredApi(const clap_plugin_t *, const char **api, bool *isFloating) {
      *api = kWindowApi;
      *isFloating = false;
      return true;
   }

   static bool guiCreate(const clap_plugin_t *p, const char *api, bool isFloating) {
      if (!guiIsApiSupported(p, api, isFloating))
         return false;
      ShapeauPlugin *plug = self(p);
      if (plug->mGui)
         return true;
      plug->ensurePresetList();
      plug->mGui = createGui(*plug, *plug);
      if (!plug->mGui)
         return false;
      plug->startGuiClock();
      return true;
   }

   static void guiDestroy(const clap_plugin_t *p) {
      ShapeauPlugin *plug = self(p);
      plug->stopGuiClock();
      delete plug->mGui;
      plug->mGui = nullptr;
      plug->mGuiOpen.store(false);
      plug->updateScopeTarget();
   }

   static bool guiSetScale(const clap_plugin_t *p, double scale) {
#if defined(__APPLE__)
      // The cocoa API works in logical sizes and has no scale to set; the view
      // applies the Retina factor itself. Host resizing still zooms.
      (void)p;
      (void)scale;
      return false;
#else
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->setScale(scale);
      return true;
#endif
   }

   static bool guiGetSize(const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->size(width, height);
      return true;
   }

   static bool guiCanResize(const clap_plugin_t *) { return true; }

   static bool guiGetResizeHints(const clap_plugin_t *p, clap_gui_resize_hints_t *hints) {
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui || !hints)
         return false;
      uint32_t w = 0, h = 0;
      plug->mGui->designSize(&w, &h);
      hints->can_resize_horizontally = true;
      hints->can_resize_vertically = true;
      hints->preserve_aspect_ratio = true;
      hints->aspect_ratio_width = w;
      hints->aspect_ratio_height = h;
      return true;
   }

   static bool guiAdjustSize(const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui || !width || !height)
         return false;
      plug->mGui->fitSize(width, height);
      return true;
   }

   static bool guiSetSize(const clap_plugin_t *p, uint32_t width, uint32_t height) {
      ShapeauPlugin *plug = self(p);
      return plug->mGui && plug->mGui->resize(width, height);
   }

   static bool guiSetParent(const clap_plugin_t *p, const clap_window_t *window) {
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui || !window || std::strcmp(window->api, kWindowApi) != 0)
         return false;
      return plug->mGui->embed(nativeHandle(*window));
   }

   static bool guiSetTransient(const clap_plugin_t *p, const clap_window_t *window) {
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui || !window || std::strcmp(window->api, kWindowApi) != 0)
         return false;
      return plug->mGui->setTransientFor(nativeHandle(*window));
   }

   static void guiSuggestTitle(const clap_plugin_t *p, const char *title) {
      ShapeauPlugin *plug = self(p);
      if (plug->mGui)
         plug->mGui->setTitle(title);
   }

   static bool guiShow(const clap_plugin_t *p) {
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->show();
      plug->mGuiOpen.store(true);
      plug->updateScopeTarget();
      return true;
   }

   static bool guiHide(const clap_plugin_t *p) {
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->hide();
      plug->mGuiOpen.store(false);
      plug->updateScopeTarget();
      return true;
   }

   static void guiOnTimer(const clap_plugin_t *p, clap_id timerId) {
      ShapeauPlugin *plug = self(p);
      if (plug->mGui && timerId == plug->mTimerId)
         plug->mGui->tick();
   }

   void startGuiClock() {
      if (mHost) {
         auto *timer = static_cast<const clap_host_timer_support_t *>(
            mHost->get_extension(mHost, CLAP_EXT_TIMER_SUPPORT));
         if (timer && timer->register_timer && timer->register_timer(mHost, 33, &mTimerId))
            return;
      }
      mTimerId = CLAP_INVALID_ID;
#if defined(__APPLE__)
      // Not a thread on macOS: AppKit may only be touched from the main
      // thread, so the fallback is a timer on the main run loop, which is
      // what a host timer would have been.
      CFRunLoopTimerContext context{};
      context.info = this;
      mGuiTimer = CFRunLoopTimerCreate(
         kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.033, 0.033, 0, 0,
         [](CFRunLoopTimerRef, void *info) {
            auto *plug = static_cast<ShapeauPlugin *>(info);
            if (plug->mGui)
               plug->mGui->tick();
         },
         &context);
      if (mGuiTimer)
         CFRunLoopAddTimer(CFRunLoopGetMain(), mGuiTimer, kCFRunLoopCommonModes);
      return;
#endif
      mGuiThreadRun.store(true, std::memory_order_release);
      mGuiThread = std::thread([this] {
         while (mGuiThreadRun.load(std::memory_order_acquire)) {
            if (mGui)
               mGui->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
         }
      });
   }

   void stopGuiClock() {
#if defined(__APPLE__)
      if (mGuiTimer) {
         CFRunLoopTimerInvalidate(mGuiTimer);
         CFRelease(mGuiTimer);
         mGuiTimer = nullptr;
         return;
      }
#endif
      if (mGuiThreadRun.exchange(false, std::memory_order_acq_rel)) {
         if (mGuiThread.joinable())
            mGuiThread.join();
         return;
      }
      if (mHost && mTimerId != CLAP_INVALID_ID) {
         auto *timer = static_cast<const clap_host_timer_support_t *>(
            mHost->get_extension(mHost, CLAP_EXT_TIMER_SUPPORT));
         if (timer && timer->unregister_timer)
            timer->unregister_timer(mHost, mTimerId);
      }
      mTimerId = CLAP_INVALID_ID;
   }
#endif // SHAPEAU_WITH_GUI

#if SHAPEAU_WEBVIEW
   // ------------------------------------------------------------ the webview
   //
   // The window is the host's webview showing the bundle's page; sizes are
   // logical pixels (no set_scale with the webview API). The page zooms the
   // editor to whatever size it gets, keeping its proportions.

   static constexpr double kWebMinScale = 0.5;
   static constexpr double kWebMaxScale = 4.0;

   static bool guiIsApiSupported(const clap_plugin_t *, const char *api, bool isFloating) {
      return !isFloating && api && std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) == 0;
   }
   static bool guiGetPreferredApi(const clap_plugin_t *, const char **api, bool *isFloating) {
      *api = CLAP_WINDOW_API_WEBVIEW;
      *isFloating = false;
      return true;
   }
   static bool guiCreate(const clap_plugin_t *p, const char *api, bool isFloating) {
      return guiIsApiSupported(p, api, isFloating);
   }
   static void guiDestroy(const clap_plugin_t *) {}
   static bool guiSetScale(const clap_plugin_t *, double) { return false; }
   static bool guiGetSize(const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
      *width = self(p)->mWebW;
      *height = self(p)->mWebH;
      return true;
   }
   static bool guiCanResize(const clap_plugin_t *) { return true; }
   static bool guiGetResizeHints(const clap_plugin_t *, clap_gui_resize_hints_t *hints) {
      hints->can_resize_horizontally = true;
      hints->can_resize_vertically = true;
      hints->preserve_aspect_ratio = true;
      hints->aspect_ratio_width = web::kDesignW;
      hints->aspect_ratio_height = web::kDesignH;
      return true;
   }
   static bool guiAdjustSize(const clap_plugin_t *, uint32_t *width, uint32_t *height) {
      const double s = std::clamp(std::min(*width / static_cast<double>(web::kDesignW),
                                           *height / static_cast<double>(web::kDesignH)),
                                  kWebMinScale, kWebMaxScale);
      *width = static_cast<uint32_t>(web::kDesignW * s + 0.5);
      *height = static_cast<uint32_t>(web::kDesignH * s + 0.5);
      return true;
   }
   static bool guiSetSize(const clap_plugin_t *p, uint32_t width, uint32_t height) {
      self(p)->mWebW = width;
      self(p)->mWebH = height;
      return true;
   }
   static bool guiSetParent(const clap_plugin_t *, const clap_window_t *) { return true; }
   static bool guiSetTransient(const clap_plugin_t *, const clap_window_t *) { return false; }
   static void guiSuggestTitle(const clap_plugin_t *, const char *) {}
   static bool guiShow(const clap_plugin_t *) { return true; }
   static bool guiHide(const clap_plugin_t *) { return true; }

   static int32_t webviewGetUri(const clap_plugin_t *, char *uri, uint32_t capacity) {
      static const char page[] = "/index.html";
      if (uri && capacity > 0)
         std::snprintf(uri, capacity, "%s", page);
      return static_cast<int32_t>(sizeof(page));
   }
   // The host serves the bundle's files (index.html, ui.wasm, fonts) itself.
   static bool webviewGetResource(const clap_plugin_t *, const char *, char *, uint32_t,
                                  const clap_ostream_t *) {
      return false;
   }
   static bool webviewReceive(const clap_plugin_t *p, const void *data, uint32_t size) {
      return self(p)->webReceive(static_cast<const uint8_t *>(data), size);
   }

   const clap_host_webview_t *hostWebview() {
      if (!mHostWebview && mHost)
         mHostWebview = static_cast<const clap_host_webview_t *>(
            mHost->get_extension(mHost, CLAP_EXT_WEBVIEW));
      return mHostWebview;
   }

   // The end of one edit in the page: one undo step in a host that takes a
   // state save as one, for everything the page changed since the last.
   void webCommit() {
      if (!mWebPending)
         return;
      mWebPending = false;
      if (!mHost)
         return;
      auto *state =
         static_cast<const clap_host_state_t *>(mHost->get_extension(mHost, CLAP_EXT_STATE));
      if (state && state->mark_dirty)
         state->mark_dirty(mHost);
   }

   // The scope while a page polls: switched on by a poll, off again once none
   // has come for a second of audio.
   void webScopeAlive(uint32_t frames) {
      if (!mGuiOpen.load(std::memory_order_relaxed))
         return;
      mWebSilence += frames;
      if (mWebSilence > static_cast<uint64_t>(mSampleRate)) {
         mGuiOpen.store(false);
         updateScopeTarget();
      }
   }

   bool webReceive(const uint8_t *data, uint32_t size) {
      if (!data || size < 4)
         return false;
      web::Reader in{data, size};
      in.pos = 4;
      switch (data[0]) {
      case web::PagePoll: {
         const uint32_t valueSerial = in.u32();
         const uint32_t curveSerial = in.u32();
         const uint32_t scopeGen = in.u32();
         const uint32_t editCount = in.u32();
         if (!in.ok)
            return false;
         mWebSilence = 0;
         if (!mGuiOpen.load()) {
            mGuiOpen.store(true);
            updateScopeTarget();
         }
         webSendStatus(valueSerial, curveSerial, scopeGen, editCount);
         return true;
      }
      case web::PageBegin:
      case web::PageSet:
      case web::PageEnd: {
         const uint32_t id = in.u32();
         const double value = data[0] == web::PageSet ? in.f64() : 0.0;
         if (!in.ok || id >= kNumParams)
            return false;
         if (data[0] == web::PageBegin) {
            guiBeginEdit(id);
         } else if (data[0] == web::PageSet) {
            guiSetParam(id, value);
            mWebPending = true;
         } else {
            guiEndEdit(id);
            webCommit();
         }
         return true;
      }
      case web::PageCurves: {
         // Read into a copy, so a malformed message changes nothing.
         mWebScratch = mCurves;
         for (int i = 0; i < data[1]; ++i)
            if (!web::getCurve(in, mWebScratch) || !in.ok)
               return false;
         std::memcpy(mCurves.curve, mWebScratch.curve, sizeof(mCurves.curve));
         curveChanged();
         mWebPending = true;
         return true;
      }
      case web::PageChain: {
         mWebScratch = mCurves;
         web::getChain(in, mWebScratch);
         if (!in.ok)
            return false;
         std::memcpy(mCurves.chain, mWebScratch.chain, sizeof(mCurves.chain));
         mCurves.chainCount = mWebScratch.chainCount;
         curveChanged();
         mWebPending = true;
         return true;
      }
      case web::PageView:
         mCurveModule.store(std::clamp<int>(data[1], 0, kNumModules - 1));
         mCurveBand.store(std::clamp<int>(data[2], 0, kNumBands - 1));
         mEditAllBands.store((data[3] & web::kViewAllBands) != 0);
         mSnap.store((data[3] & web::kViewSnap) != 0);
         mStepMode.store((data[3] & web::kViewStepMode) != 0);
         modelViewChanged();
         return true;
      case web::PagePreset: {
         const uint32_t length = in.u32();
         if (!in.need(length))
            return false;
         PresetData preset;
         std::string error;
         if (!parsePreset(reinterpret_cast<const char *>(data + in.pos), length, preset,
                          mWebScratch, error))
            return false;
         applyPreset(preset, mWebScratch);
         mPresetEdited.store(false);
         mWebPending = true;
         webCommit();
         return true;
      }
      case web::PageCommit:
         webCommit();
         return true;
      case web::PageResize: {
         uint32_t w = in.u32(), h = in.u32();
         if (!in.ok || !w || !h)
            return false;
         guiAdjustSize(&mPlugin, &w, &h);
         auto *gui =
            mHost ? static_cast<const clap_host_gui_t *>(mHost->get_extension(mHost, CLAP_EXT_GUI))
                  : nullptr;
         if (gui && gui->request_resize && gui->request_resize(mHost, w, h)) {
            mWebW = w;
            mWebH = h;
         }
         return true;
      }
      default:
         return false;
      }
   }

   // Bins that changed since the last poll get a new generation; a page is
   // sent the bins newer than the generation it has seen, as runs.
   void webScopeScan() {
      const Scope &scope = mEngine.scope();
      bool changed = false;
      for (int k = 0; k < Scope::kBins; ++k) {
         const int16_t v[web::kScopeFields] = {
            web::scopeEncode(scope.inLo[k].load(std::memory_order_relaxed), false),
            web::scopeEncode(scope.inHi[k].load(std::memory_order_relaxed), false),
            web::scopeEncode(scope.outLo[k].load(std::memory_order_relaxed), false),
            web::scopeEncode(scope.outHi[k].load(std::memory_order_relaxed), false),
            web::scopeEncode(scope.sideLo[k].load(std::memory_order_relaxed), false),
            web::scopeEncode(scope.sideHi[k].load(std::memory_order_relaxed), false),
            web::scopeEncode(scope.trace[k].load(std::memory_order_relaxed), true)};
         if (std::memcmp(v, mScopeShadow[k], sizeof(v)) != 0) {
            std::memcpy(mScopeShadow[k], v, sizeof(v));
            mScopeStamp[k] = mScopeGen + 1;
            changed = true;
         }
      }
      if (changed)
         ++mScopeGen;
   }

   void webSendStatus(uint32_t pageValues, uint32_t pageCurves, uint32_t pageScope,
                      uint32_t editCount) {
      const clap_host_webview_t *webview = hostWebview();
      if (!webview)
         return;
      bool changed = mWebSent.size() != kNumParams;
      for (uint32_t i = 0; i < kNumParams && !changed; ++i)
         changed = mWebSent[i] != rawValue(i);
      if (changed) {
         mWebSent.resize(kNumParams);
         for (uint32_t i = 0; i < kNumParams; ++i)
            mWebSent[i] = rawValue(i);
         if (++mWebValueSerial == 0)
            mWebValueSerial = 1;
      }
      const bool withValues = pageValues != mWebValueSerial;
      const bool withCurves = pageCurves != mWebCurveSerial;
      webScopeScan();
      if (pageScope > mScopeGen)
         pageScope = 0; // a page that saw an earlier instance

      const Scope &scope = mEngine.scope();
      std::vector<uint8_t> &m = mWebOut;
      m.clear();
      web::put8(m, web::PluginStatus);
      web::put8(m, static_cast<uint8_t>((withValues ? web::kStatusValues : 0) |
                                        (withCurves ? web::kStatusCurves : 0)));
      web::put16(m, static_cast<uint16_t>(kNumParams));
      web::put32(m, mWebValueSerial);
      web::put32(m, mWebCurveSerial);
      web::put32(m, editCount);
      web::put32(m, mScopeGen);
      web::put32(m, scope.blocks.load(std::memory_order_relaxed));
      web::put32(m, scope.triggers.load(std::memory_order_relaxed));
      web::putF32(m, scope.playhead.load(std::memory_order_relaxed));
      web::put32(m, 0); // the run count, filled in below
      if (withValues)
         for (uint32_t i = 0; i < kNumParams; ++i)
            web::putF64(m, mWebSent[i]);
      if (withCurves) {
         web::put8(m, static_cast<uint8_t>(mCurveModule.load()));
         web::put8(m, static_cast<uint8_t>(mCurveBand.load()));
         web::put8(m, static_cast<uint8_t>((mEditAllBands.load() ? web::kViewAllBands : 0) |
                                           (mSnap.load() ? web::kViewSnap : 0) |
                                           (mStepMode.load() ? web::kViewStepMode : 0)));
         web::putChain(m, mCurves);
         for (int mod = 0; mod < kNumModules; ++mod)
            for (int b = 0; b < kNumBands; ++b)
               web::putCurve(m, mod, b, mCurves.curve[mod][b]);
      }
      uint32_t runs = 0;
      for (int k = 0; k < Scope::kBins;) {
         if (mScopeStamp[k] <= pageScope) {
            ++k;
            continue;
         }
         int end = k;
         while (end < Scope::kBins && mScopeStamp[end] > pageScope)
            ++end;
         web::put16(m, static_cast<uint16_t>(k));
         web::put16(m, static_cast<uint16_t>(end - k));
         for (int j = k; j < end; ++j)
            for (int f = 0; f < web::kScopeFields; ++f)
               web::put16(m, static_cast<uint16_t>(mScopeShadow[j][f]));
         ++runs;
         k = end;
      }
      web::at32(m, 32, runs);
      webview->send(mHost, m.data(), static_cast<uint32_t>(m.size()));
   }
#endif // SHAPEAU_WEBVIEW

   // --------------------------------------------------------------- extensions

   const void *getExtension(const char *id) const {
      static const clap_plugin_params_t kParamsExt = {paramsCount,       paramsGetInfo,
                                                      paramsGetValue,    paramsValueToText,
                                                      paramsTextToValue, paramsFlush};
      static const clap_plugin_audio_ports_t kAudioPortsExt = {audioPortsCount, audioPortsGet};
      static const clap_plugin_note_ports_t kNotePortsExt = {notePortsCount, notePortsGet};
      static const clap_plugin_state_t kStateExt = {stateSave, stateLoad};
      static const clap_plugin_tail_t kTailExt = {tailGet};
      static const clap_plugin_preset_load_t kPresetLoadExt = {presetLoadFromLocation};
#if defined(SHAPEAU_WITH_GUI) || SHAPEAU_WEBVIEW
      static const clap_plugin_gui_t kGuiExt = {
         guiIsApiSupported, guiGetPreferredApi, guiCreate,    guiDestroy,
         guiSetScale,       guiGetSize,         guiCanResize, guiGetResizeHints,
         guiAdjustSize,     guiSetSize,         guiSetParent, guiSetTransient,
         guiSuggestTitle,   guiShow,            guiHide};
#endif
#ifdef SHAPEAU_WITH_GUI
      static const clap_plugin_timer_support_t kTimerExt = {guiOnTimer};
#endif
#if SHAPEAU_WEBVIEW
      static const clap_plugin_webview_t kWebviewExt = {webviewGetUri, webviewGetResource,
                                                        webviewReceive};
      if (std::strcmp(id, CLAP_EXT_GUI) == 0)
         return &kGuiExt;
      if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0)
         return &kWebviewExt;
#endif
      if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
         return &kParamsExt;
      if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
         return &kAudioPortsExt;
      if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
         return &kNotePortsExt;
      if (std::strcmp(id, CLAP_EXT_STATE) == 0)
         return &kStateExt;
      if (std::strcmp(id, CLAP_EXT_TAIL) == 0)
         return &kTailExt;
      if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0 ||
          std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0)
         return &kPresetLoadExt;
#ifdef SHAPEAU_WITH_GUI
      if (std::strcmp(id, CLAP_EXT_GUI) == 0)
         return &kGuiExt;
      if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
         return &kTimerExt;
#endif
      return nullptr;
   }

public:
   const clap_plugin_t *clapPlugin() const { return &mPlugin; }

private:
   clap_plugin_t mPlugin{};
   const clap_host_t *mHost = nullptr;
   double mSampleRate = 48000.0;

   std::atomic<double> mValues[kNumParams];
   std::atomic<bool> mParamsDirty{true};

   // The shapes, the chain and the editor's view are CurveModel's members.
   // Whether a window is showing, which is the only time the scope is worth
   // the audio thread's time.
   std::atomic<bool> mGuiOpen{false};

   Engine mEngine;

   std::atomic<uint32_t> mEditWrite{0};
   std::atomic<uint32_t> mEditRead{0};
   ParamEdit mEditQueue[kEditQueueSize]{};

   float mFallingPeakL = 0.0f, mFallingPeakR = 0.0f;
   std::atomic<float> mPeakL{0.0f};
   std::atomic<float> mPeakR{0.0f};

   std::atomic<bool> mPresetEdited{false};
   int mCurrentPreset = -1;
   bool mPresetsScanned = false;
   std::vector<GuiPreset> mPresets;

#if SHAPEAU_WEBVIEW
   const clap_host_webview_t *mHostWebview = nullptr;
   uint32_t mWebW = web::kDesignW, mWebH = web::kDesignH;
   std::vector<double> mWebSent;
   uint32_t mWebValueSerial = 0;
   uint32_t mWebCurveSerial = 1;
   bool mWebPending = false;  // the page changed something since its last commit
   uint64_t mWebSilence = 0;  // samples since the page last polled
   // The scope as last sent, encoded, and the generation each bin changed in.
   int16_t mScopeShadow[Scope::kBins][web::kScopeFields] = {};
   uint32_t mScopeStamp[Scope::kBins] = {};
   uint32_t mScopeGen = 0;
   std::vector<uint8_t> mWebOut;
   CurveSet mWebScratch; // what a message is read into, off wasm's small stack
#endif

#ifdef SHAPEAU_WITH_GUI
   Gui *mGui = nullptr;
   std::thread mGuiThread;
   std::atomic<bool> mGuiThreadRun{false};
   clap_id mTimerId = CLAP_INVALID_ID;
#if defined(__APPLE__)
   CFRunLoopTimerRef mGuiTimer = nullptr; // the fallback clock on macOS
#endif
#endif
};

namespace {

uint32_t factoryCount(const clap_plugin_factory_t *) { return 1; }

const clap_plugin_descriptor_t *factoryGetDescriptor(const clap_plugin_factory_t *,
                                                     uint32_t index) {
   return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t *factoryCreate(const clap_plugin_factory_t *, const clap_host_t *host,
                                   const char *pluginId) {
   if (!host || !pluginId || std::strcmp(pluginId, kPluginId) != 0)
      return nullptr;
   ShapeauPlugin *plugin = new ShapeauPlugin(host);
   return plugin->clapPlugin();
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryGetDescriptor, factoryCreate};

} // namespace

bool entryInit(const char *) { return true; }
void entryDeinit() {}
const void *entryGetFactory(const char *factoryId) {
   if (factoryId && std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
      return &kFactory;
   if (factoryId && std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0)
      return presetDiscoveryFactory();
   return nullptr;
}

} // namespace shapeau
