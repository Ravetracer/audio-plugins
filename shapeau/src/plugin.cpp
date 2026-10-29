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
#include "gui/curveaccess.h"
#include "params.h"
#include "presets_generated.h"
#include "shapeau.h"

#ifdef SHAPEAU_WITH_GUI
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "gui/gui.h"
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

constexpr const char *kFactoryFolder = "Factory Presets";

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

class ShapeauPlugin final : public GuiDelegate, public CurveAccess {
public:
   explicit ShapeauPlugin(const clap_host_t *host) : mHost(host) {
      for (uint32_t i = 0; i < kNumParams; ++i)
         mValues[i].store(paramTable()[i].def, std::memory_order_relaxed);
      resetCurves(mCurves);
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

   static uint32_t audioPortsCount(const clap_plugin_t *, bool) { return 1; }

   static bool audioPortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                             clap_audio_port_info_t *info) {
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
      // the block split at every event is the next piece of work.
      const uint32_t count = pr->in_events ? pr->in_events->size(pr->in_events) : 0;
      for (uint32_t i = 0; i < count; ++i)
         handleEvent(pr->in_events->get(pr->in_events, i));
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

      double beats = -1.0;
      double bpm = 0.0;
      if (pr->transport) {
         bpm = pr->transport->tempo;
         if ((pr->transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) &&
             (pr->transport->flags & CLAP_TRANSPORT_IS_PLAYING))
            beats = static_cast<double>(pr->transport->song_pos_beats) /
                    static_cast<double>(CLAP_BEATTIME_FACTOR);
      }

      const plugincore::ScopedNoDenormals denormals;
      mEngine.process(l, r, frames, beats, bpm);
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
      for (unsigned i = 0; i < kNumBuiltinPresets; ++i) {
         PresetData data;
         std::string error;
         if (!parsePreset(kBuiltinPresets[i].text, std::strlen(kBuiltinPresets[i].text), data,
                          error))
            continue;
         GuiPreset entry;
         entry.name = data.name.empty() ? kBuiltinPresets[i].loadKey : data.name;
         entry.description = data.description;
         entry.loadKey = kBuiltinPresets[i].loadKey;
         entry.folder = kFactoryFolder;
         mPresets.push_back(entry);
      }

      const std::string dir = userPresetDir();
      std::error_code ec;
      if (dir.empty() || !std::filesystem::is_directory(dir, ec))
         return;
      std::vector<GuiPreset> user;
      const std::string suffix = std::string(".") + kPresetExtension;
      auto scan = [&](const std::filesystem::path &from, const std::string &folder) {
         std::error_code dirEc;
         for (const auto &entry : std::filesystem::directory_iterator(from, dirEc)) {
            if (!entry.is_regular_file())
               continue;
            const std::string name = entry.path().filename().string();
            if (name.size() <= suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
               continue;
            const std::string path = entry.path().string();
            PresetData data;
            std::string error;
            if (!parsePresetFile(path, data, error))
               continue;
            GuiPreset item;
            item.name = data.name.empty() ? name.substr(0, name.size() - suffix.size()) : data.name;
            item.description = data.description;
            item.path = path;
            item.userContent = true;
            item.folder = folder;
            user.push_back(item);
         }
      };
      scan(dir, std::string());
      std::vector<std::string> folders;
      for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
         if (entry.is_directory())
            folders.push_back(entry.path().filename().string());
      std::sort(folders.begin(), folders.end());
      for (const auto &folder : folders)
         scan(std::filesystem::path(dir) / folder, folder);
      std::sort(user.begin(), user.end(), [](const GuiPreset &a, const GuiPreset &b) {
         return a.folder == b.folder ? a.name < b.name : a.folder < b.folder;
      });
      mPresets.insert(mPresets.end(), user.begin(), user.end());
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

   // -------------------------------------------------------------- CurveAccess

   int curveModule() const override { return mCurveModule.load(); }
   int curveBand() const override { return mCurveBand.load(); }

   void curveSelectModule(int module) override {
      mCurveModule.store(std::clamp(module, 0, kNumModules - 1));
      updateScopeTarget();
   }
   void curveSelectBand(int band) override {
      mCurveBand.store(std::clamp(band, 0, kNumBands - 1));
      mEditAllBands.store(false);
      updateScopeTarget();
   }

   bool curveEditAllBands() const override { return mEditAllBands.load(); }
   void curveSetEditAllBands(bool all) override {
      mEditAllBands.store(all);
      updateScopeTarget();
   }

   // The scope follows what the editor shows, and runs only while a window is
   // open to show it.
   void updateScopeTarget() {
      mEngine.setScopeTarget(mCurveModule.load(), mCurveBand.load(), mEditAllBands.load(),
                             mGuiOpen.load());
   }

   const Scope *curveScope() const override { return &mEngine.scope(); }

   // ------------------------------------------------------------ the chain
   //
   // The effects in the chain, in order: the listed ones, then any that are
   // switched on without being listed (see CurveSet). Every edit first writes
   // that whole order into the list, so what the bar showed is what the edit
   // starts from, and then publishes it to the audio thread with the shapes.

   int curveChain(int *out) const override {
      bool on[kNumModules];
      for (int m = 0; m < kNumModules; ++m)
         on[m] = switched(onParamForModule(m));
      return effectiveChain(mCurves, on, out);
   }

   void materializeChain() {
      int order[kNumModules];
      const int n = curveChain(order);
      mCurves.chainCount = 0;
      for (int i = 0; i < n; ++i)
         chainAdd(mCurves, order[i]);
   }

   void curveChainAdd(int module) override {
      materializeChain();
      chainAdd(mCurves, module);
      curveChanged();
      if (!switched(onParamForModule(module)))
         guiSetParam(onParamForModule(module), 1.0);
      curveSelectModule(module);
   }

   // Out of the list and switched off, or it would come back as an unlisted
   // effect that is on. The editor moves to the effect that took its place.
   void curveChainRemove(int module) override {
      materializeChain();
      int at = 0;
      while (at < mCurves.chainCount && mCurves.chain[at] != module)
         ++at;
      chainRemove(mCurves, module);
      curveChanged();
      if (switched(onParamForModule(module)))
         guiSetParam(onParamForModule(module), 0.0);
      if (mCurves.chainCount > 0)
         curveSelectModule(mCurves.chain[std::min(at, mCurves.chainCount - 1)]);
   }

   void curveChainMove(int module, int place) override {
      materializeChain();
      chainMove(mCurves, module, place);
      curveChanged();
   }

   // The power dot: bypass without leaving the chain. Listed first, so an
   // effect switched off stays on the bar instead of vanishing from it.
   void curveToggleModule(int module) override {
      materializeChain();
      curveChanged();
      const uint32_t on = onParamForModule(module);
      guiSetParam(on, switched(on) ? 0.0 : 1.0);
   }

   const Curve &curveAt(int module, int band) const override {
      return mCurves.curve[std::clamp(module, 0, kNumModules - 1)]
                         [std::clamp(band, 0, kNumBands - 1)];
   }

   bool curveModuleEnabled(int module) const override {
      return switched(onParamForModule(module));
   }
   bool curveBandEnabled(int band) const override {
      return switched(kParamBandLow + std::clamp(band, 0, kNumBands - 1));
   }

   void curveEnableModule(int module) override { ensureStageForEdit(module); }

   void curveChanged() {
      mEngine.publishCurves(mCurves);
      mPresetEdited.store(true);
   }

   int curveAddPoint(int module, int band, float x, float y, int weight) override {
      (void)band;
      ensureStageForEdit(module);
      int result = -1;
      forEditedBands([&](int b) {
         const int index =
            mCurves.curve[module][b].addPoint(x, y, static_cast<uint8_t>(weight));
         if (b == mCurveBand.load())
            result = index;
      });
      curveChanged();
      return result;
   }

   void curveMovePoint(int module, int band, int index, float x, float y) override {
      (void)band;
      ensureStageForEdit(module);
      // In ALL mode the bands need not hold the same number of points (a loaded
      // preset can differ), so an index alone would move the wrong point in a
      // band that is shorter. The selected band's x is the anchor and each
      // other band moves its nearest point to it.
      const float anchor = mCurves.curve[module][mCurveBand.load()].point(index).x;
      forEditedBands([&](int b) {
         Curve &c = mCurves.curve[module][b];
         c.movePoint(b == mCurveBand.load() ? index : nearestPoint(c, anchor), x, y);
      });
      curveChanged();
   }

   void curveRemovePoint(int module, int band, int index) override {
      (void)band;
      ensureStageForEdit(module);
      const Curve &selected = mCurves.curve[module][mCurveBand.load()];
      const float anchor = selected.point(index).x;
      forEditedBands([&](int b) {
         Curve &c = mCurves.curve[module][b];
         c.removePoint(b == mCurveBand.load() ? index : nearestPoint(c, anchor));
      });
      curveChanged();
   }

   void curveSetWeight(int module, int band, int index, int weight, bool all) override {
      (void)band;
      ensureStageForEdit(module);
      const float anchor = mCurves.curve[module][mCurveBand.load()].point(index).x;
      forEditedBands([&](int b) {
         Curve &c = mCurves.curve[module][b];
         if (all) {
            for (int i = 0; i < c.count(); ++i)
               c.setWeight(i, static_cast<uint8_t>(weight));
         } else {
            c.setWeight(b == mCurveBand.load() ? index : nearestPoint(c, anchor),
                        static_cast<uint8_t>(weight));
         }
      });
      curveChanged();
   }

   void curveSetFlat(int module, int band, float y) override {
      // A flat reset is not a request to hear the stage: switching a drive on
      // over a neutral curve would colour the sound the user just cleared.
      (void)band;
      forEditedBands([&](int b) { mCurves.curve[module][b].setFlat(y); });
      curveChanged();
   }

   void curveSetBend(int module, int band, int index, float bend) override {
      (void)band;
      ensureStageForEdit(module);
      const float anchor = mCurves.curve[module][mCurveBand.load()].point(index).x;
      forEditedBands([&](int b) {
         Curve &c = mCurves.curve[module][b];
         c.setBend(b == mCurveBand.load() ? index : nearestPoint(c, anchor), bend);
      });
      curveChanged();
   }

   void curveSetStep(int module, int band, int step, int steps, float y) override {
      (void)band;
      ensureStageForEdit(module);
      forEditedBands(
         [&](int b) { mCurves.curve[module][b].setStep(step, steps, y); });
      curveChanged();
   }

   void curveFillSteps(int module, int band, int steps) override {
      // Switching the editor into step mode converts the drawing; that is a
      // view change, not a reason to switch a stage on.
      (void)band;
      forEditedBands([&](int b) { mCurves.curve[module][b].fillSteps(steps); });
      curveChanged();
   }

   bool curveSnap() const override { return mSnap.load(); }
   void curveToggleSnap() override { mSnap.store(!mSnap.load()); }

   bool curveStepMode() const override { return mStepMode.load(); }
   void curveSetStepMode(bool on) override { mStepMode.store(on); }

   static int nearestPoint(const Curve &curve, float x) {
      int best = 0;
      float distance = 2.0f;
      for (int i = 0; i < curve.count(); ++i) {
         const float d = std::fabs(curve.point(i).x - x);
         if (d < distance) {
            distance = d;
            best = i;
         }
      }
      return best;
   }

   template <typename F> void forEditedBands(F apply) {
      if (mEditAllBands.load()) {
         for (int b = 0; b < kNumBands; ++b)
            apply(b);
      } else {
         apply(mCurveBand.load());
      }
   }

   static uint32_t onParamForModule(int module) {
      switch (module) {
      case kModuleVolume:
         return kParamVolOn;
      case kModuleFilter:
         return kParamFltOn;
      case kModuleDrive:
         return kParamDrvOn;
      case kModuleTime:
         return kParamTimOn;
      case kModuleCrush:
         return kParamCrsOn;
      case kModulePan:
         return kParamPanOn;
      case kModuleWidth:
         return kParamWidOn;
      default:
         return kParamVolOn;
      }
   }

   // Drawing a shape into a stage that is switched off is a request to hear
   // it. It also switches on the bands being edited, so a shape drawn on the
   // low band while the low band is bypassed is not silent for a reason the
   // drawing gives no clue about.
   void ensureStageForEdit(int module) {
      const uint32_t on = onParamForModule(module);
      if (!switched(on))
         guiSetParam(on, 1.0);
      forEditedBands([&](int b) {
         const uint32_t bandOn = kParamBandLow + b;
         if (!switched(bandOn))
            guiSetParam(bandOn, 1.0);
      });
   }

   // ----------------------------------------------------------- gui extension

#ifdef SHAPEAU_WITH_GUI
   static constexpr const char *kWindowApi =
#ifdef _WIN32
      CLAP_WINDOW_API_WIN32;
#else
      CLAP_WINDOW_API_X11;
#endif

   static uintptr_t nativeHandle(const clap_window_t &w) {
#ifdef _WIN32
      return reinterpret_cast<uintptr_t>(w.win32);
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
      ShapeauPlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->setScale(scale);
      return true;
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

   // --------------------------------------------------------------- extensions

   const void *getExtension(const char *id) const {
      static const clap_plugin_params_t kParamsExt = {paramsCount,       paramsGetInfo,
                                                      paramsGetValue,    paramsValueToText,
                                                      paramsTextToValue, paramsFlush};
      static const clap_plugin_audio_ports_t kAudioPortsExt = {audioPortsCount, audioPortsGet};
      static const clap_plugin_state_t kStateExt = {stateSave, stateLoad};
      static const clap_plugin_tail_t kTailExt = {tailGet};
      static const clap_plugin_preset_load_t kPresetLoadExt = {presetLoadFromLocation};
#ifdef SHAPEAU_WITH_GUI
      static const clap_plugin_gui_t kGuiExt = {
         guiIsApiSupported, guiGetPreferredApi, guiCreate,    guiDestroy,
         guiSetScale,       guiGetSize,         guiCanResize, guiGetResizeHints,
         guiAdjustSize,     guiSetSize,         guiSetParent, guiSetTransient,
         guiSuggestTitle,   guiShow,            guiHide};
      static const clap_plugin_timer_support_t kTimerExt = {guiOnTimer};
#endif
      if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
         return &kParamsExt;
      if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
         return &kAudioPortsExt;
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

   CurveSet mCurves;
   std::atomic<int> mCurveModule{0};
   std::atomic<int> mCurveBand{1};
   // All three bands are edited by default: a shape on the mid band alone is
   // only heard by the mid band, which is not what drawing a volume stutter
   // across the graph looks like it should do.
   std::atomic<bool> mEditAllBands{true};
   std::atomic<bool> mStepMode{false};
   // Whether a window is showing, which is the only time the scope is worth
   // the audio thread's time.
   std::atomic<bool> mGuiOpen{false};
   std::atomic<bool> mSnap{true};

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

#ifdef SHAPEAU_WITH_GUI
   Gui *mGui = nullptr;
   std::thread mGuiThread;
   std::atomic<bool> mGuiThreadRun{false};
   clap_id mTimerId = CLAP_INVALID_ID;
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
