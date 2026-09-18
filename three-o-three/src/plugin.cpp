#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "plugincore/dsp/denormals.h"
#include "plugincore/dsp/fastmath.h"
#include "dsp/acid_engine.h"
#include "entry.h"
#include "factories.h"
#include "params.h"
#include "presets_generated.h"
#include "threeohthree.h"

#ifdef THREEOHTHREE_WITH_GUI
#include <chrono>
#include <thread>

#include <filesystem>

#include "gui/gui.h"
#endif

namespace threeohthree {

namespace {

const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                 CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                 CLAP_PLUGIN_FEATURE_STEREO,
                                 "bass",
                                 "acid",
                                 "analog",
                                 nullptr};

const clap_plugin_descriptor_t kDescriptor = {
   CLAP_VERSION_INIT,  kPluginId,          kPluginName, kPluginVendor,
   kPluginUrl,         kPluginUrl,         kPluginUrl,  kPluginVersion,
   kPluginDescription, kFeatures,
};

// State chunk header. Values are stored per parameter id so that adding
// parameters later cannot break older saved state.
constexpr uint32_t kStateMagic = 0x33303354u; // 'T303' little-endian
// Version 2 appends the sixteen-step pattern after the parameter block. A
// version 1 blob simply stops before it, and the pattern keeps its default,
// which is what a project saved before the sequencer existed should do.
//
// Version 3 appends the other sixty-three patterns of the bank and the one bit
// of window state worth keeping -- whether the collapsible panel section is
// open. A version 2 blob still loads: its one pattern becomes pattern 1 and the
// rest of the bank keeps its (empty) default.
//
// Version 4 changes no layout at all. It exists because four parameters had
// their ranges widened for the Devil Fish controls, and a blob stores the *raw*
// host value of each -- which for a logarithmic parameter is a position on its
// own curve, not a frequency. Widening the curve therefore moves every saved
// value unless the old position is converted, and a project saved before this
// would come back with the wrong cutoff. Preset files are safe either way:
// they are written in real units. See migrateRanges().
constexpr uint32_t kStateVersion = 4;

// What those four parameters used to span, so an older blob can be read with
// the mapping it was written under.
struct OldRange {
   uint32_t id;
   bool logarithmic;
   double lo;
   double hi;
};

} // namespace

#ifdef THREEOHTHREE_WITH_GUI
class ThreeOhThreePlugin final : public GuiDelegate, public PatternAccess, public WindowHost {
#else
class ThreeOhThreePlugin : public PatternAccess {
#endif
public:
   explicit ThreeOhThreePlugin(const clap_host_t *host) : mHost(host) {
      {
         // Pattern 1 shows what the instrument does; the other sixty-three
         // start empty, because a bank of sixty-four copies of the same line
         // would only have to be cleared before it could be used.
         uint16_t seed[kMaxSteps];
         defaultPattern(seed);
         uint16_t rests[kMaxSteps];
         clearPattern(rests);
         for (int p = 0; p < kMaxPatterns; ++p)
            for (int i = 0; i < kMaxSteps; ++i)
               mPattern[p][i].store(p == 0 ? seed[i] : rests[i], std::memory_order_relaxed);
      }
      for (uint32_t i = 0; i < kNumParams; ++i) {
         mValues[i].store(paramTable()[i].def, std::memory_order_relaxed);
         mMods[i].store(0.0, std::memory_order_relaxed);
      }
      mPlugin.desc = &kDescriptor;
      mPlugin.plugin_data = this;
      mPlugin.init = [](const clap_plugin_t *p) { return self(p)->init(); };
      mPlugin.destroy = [](const clap_plugin_t *p) { delete self(p); };
      mPlugin.activate = [](const clap_plugin_t *p, double sr, uint32_t minF, uint32_t maxF) {
         return self(p)->activate(sr, minF, maxF);
      };
      mPlugin.deactivate = [](const clap_plugin_t *p) { self(p)->deactivate(); };
      mPlugin.start_processing = [](const clap_plugin_t *) { return true; };
      mPlugin.stop_processing = [](const clap_plugin_t *) {};
      mPlugin.reset = [](const clap_plugin_t *p) {
         ThreeOhThreePlugin *plug = self(p);
         plug->seqStopAll();
         plug->mSeqRunning = false;
         plug->mLastFired = -1.0e18;
         plug->mEngine.reset();
      };
      mPlugin.process = [](const clap_plugin_t *p, const clap_process_t *pr) {
         return self(p)->process(pr);
      };
      mPlugin.get_extension = [](const clap_plugin_t *p, const char *id) {
         return self(p)->getExtension(id);
      };
      mPlugin.on_main_thread = [](const clap_plugin_t *) {};
   }

#ifdef THREEOHTHREE_WITH_GUI
   ~ThreeOhThreePlugin() override {
      // Hosts normally call gui.destroy() first, but a plugin must not depend
      // on that to avoid taking its window down with it.
      stopGuiClock();
      delete mGui;
   }
#endif

   const clap_plugin_t *clapPlugin() const { return &mPlugin; }

private:
   static ThreeOhThreePlugin *self(const clap_plugin_t *p) {
      return static_cast<ThreeOhThreePlugin *>(p->plugin_data);
   }

   // ---------------------------------------------------------------- lifecycle

   bool init() { return true; }

   bool activate(double sampleRate, uint32_t /*minFrames*/, uint32_t maxFrames) {
      mSampleRate = sampleRate;
      mEngine.prepare(sampleRate, maxFrames);
      mParamsDirty.store(true, std::memory_order_release);
      return true;
   }

   void deactivate() {}

   // ------------------------------------------------------------------- params

   double effective(uint32_t id) const {
      const ParamDesc &d = paramTable()[id];
      const double v = mValues[id].load(std::memory_order_relaxed) +
                       mMods[id].load(std::memory_order_relaxed);
      return v < d.min ? d.min : (v > d.max ? d.max : v);
   }

   double realValue(uint32_t id) const { return paramToReal(paramTable()[id], effective(id)); }

