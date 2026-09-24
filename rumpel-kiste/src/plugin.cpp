#include <algorithm>
#include <atomic>
// <chrono> is used by the pattern generator's salt, which is compiled whether
// or not this build has a window; on mingw it does not arrive transitively.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "plugincore/dsp/denormals.h"
#include "plugincore/dsp/fastmath.h"
#include "dsp/drums.h"
#include "entry.h"
#include "factories.h"
#include "midifile.h"
#include "params.h"
#include "presets_generated.h"
#include "rumpelkiste.h"

#ifdef RUMPELKISTE_WITH_GUI
#include <thread>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "gui/gui.h"
#endif

namespace rumpelkiste {

namespace {

const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                 CLAP_PLUGIN_FEATURE_DRUM_MACHINE,
                                 CLAP_PLUGIN_FEATURE_DRUM,
                                 CLAP_PLUGIN_FEATURE_STEREO,
                                 "analog",
                                 nullptr};

const clap_plugin_descriptor_t kDescriptor = {
   CLAP_VERSION_INIT,  kPluginId,          kPluginName, kPluginVendor,
   kPluginUrl,         kPluginUrl,         kPluginUrl,  kPluginVersion,
   kPluginDescription, kFeatures,
};

// What the browser calls the presets compiled into the binary.
constexpr const char *kFactoryFolder = "Factory Presets";

constexpr uint32_t kStateMagic = 0x54534B52u; // 'RKST' little-endian
// Version 1. The layout: header, one (id, value) pair per parameter, the
// sixty-four patterns of the bank at kMaxSteps words each, one byte for
// whether the collapsible section is open, the pattern map (one signed byte
// per MIDI note), each pattern's chain (a mode byte and a 16-bit repeat), and
// one byte per voice for its mute. Anything appended later goes after that,
// and a shorter blob stops where it stops and leaves the rest at defaults.
constexpr uint32_t kStateVersion = 1;

} // namespace