   void syncEngineParams() {
      EngineParams p;

      // VCO
      p.waveform = static_cast<int>(realValue(kParamWaveform));
      p.tuningCents = static_cast<float>(realValue(kParamTuning));

      // VCF
      p.cutoffHz = static_cast<float>(realValue(kParamCutoff));
      p.resonance = static_cast<float>(realValue(kParamResonance));
      p.envMod = static_cast<float>(realValue(kParamEnvMod));
      p.decaySec = static_cast<float>(realValue(kParamDecay)) * 0.001f;
      p.tracking = static_cast<float>(realValue(kParamTracking));

      // Accent
      p.accent = static_cast<float>(realValue(kParamAccent));
      p.accentVelocity = static_cast<float>(realValue(kParamAccentThreshold));
      p.accentSweepSec = static_cast<float>(realValue(kParamAccentDecay)) * 0.001f;

      // Slide
      p.slideSec = static_cast<float>(realValue(kParamSlideTime)) * 0.001f;

      // Drive, and the master
      p.drive = static_cast<float>(realValue(kParamDrive));
      p.toneHz = static_cast<float>(realValue(kParamTone));
      p.gain = dbToGain(static_cast<float>(realValue(kParamVolume)));

      // Mods: the constants that used to be hard-coded.
      p.envBiasOct = static_cast<float>(realValue(kParamEnvBias));
      p.envDepthOct = static_cast<float>(realValue(kParamEnvDepth));
      p.accSweepOct = static_cast<float>(realValue(kParamAccSweep));
      p.accBuild = static_cast<float>(realValue(kParamAccBuild));
      p.accGain = static_cast<float>(realValue(kParamAccGain)) * 0.01f;
      p.accDecaySec = static_cast<float>(realValue(kParamAccDecay)) * 0.001f;
      p.droopHz = static_cast<float>(realValue(kParamDroop));
      p.ladder = static_cast<float>(realValue(kParamLadder)) * 0.01f;
      p.resRange = static_cast<float>(realValue(kParamResRange)) * 0.01f;
      p.drift = static_cast<float>(realValue(kParamDrift));

      // Vibrato.
      p.vibratoCents = static_cast<float>(realValue(kParamVibDepth));
      p.vibratoHz = static_cast<float>(realValue(kParamVibRate));
      p.vibratoDelaySec = static_cast<float>(realValue(kParamVibDelay)) * 0.001f;

      // The Devil Fish. Overdrive is written in dB about the machine's own
      // level, so the bottom of its travel is silence rather than a very small
      // number: at -60 dB the oscillator is gone, which is the setting that
      // leaves a self-oscillating filter on its own.
      p.oscDrive = dbToGain(static_cast<float>(realValue(kParamOverdrive)));
      p.filterFm = static_cast<float>(realValue(kParamFilterFM));
      p.muffler = static_cast<int>(realValue(kParamMuffler));
      p.softAttackSec = static_cast<float>(realValue(kParamSoftAttack)) * 0.001f;
      p.ampDecaySec = static_cast<float>(realValue(kParamAmpDecay)) * 0.001f;
      p.ampSustain = static_cast<float>(realValue(kParamAmpSustain));
      p.sweepSpeed = static_cast<int>(realValue(kParamSweepSpeed));
      p.accentHold = static_cast<int>(realValue(kParamAccentHold)) != 0;

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
      info->cookie = nullptr;
      std::snprintf(info->name, sizeof(info->name), "%s", d.name);
      std::snprintf(info->module, sizeof(info->module), "%s", d.module);
      return true;
   }

   static bool paramsGetValue(const clap_plugin_t *p, clap_id id, double *out) {
      if (id >= kNumParams)
         return false;
      *out = self(p)->mValues[id].load(std::memory_order_relaxed);
      return true;
   }

   static bool paramsValueToText(const clap_plugin_t *, clap_id id, double value, char *out,
                                 uint32_t size) {
      const ParamDesc *d = paramById(id);
      if (!d)
         return false;
      return paramValueToText(*d, value, out, size);
   }

   static bool paramsTextToValue(const clap_plugin_t *, clap_id id, const char *text,
                                 double *out) {
      const ParamDesc *d = paramById(id);
      if (!d)
         return false;
      return paramTextToValue(*d, text, out);
   }

   static void paramsFlush(const clap_plugin_t *p, const clap_input_events_t *in,
                           const clap_output_events_t *out) {
      ThreeOhThreePlugin *plug = self(p);
      const uint32_t n = in ? in->size(in) : 0;
      for (uint32_t i = 0; i < n; ++i)
         plug->handleEvent(in->get(in, i));
      plug->drainGuiEdits(out, 0);
   }

   // ------------------------------------------------------------- gui edits
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

   static constexpr uint32_t kEditQueueSize = 512; // power of two

   void pushGuiEdit(uint32_t id, double value, EditKind kind) {
      const uint32_t write = mEditWrite.load(std::memory_order_relaxed);
      const uint32_t read = mEditRead.load(std::memory_order_acquire);
      if (write - read >= kEditQueueSize)
         return; // full: the host is not calling us, dropping is the safe move
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

   void drainGuiEdits(const clap_output_events_t *out, uint32_t time) {
      const uint32_t write = mEditWrite.load(std::memory_order_acquire);
      uint32_t read = mEditRead.load(std::memory_order_relaxed);
      for (; read != write; ++read) {
         const ParamEdit &edit = mEditQueue[read & (kEditQueueSize - 1)];
         if (!out)
            continue;
         if (edit.kind == EditKind::Value) {
            clap_event_param_value_t ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.header.size = sizeof(ev);
            ev.header.time = time;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.param_id = edit.id;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = edit.value;
            out->try_push(out, &ev.header);
         } else {
            clap_event_param_gesture_t ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.header.size = sizeof(ev);
            ev.header.time = time;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = edit.kind == EditKind::GestureBegin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                                 : CLAP_EVENT_PARAM_GESTURE_END;
            ev.param_id = edit.id;
            out->try_push(out, &ev.header);
         }
      }
      mEditRead.store(read, std::memory_order_release);
   }

   // -------------------------------------------------------------------- state

   static bool stateSave(const clap_plugin_t *p, const clap_ostream_t *stream) {
      ThreeOhThreePlugin *plug = self(p);
      std::string blob;
      const uint32_t header[3] = {kStateMagic, kStateVersion, kNumParams};
      blob.append(reinterpret_cast<const char *>(header), sizeof(header));
      for (uint32_t i = 0; i < kNumParams; ++i) {
         const uint32_t id = paramTable()[i].id;
         const double v = plug->mValues[i].load(std::memory_order_relaxed);
         blob.append(reinterpret_cast<const char *>(&id), sizeof(id));
         blob.append(reinterpret_cast<const char *>(&v), sizeof(v));
      }
      // The bank, which is not in the parameter table: sixty-four patterns of
      // sixteen packed words after the parameters. See pattern.h for why it is
      // not. Pattern 1 comes first, so the first sixteen words are exactly
      // what a version 2 blob held.
      for (int pat = 0; pat < kMaxPatterns; ++pat) {
         for (int i = 0; i < kMaxSteps; ++i) {
            const uint16_t packed = plug->mPattern[pat][i].load(std::memory_order_relaxed);
            blob.append(reinterpret_cast<const char *>(&packed), sizeof(packed));
         }
      }
      const uint8_t advanced = plug->mAdvancedOpen.load(std::memory_order_relaxed) ? 1u : 0u;
      blob.append(reinterpret_cast<const char *>(&advanced), sizeof(advanced));

      size_t written = 0;
      while (written < blob.size()) {
         const int64_t n =
            stream->write(stream, blob.data() + written, blob.size() - written);
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

   // Converts the four widened parameters from the mapping a version 3 or older
   // blob was written under to the current one, by way of the real value both
   // agree on. Anything outside the new range is clamped rather than dropped;
   // none of the four can be, because every old range is inside its new one.
   static void migrateRanges(ThreeOhThreePlugin *plug) {
      static const OldRange kOld[] = {
         {kParamCutoff, true, 100.0, 2500.0},
         {kParamDecay, true, 200.0, 2500.0},
         {kParamSlideTime, true, 10.0, 300.0},
         // Tracking is not in this list on purpose. Its maximum went from 100 %
         // to 200 %, but it stayed a percent parameter, so the stored value
         // still means the same thing and converting it would be the bug.
      };
      for (const OldRange &o : kOld) {
         const ParamDesc *d = paramById(o.id);
         if (!d)
            continue;
         const double raw = plug->mValues[o.id].load(std::memory_order_relaxed);
         const double real = o.logarithmic
                                ? o.lo * std::pow(o.hi / o.lo, clampv(raw, 0.0, 1.0))
                                : raw;
         plug->mValues[o.id].store(clampv(realToParam(*d, real), d->min, d->max),
                                   std::memory_order_relaxed);
      }
   }

   static bool stateLoad(const clap_plugin_t *p, const clap_istream_t *stream) {
      ThreeOhThreePlugin *plug = self(p);
      uint32_t header[3] = {0, 0, 0};
      if (!readExactly(stream, header, sizeof(header)))
         return false;
      if (header[0] != kStateMagic || header[1] > kStateVersion)
         return false;

      const uint32_t count = header[2];
      // Guard against a corrupt count claiming a huge payload.
      if (count > 4096)
         return false;

      for (uint32_t i = 0; i < count; ++i) {
         uint32_t id = 0;
         double value = 0.0;
         if (!readExactly(stream, &id, sizeof(id)) ||
             !readExactly(stream, &value, sizeof(value)))
            return false;
         const ParamDesc *d = paramById(id);
         if (!d)
            continue; // unknown id from a newer version: ignore
         plug->mValues[id].store(clampv(value, d->min, d->max), std::memory_order_relaxed);
      }
      // The pattern, if this blob is new enough to have one. A version 1 blob
      // ends here and its pattern stays at whatever it already was.
      if (header[1] >= 2) {
         uint16_t packed[kMaxSteps];
         if (readExactly(stream, packed, sizeof(packed)))
            for (int i = 0; i < kMaxSteps; ++i)
               plug->mPattern[0][i].store(packed[i], std::memory_order_relaxed);
      }
      // The rest of the bank, and the window state. A truncated read leaves
      // everything after it alone rather than failing the load: a project that
      // opens with the wrong panel collapsed is better than one that does not
      // open.
      if (header[1] >= 3) {
         for (int pat = 1; pat < kMaxPatterns; ++pat) {
            uint16_t packed[kMaxSteps];
            if (!readExactly(stream, packed, sizeof(packed)))
               break;
            for (int i = 0; i < kMaxSteps; ++i)
               plug->mPattern[pat][i].store(packed[i], std::memory_order_relaxed);
         }
         uint8_t advanced = 0;
         if (readExactly(stream, &advanced, sizeof(advanced)))
            plug->mAdvancedOpen.store(advanced != 0, std::memory_order_relaxed);
      }

      if (header[1] < 4)
         migrateRanges(plug);

      plug->mParamsDirty.store(true, std::memory_order_release);
      plug->notifyParamValuesChanged();
      return true;
   }

   // -------------------------------------------------------------- preset load

   void applyPattern(const PatternData &pattern) {
      if (!pattern.present)
         return;
      for (int p = 0; p < kMaxPatterns; ++p)
         for (int i = 0; i < kMaxSteps; ++i)
            mPattern[p][i].store(pattern.pattern(p)[i], std::memory_order_relaxed);
   }

   // A preset describes the whole instrument, so anything it does not mention
   // goes back to its default rather than keeping whatever the last preset left
   // behind. That matters the moment a parameter is added: the twenty-six
   // factory presets were written before the pattern bank existed and say
   // nothing about it, and without this, loading one while pattern 12 was
   // selected would leave the sequencer pointed at a pattern the preset had
   // just emptied.
   void applyPreset(const PresetData &preset) {
      for (uint32_t i = 0; i < kNumParams; ++i)
         mValues[i].store(paramTable()[i].def, std::memory_order_relaxed);
      for (const auto &kv : preset.values) {
         const ParamDesc *d = paramById(kv.first);
         if (!d)
            continue;
         mValues[kv.first].store(clampv(kv.second, d->min, d->max), std::memory_order_relaxed);
      }
      mParamsDirty.store(true, std::memory_order_release);
      notifyParamValuesChanged();
   }

   // Any path that moves parameters behind the host's back -- loading a preset,
   // loading saved state -- has to ask the host to re-read them, or it goes on
   // showing and automating the values it last knew about. [main-thread]
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
      ThreeOhThreePlugin *plug = self(p);
      PresetData preset;
      PatternData pattern;
      std::string error;

      if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE) {
         if (!location || !location[0]) {
            plug->reportPresetError(locationKind, location, loadKey, "missing preset path");
            return false;
         }
         if (!parsePresetFile(location, preset, &pattern, error)) {
            plug->reportPresetError(locationKind, location, loadKey, error);
            return false;
         }
      } else if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
         if (!loadKey || !loadKey[0]) {
            plug->reportPresetError(locationKind, location, loadKey, "missing preset load key");
            return false;
         }
         const BuiltinPreset *found = nullptr;
         for (unsigned i = 0; i < kNumBuiltinPresets; ++i) {
            if (std::strcmp(kBuiltinPresets[i].loadKey, loadKey) == 0) {
               found = &kBuiltinPresets[i];
               break;
            }
         }
         if (!found) {
            plug->reportPresetError(locationKind, location, loadKey,
                                    std::string("unknown built-in preset '") + loadKey + "'");
            return false;
         }
         if (!parsePreset(found->text, std::strlen(found->text), preset, &pattern, error)) {
            plug->reportPresetError(locationKind, location, loadKey, error);
            return false;
         }
      } else {
         plug->reportPresetError(locationKind, location, loadKey, "unsupported location kind");
         return false;
      }

      plug->applyPreset(preset);
      plug->applyPattern(pattern);
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

   // --------------------------------------------------------------------- ports

   static uint32_t audioPortsCount(const clap_plugin_t *, bool isInput) {
      return isInput ? 0 : 1;
   }

   static bool audioPortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                             clap_audio_port_info_t *info) {
      if (isInput || index != 0)
         return false;
      std::memset(info, 0, sizeof(*info));
      info->id = 0;
      std::snprintf(info->name, sizeof(info->name), "Out");
      info->flags = CLAP_AUDIO_PORT_IS_MAIN;
      info->channel_count = 2;
      info->port_type = CLAP_PORT_STEREO;
      info->in_place_pair = CLAP_INVALID_ID;
      return true;
   }

   static uint32_t notePortsCount(const clap_plugin_t *, bool isInput) {
      return isInput ? 1 : 0;
   }

   static bool notePortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                            clap_note_port_info_t *info) {
      if (!isInput || index != 0)
         return false;
      std::memset(info, 0, sizeof(*info));
      info->id = 0;
      info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
      info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
      std::snprintf(info->name, sizeof(info->name), "Note In");
      return true;
   }

   static uint32_t tailGet(const clap_plugin_t *p) {
      ThreeOhThreePlugin *plug = self(p);
      const double tail = plug->mEngine.tailSeconds() * plug->mSampleRate;
      return static_cast<uint32_t>(clampv(tail, 0.0, 2.0e9));
   }

   static bool voiceInfoGet(const clap_plugin_t *, clap_voice_info_t *info) {
      info->voice_count = AcidEngine::kMaxVoices;
      info->voice_capacity = AcidEngine::kMaxVoices;
      info->flags = CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES;
      return true;
   }


   // ------------------------------------------------------------- sequencer
   //
   // The clock. Everything here is in *steps* rather than in frames or in
   // seconds, because that is the unit the pattern is written in and the one
   // swing bends: a step's onset is its index, plus a fraction of a step if it
   // is an odd one and swing is on.
   //
   // The position is re-read from the host's beat timeline at the top of every
   // block, so the pattern is locked to the song rather than free-running
   // beside it -- scrubbing, looping and tempo changes all land where they
   // should. Inside the block the loop splits at every step boundary, so notes
   // are sample-accurate and not quantised to the block size.

   bool sequencerMode() const {
      return static_cast<int>(realValue(kParamMode)) == kModeSequencer;
   }

   // Where the odd steps land. 50 % is straight, 66.7 % is triplet swing.
   double swingAmount() const { return (realValue(kParamSwing) * 0.01 - 0.5) * 2.0; }

   double onsetOf(long k) const {
      return static_cast<double>(k) + ((k & 1) ? mSwingAmt : 0.0);
   }

   // The last onset at or before `pos`.
   double lastOnsetAtOrBefore(double pos) const {
      const long k = static_cast<long>(std::floor(pos)) + 2;
      for (int i = 0; i < 6; ++i) {
         const double t = onsetOf(k - i);
         if (t <= pos + 1.0e-9)
            return t;
      }
      return onsetOf(k - 6);
   }