#ifdef RUMPELKISTE_WITH_GUI
class RumpelKistePlugin final : public GuiDelegate, public PatternAccess, public WindowHost {
#else
class RumpelKistePlugin : public PatternAccess {
#endif
public:
   explicit RumpelKistePlugin(const clap_host_t *host) : mHost(host) {
      {
         // Pattern 1 shows what the instrument does; the other sixty-three
         // start empty.
         uint32_t seed[kMaxSteps];
         defaultPattern(seed);
         for (int p = 0; p < kMaxPatterns; ++p)
            for (int i = 0; i < kMaxSteps; ++i)
               mPattern[p][i].store(p == 0 ? seed[i] : 0u, std::memory_order_relaxed);
      }
      for (uint32_t i = 0; i < kNumParams; ++i) {
         mValues[i].store(paramTable()[i].def, std::memory_order_relaxed);
         mMods[i].store(0.0, std::memory_order_relaxed);
      }
      for (int note = 0; note < 128; ++note)
         mNoteMap[note].store(static_cast<int8_t>(kNoteNone), std::memory_order_relaxed);
      for (int v = 0; v < kNumVoices; ++v) {
         mMuted[v].store(false, std::memory_order_relaxed);
         mHits[v].store(0, std::memory_order_relaxed);
      }
      setChainForAll(kChainStay, 1);
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
         RumpelKistePlugin *plug = self(p);
         plug->seqStopAll();
         plug->mSeqRunning = false;
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

#ifdef RUMPELKISTE_WITH_GUI
   ~RumpelKistePlugin() override {
      stopGuiClock();
      delete mGui;
   }
#endif

   const clap_plugin_t *clapPlugin() const { return &mPlugin; }

private:
   static RumpelKistePlugin *self(const clap_plugin_t *p) {
      return static_cast<RumpelKistePlugin *>(p->plugin_data);
   }

   // ---------------------------------------------------------------- lifecycle

   bool init() { return true; }

   bool activate(double sampleRate, uint32_t /*minFrames*/, uint32_t /*maxFrames*/) {
      mSampleRate = sampleRate;
      mEngine.prepare(sampleRate);
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
   float realF(uint32_t id) const { return static_cast<float>(realValue(id)); }

   void syncEngineParams() {
      DrumParams p;
      p.bdTune = realF(kParamBdTune);
      p.bdLevel = realF(kParamBdLevel);
      p.bdAttack = realF(kParamBdAttack);
      p.bdDecaySec = realF(kParamBdDecay) * 0.001f;
      p.sdTune = realF(kParamSdTune);
      p.sdLevel = realF(kParamSdLevel);
      p.sdTone = realF(kParamSdTone);
      p.sdSnappy = realF(kParamSdSnappy);
      const uint32_t tune[3] = {kParamLtTune, kParamMtTune, kParamHtTune};
      const uint32_t level[3] = {kParamLtLevel, kParamMtLevel, kParamHtLevel};
      const uint32_t decay[3] = {kParamLtDecay, kParamMtDecay, kParamHtDecay};
      for (int i = 0; i < 3; ++i) {
         p.tomTune[i] = realF(tune[i]);
         p.tomLevel[i] = realF(level[i]);
         p.tomDecaySec[i] = realF(decay[i]) * 0.001f;
      }
      p.rsLevel = realF(kParamRsLevel);
      p.cpLevel = realF(kParamCpLevel);
      p.hhLevel = realF(kParamHhLevel);
      p.chDecaySec = realF(kParamChDecay) * 0.001f;
      p.ohDecaySec = realF(kParamOhDecay) * 0.001f;
      p.crLevel = realF(kParamCrLevel);
      p.crTune = realF(kParamCrTune);
      p.rdLevel = realF(kParamRdLevel);
      p.rdTune = realF(kParamRdTune);
      p.gain = dbToGain(realF(kParamVolume));

      p.bdPitchHz = realF(kParamBdPitch);
      p.bdSweep = realF(kParamBdSweep);
      p.bdShape = realF(kParamBdShape);
      p.sdPitchHz = realF(kParamSdPitch);
      p.tomPitchHz = realF(kParamTomPitch);
      p.tomSweep = realF(kParamTomSweep);
      p.tomNoise = realF(kParamTomNoise);
      p.rsGateSec = realF(kParamRsDecay) * 0.001f;
      p.cpSpreadSec = realF(kParamCpSpread) * 0.001f;
      p.hatColor = realF(kParamHatColor);
      p.cymColor = realF(kParamCymColor);
      p.dacBits = static_cast<int>(realValue(kParamDacBits));
      p.driveMode = static_cast<int>(realValue(kParamDriveMode));
      p.driveModel = static_cast<int>(realValue(kParamDistType));
      p.drive = realF(kParamDrive);
      p.driveBias = realF(kParamDistBias);
      p.driveToneHz = realF(kParamDriveTone);
      p.driveMix = realF(kParamDistMix);
      for (int v = 0; v < kNumVoices; ++v)
         p.route[v] = realValue(kParamRouteBd + v) > 0.5;
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
      if (isChainHandle(d.id))
         info->flags |= CLAP_PARAM_IS_HIDDEN;
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
      return d && paramValueToText(*d, value, out, size);
   }

   static bool paramsTextToValue(const clap_plugin_t *, clap_id id, const char *text,
                                 double *out) {
      const ParamDesc *d = paramById(id);
      return d && paramTextToValue(*d, text, out);
   }

   static void paramsFlush(const clap_plugin_t *p, const clap_input_events_t *in,
                           const clap_output_events_t *out) {
      RumpelKistePlugin *plug = self(p);
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

   // A parameter written from the main thread as a whole gesture, so the host
   // records it like a knob move.
   void setParamAsEdit(uint32_t id, double value) {
      const ParamDesc &d = paramTable()[id];
      const double v = clampv(value, d.min, d.max);
      mValues[id].store(v, std::memory_order_relaxed);
      mParamsDirty.store(true, std::memory_order_release);
      pushGuiEdit(id, 0.0, EditKind::GestureBegin);
      pushGuiEdit(id, v, EditKind::Value);
      pushGuiEdit(id, 0.0, EditKind::GestureEnd);
   }

   // -------------------------------------------------------------------- state

   static void put(std::string &blob, const void *data, size_t bytes) {
      blob.append(static_cast<const char *>(data), bytes);
   }

   static bool stateSave(const clap_plugin_t *p, const clap_ostream_t *stream) {
      RumpelKistePlugin *plug = self(p);
      std::string blob;
      const uint32_t header[3] = {kStateMagic, kStateVersion, kNumParams};
      put(blob, header, sizeof(header));
      for (uint32_t i = 0; i < kNumParams; ++i) {
         const uint32_t id = paramTable()[i].id;
         const double v = plug->mValues[i].load(std::memory_order_relaxed);
         put(blob, &id, sizeof(id));
         put(blob, &v, sizeof(v));
      }
      for (int pat = 0; pat < kMaxPatterns; ++pat)
         for (int i = 0; i < kMaxSteps; ++i) {
            const uint32_t w = plug->mPattern[pat][i].load(std::memory_order_relaxed);
            put(blob, &w, sizeof(w));
         }
      const uint8_t advanced = plug->mAdvancedOpen.load(std::memory_order_relaxed) ? 1u : 0u;
      put(blob, &advanced, sizeof(advanced));
      for (int note = 0; note < 128; ++note) {
         const int8_t action = plug->mNoteMap[note].load(std::memory_order_relaxed);
         put(blob, &action, sizeof(action));
      }
      for (int pat = 0; pat < kMaxPatterns; ++pat) {
         const uint8_t mode =
            static_cast<uint8_t>(plug->mChainMode[pat].load(std::memory_order_relaxed));
         const uint16_t repeat =
            static_cast<uint16_t>(plug->mChainRepeat[pat].load(std::memory_order_relaxed));
         put(blob, &mode, sizeof(mode));
         put(blob, &repeat, sizeof(repeat));
      }
      for (int v = 0; v < kNumVoices; ++v) {
         const uint8_t m = plug->mMuted[v].load(std::memory_order_relaxed) ? 1u : 0u;
         put(blob, &m, sizeof(m));
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
      RumpelKistePlugin *plug = self(p);
      uint32_t header[3] = {0, 0, 0};
      if (!readExactly(stream, header, sizeof(header)))
         return false;
      if (header[0] != kStateMagic || header[1] > kStateVersion)
         return false;
      const uint32_t count = header[2];
      if (count > 4096)
         return false;
      for (uint32_t i = 0; i < count; ++i) {
         uint32_t id = 0;
         double value = 0.0;
         if (!readExactly(stream, &id, sizeof(id)) || !readExactly(stream, &value, sizeof(value)))
            return false;
         const ParamDesc *d = paramById(id);
         if (!d)
            continue; // unknown id from a newer version: ignore
         plug->mValues[id].store(clampv(value, d->min, d->max), std::memory_order_relaxed);
      }
      // Everything after the parameters is read as far as it goes: a project
      // that opens with its mutes lost is better than one that does not open.
      for (int pat = 0; pat < kMaxPatterns; ++pat) {
         uint32_t words[kMaxSteps];
         if (!readExactly(stream, words, sizeof(words)))
            break;
         for (int i = 0; i < kMaxSteps; ++i)
            plug->mPattern[pat][i].store(words[i], std::memory_order_relaxed);
      }
      uint8_t advanced = 0;
      if (readExactly(stream, &advanced, sizeof(advanced)))
         plug->mAdvancedOpen.store(advanced != 0, std::memory_order_relaxed);
      int8_t map[128];
      if (readExactly(stream, map, sizeof(map)))
         for (int note = 0; note < 128; ++note) {
            const int action = map[note];
            const bool valid = action == kNoteNone || action == kNotePrevPattern ||
                               action == kNoteNextPattern || (action >= 0 && action < kMaxPatterns);
            plug->mNoteMap[note].store(static_cast<int8_t>(valid ? action : kNoteNone),
                                       std::memory_order_relaxed);
         }
      uint8_t chains[kMaxPatterns * 3];
      if (readExactly(stream, chains, sizeof(chains)))
         for (int pat = 0; pat < kMaxPatterns; ++pat) {
            uint16_t repeat = 1;
            std::memcpy(&repeat, chains + pat * 3 + 1, sizeof(repeat));
            plug->setChain(pat, chains[pat * 3], repeat);
         }
      uint8_t mutes[kNumVoices];
      if (readExactly(stream, mutes, sizeof(mutes)))
         for (int v = 0; v < kNumVoices; ++v)
            plug->mMuted[v].store(mutes[v] != 0, std::memory_order_relaxed);

      plug->mParamsDirty.store(true, std::memory_order_release);
      plug->notifyParamValuesChanged();
      return true;
   }

   // -------------------------------------------------------------- preset load

   // A preset that carries a bank replaces the bank; one that carries none --
   // a kit, only the sound -- leaves the patterns where they are, so a kit can
   // be auditioned under the groove that is already playing.
   void applyPattern(const PatternData &pattern) {
      if (!pattern.present && !pattern.chainPresent)
         return;
      for (int p = 0; p < kMaxPatterns; ++p) {
         setChain(p, pattern.chainMode[p], pattern.chainRepeat[p]);
         for (int i = 0; i < kMaxSteps; ++i)
            mPattern[p][i].store(pattern.pattern(p)[i], std::memory_order_relaxed);
      }
   }

   // A preset describes the whole instrument, so anything it does not mention
   // goes back to its default rather than keeping whatever the last one left.
   // The exception is a kit -- a preset without a bank -- which leaves the
   // groove settings alone entirely: they belong to the pattern that is
   // playing, not to the sound.
   void applyPreset(const PresetData &preset, bool hasBank) {
      for (uint32_t i = 0; i < kNumParams; ++i) {
         if (!hasBank && isGrooveParam(i))
            continue;
         mValues[i].store(paramTable()[i].def, std::memory_order_relaxed);
      }
      for (const auto &kv : preset.values) {
         const ParamDesc *d = paramById(kv.first);
         if (!d || (!hasBank && isGrooveParam(kv.first)))
            continue;
         mValues[kv.first].store(clampv(kv.second, d->min, d->max), std::memory_order_relaxed);
      }
      mParamsDirty.store(true, std::memory_order_release);
      notifyParamValuesChanged();
   }

   // What a kit leaves alone -- even when the kit's file mentions it, so a
   // saved preset with its pattern lines deleted is a kit and nothing more: everything that says what the pattern does
   // rather than what the drums sound like.
   static bool isGrooveParam(uint32_t id) {
      switch (id) {
      case kParamMode:
      case kParamScale:
      case kParamSteps:
      case kParamShuffle:
      case kParamFlam:
      case kParamPattern:
      case kParamChainMode:
      case kParamChainLength:
      case kParamPatternTrigger:
      case kParamChainRepeat:
      case kParamGenSeed:
      case kParamGenStyle:
      case kParamGenBusy:
      case kParamGenAccent:
         return true;
      default:
         return false;
      }
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
      RumpelKistePlugin *plug = self(p);
      PresetData preset;
      // 16 kB; on the heap rather than on whatever stack the host called from.
      std::unique_ptr<PatternData> pattern(new PatternData());
      std::string error;

      if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE) {
         if (!location || !location[0]) {
            plug->reportPresetError(locationKind, location, loadKey, "missing preset path");
            return false;
         }
         if (!parsePresetFile(location, preset, pattern.get(), error)) {
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
         if (!parsePreset(found->text, std::strlen(found->text), preset, pattern.get(), error)) {
            plug->reportPresetError(locationKind, location, loadKey, error);
            return false;
         }
      } else {
         plug->reportPresetError(locationKind, location, loadKey, "unsupported location kind");
         return false;
      }

      plug->applyPreset(preset, pattern->present);
      plug->applyPattern(*pattern);
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

   static uint32_t notePortsCount(const clap_plugin_t *, bool) { return 1; }

   static bool notePortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                            clap_note_port_info_t *info) {
      if (index != 0)
         return false;
      std::memset(info, 0, sizeof(*info));
      info->id = isInput ? 0 : 1;
      info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
      info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
      std::snprintf(info->name, sizeof(info->name), isInput ? "Note In" : "Note Out");
      return true;
   }

   static uint32_t tailGet(const clap_plugin_t *p) {
      RumpelKistePlugin *plug = self(p);
      const double tail = plug->mEngine.tailSeconds() * plug->mSampleRate;
      return static_cast<uint32_t>(clampv(tail, 0.0, 2.0e9));
   }

   // ------------------------------------------------------------- sequencer
   //
   // The clock is SäureKiste's, and so are its rules: everything is in steps,
   // the position is re-read from the host's beat timeline at the top of every
   // block so the pattern is locked to the song, and inside the block the loop
   // splits at every step boundary so hits are sample-accurate.

   bool sequencerMode() const { return static_cast<int>(realValue(kParamMode)) == kModeSequencer; }

   // How late the second step of each pair lands, in steps. Setting 1 is
   // straight and each setting above it adds one Shuffle Unit.
   double shuffleAmount() const {
      const int setting = static_cast<int>(realValue(kParamShuffle));
      return (setting < 1 ? 0 : setting - 1) * realValue(kParamShuffleUnit) * 0.01;
   }

   double onsetOf(long k) const { return static_cast<double>(k) + ((k & 1) ? mShuffleAmt : 0.0); }

   double lastOnsetAtOrBefore(double pos) const {
      const long k = static_cast<long>(std::floor(pos)) + 2;
      for (int i = 0; i < 6; ++i) {
         const double t = onsetOf(k - i);
         if (t <= pos + 1.0e-9)
            return t;
      }
      return onsetOf(k - 6);
   }

   double firstOnsetFrom(double pos) const {
      const long k = static_cast<long>(std::floor(pos)) - 2;
      for (int i = 0; i < 6; ++i) {
         const double t = onsetOf(k + i);
         if (t >= pos - 1.0e-9)
            return t;
      }
      return onsetOf(k + 6);
   }

   // The sequencer's hits, sent out of the note port as well as played: the
   // machine's key for each voice, an accent as a velocity above the threshold.
   void emitNote(uint16_t type, int key, bool accent) {
      if (!mOut || !mOut->try_push)
         return;
      clap_event_note_t ev{};
      ev.header.size = sizeof(ev);
      ev.header.time = mOutFrame;
      ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      ev.header.type = type;
      ev.header.flags = 0;
      ev.note_id = -1;
      ev.port_index = 1;
      ev.channel = 9;
      ev.key = static_cast<int16_t>(key);
      const double threshold = realValue(kParamAccentThreshold);
      const double plain = threshold - 10.0 < 90.0 ? threshold - 10.0 : 90.0;
      ev.velocity = type == CLAP_EVENT_NOTE_OFF ? 0.0
                                                : (accent ? 1.0 : (plain < 1.0 ? 1.0 : plain) / 127.0);
      mOut->try_push(mOut, &ev.header);
   }

   // Every note the port has open, closed, and the playhead parked.
   void seqStopAll() {
      for (int v = 0; v < kNumVoices; ++v)
         if (mNoteOpen[v]) {
            emitNote(CLAP_EVENT_NOTE_OFF, keyForVoice(v), false);
            mNoteOpen[v] = false;
         }
      mPlayhead.store(-1, std::memory_order_relaxed);
      mPlayingPattern.store(-1, std::memory_order_relaxed);
      mActivePattern.store(-1, std::memory_order_relaxed);
      mStepOffset = 0;
   }

   // One hit on the engine, counted for the window.
   void hit(int voice, float accent, float gain = 1.0f) {
      mEngine.trigger(voice, accent, gain);
      mHits[voice].fetch_add(1, std::memory_order_relaxed);
      mHitCounter.fetch_add(1, std::memory_order_relaxed);
   }

   void seqStartStep(double onsetPos) {
      const int trigger = static_cast<int>(realValue(kParamPatternTrigger));
      int base = mActivePattern.load(std::memory_order_relaxed);
      if (trigger == kTriggerRestart && base >= 0 && base != seqPattern())
         mStepOffset = static_cast<long>(std::floor(onsetPos + 1.0e-6));
      const long k = static_cast<long>(std::floor(onsetPos + 1.0e-6)) - mStepOffset;
      const int len = seqLength();
      long cycle = k / len;
      if (k < 0 && k % len != 0)
         --cycle;
      const long idx = k - cycle * len;
      if (idx == 0 || base < 0 || trigger != kTriggerAtEnd) {
         const int taken = seqPattern();
         if (base < 0 || taken != base) {
            mChainOrigin = base < 0 ? 0 : cycle;
            mChainWalk = ChainWalk{};
         }
         base = taken;
         mActivePattern.store(base, std::memory_order_relaxed);
      }
      int modes[kMaxPatterns], repeats[kMaxPatterns];
      for (int p = 0; p < kMaxPatterns; ++p) {
         modes[p] = mChainMode[p].load(std::memory_order_relaxed);
         repeats[p] = mChainRepeat[p].load(std::memory_order_relaxed);
      }
      const int pattern =
         chainPatternAt(mChainWalk, modes, repeats, base,
                        static_cast<int>(realValue(kParamChainLength)), cycle - mChainOrigin);
      mLastFired = onsetPos;
      mPlayhead.store(static_cast<int>(idx), std::memory_order_relaxed);
      mPlayingPattern.store(pattern, std::memory_order_relaxed);

      const uint32_t w = mPattern[pattern][idx].load(std::memory_order_relaxed);
      if (w == 0)
         return;

      // The accent a voice gets: Total Accent if the step carries it, the
      // fixed local accent if the voice's own cell does, both if both.
      const float total = stepAccent(w) ? realF(kParamAccent) : 0.0f;
      const float local = realF(kParamLocalAccent);
      const float grace = realF(kParamFlamGrace);
      const uint32_t flamSamples = static_cast<uint32_t>(
         realValue(kParamFlam) * realValue(kParamFlamUnit) * 0.001 * mSampleRate + 0.5);
      // Where the port's notes end: the next step. A drum hit has no length,
      // and a host shows a note that ends where the next one starts as what
      // it is.
      const double nextOnset = firstOnsetFrom(onsetPos + 1.0e-6);
      for (int v = 0; v < kNumVoices; ++v) {
         const int level = stepLevel(w, v);
         if (level == kLevelOff)
            continue;
         float a = total + (level == kLevelAccent ? local : 0.0f);
         a = a > 1.0f ? 1.0f : a;
         if (stepFlam(w, v) && flamSamples > 0) {
            // The owner's manual's order: the first sound of a flam is in time
            // for the step, and the second follows it.
            hit(v, a, grace);
            mEngine.triggerLater(v, a, flamSamples);
         } else {
            hit(v, a);
         }
         if (mNoteOpen[v])
            emitNote(CLAP_EVENT_NOTE_OFF, keyForVoice(v), false);
         emitNote(CLAP_EVENT_NOTE_ON, keyForVoice(v), a > 0.0f);
         mNoteOpen[v] = true;
         mNoteOff[v] = nextOnset;
      }
   }

   void seqFireDue() {
      int guard = 0;
      while (mStepPos >= mNextOn - 1.0e-9 && guard++ <= kMaxSteps) {
         const double fired = mNextOn;
         seqStartStep(fired);
         mNextOn = firstOnsetFrom(fired + 1.0e-6);
      }
      for (int v = 0; v < kNumVoices; ++v)
         if (mNoteOpen[v] && mStepPos >= mNoteOff[v] - 1.0e-9) {
            emitNote(CLAP_EVENT_NOTE_OFF, keyForVoice(v), false);
            mNoteOpen[v] = false;
         }
   }

   double seqNextBoundary() const {
      double b = mNextOn;
      for (int v = 0; v < kNumVoices; ++v)
         if (mNoteOpen[v] && mNoteOff[v] < b)
            b = mNoteOff[v];
      return b;
   }

   void syncPlayMode() {
      const bool seqMode = sequencerMode();
      if (seqMode == mLastSeqMode)
         return;
      mLastSeqMode = seqMode;
      seqStopAll();
      mSeqRunning = false;
      mLastFired = -1.0e18;
   }

   void updateSequencerClock(const clap_process_t *pr) {
      const clap_event_transport_t *tr = pr ? pr->transport : nullptr;
      // The tempo, whether or not the host is rolling: a stopped host still
      // says what the project's tempo is, and free running needs it.
      if (tr && (tr->flags & CLAP_TRANSPORT_HAS_TEMPO) && tr->tempo > 1.0)
         mTempo = tr->tempo;
      const bool hostPlaying = tr && (tr->flags & CLAP_TRANSPORT_IS_PLAYING);
      mHostPlaying.store(hostPlaying, std::memory_order_relaxed);
      mTempoPublished.store(mTempo, std::memory_order_relaxed);

      if (!sequencerMode()) {
         if (mSeqRunning) {
            seqStopAll();
            mSeqRunning = false;
         }
         return;
      }

      mShuffleAmt = shuffleAmount();
      const double perBeat = stepsPerBeat(static_cast<int>(realValue(kParamScale)));

      bool playing = false;
      bool lockedToHost = false;
      if (hostPlaying) {
         if (tr->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
            const double beats =
               static_cast<double>(tr->song_pos_beats) / static_cast<double>(CLAP_BEATTIME_FACTOR);
            const double fromSong = beats * perBeat;
            // A loop, a seek or a jump from free running to the song: move
            // everything scheduled by the same amount rather than throwing it
            // away -- SäureKiste lost notes both ways before it did this.
            if (mSeqRunning) {
               const double delta = fromSong - mStepPos;
               if (std::fabs(delta) > 0.5) {
                  for (int v = 0; v < kNumVoices; ++v)
                     mNoteOff[v] += delta;
                  mLastFired += delta;
                  mStepOffset = 0;
               }
            }
            mStepPos = fromSong;
            lockedToHost = true;
         }
         playing = true;
      } else if (mFreeRun.load(std::memory_order_relaxed)) {
         playing = true;
      }

      if (playing && !mSeqRunning) {
         if (!lockedToHost)
            mStepPos = 0.0;
         mLastFired = -1.0e18;
      }
      if (!playing && mSeqRunning)
         seqStopAll();
      mSeqRunning = playing;

      if (mSeqRunning) {
         mNextOn = firstOnsetFrom(mStepPos);
         if (mNextOn <= mLastFired + 1.0e-9)
            mNextOn = firstOnsetFrom(mLastFired + 1.0e-6);
         // An onset just behind us that has never fired still has to: a host's
         // loop point almost never lands on a block boundary.
         const double behind = lastOnsetAtOrBefore(mStepPos);
         if (behind > mLastFired + 1.0e-9 && mStepPos - behind < 0.5)
            mNextOn = behind;
      }

      const double sr = mSampleRate > 0.0 ? mSampleRate : 48000.0;
      mFramesPerStep = 60.0 / (mTempo > 1.0 ? mTempo : 120.0) / perBeat * sr;
      if (mFramesPerStep < 8.0)
         mFramesPerStep = 8.0;
   }

   // ------------------------------------------------------------- live map

   // Returns true if the note was the map's and must not do anything else.
   bool liveNoteConsumed(int16_t key, uint32_t frame) {
      if (!sequencerMode() || key < 0 || key > 127)
         return false;
      const int target = mLearnTarget.exchange(kNoteNone, std::memory_order_acq_rel);
      if (target != kNoteNone) {
         mNoteMap[key].store(static_cast<int8_t>(target), std::memory_order_relaxed);
         return true;
      }
      const int action = mNoteMap[key].load(std::memory_order_relaxed);
      if (action == kNoteNone)
         return false;
      const int from = seqPattern();
      int want = from;
      if (action == kNotePrevPattern)
         want = steppedPattern(from, -1, kMaxPatterns);
      else if (action == kNoteNextPattern)
         want = steppedPattern(from, +1, kMaxPatterns);
      else
         want = action < 0 || action >= kMaxPatterns ? from : action;
      if (want != from)
         setPatternFromAudioThread(want, frame);
      return true;
   }

   void setPatternFromAudioThread(int pattern, uint32_t frame) {
      const ParamDesc &d = paramTable()[kParamPattern];
      const double v = clampv(static_cast<double>(pattern + 1), d.min, d.max);
      mValues[kParamPattern].store(v, std::memory_order_relaxed);
      mParamsDirty.store(true, std::memory_order_relaxed);
      if (!mOut || !mOut->try_push)
         return;
      clap_event_param_gesture_t g{};
      g.header.size = sizeof(g);
      g.header.time = frame;
      g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      g.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
      g.param_id = kParamPattern;
      mOut->try_push(mOut, &g.header);
      clap_event_param_value_t ev{};
      ev.header.size = sizeof(ev);
      ev.header.time = frame;
      ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      ev.header.type = CLAP_EVENT_PARAM_VALUE;
      ev.param_id = kParamPattern;
      ev.note_id = -1;
      ev.port_index = -1;
      ev.channel = -1;
      ev.key = -1;
      ev.value = v;
      mOut->try_push(mOut, &ev.header);
      g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
      mOut->try_push(mOut, &g.header);
   }

   // --------------------------------------------------------- PatternAccess

   uint32_t seqStep(int pattern, int index) const override {
      if (pattern < 0 || pattern >= kMaxPatterns || index < 0 || index >= kMaxSteps)
         return 0;
      return mPattern[pattern][index].load(std::memory_order_relaxed);
   }

   void seqSetStep(int pattern, int index, uint32_t step) override {
      if (pattern < 0 || pattern >= kMaxPatterns || index < 0 || index >= kMaxSteps)
         return;
      mPattern[pattern][index].store(step, std::memory_order_relaxed);
      mPresetEdited = true;
   }

   int seqPattern() const override {
      const int n = static_cast<int>(realValue(kParamPattern)) - 1;
      return n < 0 ? 0 : (n >= kMaxPatterns ? kMaxPatterns - 1 : n);
   }

   int seqPlayingPattern() const override { return mPlayingPattern.load(std::memory_order_relaxed); }

   int seqShownPattern() const override {
      const int playing = seqPlayingPattern();
      const int active = mActivePattern.load(std::memory_order_relaxed);
      const int from = active >= 0 ? active : seqPattern();
      const bool chained = mChainMode[from].load(std::memory_order_relaxed) != kChainStay;
      return chained && playing >= 0 ? playing : seqPattern();
   }

   int seqPendingPattern() const override {
      const int active = mActivePattern.load(std::memory_order_relaxed);
      const int selected = seqPattern();
      return active >= 0 && active != selected ? selected : -1;
   }

   bool seqPatternEmpty(int pattern) const override {
      if (pattern < 0 || pattern >= kMaxPatterns)
         return true;
      for (int i = 0; i < kMaxSteps; ++i)
         if (mPattern[pattern][i].load(std::memory_order_relaxed) != 0)
            return false;
      return true;
   }

   int seqLength() const override {
      const int n = static_cast<int>(realValue(kParamSteps));
      return n < 1 ? 1 : (n > kMaxSteps ? kMaxSteps : n);
   }

   int seqPlayhead() const override { return mPlayhead.load(std::memory_order_relaxed); }

   bool seqEnabled() const override { return sequencerMode(); }

   uint32_t seqSeed() const override { return static_cast<uint32_t>(realValue(kParamGenSeed)); }

   uint32_t seqSeedMax() const override {
      return static_cast<uint32_t>(paramTable()[kParamGenSeed].max);
   }

   void seqSetSeed(uint32_t seed) override {
      setParamAsEdit(kParamGenSeed, static_cast<double>(seed));
      seqGenerate();
   }

   void seqGenerate() override {
      GenSettings g;
      g.seed = static_cast<uint32_t>(realValue(kParamGenSeed));
      g.style = static_cast<int>(realValue(kParamGenStyle));
      g.busy = realValue(kParamGenBusy);
      g.accent = realValue(kParamGenAccent);
      g.length = seqLength();
      uint32_t steps[kMaxSteps];
      generatePattern(g, steps);
      const int pattern = seqShownPattern();
      for (int i = 0; i < kMaxSteps; ++i)
         mPattern[pattern][i].store(steps[i], std::memory_order_relaxed);
      mPresetEdited = true;
   }

   // GEN: a new groove, not the same one again. The salt is per instance --
   // two editors on two tracks must not hand out the same seeds, and nothing
   // may live at file scope for exactly that reason.
   void seqGenerateNew() override {
      const auto now = std::chrono::steady_clock::now().time_since_epoch();
      const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
      const uint32_t salt = ++mGenSalt ^ static_cast<uint32_t>(ticks);
      seqSetSeed(nextGeneratorSeed(seqSeed(), salt, seqSeedMax()));
   }

   int seqLearnTarget() const override { return mLearnTarget.load(std::memory_order_relaxed); }

   void seqSetLearnTarget(int target) override {
      mLearnTarget.store(target, std::memory_order_release);
   }

   int seqMappedNote(int target) const override {
      for (int note = 0; note < 128; ++note)
         if (mNoteMap[note].load(std::memory_order_relaxed) == target)
            return note;
      return -1;
   }

   void seqClearMap(int target) override {
      for (int note = 0; note < 128; ++note)
         if (target == kNoteNone || mNoteMap[note].load(std::memory_order_relaxed) == target)
            mNoteMap[note].store(static_cast<int8_t>(kNoteNone), std::memory_order_relaxed);
   }

   void seqStepPattern(int delta) override {
      setParamAsEdit(kParamPattern,
                     static_cast<double>(steppedPattern(seqShownPattern(), delta, kMaxPatterns) + 1));
   }

   bool seqMuted(int voice) const override {
      return voice >= 0 && voice < kNumVoices && mMuted[voice].load(std::memory_order_relaxed);
   }

   void seqSetMuted(int voice, bool muted) override {
      if (voice >= 0 && voice < kNumVoices)
         mMuted[voice].store(muted, std::memory_order_relaxed);
   }

   void seqAudition(int voice) override {
      if (voice < 0 || voice >= kNumVoices)
         return;
      mAudition.fetch_or(1u << voice, std::memory_order_relaxed);
      if (mHost && mHost->request_process)
         mHost->request_process(mHost);
   }

   uint32_t seqVoiceHits(int voice) const override {
      return voice >= 0 && voice < kNumVoices ? mHits[voice].load(std::memory_order_relaxed) : 0;
   }

   bool seqFreeRunning() const override { return mFreeRun.load(std::memory_order_relaxed); }

   void seqSetFreeRunning(bool on) override {
      mFreeRun.store(on, std::memory_order_relaxed);
      if (mHost && mHost->request_process)
         mHost->request_process(mHost);
   }

   bool seqHostPlaying() const override { return mHostPlaying.load(std::memory_order_relaxed); }

   std::string seqExportMidi() override {
      const int pat = seqShownPattern();
      uint32_t steps[kMaxSteps];
      for (int i = 0; i < kMaxSteps; ++i)
         steps[i] = mPattern[pat][i].load(std::memory_order_relaxed);
      MidiExport spec;
      spec.steps = steps;
      spec.length = seqLength();
      spec.stepsPerBeat = stepsPerBeat(static_cast<int>(realValue(kParamScale)));
      spec.shuffle = shuffleAmount();
      spec.flamSeconds = realValue(kParamFlam) * realValue(kParamFlamUnit) * 0.001;
      spec.tempoBpm = mTempoPublished.load(std::memory_order_relaxed);
      spec.accentVelocity = realValue(kParamAccentThreshold);
      spec.patternNumber = pat + 1;
      const std::string bytes = patternToMidiFile(spec);
      if (bytes.empty())
         return std::string();
      const std::string path = midiExportPath(spec.patternNumber);
      std::string error;
      if (!writeMidiFile(path, bytes, error))
         return std::string();
      return path;
   }

   // ------------------------------------------------------------- chains

   static bool isChainHandle(uint32_t id) { return id == kParamChainMode || id == kParamChainRepeat; }

   void setChain(int pattern, int mode, int repeat) {
      mChainMode[pattern].store(mode < 0 || mode >= kNumChainModes ? kChainStay : mode,
                                std::memory_order_relaxed);
      mChainRepeat[pattern].store(
         repeat < 1 ? 1 : (repeat > kMaxChainRepeat ? kMaxChainRepeat : repeat),
         std::memory_order_relaxed);
   }

   void setChainForAll(int mode, int repeat) {
      for (int p = 0; p < kMaxPatterns; ++p)
         setChain(p, mode, repeat);
   }

   std::atomic<int> &chainSlot(uint32_t id) const {
      const int p = seqShownPattern();
      return id == kParamChainMode ? mChainMode[p] : mChainRepeat[p];
   }

   // ------------------------------------------------------------------ process

   // The voice a MIDI key plays, and with what accent: the machine's accent is
   // a switch, so a velocity at or above the threshold plays the voice with
   // Total Accent and one below plays it plain.
   void playKey(int key, double velocity01) {
      const int v = voiceForKey(key);
      if (v < 0)
         return;
      const bool accented = velocity01 * 127.0 >= realValue(kParamAccentThreshold) - 0.5;
      hit(v, accented ? realF(kParamAccent) : 0.0f);
   }

   void handleEvent(const clap_event_header_t *hdr) {
      if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
         return;
      switch (hdr->type) {
      case CLAP_EVENT_NOTE_ON: {
         const auto *ev = reinterpret_cast<const clap_event_note_t *>(hdr);
         if (liveNoteConsumed(ev->key, hdr->time))
            break;
         playKey(ev->key, ev->velocity);
         break;
      }
      case CLAP_EVENT_PARAM_VALUE: {
         const auto *ev = reinterpret_cast<const clap_event_param_value_t *>(hdr);
         const ParamDesc *d = paramById(ev->param_id);
         if (!d)
            break;
         const double value = clampv(ev->value, d->min, d->max);
         const double before = mValues[ev->param_id].exchange(value, std::memory_order_relaxed);
         mParamsDirty.store(true, std::memory_order_relaxed);
         // The chain handles set every pattern when a host moves them -- an
         // automation lane, or a render asking for a chain -- but only when
         // they move, so a host resending a value does not flatten the bank.
         if (isChainHandle(ev->param_id) && value != before)
            setChainForAll(static_cast<int>(realValue(kParamChainMode)),
                           static_cast<int>(realValue(kParamChainRepeat)));
         if (ev->param_id == kParamMode)
            syncPlayMode();
         break;
      }
      case CLAP_EVENT_PARAM_MOD: {
         const auto *ev = reinterpret_cast<const clap_event_param_mod_t *>(hdr);
         if (ev->note_id >= 0 || !paramById(ev->param_id))
            break;
         mMods[ev->param_id].store(ev->amount, std::memory_order_relaxed);
         mParamsDirty.store(true, std::memory_order_relaxed);
         break;
      }
      case CLAP_EVENT_MIDI: {
         const auto *ev = reinterpret_cast<const clap_event_midi_t *>(hdr);
         const uint8_t status = ev->data[0] & 0xF0;
         const int16_t key = static_cast<int16_t>(ev->data[1] & 0x7F);
         const uint8_t vel = ev->data[2] & 0x7F;
         if (status == 0x90 && vel > 0) {
            if (!liveNoteConsumed(key, hdr->time))
               playKey(key, vel / 127.0);
         } else if (status == 0xB0 && (ev->data[1] == 120 || ev->data[1] == 123)) {
            mEngine.reset();
         }
         break;
      }
      default:
         break;
      }
   }

   void applyMutes() {
      for (int v = 0; v < kNumVoices; ++v)
         mEngine.setMuted(v, mMuted[v].load(std::memory_order_relaxed));
   }

   clap_process_status process(const clap_process_t *pr) {
      if (!pr || pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 2)
         return CLAP_PROCESS_ERROR;
      const ScopedNoDenormals noDenormals;

      float *outL = pr->audio_outputs[0].data32[0];
      float *outR = pr->audio_outputs[0].data32[1];
      if (!outL || !outR)
         return CLAP_PROCESS_ERROR;

      const uint32_t numFrames = pr->frames_count;
      const clap_input_events_t *in = pr->in_events;
      const uint32_t numEvents = in ? in->size(in) : 0;

      drainGuiEdits(pr->out_events, 0);
      mOut = pr->out_events;
      mOutFrame = 0;

      syncPlayMode();
      applyMutes();

      // Row labels clicked in the editor since the last block.
      const uint32_t audition = mAudition.exchange(0, std::memory_order_acquire);
      if (audition) {
         if (mParamsDirty.exchange(false, std::memory_order_acq_rel))
            syncEngineParams();
         for (int v = 0; v < kNumVoices; ++v)
            if (audition & (1u << v))
               hit(v, 0.0f);
      }

      uint32_t eventIndex = 0;
      uint32_t frame = 0;
      bool clockUpdated = false;

      while (frame < numFrames) {
         while (eventIndex < numEvents) {
            const clap_event_header_t *hdr = in->get(in, eventIndex);
            if (!hdr || hdr->time > frame)
               break;
            // A parameter landing with a hit on the same frame has to be in the
            // engine before the hit is.
            if (hdr->type == CLAP_EVENT_NOTE_ON || hdr->type == CLAP_EVENT_MIDI)
               if (mParamsDirty.exchange(false, std::memory_order_acq_rel))
                  syncEngineParams();
            handleEvent(hdr);
            ++eventIndex;
         }
         // Once per block, after the events landing on frame 0: a Mode or a
         // Scale arriving on the first sample has to be seen by the first step.
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

         if (mSeqRunning) {
            mOutFrame = frame;
            seqFireDue();
            const double ahead = (seqNextBoundary() - mStepPos) * mFramesPerStep;
            if (ahead >= 0.0 && ahead < static_cast<double>(numFrames)) {
               const uint32_t at = frame + static_cast<uint32_t>(std::ceil(ahead));
               if (at > frame && at < next)
                  next = at;
            }
         }

         const uint32_t n = next - frame;
         mEngine.process(outL + frame, n);
         std::memcpy(outR + frame, outL + frame, n * sizeof(float));
         if (mSeqRunning)
            mStepPos += static_cast<double>(n) / mFramesPerStep;
         frame = next;
      }

      mOut = nullptr;
      mVoiceMeter.store(mEngine.activeVoiceCount(), std::memory_order_relaxed);
      publishOutputPeaks(outL, outR, numFrames);

      // A stopped sequencer with nothing ringing may sleep; a running one may
      // not, or the next step would never be reached.
      return mEngine.isSilent() && !mSeqRunning ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
   }

   // ----------------------------------------------------------- preset list

   void ensurePresetList() {
      if (mPresetsScanned)
         return;
      mPresetsScanned = true;
#ifdef RUMPELKISTE_WITH_GUI
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
#ifdef RUMPELKISTE_WITH_GUI
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

   void publishOutputPeaks(const float *l, const float *r, uint32_t frames) {
      if (frames == 0)
         return;
      float peakL = 0.0f;
      float peakR = 0.0f;
      for (uint32_t i = 0; i < frames; ++i) {
         peakL = std::max(peakL, std::fabs(l[i]));
         peakR = std::max(peakR, std::fabs(r[i]));
      }
      const double seconds =
         static_cast<double>(frames) / (mSampleRate > 0 ? mSampleRate : 48000.0);
      const float fall = static_cast<float>(std::exp(-seconds / 0.35));
      mFallingPeakL = std::max(peakL, mFallingPeakL * fall);
      mFallingPeakR = std::max(peakR, mFallingPeakR * fall);
      mPeakL.store(mFallingPeakL, std::memory_order_relaxed);
      mPeakR.store(mFallingPeakR, std::memory_order_relaxed);
   }

#ifdef RUMPELKISTE_WITH_GUI
#if defined(_WIN32)
   static constexpr const char *kWindowApi = CLAP_WINDOW_API_WIN32;
   static uintptr_t nativeHandle(const clap_window_t &w) {
      return reinterpret_cast<uintptr_t>(w.win32);
   }
#else
   static constexpr const char *kWindowApi = CLAP_WINDOW_API_X11;
   static uintptr_t nativeHandle(const clap_window_t &w) { return static_cast<uintptr_t>(w.x11); }
#endif

   // -------------------------------------------------------- GuiDelegate

   double guiParamValue(uint32_t id) const override {
      if (isChainHandle(id))
         return chainSlot(id).load(std::memory_order_relaxed);
      return id < kNumParams ? mValues[id].load(std::memory_order_relaxed) : 0.0;
   }

   void guiBeginEdit(uint32_t id) override {
      if (!isChainHandle(id))
         pushGuiEdit(id, 0.0, EditKind::GestureBegin);
   }

   void guiSetParam(uint32_t id, double value) override {
      const ParamDesc *d = paramById(id);
      if (!d)
         return;
      const double clamped = clampv(value, d->min, d->max);
      if (isChainHandle(id)) {
         chainSlot(id).store(static_cast<int>(std::floor(clamped + 0.5)), std::memory_order_relaxed);
         mPresetEdited = true;
         return;
      }
      mValues[id].store(clamped, std::memory_order_relaxed);
      mParamsDirty.store(true, std::memory_order_release);
      mPresetEdited = true;
      pushGuiEdit(id, clamped, EditKind::Value);
   }

   void guiEndEdit(uint32_t id) override {
      if (!isChainHandle(id))
         pushGuiEdit(id, 0.0, EditKind::GestureEnd);
   }

   void guiOutputPeaks(float &left, float &right) const override {
      left = mPeakL.load(std::memory_order_relaxed);
      right = mPeakR.load(std::memory_order_relaxed);
   }

   uint32_t guiVoiceCount() const override { return mVoiceMeter.load(std::memory_order_relaxed); }
   uint32_t guiVoiceLimit() const override { return kNumVoices; }
   uint32_t guiEventCounter() const override {
      return mHitCounter.load(std::memory_order_relaxed);
   }

   // Clicking the version label plays a kick and an open hat, accented.
   void guiVersionClicked() override {
      mAudition.fetch_or((1u << kVoiceBD) | (1u << kVoiceOH), std::memory_order_relaxed);
      if (mHost && mHost->request_process)
         mHost->request_process(mHost);
   }

   std::string guiSuggestedPresetName() const override {
      if (mCurrentPreset >= 0 && mCurrentPreset < static_cast<int>(mPresets.size()))
         return mPresets[static_cast<size_t>(mCurrentPreset)].name;
      return "My Groove";
   }

   static void splitFolder(const std::string &input, std::string &folder, std::string &leaf) {
      const size_t cut = input.find_last_of("/\\");
      if (cut == std::string::npos) {
         folder.clear();
         leaf = input;
         return;
      }
      folder = input.substr(0, cut);
      leaf = input.substr(cut + 1);
      for (char &c : folder)
         if (c == '/' || c == '\\')
            c = '_';
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
      for (uint32_t i = 0; i < kNumParams; ++i) {
         const uint32_t id = paramTable()[i].id;
         data.values.emplace_back(id, mValues[id].load(std::memory_order_relaxed));
      }
      std::unique_ptr<PatternData> pattern(new PatternData());
      pattern->present = true;
      for (int i = 0; i < kMaxPatterns * kMaxSteps; ++i)
         pattern->steps[i] = mPattern[i / kMaxSteps][i % kMaxSteps].load(std::memory_order_relaxed);
      pattern->chainPresent = true;
      for (int p = 0; p < kMaxPatterns; ++p) {
         pattern->chainMode[p] = mChainMode[p].load(std::memory_order_relaxed);
         pattern->chainRepeat[p] = mChainRepeat[p].load(std::memory_order_relaxed);
      }
      if (!writePresetFile(path, formatPreset(data, pattern.get()), error))
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
      mPresetEdited = false;
      return true;
   }

   bool guiPresetFoldersSupported() const override { return true; }

   std::string guiPackPathFor(const std::string &folder) const override {
      const std::string dir = plugincore::presetPackDir(presetContext());
      if (dir.empty())
         return {};
      std::string stem =
         plugincore::presetFileStem(folder.empty() ? std::string("presets") : folder);
      if (stem.empty())
         stem = "presets";
      return dir + "/" + stem + "." + plugincore::presetPackExtension(presetContext());
   }

   std::vector<std::string> guiPresetPacks() const override {
      std::vector<std::string> out;
      const std::string dir = plugincore::presetPackDir(presetContext());
      std::error_code ec;
      if (dir.empty() || !std::filesystem::is_directory(dir, ec))
         return out;
      const std::string suffix = "." + plugincore::presetPackExtension(presetContext());
      for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
         if (!entry.is_regular_file())
            continue;
         const std::string name = entry.path().filename().string();
         if (name.size() > suffix.size() &&
             name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(entry.path().string());
      }
      std::sort(out.begin(), out.end());
      return out;
   }

   bool guiExportPack(const std::string &folder, const std::string &path,
                      std::string &error) override {
      ensurePresetList();
      std::vector<plugincore::PresetPackEntry> entries;
      for (const auto &preset : mPresets) {
         if (preset.folder != folder)
            continue;
         plugincore::PresetPackEntry e;
         e.name = preset.name;
         if (!preset.loadKey.empty()) {
            for (unsigned i = 0; i < kNumBuiltinPresets; ++i)
               if (preset.loadKey == kBuiltinPresets[i].loadKey)
                  e.text = kBuiltinPresets[i].text;
         } else {
            std::ifstream file(preset.path, std::ios::binary);
            if (!file)
               continue;
            std::ostringstream buf;
            buf << file.rdbuf();
            e.text = buf.str();
         }
         if (!e.text.empty())
            entries.push_back(e);
      }
      if (entries.empty()) {
         error = "That folder has no presets in it.";
         return false;
      }
      const std::string text = plugincore::formatPresetPack(
         presetContext(), folder.empty() ? std::string("Presets") : folder, entries);
      return writePresetFile(path, text, error);
   }

   bool guiImportPack(const std::string &path, std::string &folder, std::string &error) override {
      std::string packName;
      std::vector<plugincore::PresetPackEntry> entries;
      if (!plugincore::parsePresetPackFile(presetContext(), path, packName, entries, error))
         return false;
      if (packName.empty())
         packName = std::filesystem::path(path).stem().string();
      const std::string dir = userPresetDir();
      if (dir.empty()) {
         error = "No user preset directory: neither XDG_CONFIG_HOME nor HOME is set.";
         return false;
      }
      std::string stem = plugincore::presetFileStem(packName);
      if (stem.empty())
         stem = "pack";
      std::string unique = stem;
      std::error_code ec;
      for (int n = 2; n < 100 && std::filesystem::exists(std::filesystem::path(dir) / unique, ec);
           ++n)
         unique = stem + "_" + std::to_string(n);
      int written = 0;
      for (const auto &entry : entries) {
         const std::string file = plugincore::userPresetPathIn(presetContext(), unique, entry.name);
         if (file.empty())
            continue;
         std::string werr;
         if (writePresetFile(file, entry.text, werr))
            ++written;
         else
            error = werr;
      }
      if (written == 0) {
         if (error.empty())
            error = "Nothing in the pack could be written.";
         return false;
      }
      folder = unique;
      mPresets.clear();
      mPresetsScanned = false;
      ensurePresetList();
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

   void windowRequestResize(uint32_t width, uint32_t height) override {
      if (!mHost)
         return;
      auto *hostGui = static_cast<const clap_host_gui_t *>(mHost->get_extension(mHost, CLAP_EXT_GUI));
      if (!hostGui)
         return;
      if (hostGui->resize_hints_changed)
         hostGui->resize_hints_changed(mHost);
      if (hostGui->request_resize)
         hostGui->request_resize(mHost, width, height);
   }

   bool windowAdvancedOpen() const override { return mAdvancedOpen.load(std::memory_order_relaxed); }
   void windowSetAdvancedOpen(bool open) override {
      mAdvancedOpen.store(open, std::memory_order_relaxed);
   }

   // ----------------------------------------------------------- gui extension

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
      RumpelKistePlugin *plug = self(p);
      if (plug->mGui)
         return true;
      plug->ensurePresetList();
      plug->mGui = rumpelkiste::createGui(*plug, *plug, *plug);
      if (!plug->mGui)
         return false;
      plug->startGuiClock();
      return true;
   }

   static void guiDestroy(const clap_plugin_t *p) {
      RumpelKistePlugin *plug = self(p);
      plug->stopGuiClock();
      delete plug->mGui;
      plug->mGui = nullptr;
   }

   static bool guiSetScale(const clap_plugin_t *p, double scale) {
      RumpelKistePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->setScale(scale);
      return true;
   }

   static bool guiGetSize(const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
      RumpelKistePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->size(width, height);
      return true;
   }

   static bool guiCanResize(const clap_plugin_t *) { return true; }

   static bool guiGetResizeHints(const clap_plugin_t *p, clap_gui_resize_hints_t *hints) {
      RumpelKistePlugin *plug = self(p);
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
      RumpelKistePlugin *plug = self(p);
      if (!plug->mGui || !width || !height)
         return false;
      plug->mGui->fitSize(width, height);
      return true;
   }

   static bool guiSetSize(const clap_plugin_t *p, uint32_t width, uint32_t height) {
      RumpelKistePlugin *plug = self(p);
      return plug->mGui && plug->mGui->resize(width, height);
   }

   static bool guiSetParent(const clap_plugin_t *p, const clap_window_t *window) {
      RumpelKistePlugin *plug = self(p);
      if (!plug->mGui || !window || std::strcmp(window->api, kWindowApi) != 0)
         return false;
      return plug->mGui->embed(nativeHandle(*window));
   }

   static bool guiSetTransient(const clap_plugin_t *p, const clap_window_t *window) {
      RumpelKistePlugin *plug = self(p);
      if (!plug->mGui || !window || std::strcmp(window->api, kWindowApi) != 0)
         return false;
      return plug->mGui->setTransientFor(nativeHandle(*window));
   }

   static void guiSuggestTitle(const clap_plugin_t *p, const char *title) {
      RumpelKistePlugin *plug = self(p);
      if (plug->mGui)
         plug->mGui->setTitle(title);
   }

   static bool guiShow(const clap_plugin_t *p) {
      RumpelKistePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->show();
      return true;
   }

   static bool guiHide(const clap_plugin_t *p) {
      RumpelKistePlugin *plug = self(p);
      if (!plug->mGui)
         return false;
      plug->mGui->hide();
      return true;
   }

   // ------------------------------------------------------------- gui clock
   //
   // The window is repainted from the host's timer, which is the main thread.
   // Hosts are not required to offer timers, so there is a fallback thread.

   static void guiOnTimer(const clap_plugin_t *p, clap_id timerId) {
      RumpelKistePlugin *plug = self(p);
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
#endif // RUMPELKISTE_WITH_GUI

   // ---------------------------------------------------------------- extensions

   const void *getExtension(const char *id) const {
      static const clap_plugin_params_t kParamsExt = {paramsCount,       paramsGetInfo,
                                                      paramsGetValue,    paramsValueToText,
                                                      paramsTextToValue, paramsFlush};
      static const clap_plugin_audio_ports_t kAudioPortsExt = {audioPortsCount, audioPortsGet};
      static const clap_plugin_note_ports_t kNotePortsExt = {notePortsCount, notePortsGet};
      static const clap_plugin_state_t kStateExt = {stateSave, stateLoad};
      static const clap_plugin_tail_t kTailExt = {tailGet};
      static const clap_plugin_preset_load_t kPresetLoadExt = {presetLoadFromLocation};
#ifdef RUMPELKISTE_WITH_GUI
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
      if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
         return &kNotePortsExt;
      if (std::strcmp(id, CLAP_EXT_STATE) == 0)
         return &kStateExt;
      if (std::strcmp(id, CLAP_EXT_TAIL) == 0)
         return &kTailExt;
      if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0 ||
          std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0)
         return &kPresetLoadExt;
#ifdef RUMPELKISTE_WITH_GUI
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
   std::atomic<uint32_t> mHitCounter{0};
   std::atomic<uint32_t> mHits[kNumVoices];
   // Voices the editor asked to hear, one bit each, drained by the audio thread.
   std::atomic<uint32_t> mAudition{0};
   std::atomic<bool> mMuted[kNumVoices];
   std::atomic<float> mPeakL{0.0f};
   std::atomic<float> mPeakR{0.0f};
   float mFallingPeakL = 0.0f;
   float mFallingPeakR = 0.0f;

   // The bank. Read by the audio thread and written by the window, one relaxed
   // atomic word per step.
   std::atomic<uint32_t> mPattern[kMaxPatterns][kMaxSteps];
   std::atomic<int> mPlayhead{-1};
   std::atomic<int> mPlayingPattern{-1};
   std::atomic<int> mActivePattern{-1};
   mutable std::atomic<int> mChainMode[kMaxPatterns];
   mutable std::atomic<int> mChainRepeat[kMaxPatterns];
   ChainWalk mChainWalk;
   long mChainOrigin = 0;
   long mStepOffset = 0;
   std::atomic<bool> mAdvancedOpen{false};
   // PLAY: run the pattern while the host is stopped. Not saved -- a project
   // that started playing by itself when it was opened would be a bad joke.
   std::atomic<bool> mFreeRun{false};
   std::atomic<bool> mHostPlaying{false};
   bool mSeqRunning = false;
   double mStepPos = 0.0;
   double mNextOn = 0.0;
   double mLastFired = -1.0e18;
   double mShuffleAmt = 0.0;
   double mFramesPerStep = 1000.0;
   double mTempo = 120.0;
   std::atomic<double> mTempoPublished{120.0};
   // The note port's open notes, one per voice, and where each one ends.
   bool mNoteOpen[kNumVoices] = {false, false, false, false, false, false,
                                 false, false, false, false, false};
   double mNoteOff[kNumVoices] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
   std::atomic<int8_t> mNoteMap[128];
   std::atomic<int> mLearnTarget{kNoteNone};
   bool mLastSeqMode = true;
   uint32_t mGenSalt = 0;
   const clap_output_events_t *mOut = nullptr;
   uint32_t mOutFrame = 0;

   ParamEdit mEditQueue[kEditQueueSize];
   std::atomic<uint32_t> mEditWrite{0};
   std::atomic<uint32_t> mEditRead{0};

   bool mPresetsScanned = false;
   int mCurrentPreset = -1;
   bool mPresetEdited = false;
#ifdef RUMPELKISTE_WITH_GUI
   std::vector<GuiPreset> mPresets;
   Gui *mGui = nullptr;
   clap_id mTimerId = CLAP_INVALID_ID;
   std::thread mGuiThread;
   std::atomic<bool> mGuiThreadRun{false};
#endif

   DrumEngine mEngine;
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
   auto *plug = new RumpelKistePlugin(host);
   return plug->clapPlugin();
}

} // namespace

const clap_plugin_factory_t gPluginFactory = {factoryCount, factoryGetDescriptor, factoryCreate};

} // namespace rumpelkiste

extern "C" {

bool rumpelkisteEntryInit(const char *) { return true; }
void rumpelkisteEntryDeinit() {}

const void *rumpelkisteEntryGetFactory(const char *factoryId) {
   if (!factoryId)
      return nullptr;
   if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
      return &rumpelkiste::gPluginFactory;
   if (std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0 ||
       std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0)
      return rumpelkiste::presetDiscoveryFactory();
   return nullptr;
}
}