   // The first onset at or after `pos`. Two steps of slack either side, because
   // swing moves an onset later and the search must not start past it.
   double firstOnsetFrom(double pos) const {
      const long k = static_cast<long>(std::floor(pos)) - 2;
      for (int i = 0; i < 6; ++i) {
         const double t = onsetOf(k + i);
         if (t >= pos - 1.0e-9)
            return t;
      }
      return onsetOf(k + 6);
   }

   // Everything the sequencer has sounding, gone -- and gone for certain.
   //
   // Note-offs by key would be enough if the bookkeeping were always right, and
   // it is exactly the place where it was not: releaseAll() cannot leave a
   // stale entry behind. In Sequencer mode the engine's held stack holds
   // nothing but the sequencer's own notes anyway, because MIDI notes only
   // transpose, so there is nothing else to lose.
   void seqStopAll() {
      const bool hadNotes = mSeqKeyA >= 0 || mSeqKeyB >= 0;
      mSeqKeyA = -1;
      mSeqKeyB = -1;
      mPlayhead.store(-1, std::memory_order_relaxed);
      mPlayingPattern.store(-1, std::memory_order_relaxed);
      if (hadNotes)
         mEngine.releaseAll();
   }

   void seqStartStep(double onsetPos) {
      const long k = static_cast<long>(std::floor(onsetPos + 1.0e-6));
      const int len = seqLength();
      // How many times round the pattern we are, which is what the chain
      // advances on, and where in it. Floor division rather than C's truncating
      // one, so a negative position -- a host that lets the playhead run before
      // bar one -- counts backwards instead of mirroring around zero.
      long cycle = k / len;
      if (k < 0 && k % len != 0)
         --cycle;
      const long idx = k - cycle * len;
      const int pattern = chainPatternAt(static_cast<int>(realValue(kParamChainMode)),
                                         seqPattern(), static_cast<int>(realValue(kParamChainLength)),
                                         cycle);
      mLastFired = onsetPos;
      mPlayhead.store(static_cast<int>(idx), std::memory_order_relaxed);
      mPlayingPattern.store(pattern, std::memory_order_relaxed);

      const Step st = seqStep(pattern, static_cast<int>(idx));
      if (st.note < 0)
         return; // a rest. Whatever was sounding still ends at its own gate.

      int key = 36 + st.note + 12 * st.octave + mTranspose;
      key = key < 0 ? 0 : (key > 127 ? 127 : key);

      // Where this note's gate ends. A slid step holds past the *next* step's
      // start, and that overlap is the whole mechanism: the engine reads a note
      // arriving while another is held as a slide and does not retrigger.
      const double nextOnset = firstOnsetFrom(onsetPos + 1.0e-6);
      const double gate = clampv(realValue(kParamGate), 0.05, 1.0);
      const double off =
         st.slide ? nextOnset + 0.02 : onsetPos + gate * (nextOnset - onsetPos);

      // Slot B is the note a slide is moving away from. If it is somehow still
      // occupied, release it before it is overwritten -- an overwritten slot is
      // a note that never gets its note-off, and one of those is enough to make
      // every later note look like a slide for ever.
      if (mSeqKeyA >= 0) {
         if (mSeqKeyB >= 0)
            mEngine.noteOff(0, 0, static_cast<int16_t>(mSeqKeyB), -1);
         mSeqKeyB = mSeqKeyA;
         mSeqOffB = mSeqOffA;
      }
      mSeqKeyA = key;
      mSeqOffA = off;
      mEngine.noteOnStep(static_cast<int16_t>(key), st.accent, st.vibrato);
   }

   // Note-ons before note-offs, always: releasing first would break a slide,
   // because the overlap is what the engine reads.
   void seqFireDue() {
      int guard = 0;
      while (mStepPos >= mNextOn - 1.0e-9 && guard++ <= kMaxSteps) {
         const double fired = mNextOn;
         seqStartStep(fired);
         mNextOn = firstOnsetFrom(fired + 1.0e-6);
      }
      if (mSeqKeyB >= 0 && mStepPos >= mSeqOffB - 1.0e-9) {
         mEngine.noteOff(0, 0, static_cast<int16_t>(mSeqKeyB), -1);
         mSeqKeyB = -1;
      }
      if (mSeqKeyA >= 0 && mStepPos >= mSeqOffA - 1.0e-9) {
         mEngine.noteOff(0, 0, static_cast<int16_t>(mSeqKeyA), -1);
         mSeqKeyA = -1;
      }
   }

   double seqNextBoundary() const {
      double b = mNextOn;
      if (mSeqKeyB >= 0 && mSeqOffB < b)
         b = mSeqOffB;
      if (mSeqKeyA >= 0 && mSeqOffA < b)
         b = mSeqOffA;
      return b;
   }

   void updateSequencerClock(const clap_process_t *pr) {
      if (!sequencerMode()) {
         if (mSeqRunning) {
            seqStopAll();
            mSeqRunning = false;
         }
         return;
      }

      mSwingAmt = swingAmount();
      const double perBeat = stepsPerBeat(static_cast<int>(realValue(kParamSeqRate)));

      bool playing = false;
      const clap_event_transport_t *tr = pr ? pr->transport : nullptr;
      if (tr && (tr->flags & CLAP_TRANSPORT_IS_PLAYING)) {
         if ((tr->flags & CLAP_TRANSPORT_HAS_TEMPO) && tr->tempo > 1.0)
            mTempo = tr->tempo;
         if (tr->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
            const double beats =
               static_cast<double>(tr->song_pos_beats) / static_cast<double>(CLAP_BEATTIME_FACTOR);
            const double fromSong = beats * perBeat;
            // A loop, a seek, or the playhead dragged: the position moves by
            // something other than the time that has passed.
            //
            // Everything the sequencer has scheduled -- the pending note-offs,
            // and the record of what has already fired -- is a position on this
            // same timeline, so a jump moves all of it by the same amount. It
            // does *not* invalidate any of it.
            //
            // Getting this wrong cost two bugs in a row. Leaving the offs where
            // they were put them out of reach after a backwards jump, so their
            // notes were never released: one stale held note makes every later
            // note look like a slide, nothing retriggers, and the instrument
            // fades out over about twenty seconds. Throwing them away instead
            // killed a note that had legitimately just started -- when the loop
            // is a whole number of patterns long, the position before the jump
            // and the position after it are the *same step*, and the note
            // across the seam has to play on. Rebasing is the only answer that
            // is right in both cases.
            //
            // Half a step of tolerance: ordinary block-to-block drift is a
            // rounding error.
            if (mSeqRunning) {
               const double delta = fromSong - mStepPos;
               if (std::fabs(delta) > 0.5) {
                  mSeqOffA += delta;
                  mSeqOffB += delta;
                  mLastFired += delta;
               }
            }
            mStepPos = fromSong;
         }
         playing = true;
      } else if (mHeldKeyCount > 0) {
         // No transport, or a stopped one: a held note runs the pattern anyway,
         // so it can be auditioned without putting the song into play.
         playing = true;
      }

      if (playing && !mSeqRunning) {
         if (!(tr && (tr->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)))
            mStepPos = 0.0;
         mLastFired = -1.0e18;
      }
      if (!playing && mSeqRunning)
         seqStopAll();
      mSeqRunning = playing;

      if (mSeqRunning) {
         // A last guarantee, and a cheap one. If the sequencer believes nothing
         // of its own is sounding, then nothing should be held -- in this mode
         // the engine's stack is the sequencer's alone. Anything left in it is
         // a leak from a path not thought of, and one stale note is enough to
         // wedge the instrument until it is reloaded, so it is cleared here
         // rather than trusted not to happen.
         if (mSeqKeyA < 0 && mSeqKeyB < 0 && mEngine.heldCount() > 0)
            mEngine.releaseAll();

         mNextOn = firstOnsetFrom(mStepPos);
         // Without this a block ending exactly on an onset would fire it, and
         // the next block, starting on the same position, would fire it again.
         if (mNextOn <= mLastFired + 1.0e-9)
            mNextOn = firstOnsetFrom(mLastFired + 1.0e-6);
         // And the other way round: an onset just behind us that has never
         // fired still has to. A host's loop point almost never lands on a
         // block boundary, so the first step after a loop is typically a few
         // samples in the past by the time the new position is read -- firing
         // it a fraction of a step late is right, and dropping it is not.
         const double behind = lastOnsetAtOrBefore(mStepPos);
         if (behind > mLastFired + 1.0e-9 && mStepPos - behind < 0.5)
            mNextOn = behind;
      }

      const double sr = mSampleRate > 0.0 ? mSampleRate : 48000.0;
      mFramesPerStep = 60.0 / (mTempo > 1.0 ? mTempo : 120.0) / perBeat * sr;
      if (mFramesPerStep < 8.0)
         mFramesPerStep = 8.0;
   }

   void pushTranspose(int16_t key) {
      if (mHeldKeyCount < static_cast<int>(sizeof(mHeldKeys) / sizeof(mHeldKeys[0])))
         mHeldKeys[mHeldKeyCount++] = key;
      if (mHeldKeyCount > 0)
         mTranspose = mHeldKeys[mHeldKeyCount - 1] - 36;
   }

   void popTranspose(int16_t key) {
      for (int i = 0; i < mHeldKeyCount; ++i) {
         if (mHeldKeys[i] == key) {
            for (int j = i + 1; j < mHeldKeyCount; ++j)
               mHeldKeys[j - 1] = mHeldKeys[j];
            --mHeldKeyCount;
            break;
         }
      }
      mTranspose = mHeldKeyCount > 0 ? mHeldKeys[mHeldKeyCount - 1] - 36 : 0;
   }

   // --------------------------------------------------------- PatternAccess

   Step seqStep(int pattern, int index) const override {
      if (pattern < 0 || pattern >= kMaxPatterns || index < 0 || index >= kMaxSteps)
         return Step();
      return Step::unpack(mPattern[pattern][index].load(std::memory_order_relaxed));
   }

   void seqSetStep(int pattern, int index, const Step &step) override {
      if (pattern < 0 || pattern >= kMaxPatterns || index < 0 || index >= kMaxSteps)
         return;
      mPattern[pattern][index].store(step.pack(), std::memory_order_relaxed);
      mPresetEdited = true;
   }

   // Zero based; the parameter counts from one, because that is how a pattern
   // is named on every machine that has ever had more than one.
   int seqPattern() const override {
      const int n = static_cast<int>(realValue(kParamPattern)) - 1;
      return n < 0 ? 0 : (n >= kMaxPatterns ? kMaxPatterns - 1 : n);
   }

   int seqPlayingPattern() const override { return mPlayingPattern.load(std::memory_order_relaxed); }

   bool seqPatternEmpty(int pattern) const override {
      if (pattern < 0 || pattern >= kMaxPatterns)
         return true;
      uint16_t packed[kMaxSteps];
      for (int i = 0; i < kMaxSteps; ++i)
         packed[i] = mPattern[pattern][i].load(std::memory_order_relaxed);
      return patternEmpty(packed);
   }

   int seqLength() const override {
      const int n = static_cast<int>(realValue(kParamSeqSteps));
      return n < 1 ? 1 : (n > kMaxSteps ? kMaxSteps : n);
   }

   int seqPlayhead() const override { return mPlayhead.load(std::memory_order_relaxed); }

   bool seqEnabled() const override { return sequencerMode(); }


   // ------------------------------------------------------- pattern generator
   //
   // A seed and six densities, turned into sixteen steps. Two things make the
   // difference between this and a random number generator wired to a piano
   // roll:
   //
   // 1. Every step draws all six of its values whether or not it uses them, so
   //    nudging one density changes only what that density controls. Turning
   //    Accents up must not reshuffle the notes, or the thing is a slot machine.
   //
   // 2. It knows a few things about bass lines. The root comes up far more
   //    often than any other degree; the first step of each beat is more likely
   //    to sound and more likely to be the root; octave jumps prefer accented
   //    notes, because accent-plus-octave is *the* gesture; and a slide into a
   //    rest is removed afterwards, because it slides into nothing.
   //
   // Uniform noise over twelve semitones does not sound like a bass line and
   // never did.

   int seqSeed() const override { return static_cast<int>(realValue(kParamRandSeed)); }

   void seqSetSeed(int seed) override {
      const ParamDesc &d = paramTable()[kParamRandSeed];
      const int wrapped = seed < static_cast<int>(d.min)
                             ? static_cast<int>(d.max)
                             : (seed > static_cast<int>(d.max) ? static_cast<int>(d.min) : seed);
      const double v = static_cast<double>(wrapped);
      mValues[kParamRandSeed].store(v, std::memory_order_relaxed);
      mParamsDirty.store(true, std::memory_order_release);
      // Through the host, so automation and undo see it like any knob move.
      pushGuiEdit(kParamRandSeed, 0.0, EditKind::GestureBegin);
      pushGuiEdit(kParamRandSeed, v, EditKind::Value);
      pushGuiEdit(kParamRandSeed, 0.0, EditKind::GestureEnd);
      seqGenerate();
   }

   void seqGenerate() override {
      GenSettings g;
      g.seed = static_cast<uint32_t>(realValue(kParamRandSeed));
      g.scale = static_cast<int>(realValue(kParamRandScale));
      g.root = static_cast<int>(realValue(kParamRandRoot));
      g.notes = realValue(kParamRandNotes);
      g.accent = realValue(kParamRandAccent);
      g.slide = realValue(kParamRandSlide);
      g.octave = realValue(kParamRandOctave);
      g.vibrato = realValue(kParamRandVibrato);

      uint16_t steps[kMaxSteps];
      generatePattern(g, steps);
      const int pattern = seqPattern();
      for (int i = 0; i < kMaxSteps; ++i)
         mPattern[pattern][i].store(steps[i], std::memory_order_relaxed);
      mPresetEdited = true;
   }

   // ------------------------------------------------------------------ process

   void handleEvent(const clap_event_header_t *hdr) {
      if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
         return;

      switch (hdr->type) {
      case CLAP_EVENT_NOTE_ON: {
         const auto *ev = reinterpret_cast<const clap_event_note_t *>(hdr);
         if (sequencerMode()) {
            // The pattern is the player. A note does not sound -- it moves the
            // whole pattern, which is what the machine's own keyboard did.
            pushTranspose(ev->key);
         } else {
            mEngine.noteOn(ev->port_index, ev->channel, ev->key, ev->note_id, ev->velocity);
         }
         break;
      }
      case CLAP_EVENT_NOTE_OFF: {
         const auto *ev = reinterpret_cast<const clap_event_note_t *>(hdr);
         if (sequencerMode())
            popTranspose(ev->key);
         else
            mEngine.noteOff(ev->port_index, ev->channel, ev->key, ev->note_id);
         break;
      }
      case CLAP_EVENT_NOTE_CHOKE: {
         const auto *ev = reinterpret_cast<const clap_event_note_t *>(hdr);
         if (sequencerMode())
            popTranspose(ev->key);
         else
            mEngine.choke(ev->port_index, ev->channel, ev->key, ev->note_id);
         break;
      }
      case CLAP_EVENT_PARAM_VALUE: {
         const auto *ev = reinterpret_cast<const clap_event_param_value_t *>(hdr);
         const ParamDesc *d = paramById(ev->param_id);
         if (!d)
            break;
         mValues[ev->param_id].store(clampv(ev->value, d->min, d->max),
                                     std::memory_order_relaxed);
         mParamsDirty.store(true, std::memory_order_relaxed);
         break;
      }
      case CLAP_EVENT_PARAM_MOD: {
         const auto *ev = reinterpret_cast<const clap_event_param_mod_t *>(hdr);
         if (ev->note_id >= 0)
            break; // per-note modulation is not supported
         if (!paramById(ev->param_id))
            break;
         mMods[ev->param_id].store(ev->amount, std::memory_order_relaxed);
         mParamsDirty.store(true, std::memory_order_relaxed);
         break;
      }
      case CLAP_EVENT_MIDI: {
         const auto *ev = reinterpret_cast<const clap_event_midi_t *>(hdr);
         const uint8_t status = ev->data[0] & 0xF0;
         const int16_t channel = static_cast<int16_t>(ev->data[0] & 0x0F);
         const int16_t key = static_cast<int16_t>(ev->data[1] & 0x7F);
         const uint8_t vel = ev->data[2] & 0x7F;
         if (status == 0x90 && vel > 0) {
            if (sequencerMode())
               pushTranspose(key);
            else
               mEngine.noteOn(ev->port_index, channel, key, -1, vel / 127.0);
         } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
            if (sequencerMode())
               popTranspose(key);
            else
               mEngine.noteOff(ev->port_index, channel, key, -1);
         } else if (status == 0xB0 && ev->data[1] == 1) {
            // CC1. In MIDI mode this is the vibrato, since there is no step to
            // carry a vibrato bit.
            mEngine.setModWheel(vel / 127.0f);
         } else if (status == 0xB0 && (ev->data[1] == 120 || ev->data[1] == 123)) {
            mHeldKeyCount = 0;
            seqStopAll();
            mEngine.allSoundOff();
         }
         break;
      }
      default:
         break;
      }
   }

   clap_process_status process(const clap_process_t *pr) {
      if (!pr || pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 2)
         return CLAP_PROCESS_ERROR;

      // Contained to this call; the host's FPU mode is restored on the way out.
      const ScopedNoDenormals noDenormals;

      float *outL = pr->audio_outputs[0].data32[0];
      float *outR = pr->audio_outputs[0].data32[1];
      if (!outL || !outR)
         return CLAP_PROCESS_ERROR;

      const uint32_t numFrames = pr->frames_count;
      const clap_input_events_t *in = pr->in_events;
      const uint32_t numEvents = in ? in->size(in) : 0;

      drainGuiEdits(pr->out_events, 0);

      // Anything the editor has asked for since the last block. Cleared with a
      // single exchange, so a click landing during this loop is served by the
      // next block rather than lost.
      uint32_t shots = mShotRequests.exchange(0, std::memory_order_acquire);
      if (shots > kMaxShotsPerBlock)
         shots = kMaxShotsPerBlock;
      for (uint32_t i = 0; i < shots; ++i)
         mEngine.triggerShot();

      uint32_t eventIndex = 0;
      uint32_t frame = 0;
      bool clockUpdated = false;

      while (frame < numFrames) {
         // Consume every event scheduled at or before the current frame, so
         // parameter changes and notes land sample-accurately.
         while (eventIndex < numEvents) {
            const clap_event_header_t *hdr = in->get(in, eventIndex);
            if (!hdr || hdr->time > frame)
               break;
            handleEvent(hdr);
            ++eventIndex;
         }

         // The clock is read once per block, but *after* the events landing on
         // frame 0 -- a Mode or a Rate arriving on the first sample has to be
         // seen by the first step rather than a block later, and reading it
         // before the events meant the first step of a pattern was silently
         // dropped every time the mode was switched on.
         if (!clockUpdated) {
            updateSequencerClock(pr);
            clockUpdated = true;
         }

         uint32_t next = numFrames;
         if (eventIndex < numEvents) {
            const clap_event_header_t *hdr = in->get(in, eventIndex);
            if (hdr && hdr->time > frame && hdr->time < numFrames)
               next = hdr->time;
         }

         if (mParamsDirty.exchange(false, std::memory_order_acq_rel))
            syncEngineParams();

         // Split the block again at the next step boundary, so a note lands on
         // the sample it is due on rather than on the next buffer.
         if (mSeqRunning) {
            seqFireDue();
            const double ahead = (seqNextBoundary() - mStepPos) * mFramesPerStep;
            if (ahead >= 0.0 && ahead < static_cast<double>(numFrames)) {
               const uint32_t at = frame + static_cast<uint32_t>(std::ceil(ahead));
               if (at > frame && at < next)
                  next = at;
            }
         }

         const uint32_t n = next - frame;
         std::memset(outL + frame, 0, n * sizeof(float));
         std::memset(outR + frame, 0, n * sizeof(float));
         mEngine.process(outL + frame, outR + frame, n);
         if (mSeqRunning)
            mStepPos += static_cast<double>(n) / mFramesPerStep;
         frame = next;
      }

      // Published for the window's activity meter; the GUI never reads engine
      // state directly.
      mVoiceMeter.store(mEngine.activeVoiceCount(), std::memory_order_relaxed);
      mNoteCounterMeter.store(mEngine.noteCounter(), std::memory_order_relaxed);
      publishOutputPeaks(outL, outR, numFrames);

      return mEngine.isSilent() ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
   }


   // ----------------------------------------------------------- preset list
   //
   // What the window's browser shows: the factory library, which is embedded in
   // the binary, followed by whatever the user has put in their own preset
   // directory. Loading goes through the same preset-load path a host uses, so
   // there is exactly one code path for it.

   void ensurePresetList() {
      if (mPresetsScanned)
         return;
      mPresetsScanned = true;
#ifdef THREEOHTHREE_WITH_GUI
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
         mPresets.push_back(entry);
      }

      const std::string dir = userPresetDir();
      std::error_code ec;
      if (dir.empty() || !std::filesystem::is_directory(dir, ec))
         return;
      std::vector<GuiPreset> user;
      const std::string suffix = std::string(".") + kPresetExtension;
      for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
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
         user.push_back(item);
      }
      std::sort(user.begin(), user.end(),
                [](const GuiPreset &a, const GuiPreset &b) { return a.name < b.name; });
      mPresets.insert(mPresets.end(), user.begin(), user.end());
#endif
   }

   void notePresetLoaded(uint32_t locationKind, const char *loadKey, const char *location) {
#ifdef THREEOHTHREE_WITH_GUI
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
      mPresetEdited = false;
#else
      (void)locationKind;
      (void)loadKey;
      (void)location;
#endif
   }

   // A peak meter that fell as fast as the signal would be unreadable, so the
   // published value decays towards the true peak over about 350 ms instead.
   void publishOutputPeaks(const float *l, const float *r, uint32_t frames) {
      if (frames == 0)
         return;
      float peakL = 0.0f;
      float peakR = 0.0f;
      for (uint32_t i = 0; i < frames; ++i) {
         peakL = std::max(peakL, std::fabs(l[i]));
         peakR = std::max(peakR, std::fabs(r[i]));
      }
      const double seconds = static_cast<double>(frames) / (mSampleRate > 0 ? mSampleRate : 48000.0);
      const float fall = static_cast<float>(std::exp(-seconds / 0.35));
      mFallingPeakL = std::max(peakL, mFallingPeakL * fall);
      mFallingPeakR = std::max(peakR, mFallingPeakR * fall);
      mPeakL.store(mFallingPeakL, std::memory_order_relaxed);
      mPeakR.store(mFallingPeakR, std::memory_order_relaxed);
   }

#ifdef THREEOHTHREE_WITH_GUI
   // Which windowing system this build's window speaks, and how the host's
   // parent handle reaches it. The window itself is in src/gui/gui.cpp.
#if defined(_WIN32)
   static constexpr const char *kWindowApi = CLAP_WINDOW_API_WIN32;
   static uintptr_t nativeHandle(const clap_window_t &w) {
      return reinterpret_cast<uintptr_t>(w.win32);
   }
#else
   static constexpr const char *kWindowApi = CLAP_WINDOW_API_X11;
   static uintptr_t nativeHandle(const clap_window_t &w) {
      return static_cast<uintptr_t>(w.x11);
   }
#endif

   // -------------------------------------------------------- GuiDelegate

   double guiParamValue(uint32_t id) const override {
      return id < kNumParams ? mValues[id].load(std::memory_order_relaxed) : 0.0;
   }

   void guiBeginEdit(uint32_t id) override { pushGuiEdit(id, 0.0, EditKind::GestureBegin); }

   void guiSetParam(uint32_t id, double value) override {
      const ParamDesc *d = paramById(id);
      if (!d)
         return;
      const double clamped = clampv(value, d->min, d->max);
      mValues[id].store(clamped, std::memory_order_relaxed);
      mParamsDirty.store(true, std::memory_order_release);
      mPresetEdited = true;
      pushGuiEdit(id, clamped, EditKind::Value);
   }

   void guiEndEdit(uint32_t id) override { pushGuiEdit(id, 0.0, EditKind::GestureEnd); }

   void guiOutputPeaks(float &left, float &right) const override {
      left = mPeakL.load(std::memory_order_relaxed);
      right = mPeakR.load(std::memory_order_relaxed);
   }

   uint32_t guiVoiceCount() const override {
      return mVoiceMeter.load(std::memory_order_relaxed);
   }

   // One voice, because the machine has one. The meter therefore reads as a
   // gate lamp rather than as a load, which is the honest thing for it to show.
   uint32_t guiVoiceLimit() const override { return AcidEngine::kMaxVoices; }

   // The header ornament redraws its filter curve on every note, so it needs
   // the count rather than only the gate.
   uint32_t guiEventCounter() const override {
      return mNoteCounterMeter.load(std::memory_order_relaxed);
   }

   // Clicking the version label plays one free note -- low and accented, so it
   // demonstrates what the preset actually does. The window runs on the main
   // thread and must not touch the engine, so the click is counted here and the
   // audio thread consumes the count on its next process(). request_process()
   // is what makes it work with nothing held: the engine reports silence when
   // no note is sounding, so the host may well have stopped calling process.
   void guiVersionClicked() override {
      mShotRequests.fetch_add(1, std::memory_order_relaxed);
      if (mHost && mHost->request_process)
         mHost->request_process(mHost);
   }

   std::string guiSuggestedPresetName() const override {
      if (mCurrentPreset >= 0 && mCurrentPreset < static_cast<int>(mPresets.size()))
         return mPresets[static_cast<size_t>(mCurrentPreset)].name;
      return "My Line";
   }

   bool guiSavePreset(const std::string &name, std::string &error) override {
      const std::string path = userPresetPath(name);
      if (path.empty()) {
         error = "No user preset directory: neither XDG_CONFIG_HOME nor HOME is set.";
         return false;
      }

      // Everything the engine is currently using, written as the user's own
      // preset. Author and description are left out: they belong to whoever
      // wrote the preset this was derived from, not to this copy.
      PresetData data;
      data.name = name;
      for (uint32_t i = 0; i < kNumParams; ++i) {
         const uint32_t id = paramTable()[i].id;
         data.values.emplace_back(id, mValues[id].load(std::memory_order_relaxed));
      }
      PatternData pattern;
      pattern.present = true;
      for (int i = 0; i < kMaxPatterns * kMaxSteps; ++i)
         pattern.steps[i] = mPattern[i / kMaxSteps][i % kMaxSteps].load(std::memory_order_relaxed);

      if (!writePresetFile(path, formatPreset(data, &pattern), error))
         return false;

      // Rescan so the browser shows it at once, and select what was just saved.
      mPresets.clear();
      mPresetsScanned = false;
      ensurePresetList();
      mCurrentPreset = -1;
      for (size_t i = 0; i < mPresets.size(); ++i) {
         if (mPresets[i].path == path) {
            mCurrentPreset = static_cast<int>(i);
            break;
         }
      }
      mPresetEdited = false;
      return true;
   }

   const std::vector<GuiPreset> &guiPresets() const override { return mPresets; }
   int guiCurrentPreset() const override { return mCurrentPreset; }
   bool guiPresetEdited() const override { return mPresetEdited; }

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

   // -------------------------------------------------------------- WindowHost

   // The window changed height because its collapsible section opened or
   // closed. A CLAP editor cannot resize itself, so this asks: the hints go
   // first because the aspect ratio the host is holding the window to has just
   // changed, and a request measured against the old one would be snapped
   // straight back.
   void windowRequestResize(uint32_t width, uint32_t height) override {
      if (!mHost)
         return;
      auto *hostGui =
         static_cast<const clap_host_gui_t *>(mHost->get_extension(mHost, CLAP_EXT_GUI));
      if (!hostGui)
         return;
      if (hostGui->resize_hints_changed)
         hostGui->resize_hints_changed(mHost);
      if (hostGui->request_resize)
         hostGui->request_resize(mHost, width, height);
   }

   bool windowAdvancedOpen() const override {
      return mAdvancedOpen.load(std::memory_order_relaxed);
   }

   void windowSetAdvancedOpen(bool open) override {
      mAdvancedOpen.store(open, std::memory_order_relaxed);
   }

   // ----------------------------------------------------------- gui extension

   static bool guiIsApiSupported(const clap_plugin_t *, const char *api, bool isFloating) {
      // Embedded X11 only. A floating window would mean owning a top-level
      // window and its focus behaviour, which is the host's job here.
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
      ThreeOhThreePlugin *plug = self(p);
      if (plug->mGui)
         return true;
      plug->ensurePresetList();
      plug->mGui = threeohthree::createGui(*plug, *plug, *plug);
      if (!plug->mGui)
         return false;
      plug->startGuiClock();
      return true;
   }

   static void guiDestroy(const clap_plugin_t *p) {
      ThreeOhThreePlugin *plug = self(p);
      plug->stopGuiClock();
      delete plug->mGui;
      plug->mGui = nullptr;
   }

   static bool guiSetScale(const clap_plugin_t *p, double scale) {
      ThreeOhThreePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->setScale(scale);
      return true;
   }

   static bool guiGetSize(const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
      ThreeOhThreePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->size(width, height);
      return true;
   }

   // The window resizes by zooming: one layout, one cairo scale, so it keeps
   // its aspect ratio and the host is told so. adjust_size snaps whatever the
   // host proposes to the nearest size the window can take, and set_size then
   // takes it.
   static bool guiCanResize(const clap_plugin_t *) { return true; }

   static bool guiGetResizeHints(const clap_plugin_t *p, clap_gui_resize_hints_t *hints) {
      ThreeOhThreePlugin *plug = self(p);
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
      ThreeOhThreePlugin *plug = self(p);
      if (!plug->mGui || !width || !height)
         return false;
      plug->mGui->fitSize(width, height);
      return true;
   }

   static bool guiSetSize(const clap_plugin_t *p, uint32_t width, uint32_t height) {
      ThreeOhThreePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      return plug->mGui->resize(width, height);
   }

   static bool guiSetParent(const clap_plugin_t *p, const clap_window_t *window) {
      ThreeOhThreePlugin *plug = self(p);
      if (!plug->mGui || !window || std::strcmp(window->api, kWindowApi) != 0)
         return false;
      return plug->mGui->embed(nativeHandle(*window));
   }

   static bool guiSetTransient(const clap_plugin_t *p, const clap_window_t *window) {
      ThreeOhThreePlugin *plug = self(p);
      if (!plug->mGui || !window || std::strcmp(window->api, kWindowApi) != 0)
         return false;
      return plug->mGui->setTransientFor(nativeHandle(*window));
   }

   static void guiSuggestTitle(const clap_plugin_t *p, const char *title) {
      ThreeOhThreePlugin *plug = self(p);
      if (plug->mGui)
         plug->mGui->setTitle(title);
   }

   static bool guiShow(const clap_plugin_t *p) {
      ThreeOhThreePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->show();
      return true;
   }

   static bool guiHide(const clap_plugin_t *p) {
      ThreeOhThreePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->hide();
      return true;
   }

   // ------------------------------------------------------------- gui clock
   //
   // The window is repainted from the host's timer, which is the main thread
   // and so the only place CLAP allows GUI work. Hosts are not required to
   // offer timers, though, and a window that never repaints is useless -- so
   // there is a fallback thread with its own X11 connection for those.

   static void guiOnTimer(const clap_plugin_t *p, clap_id timerId) {
      ThreeOhThreePlugin *plug = self(p);
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
#endif // THREEOHTHREE_WITH_GUI

   // ---------------------------------------------------------------- extensions

   const void *getExtension(const char *id) const {
      static const clap_plugin_params_t kParamsExt = {
         paramsCount, paramsGetInfo, paramsGetValue, paramsValueToText, paramsTextToValue,
         paramsFlush};
      static const clap_plugin_audio_ports_t kAudioPortsExt = {audioPortsCount, audioPortsGet};
      static const clap_plugin_note_ports_t kNotePortsExt = {notePortsCount, notePortsGet};
      static const clap_plugin_state_t kStateExt = {stateSave, stateLoad};
      static const clap_plugin_tail_t kTailExt = {tailGet};
      static const clap_plugin_voice_info_t kVoiceInfoExt = {voiceInfoGet};
      static const clap_plugin_preset_load_t kPresetLoadExt = {presetLoadFromLocation};
#ifdef THREEOHTHREE_WITH_GUI
      static const clap_plugin_gui_t kGuiExt = {
         guiIsApiSupported, guiGetPreferredApi, guiCreate,      guiDestroy,
         guiSetScale,       guiGetSize,         guiCanResize,   guiGetResizeHints,
         guiAdjustSize,     guiSetSize,         guiSetParent,   guiSetTransient,
         guiSuggestTitle,   guiShow,            guiHide};
      static const clap_plugin_timer_support_t kTimerExt = {guiOnTimer};
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
      if (std::strcmp(id, CLAP_EXT_VOICE_INFO) == 0)
         return &kVoiceInfoExt;
      if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0 ||
          std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0)
         return &kPresetLoadExt;
#ifdef THREEOHTHREE_WITH_GUI
      if (std::strcmp(id, CLAP_EXT_GUI) == 0)
         return &kGuiExt;
      if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
         return &kTimerExt;
#endif
      return nullptr;
   }

   clap_plugin_t mPlugin{};
   const clap_host_t *mHost = nullptr;

   std::atomic<double> mValues[kNumParams];
   std::atomic<double> mMods[kNumParams];
   std::atomic<bool> mParamsDirty{true};
   std::atomic<uint32_t> mVoiceMeter{0};
   std::atomic<uint32_t> mNoteCounterMeter{0};
   // Free phrases requested by the editor, drained by the audio thread. Capped
   // per block so that a stuck mouse button cannot spawn thousands in one go.
   static constexpr uint32_t kMaxShotsPerBlock = 4;
   std::atomic<uint32_t> mShotRequests{0};
   std::atomic<float> mPeakL{0.0f};
   std::atomic<float> mPeakR{0.0f};
   float mFallingPeakL = 0.0f; // audio thread only
   float mFallingPeakR = 0.0f;

   // The sequencer. The pattern is read by the audio thread and written by the
   // window, one relaxed atomic word per step.
   std::atomic<uint16_t> mPattern[kMaxPatterns][kMaxSteps];
   std::atomic<int> mPlayhead{-1};
   // Which pattern is sounding. Written by the audio thread when a step fires
   // and read by the window, which draws the bank grid from it; -1 when the
   // sequencer is not running.
   std::atomic<int> mPlayingPattern{-1};
   // Whether the window's collapsible panel section is open. Not a parameter
   // and not part of a preset -- it is how the editor was left, nothing about
   // the sound -- but it belongs in the plugin's state so a reopened project
   // looks the way it was closed.
   std::atomic<bool> mAdvancedOpen{false};
   bool mSeqRunning = false;
   double mStepPos = 0.0;      // absolute position, in steps
   double mNextOn = 0.0;
   double mLastFired = -1.0e18;
   double mSwingAmt = 0.0;
   double mFramesPerStep = 1000.0;
   double mTempo = 120.0;
   int mSeqKeyA = -1;          // the sounding note
   int mSeqKeyB = -1;          // the one a slide is moving away from
   double mSeqOffA = 0.0;
   double mSeqOffB = 0.0;
   int mTranspose = 0;
   int mHeldKeyCount = 0;
   int mHeldKeys[16] = {0};

   ParamEdit mEditQueue[kEditQueueSize];
   std::atomic<uint32_t> mEditWrite{0};
   std::atomic<uint32_t> mEditRead{0};

   bool mPresetsScanned = false;
   int mCurrentPreset = -1;
   bool mPresetEdited = false;
#ifdef THREEOHTHREE_WITH_GUI
   std::vector<GuiPreset> mPresets;
   Gui *mGui = nullptr;
   clap_id mTimerId = CLAP_INVALID_ID;
   std::thread mGuiThread;
   std::atomic<bool> mGuiThreadRun{false};
#endif

   AcidEngine mEngine;
   double mSampleRate = 48000.0;
};

// ------------------------------------------------------------- plugin factory

namespace {

uint32_t factoryCount(const clap_plugin_factory_t *) { return 1; }

const clap_plugin_descriptor_t *factoryGetDescriptor(const clap_plugin_factory_t *,
                                                     uint32_t index) {
   return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t *factoryCreate(const clap_plugin_factory_t *, const clap_host_t *host,
                                   const char *pluginId) {
   if (!host || !clap_version_is_compatible(host->clap_version))
      return nullptr;
   if (!pluginId || std::strcmp(pluginId, kPluginId) != 0)
      return nullptr;
   auto *plug = new ThreeOhThreePlugin(host);
   return plug->clapPlugin();
}

} // namespace

const clap_plugin_factory_t gPluginFactory = {factoryCount, factoryGetDescriptor, factoryCreate};

} // namespace threeohthree

// --------------------------------------------------------------------- entry
//
// The clap_entry structure itself is not here: it is assembled per plugin
// format from the three functions below, so that the .clap and the .vst3 can be
// built from one copy of the plugin. See entry.h.

extern "C" {

bool threeohthreeEntryInit(const char *) { return true; }
void threeohthreeEntryDeinit() {}

const void *threeohthreeEntryGetFactory(const char *factoryId) {
   if (!factoryId)
      return nullptr;
   if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
      return &threeohthree::gPluginFactory;
   if (std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0 ||
       std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0)
      return threeohthree::presetDiscoveryFactory();
   return nullptr;
}
}
