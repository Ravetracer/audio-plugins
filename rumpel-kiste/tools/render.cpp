// Minimal CLAP host used to verify RumpelKiste outside a DAW: it walks the
// preset discovery factory as a host would, loads presets through the
// preset-load extension, runs the plugin's own sequencer against a rolling
// transport, and renders the result to a WAV file. --selftest runs the suite:
// the parameter table, the pattern format, the generator, the voices measured
// against the numbers the service notes give, the sequencer's timing, the
// state and the presets.
//
//   rumpelkiste-render --list
//   rumpelkiste-render --preset "Warehouse" --out groove.wav --seconds 16
//   rumpelkiste-render --all --outdir /tmp/rk --bpm 128
//   rumpelkiste-render --voice bd --accent 1 --out kick.wav
//   rumpelkiste-render --selftest
//   rumpelkiste-render --defaults > presets/new.rumpelkiste

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#if defined(_WIN32)
#   include <windows.h>
#else
#   include <dlfcn.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <clap/clap.h>

#if defined(_WIN32)
namespace {
void *dlopenCompat(const char *path) { return reinterpret_cast<void *>(LoadLibraryA(path)); }
void *dlsymCompat(void *h, const char *name) {
   return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
}
const char *dlerrorCompat() { return "see GetLastError()"; }
} // namespace
#   define RD_DLOPEN(p) dlopenCompat(p)
#   define RD_DLSYM(h, n) dlsymCompat(h, n)
#   define RD_DLERROR() dlerrorCompat()
#else
#   define RD_DLOPEN(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#   define RD_DLSYM(h, n) dlsym(h, n)
#   define RD_DLERROR() dlerror()
#endif

#include "dsp/drums.h"
#include "midifile.h"
#include "params.h"
#include "pattern.h"
#include "rumpelkiste.h"

using namespace rumpelkiste;

namespace {

void setEnvVar(const char *name, const char *value) {
#if defined(_WIN32)
   _putenv_s(name, value ? value : "");
#else
   if (value)
      setenv(name, value, 1);
   else
      unsetenv(name);
#endif
}

constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------- WAV out

bool writeWav(const std::string &path, const std::vector<float> &interleaved, uint32_t channels,
              uint32_t sampleRate) {
   FILE *f = std::fopen(path.c_str(), "wb");
   if (!f) {
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      return false;
   }
   const uint32_t frames = static_cast<uint32_t>(interleaved.size() / channels);
   const uint32_t dataBytes = frames * channels * 2;
   auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
   auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
   std::fwrite("RIFF", 1, 4, f);
   u32(36 + dataBytes);
   std::fwrite("WAVEfmt ", 1, 8, f);
   u32(16);
   u16(1);
   u16(static_cast<uint16_t>(channels));
   u32(sampleRate);
   u32(sampleRate * channels * 2);
   u16(static_cast<uint16_t>(channels * 2));
   u16(16);
   std::fwrite("data", 1, 4, f);
   u32(dataBytes);
   for (float s : interleaved) {
      s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
      const int16_t v = static_cast<int16_t>(std::lrintf(s * 32767.0f));
      u16(static_cast<uint16_t>(v));
   }
   std::fclose(f);
   return true;
}

// ---------------------------------------------------------------- host object

const clap_host_log_t kHostLog = {[](const clap_host_t *, clap_log_severity sev, const char *msg) {
   std::fprintf(stderr, "[plugin log %d] %s\n", sev, msg);
}};

uint32_t gRescanCount = 0;
const clap_host_params_t kHostParams = {
   [](const clap_host_t *, clap_param_rescan_flags) { ++gRescanCount; },
   [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
   [](const clap_host_t *) {},
};

uint32_t gPresetErrorCount = 0;
const clap_host_preset_load_t kHostPresetLoad = {
   [](const clap_host_t *, uint32_t, const char *loc, const char *key, int32_t, const char *msg) {
      ++gPresetErrorCount;
      std::fprintf(stderr, "[preset error] %s (%s / %s)\n", msg ? msg : "?", loc ? loc : "-",
                   key ? key : "-");
   },
   [](const clap_host_t *, uint32_t, const char *, const char *) {},
};

const clap_host_thread_check_t kHostThreadCheck = {
   [](const clap_host_t *) { return true; },
   [](const clap_host_t *) { return true; },
};

clap_host_t gHost = {
   CLAP_VERSION_INIT,
   nullptr,
   "rumpelkiste-render",
   "RumpelKiste",
   "https://example.invalid",
   "1.0.0",
   [](const clap_host_t *, const char *id) -> const void * {
      if (std::strcmp(id, CLAP_EXT_LOG) == 0)
         return &kHostLog;
      if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
         return &kHostParams;
      if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0)
         return &kHostPresetLoad;
      if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
         return &kHostThreadCheck;
      return nullptr;
   },
   [](const clap_host_t *) {},
   [](const clap_host_t *) {},
   [](const clap_host_t *) {},
};

// ------------------------------------------------------- preset discovery side

struct PresetEntry {
   std::string name;
   std::string loadKey;
   std::string location;
   uint32_t locationKind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
   std::string description;
};

struct Indexer {
   clap_preset_discovery_indexer_t iface{};
   std::vector<std::string> extensions;
   std::vector<std::pair<uint32_t, std::string>> locations;
};

struct Receiver {
   clap_preset_discovery_metadata_receiver_t iface{};
   std::vector<PresetEntry> *out = nullptr;
   uint32_t locationKind = 0;
   std::string location;
};

Indexer gIndexer;
bool gQuietDiscovery = false;

void setupIndexer() {
   gIndexer.iface.clap_version = CLAP_VERSION_INIT;
   gIndexer.iface.name = "rumpelkiste-render";
   gIndexer.iface.vendor = "RumpelKiste";
   gIndexer.iface.url = "https://example.invalid";
   gIndexer.iface.version = "1.0.0";
   gIndexer.iface.indexer_data = &gIndexer;
   gIndexer.iface.declare_filetype = [](const clap_preset_discovery_indexer_t *ix,
                                        const clap_preset_discovery_filetype_t *ft) {
      auto *self = static_cast<Indexer *>(ix->indexer_data);
      self->extensions.push_back(ft->file_extension ? ft->file_extension : "");
      if (!gQuietDiscovery)
         std::printf("  filetype: %s (.%s)\n", ft->name ? ft->name : "?",
                     ft->file_extension ? ft->file_extension : "");
      return true;
   };
   gIndexer.iface.declare_location = [](const clap_preset_discovery_indexer_t *ix,
                                        const clap_preset_discovery_location_t *loc) {
      auto *self = static_cast<Indexer *>(ix->indexer_data);
      self->locations.emplace_back(loc->kind, loc->location ? loc->location : "");
      if (!gQuietDiscovery)
         std::printf("  location: %s kind=%u path=%s\n", loc->name ? loc->name : "?", loc->kind,
                     loc->location ? loc->location : "(plugin container)");
      return true;
   };
   gIndexer.iface.declare_soundpack = [](const clap_preset_discovery_indexer_t *,
                                         const clap_preset_discovery_soundpack_t *) {
      return true;
   };
   gIndexer.iface.get_extension = [](const clap_preset_discovery_indexer_t *,
                                     const char *) -> const void * { return nullptr; };
}

void setupReceiver(Receiver &rx) {
   rx.iface.receiver_data = &rx;
   rx.iface.on_error = [](const clap_preset_discovery_metadata_receiver_t *, int32_t,
                          const char *msg) {
      std::fprintf(stderr, "  metadata error: %s\n", msg ? msg : "?");
   };
   rx.iface.begin_preset = [](const clap_preset_discovery_metadata_receiver_t *r,
                              const char *name, const char *loadKey) {
      auto *self = static_cast<Receiver *>(r->receiver_data);
      PresetEntry e;
      e.name = name ? name : "";
      e.loadKey = loadKey ? loadKey : "";
      e.location = self->location;
      e.locationKind = self->locationKind;
      self->out->push_back(e);
      return true;
   };
   rx.iface.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t *,
                               const clap_universal_plugin_id_t *) {};
   rx.iface.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t *,
                                  const char *) {};
   rx.iface.set_flags = [](const clap_preset_discovery_metadata_receiver_t *, uint32_t) {};
   rx.iface.add_creator = [](const clap_preset_discovery_metadata_receiver_t *, const char *) {};
   rx.iface.set_description = [](const clap_preset_discovery_metadata_receiver_t *r,
                                 const char *d) {
      auto *self = static_cast<Receiver *>(r->receiver_data);
      if (!self->out->empty() && d)
         self->out->back().description = d;
   };
   rx.iface.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t *, clap_timestamp,
                                clap_timestamp) {};
   rx.iface.add_feature = [](const clap_preset_discovery_metadata_receiver_t *, const char *) {};
   rx.iface.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t *, const char *,
                                const char *) {};
}

std::vector<PresetEntry> discoverPresets(const clap_plugin_entry_t *entry) {
   std::vector<PresetEntry> presets;
   const auto *factory = static_cast<const clap_preset_discovery_factory_t *>(
      entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
   if (!factory) {
      std::fprintf(stderr, "plugin exposes no preset discovery factory\n");
      return presets;
   }
   const uint32_t providerCount = factory->count(factory);
   for (uint32_t i = 0; i < providerCount; ++i) {
      const auto *desc = factory->get_descriptor(factory, i);
      if (!desc)
         continue;
      if (!gQuietDiscovery)
         std::printf("provider '%s' (%s)\n", desc->name, desc->id);
      const auto *provider = factory->create(factory, &gIndexer.iface, desc->id);
      if (!provider)
         continue;
      gIndexer.locations.clear();
      gIndexer.extensions.clear();
      if (!provider->init(provider)) {
         provider->destroy(provider);
         continue;
      }
      for (const auto &loc : gIndexer.locations) {
         Receiver rx;
         rx.out = &presets;
         setupReceiver(rx);
         if (loc.first == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
            rx.locationKind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
            provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr,
                                   &rx.iface);
            continue;
         }
         DIR *dir = opendir(loc.second.c_str());
         if (!dir)
            continue;
         std::vector<std::string> files;
         while (dirent *de = readdir(dir)) {
            const std::string name = de->d_name;
            if (name.size() < 2 || name[0] == '.')
               continue;
            for (const auto &ext : gIndexer.extensions)
               if (!ext.empty() && name.size() > ext.size() + 1 &&
                   name.compare(name.size() - ext.size(), ext.size(), ext) == 0 &&
                   name[name.size() - ext.size() - 1] == '.')
                  files.push_back(loc.second + "/" + name);
         }
         closedir(dir);
         std::sort(files.begin(), files.end());
         for (const auto &f : files) {
            rx.locationKind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
            rx.location = f;
            provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, f.c_str(),
                                   &rx.iface);
         }
      }
      provider->destroy(provider);
   }
   return presets;
}

// ------------------------------------------------------------------ rendering

struct EventList {
   std::vector<clap_event_param_value_t> params;
   std::vector<clap_event_note_t> notes;
   clap_input_events_t in{};

   // Parameters first, then notes, which is sorted by time only when every
   // parameter is at time 0 -- which is how everything here uses it.
   void build() {
      in.ctx = this;
      in.size = [](const clap_input_events_t *l) {
         auto *self = static_cast<EventList *>(l->ctx);
         return static_cast<uint32_t>(self->params.size() + self->notes.size());
      };
      in.get = [](const clap_input_events_t *l, uint32_t index) -> const clap_event_header_t * {
         auto *self = static_cast<EventList *>(l->ctx);
         if (index < self->params.size())
            return &self->params[index].header;
         index -= static_cast<uint32_t>(self->params.size());
         return index < self->notes.size() ? &self->notes[index].header : nullptr;
      };
   }
};

clap_event_param_value_t makeParamValue(clap_id id, double value) {
   clap_event_param_value_t ev{};
   ev.header.size = sizeof(ev);
   ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
   ev.header.type = CLAP_EVENT_PARAM_VALUE;
   ev.param_id = id;
   ev.note_id = -1;
   ev.port_index = -1;
   ev.channel = -1;
   ev.key = -1;
   ev.value = value;
   return ev;
}

clap_event_note_t makeNote(uint16_t type, uint32_t time, int16_t key, double velocity) {
   clap_event_note_t ev{};
   ev.header.size = sizeof(ev);
   ev.header.time = time;
   ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
   ev.header.type = type;
   ev.note_id = -1;
   ev.port_index = 0;
   ev.channel = 9;
   ev.key = key;
   ev.velocity = velocity;
   return ev;
}

std::vector<std::pair<clap_id, double>> gParamOverrides;

// What the plugin sent out of its note port, with absolute times.
std::vector<clap_event_note_t> gNoteOut;
std::vector<clap_event_param_value_t> gParamOut;
uint64_t gBlockStart = 0;

void installSink(clap_output_events_t &out) {
   out.ctx = nullptr;
   out.try_push = [](const clap_output_events_t *, const clap_event_header_t *h) {
      if (h && h->space_id == CLAP_CORE_EVENT_SPACE_ID) {
         if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
            clap_event_note_t ev = *reinterpret_cast<const clap_event_note_t *>(h);
            ev.header.time += static_cast<uint32_t>(gBlockStart);
            gNoteOut.push_back(ev);
         } else if (h->type == CLAP_EVENT_PARAM_VALUE) {
            clap_event_param_value_t ev = *reinterpret_cast<const clap_event_param_value_t *>(h);
            ev.header.time += static_cast<uint32_t>(gBlockStart);
            gParamOut.push_back(ev);
         }
      }
      return true;
   };
}

// How a render drives the transport.
struct Transport {
   bool playing = true;
   double bpm = 125.0;
   // Non-zero loops the song position back to zero every this many beats.
   double loopBeats = 0.0;
   // A jump in the song position at a given frame, for the seek tests.
   long jumpAtFrame = -1;
   double jumpToBeats = 0.0;
};

struct RenderResult {
   std::vector<float> left;
   float peak = 0.0f;
   double rms = 0.0;
   bool sawNonFinite = false;
};

// Scheduled input for a render: notes at absolute frames, parameters at
// absolute frames (applied at the top of the block containing them).
struct Scheduled {
   uint64_t frame;
   bool isNote;
   clap_event_note_t note;
   clap_event_param_value_t param;
};

RenderResult renderPlugin(const clap_plugin_t *plugin, double sampleRate, double seconds,
                          const Transport &tp, std::vector<Scheduled> input = {},
                          uint32_t block = 256) {
   RenderResult r;
   gNoteOut.clear();
   gParamOut.clear();
   const uint64_t total = static_cast<uint64_t>(seconds * sampleRate);
   r.left.resize(total, 0.0f);
   std::vector<float> L(block), R(block);
   float *chans[2] = {L.data(), R.data()};
   clap_audio_buffer_t out{};
   out.data32 = chans;
   out.channel_count = 2;
   clap_output_events_t outEvents{};
   installSink(outEvents);
   std::sort(input.begin(), input.end(),
             [](const Scheduled &a, const Scheduled &b) { return a.frame < b.frame; });
   size_t next = 0;
   bool jumped = false;
   double beatOffset = 0.0;
   double sumSq = 0.0;
   for (uint64_t pos = 0; pos < total; pos += block) {
      const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(block, total - pos));
      EventList ev;
      if (pos == 0)
         for (const auto &ov : gParamOverrides)
            ev.params.push_back(makeParamValue(ov.first, ov.second));
      while (next < input.size() && input[next].frame < pos + n) {
         if (input[next].isNote) {
            clap_event_note_t note = input[next].note;
            note.header.time = static_cast<uint32_t>(input[next].frame - pos);
            ev.notes.push_back(note);
         } else {
            clap_event_param_value_t p = input[next].param;
            p.header.time = 0;
            ev.params.push_back(p);
         }
         ++next;
      }
      ev.build();

      clap_event_transport_t tr{};
      tr.header.size = sizeof(tr);
      tr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      tr.header.type = CLAP_EVENT_TRANSPORT;
      tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
      if (tp.playing)
         tr.flags |= CLAP_TRANSPORT_IS_PLAYING;
      tr.tempo = tp.bpm;
      if (tp.jumpAtFrame >= 0 && !jumped && pos >= static_cast<uint64_t>(tp.jumpAtFrame)) {
         jumped = true;
         beatOffset = tp.jumpToBeats - static_cast<double>(pos) / sampleRate * tp.bpm / 60.0;
      }
      double beats = static_cast<double>(pos) / sampleRate * tp.bpm / 60.0 + beatOffset;
      if (tp.loopBeats > 0.0)
         beats = std::fmod(beats, tp.loopBeats);
      tr.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));

      clap_process_t proc{};
      proc.frames_count = n;
      proc.audio_outputs = &out;
      proc.audio_outputs_count = 1;
      proc.in_events = &ev.in;
      proc.out_events = &outEvents;
      proc.transport = &tr;
      proc.steady_time = static_cast<int64_t>(pos);
      gBlockStart = pos;
      plugin->process(plugin, &proc);
      for (uint32_t i = 0; i < n; ++i) {
         const float s = L[i];
         if (!std::isfinite(s) || !std::isfinite(R[i]))
            r.sawNonFinite = true;
         r.left[pos + i] = s;
         r.peak = std::max(r.peak, std::fabs(s));
         sumSq += static_cast<double>(s) * s;
      }
   }
   r.rms = total ? std::sqrt(sumSq / static_cast<double>(total)) : 0.0;
   return r;
}

const clap_plugin_t *createPlugin(const clap_plugin_entry_t *entry, double sampleRate) {
   const auto *factory =
      static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
   if (!factory || factory->get_plugin_count(factory) == 0)
      return nullptr;
   const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
   const clap_plugin_t *plugin = factory->create_plugin(factory, &gHost, desc->id);
   if (!plugin || !plugin->init(plugin))
      return nullptr;
   plugin->activate(plugin, sampleRate, 32, 4096);
   plugin->start_processing(plugin);
   return plugin;
}

void destroyPlugin(const clap_plugin_t *plugin) {
   if (!plugin)
      return;
   plugin->stop_processing(plugin);
   plugin->deactivate(plugin);
   plugin->destroy(plugin);
}

bool loadPreset(const clap_plugin_t *plugin, const PresetEntry &preset) {
   const auto *ext = static_cast<const clap_plugin_preset_load_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
   if (!ext)
      return false;
   const char *location =
      preset.locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? nullptr
                                                                   : preset.location.c_str();
   return ext->from_location(plugin, preset.locationKind, location, preset.loadKey.c_str());
}

std::string squash(const std::string &in) {
   std::string out;
   for (char c : in)
      if (std::isalnum(static_cast<unsigned char>(c)))
         out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return out;
}

// "<key or name>=<value>", value in the units the display prints, parsed by
// the plugin's own text_to_value -- so a percent parameter takes 0 to 100.
bool resolveParamOverrides(const std::vector<std::string> &specs) {
   gParamOverrides.clear();
   for (const auto &spec : specs) {
      const size_t eq = spec.find('=');
      if (eq == std::string::npos) {
         std::fprintf(stderr, "bad --param '%s', expected key=value\n", spec.c_str());
         return false;
      }
      const std::string key = squash(spec.substr(0, eq));
      const std::string value = spec.substr(eq + 1);
      const ParamDesc *found = nullptr;
      for (uint32_t i = 0; i < kNumParams && !found; ++i)
         if (squash(paramTable()[i].key) == key || squash(paramTable()[i].name) == key)
            found = &paramTable()[i];
      if (!found) {
         std::fprintf(stderr, "no such parameter: '%s'\n", spec.substr(0, eq).c_str());
         return false;
      }
      double raw = 0.0;
      if (!paramTextToValue(*found, value.c_str(), &raw)) {
         std::fprintf(stderr, "cannot parse '%s' for %s\n", value.c_str(), found->key);
         return false;
      }
      gParamOverrides.emplace_back(found->id, raw);
   }
   return true;
}

bool saveState(const clap_plugin_t *plugin, std::string &blob) {
   const auto *st =
      static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
   if (!st)
      return false;
   blob.clear();
   clap_ostream_t os{};
   os.ctx = &blob;
   os.write = [](const clap_ostream_t *s, const void *buf, uint64_t size) -> int64_t {
      static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buf), size);
      return static_cast<int64_t>(size);
   };
   return st->save(plugin, &os);
}

bool loadState(const clap_plugin_t *plugin, const std::string &blob) {
   const auto *st =
      static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
   if (!st)
      return false;
   struct Cursor {
      const std::string *b;
      size_t pos;
   } cur{&blob, 0};
   clap_istream_t is{};
   is.ctx = &cur;
   is.read = [](const clap_istream_t *s, void *buf, uint64_t size) -> int64_t {
      auto *c = static_cast<Cursor *>(s->ctx);
      const size_t n = std::min<size_t>(size, c->b->size() - c->pos);
      std::memcpy(buf, c->b->data() + c->pos, n);
      c->pos += n;
      return static_cast<int64_t>(n);
   };
   return st->load(plugin, &is);
}

// ----------------------------------------------------------------- analysis

// Frequency from rising zero crossings in [from, to) seconds, averaged.
double zeroCrossFreq(const std::vector<float> &x, double sr, double from, double to) {
   const size_t a = static_cast<size_t>(from * sr), b = std::min(x.size(), static_cast<size_t>(to * sr));
   double first = -1.0, last = -1.0;
   int n = 0;
   for (size_t i = a + 1; i < b; ++i)
      if (x[i - 1] <= 0.0f && x[i] > 0.0f) {
         const double t = (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
         if (first < 0.0)
            first = t;
         last = t;
         ++n;
      }
   if (n < 2)
      return 0.0;
   return (n - 1) * sr / (last - first);
}

double rmsIn(const std::vector<float> &x, double sr, double from, double to) {
   const size_t a = static_cast<size_t>(from * sr), b = std::min(x.size(), static_cast<size_t>(to * sr));
   double s = 0.0;
   for (size_t i = a; i < b; ++i)
      s += static_cast<double>(x[i]) * x[i];
   return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0.0;
}

double peakIn(const std::vector<float> &x, double sr, double from, double to) {
   const size_t a = static_cast<size_t>(from * sr), b = std::min(x.size(), static_cast<size_t>(to * sr));
   double p = 0.0;
   for (size_t i = a; i < b; ++i)
      p = std::max(p, static_cast<double>(std::fabs(x[i])));
   return p;
}

// Magnitude of one frequency over a window, by a single DFT bin (Hann).
double toneLevel(const std::vector<float> &x, double sr, double from, double to, double hz) {
   const size_t a = static_cast<size_t>(from * sr), b = std::min(x.size(), static_cast<size_t>(to * sr));
   const size_t n = b > a ? b - a : 0;
   if (n < 8)
      return 0.0;
   double re = 0.0, im = 0.0;
   for (size_t i = 0; i < n; ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
      const double ph = 2.0 * kPi * hz * static_cast<double>(i) / sr;
      re += w * x[a + i] * std::cos(ph);
      im -= w * x[a + i] * std::sin(ph);
   }
   return std::sqrt(re * re + im * im) / static_cast<double>(n);
}

// Power in a band, summed over single-bin estimates on a grid. Coarse, but a
// noise-like signal has to be judged over a band rather than at one bin.
double bandPower(const std::vector<float> &x, double sr, double from, double to, double lo,
                 double hi) {
   double p = 0.0;
   const double step = (hi - lo) / 60.0;
   for (double f = lo; f <= hi; f += step) {
      const double l = toneLevel(x, sr, from, to, f);
      p += l * l * step; // weighted by the width each sample stands for
   }
   return p;
}

// The frequency between lo and hi with the most energy, in steps of `step`.
double spectralPeak(const std::vector<float> &x, double sr, double from, double to, double lo,
                    double hi, double step) {
   double best = lo, bestLevel = -1.0;
   for (double f = lo; f <= hi; f += step) {
      const double l = toneLevel(x, sr, from, to, f);
      if (l > bestLevel) {
         bestLevel = l;
         best = f;
      }
   }
   return best;
}

// One voice hit on a bare engine, with every parameter at its default except
// what `tweak` changes.
template <typename F>
std::vector<float> renderVoice(int voice, float accent, double seconds, double sr, F tweak) {
   DrumParams p;
   // Defaults, from the table, so the engine is told exactly what the plugin
   // would tell it.
   auto real = [](uint32_t id) { return static_cast<float>(paramToReal(paramTable()[id], paramTable()[id].def)); };
   p.bdTune = real(kParamBdTune);
   p.bdLevel = real(kParamBdLevel);
   p.bdAttack = real(kParamBdAttack);
   p.bdDecaySec = real(kParamBdDecay) * 0.001f;
   p.sdTune = real(kParamSdTune);
   p.sdLevel = real(kParamSdLevel);
   p.sdTone = real(kParamSdTone);
   p.sdSnappy = real(kParamSdSnappy);
   for (int i = 0; i < 3; ++i) {
      p.tomTune[i] = 0.5f;
      p.tomLevel[i] = 0.8f;
      p.tomDecaySec[i] = real(kParamLtDecay) * 0.001f;
   }
   p.rsLevel = real(kParamRsLevel);
   p.cpLevel = real(kParamCpLevel);
   p.hhLevel = real(kParamHhLevel);
   p.chDecaySec = real(kParamChDecay) * 0.001f;
   p.ohDecaySec = real(kParamOhDecay) * 0.001f;
   p.crLevel = real(kParamCrLevel);
   p.crTune = real(kParamCrTune);
   p.rdLevel = real(kParamRdLevel);
   p.rdTune = real(kParamRdTune);
   p.gain = 1.0f;
   p.bdPitchHz = real(kParamBdPitch);
   p.bdSweep = real(kParamBdSweep);
   p.bdShape = real(kParamBdShape);
   p.sdPitchHz = real(kParamSdPitch);
   p.tomPitchHz = real(kParamTomPitch);
   p.tomSweep = real(kParamTomSweep);
   p.tomNoise = real(kParamTomNoise);
   p.rsGateSec = real(kParamRsDecay) * 0.001f;
   p.cpSpreadSec = real(kParamCpSpread) * 0.001f;
   p.hatColor = real(kParamHatColor);
   p.cymColor = real(kParamCymColor);
   p.dacBits = static_cast<int>(real(kParamDacBits));
   tweak(p);
   std::unique_ptr<DrumEngine> e(new DrumEngine());
   e->prepare(sr);
   e->setParams(p);
   if (voice >= 0)
      e->trigger(voice, accent);
   std::vector<float> out(static_cast<size_t>(seconds * sr), 0.0f);
   e->process(out.data(), static_cast<uint32_t>(out.size()));
   return out;
}

std::vector<float> renderVoice(int voice, float accent, double seconds, double sr) {
   return renderVoice(voice, accent, seconds, sr, [](DrumParams &) {});
}

// Time until the envelope (a moving peak) first falls below `frac` of its
// maximum, after the maximum.
double decayTime(const std::vector<float> &x, double sr, double frac) {
   const size_t win = static_cast<size_t>(0.005 * sr);
   std::vector<double> env;
   for (size_t i = 0; i + win < x.size(); i += win / 2) {
      double p = 0.0;
      for (size_t k = 0; k < win; ++k)
         p = std::max(p, static_cast<double>(std::fabs(x[i + k])));
      env.push_back(p);
   }
   size_t top = 0;
   for (size_t i = 0; i < env.size(); ++i)
      if (env[i] > env[top])
         top = i;
   for (size_t i = top; i < env.size(); ++i)
      if (env[i] < env[top] * frac)
         return static_cast<double>(i * (win / 2)) / sr;
   return static_cast<double>(x.size()) / sr;
}

// ------------------------------------------------------------------ self-test

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const char *what, double got = 0.0, double want = 0.0) {
   ++gChecks;
   if (ok) {
      std::printf("  ok    %s\n", what);
   } else {
      ++gFailures;
      std::printf("  FAIL  %s (got %.6g, want %.6g)\n", what, got, want);
   }
}

// Not called near(): <windows.h> defines that as a macro.
bool within(double got, double want, double relTol) {
   return std::fabs(got - want) <= std::fabs(want) * relTol;
}

void testParamTable() {
   std::printf("parameter table\n");
   bool ordered = true, inRange = true, tips = true, uniqueKeys = true;
   for (uint32_t i = 0; i < kNumParams; ++i) {
      const ParamDesc &d = paramTable()[i];
      ordered = ordered && d.id == i;
      inRange = inRange && d.def >= d.min && d.def <= d.max;
      tips = tips && d.tip && std::strlen(d.tip) > 10;
      for (uint32_t j = 0; j < i; ++j)
         uniqueKeys = uniqueKeys && std::strcmp(paramTable()[j].key, d.key) != 0;
   }
   check(ordered, "every row sits at its own id");
   check(inRange, "every default is inside its range");
   check(tips, "every parameter explains itself");
   check(uniqueKeys, "preset keys are unique");
   // The seed spans the whole word, which is what broke SäureKiste's GEN once.
   const uint32_t top = static_cast<uint32_t>(paramTable()[kParamGenSeed].max);
   bool differs = true;
   for (uint32_t s = 0; s < 64; ++s) {
      const uint32_t cur = top - s;
      differs = differs && nextGeneratorSeed(cur, s * 7919u, top) != cur;
   }
   check(differs && top == 0xFFFFFFFFu, "GEN moves the seed across the whole 32-bit range");
}

void testPatternFormat() {
   std::printf("pattern format\n");
   std::unique_ptr<PatternData> a(new PatternData()), b(new PatternData());
   // Every kind of cell, in a few patterns, with a chain.
   uint32_t x = 12345u;
   auto rnd = [&]() {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      return x;
   };
   for (int p : {0, 1, 7, 63})
      for (int i = 0; i < (p == 7 ? 64 : 16); ++i) {
         uint32_t w = 0;
         for (int v = 0; v < kNumVoiceTracks; ++v) {
            const int level = static_cast<int>(rnd() % 3);
            w = withLevel(w, v, level);
            if (level && voiceCanFlam(v) && rnd() % 4 == 0)
               w = withFlam(w, v, true);
         }
         w = withAccent(w, rnd() % 3 == 0);
         a->pattern(p)[i] = w;
      }
   a->present = true;
   a->chainPresent = true;
   a->chainMode[1] = kChainNext;
   a->chainRepeat[1] = 3;
   a->chainMode[40] = kChainRandom; // a chain on an empty pattern still travels
   const std::string text = formatPattern(*a);
   std::istringstream in(text);
   std::string line;
   while (std::getline(in, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos)
         continue;
      std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      while (!k.empty() && k.back() == ' ')
         k.pop_back();
      while (!v.empty() && v.front() == ' ')
         v.erase(v.begin());
      parsePatternLine(k, v, *b);
   }
   bool same = true;
   for (int i = 0; i < kMaxPatterns * kMaxSteps; ++i)
      same = same && a->steps[i] == b->steps[i];
   for (int p = 0; p < kMaxPatterns; ++p)
      same = same && a->chainMode[p] == b->chainMode[p] && a->chainRepeat[p] == b->chainRepeat[p];
   check(same, "a bank survives being written as text and read back");
   check(text.find("p3_") == std::string::npos, "an empty pattern is left out of the text");

   uint32_t w = 0;
   w = withLevel(w, kVoiceSD, kLevelAccent);
   w = withFlam(w, kVoiceSD, true);
   w = withLevel(w, kVoiceSD, kLevelOff);
   check(!stepFlam(w, kVoiceSD), "a voice that stops playing drops its flam");
   check(!stepFlam(withFlam(0, kVoiceCH, true), kVoiceCH), "the closed hat cannot flam");
}

void testGenerator() {
   std::printf("generator\n");
   GenSettings g;
   g.seed = 4242;
   uint32_t a[kMaxSteps], b[kMaxSteps];
   generatePattern(g, a);
   generatePattern(g, b);
   check(std::equal(a, a + kMaxSteps, b), "the same seed writes the same pattern");

   // Busy only adds: every hit at 30 % is still there at 80 %.
   bool superset = true;
   for (int style = 0; style < kNumGenStyles; ++style) {
      g.style = style;
      g.busy = 0.3;
      generatePattern(g, a);
      g.busy = 0.8;
      generatePattern(g, b);
      for (int i = 0; i < 16; ++i)
         for (int v = 0; v < kNumVoiceTracks; ++v)
            if (v != kVoiceCH && stepLevel(a[i], v) != kLevelOff && stepLevel(b[i], v) == kLevelOff)
               superset = false;
   }
   check(superset, "turning Busy up adds hits without moving the ones there");

   // Accents move accents and nothing else.
   g.style = kStyleTechno;
   g.busy = 0.5;
   g.accent = 0.1;
   generatePattern(g, a);
   g.accent = 0.9;
   generatePattern(g, b);
   bool sameHits = true;
   for (int i = 0; i < 16; ++i)
      for (int v = 0; v < kNumVoiceTracks; ++v)
         sameHits = sameHits && ((stepLevel(a[i], v) != kLevelOff) == (stepLevel(b[i], v) != kLevelOff));
   check(sameHits, "turning Accents up leaves every hit where it was");

   // House is four on the floor at any Busy.
   g.style = kStyleHouse;
   g.busy = 0.0;
   bool floor = true;
   for (uint32_t seed = 1; seed < 20; ++seed) {
      g.seed = seed;
      generatePattern(g, a);
      for (int i = 0; i < 16; i += 4)
         floor = floor && stepLevel(a[i], kVoiceBD) != kLevelOff;
   }
   check(floor, "House keeps the kick on every beat");
}

void testVoices(double sr) {
   std::printf("voices (%.0f Hz)\n", sr);
   char msg[160];

   // ---- bass drum
   {
      const auto x = renderVoice(kVoiceBD, 1.0f, 0.6, sr, [](DrumParams &p) {
         p.bdDecaySec = 0.345f;
         p.bdAttack = 0.0f;
      });
      const double late = zeroCrossFreq(x, sr, 0.2, 0.5);
      std::snprintf(msg, sizeof(msg), "the kick settles on BD Pitch (%.1f Hz)", late);
      check(within(late, 50.0, 0.04), msg, late, 50.0);
      // The first period: the oscillator starts from zero on the trigger, so
      // its first rising zero crossing is one whole cycle in.
      double early = 0.0;
      for (size_t i = static_cast<size_t>(0.001 * sr); i < x.size(); ++i)
         if (x[i - 1] <= 0.0f && x[i] > 0.0f) {
            early = sr / static_cast<double>(i);
            break;
         }
      std::snprintf(msg, sizeof(msg), "and its first cycle is well above it (%.1f Hz)", early);
      check(early > 1.7 * late, msg, early, 1.7 * late);

      // Tune is the sweep's time, not the pitch: the landing note does not
      // move, and the pitch stays up longer.
      const auto lo = renderVoice(kVoiceBD, 1.0f, 0.6, sr, [](DrumParams &p) {
         p.bdTune = 0.0f;
         p.bdAttack = 0.0f;
         p.bdDecaySec = 0.345f;
      });
      const auto hi = renderVoice(kVoiceBD, 1.0f, 0.6, sr, [](DrumParams &p) {
         p.bdTune = 1.0f;
         p.bdAttack = 0.0f;
         p.bdDecaySec = 0.345f;
      });
      const double fLo = zeroCrossFreq(lo, sr, 0.02, 0.09);
      const double fHi = zeroCrossFreq(hi, sr, 0.02, 0.09);
      std::snprintf(msg, sizeof(msg), "Tune holds the pitch up longer (%.1f vs %.1f Hz at 20-90 ms)",
                    fHi, fLo);
      check(fHi > fLo * 1.15, msg, fHi, fLo * 1.15);
      const double endLo = zeroCrossFreq(lo, sr, 0.3, 0.55), endHi = zeroCrossFreq(hi, sr, 0.3, 0.55);
      check(within(endHi, endLo, 0.03), "and does not move the note it lands on", endHi, endLo);

      // Decay: C8 through R58 + VR5.
      const auto shortK = renderVoice(kVoiceBD, 1.0f, 1.5, sr, [](DrumParams &p) {
         p.bdDecaySec = 0.05f;
         p.bdAttack = 0.0f;
      });
      const auto longK = renderVoice(kVoiceBD, 1.0f, 1.5, sr, [](DrumParams &p) {
         p.bdDecaySec = 0.3f;
         p.bdAttack = 0.0f;
      });
      const double tS = decayTime(shortK, sr, 0.1), tL = decayTime(longK, sr, 0.1);
      std::snprintf(msg, sizeof(msg), "Decay sets how long the kick lasts (%.0f vs %.0f ms)",
                    tS * 1000, tL * 1000);
      check(tL > 4.0 * tS, msg, tL, 4.0 * tS);

      const auto plain = renderVoice(kVoiceBD, 0.0f, 0.3, sr);
      const auto acc = renderVoice(kVoiceBD, 1.0f, 0.3, sr);
      const double ratio = peakIn(acc, sr, 0, 0.3) / peakIn(plain, sr, 0, 0.3);
      std::snprintf(msg, sizeof(msg), "an accented kick is louder (%.2fx)", ratio);
      check(ratio > 1.5, msg, ratio, 1.5);

      // Attack adds a click at the front and nothing after it.
      const auto noClick = renderVoice(kVoiceBD, 1.0f, 0.3, sr, [](DrumParams &p) { p.bdAttack = 0.0f; });
      const auto click = renderVoice(kVoiceBD, 1.0f, 0.3, sr, [](DrumParams &p) { p.bdAttack = 1.0f; });
      double front = 0.0, after = 0.0;
      for (size_t i = 0; i < click.size(); ++i) {
         const double d = std::fabs(click[i] - noClick[i]);
         (i < static_cast<size_t>(0.006 * sr) ? front : after) = std::max(i < static_cast<size_t>(0.006 * sr) ? front : after, d);
      }
      std::snprintf(msg, sizeof(msg), "Attack is a click in the first milliseconds (%.3f vs %.3f later)",
                    front, after);
      check(front > 0.05 && after < front * 0.35, msg, after, front * 0.35);
   }

   // ---- snare
   {
      const auto x = renderVoice(kVoiceSD, 1.0f, 0.3, sr, [](DrumParams &p) { p.sdSnappy = 0.0f; });
      const double f1 = spectralPeak(x, sr, 0.012, 0.06, 120.0, 260.0, 1.0);
      // The upper oscillator dies in about 10 ms and the bend is still in it
      // for the first few, so it is looked for where the bend has gone and
      // in a range that leaves the lower one out.
      const double f2 = spectralPeak(x, sr, 0.018, 0.05, 245.0, 330.0, 1.0);
      std::snprintf(msg, sizeof(msg), "the snare's lower oscillator sits at SD Pitch (%.0f Hz)", f1);
      check(within(f1, 190.0, 0.05), msg, f1, 190.0);
      std::snprintf(msg, sizeof(msg), "and the upper at 1.47 times it, C69/C71 (%.0f Hz)", f2);
      check(within(f2 / f1, 1.4706, 0.05), msg, f2 / f1, 1.4706);
      const auto tuned = renderVoice(kVoiceSD, 1.0f, 0.3, sr, [](DrumParams &p) {
         p.sdSnappy = 0.0f;
         p.sdTune = 1.0f;
      });
      const auto low = renderVoice(kVoiceSD, 1.0f, 0.3, sr, [](DrumParams &p) {
         p.sdSnappy = 0.0f;
         p.sdTune = 0.0f;
      });
      const double fu = spectralPeak(tuned, sr, 0.012, 0.06, 150.0, 330.0, 1.0);
      const double fl = spectralPeak(low, sr, 0.012, 0.06, 90.0, 200.0, 1.0);
      std::snprintf(msg, sizeof(msg), "Tune spans an octave (%.0f to %.0f Hz)", fl, fu);
      check(within(fu / fl, 2.0, 0.06), msg, fu / fl, 2.0);

      // Snappy is the noise; Tone is how long it lasts.
      const auto dry = renderVoice(kVoiceSD, 1.0f, 0.4, sr, [](DrumParams &p) { p.sdSnappy = 0.0f; });
      const auto wet = renderVoice(kVoiceSD, 1.0f, 0.4, sr, [](DrumParams &p) { p.sdSnappy = 1.0f; });
      const double hiDry = toneLevel(dry, sr, 0.0, 0.1, 6000.0), hiWet = toneLevel(wet, sr, 0.0, 0.1, 6000.0);
      check(hiWet > 20.0 * hiDry, "Snappy brings in the noise", hiWet, 20.0 * hiDry);
      const auto shortT = renderVoice(kVoiceSD, 1.0f, 0.8, sr, [](DrumParams &p) {
         p.sdSnappy = 1.0f;
         p.sdTone = 0.0f;
      });
      const auto longT = renderVoice(kVoiceSD, 1.0f, 0.8, sr, [](DrumParams &p) {
         p.sdSnappy = 1.0f;
         p.sdTone = 1.0f;
      });
      const double rS = rmsIn(shortT, sr, 0.15, 0.3), rL = rmsIn(longT, sr, 0.15, 0.3);
      std::snprintf(msg, sizeof(msg), "Tone lengthens the noise tail (%.4f vs %.4f at 150-300 ms)", rL, rS);
      check(rL > 3.0 * rS, msg, rL, 3.0 * rS);
   }

   // ---- toms: the ratios the capacitors give, and the octave of Tune
   {
      double f[3];
      for (int i = 0; i < 3; ++i) {
         const auto x = renderVoice(kVoiceLT + i, 1.0f, 0.6, sr, [](DrumParams &p) {
            p.tomSweep = 0.0f;
            p.tomNoise = 0.0f;
            for (float &d : p.tomDecaySec)
               d = 0.378f;
         });
         f[i] = spectralPeak(x, sr, 0.05, 0.3, 40.0, 260.0, 0.5);
      }
      std::snprintf(msg, sizeof(msg), "the low tom sits at Tom Pitch (%.1f Hz)", f[0]);
      check(within(f[0], 90.0, 0.03), msg, f[0], 90.0);
      std::snprintf(msg, sizeof(msg), "the mid tom 1.22 times above it, C19/C33 (%.3f)", f[1] / f[0]);
      check(within(f[1] / f[0], 0.033 / 0.027, 0.03), msg, f[1] / f[0], 0.033 / 0.027);
      std::snprintf(msg, sizeof(msg), "the hi tom 1.5 times, C19/C102 (%.3f)", f[2] / f[0]);
      check(within(f[2] / f[0], 1.5, 0.03), msg, f[2] / f[0], 1.5);
      const auto up = renderVoice(kVoiceLT, 1.0f, 0.6, sr, [](DrumParams &p) {
         p.tomSweep = 0.0f;
         p.tomNoise = 0.0f;
         p.tomTune[0] = 1.0f;
         p.tomDecaySec[0] = 0.378f;
      });
      const auto down = renderVoice(kVoiceLT, 1.0f, 0.6, sr, [](DrumParams &p) {
         p.tomSweep = 0.0f;
         p.tomNoise = 0.0f;
         p.tomTune[0] = 0.0f;
         p.tomDecaySec[0] = 0.378f;
      });
      const double fu = spectralPeak(up, sr, 0.05, 0.3, 80.0, 180.0, 0.5);
      const double fd = spectralPeak(down, sr, 0.05, 0.3, 40.0, 90.0, 0.5);
      std::snprintf(msg, sizeof(msg), "Tune spans an octave, R112/R113 (%.1f to %.1f Hz)", fd, fu);
      check(within(fu / fd, 2.0, 0.03), msg, fu / fd, 2.0);
      const auto swept = renderVoice(kVoiceLT, 1.0f, 0.6, sr);
      const double start = zeroCrossFreq(swept, sr, 0.0, 0.012);
      check(start > 1.25 * f[0], "and bends down from a sharper start", start, 1.25 * f[0]);
   }

   // ---- rim shot: three resonators and a high-pass
   {
      const auto x = renderVoice(kVoiceRS, 1.0f, 0.1, sr);
      const double lowE = toneLevel(x, sr, 0.0, 0.03, 100.0);
      const double midE = std::max(toneLevel(x, sr, 0.0, 0.03, 495.0), toneLevel(x, sr, 0.0, 0.03, 1053.0));
      std::snprintf(msg, sizeof(msg), "the rim shot's energy is above the 495 Hz high-pass (%.1fx)",
                    midE / std::max(lowE, 1e-12));
      check(midE > 4.0 * lowE, msg, midE, 4.0 * lowE);
      check(decayTime(x, sr, 0.05) < 0.05, "and is over within fifty milliseconds",
            decayTime(x, sr, 0.05), 0.05);
   }

   // ---- clap: four bursts, Clap Spread apart
   {
      const auto x = renderVoice(kVoiceCP, 1.0f, 0.2, sr);
      // Find the burst onsets: a sharp rise in a 1 ms envelope.
      const size_t w = static_cast<size_t>(0.001 * sr);
      std::vector<double> env;
      for (size_t i = 0; i + w < x.size(); i += w) {
         double p = 0.0;
         for (size_t k = 0; k < w; ++k)
            p = std::max(p, static_cast<double>(std::fabs(x[i + k])));
         env.push_back(p);
      }
      // A burst is the largest value within 3 ms either side of it, standing
      // clearly above the floor between bursts.
      double top = 0.0;
      for (double e : env)
         top = std::max(top, e);
      int bursts = 0;
      // Up to the fourth burst and a little past it: after that is the last
      // burst's own decay, whose noise has peaks of its own.
      const int n = static_cast<int>(std::min<size_t>(env.size(), 33));
      for (int i = 0; i < n; ++i) {
         bool peak = env[i] > 0.45 * top;
         for (int k = std::max(0, i - 3); k <= std::min(n - 1, i + 3) && peak; ++k)
            peak = k == i || env[k] < env[i];
         bursts += peak ? 1 : 0;
      }
      std::snprintf(msg, sizeof(msg), "the clap has four bursts (%d found)", bursts);
      check(bursts == 4, msg, bursts, 4);
   }

   // ---- hats and cymbals
   {
      const auto ch = renderVoice(kVoiceCH, 1.0f, 1.2, sr);
      const auto oh = renderVoice(kVoiceOH, 1.0f, 1.2, sr);
      const double tc = decayTime(ch, sr, 0.03), to = decayTime(oh, sr, 0.03);
      std::snprintf(msg, sizeof(msg), "the closed hat is shorter than the open one (%.0f vs %.0f ms)",
                    tc * 1000, to * 1000);
      check(to > 4.0 * tc, msg, to, 4.0 * tc);
      const double bright = bandPower(oh, sr, 0.0, 0.1, 5000.0, 12000.0);
      const double dull = bandPower(oh, sr, 0.0, 0.1, 100.0, 1000.0);
      std::snprintf(msg, sizeof(msg), "the hats are high-pass metal (%.1f dB above 5 kHz)",
                    10.0 * std::log10(bright / std::max(dull, 1e-20)));
      check(bright > 10.0 * dull, msg, bright, 10.0 * dull);

      // One voice: a closed hat cuts an open one off.
      DrumParams p;
      std::unique_ptr<DrumEngine> e(new DrumEngine());
      e->prepare(sr);
      p.gain = 1.0f;
      e->setParams(p);
      e->trigger(kVoiceOH, 1.0f);
      std::vector<float> y(static_cast<size_t>(0.6 * sr));
      e->process(y.data(), static_cast<uint32_t>(0.1 * sr));
      e->trigger(kVoiceCH, 0.0f);
      e->process(y.data() + static_cast<size_t>(0.1 * sr), static_cast<uint32_t>(y.size() - 0.1 * sr));
      const double tailChoked = rmsIn(y, sr, 0.3, 0.5), tailOpen = rmsIn(oh, sr, 0.3, 0.5);
      check(tailChoked < 0.1 * tailOpen, "a closed hat chokes an open one", tailChoked, 0.1 * tailOpen);

      // The six-bit converter's grit follows the decay down.
      const auto six = renderVoice(kVoiceOH, 1.0f, 0.5, sr);
      const auto clean = renderVoice(kVoiceOH, 1.0f, 0.5, sr, [](DrumParams &q) { q.dacBits = 16; });
      std::vector<float> diff(six.size());
      for (size_t i = 0; i < six.size(); ++i)
         diff[i] = six[i] - clean[i];
      const double nEarly = rmsIn(diff, sr, 0.0, 0.05), nLate = rmsIn(diff, sr, 0.3, 0.4);
      const double sEarly = rmsIn(clean, sr, 0.0, 0.05);
      std::snprintf(msg, sizeof(msg), "six bits are audible (%.1f %% of the signal)", 100 * nEarly / sEarly);
      check(nEarly > 0.01 * sEarly && nEarly < 0.3 * sEarly, msg, nEarly / sEarly, 0.05);
      check(nLate < 0.3 * nEarly, "and their noise decays with the sound", nLate, 0.3 * nEarly);

      const auto cr = renderVoice(kVoiceCR, 1.0f, 1.6, sr);
      const auto crFast = renderVoice(kVoiceCR, 1.0f, 1.6, sr, [](DrumParams &q) { q.crTune = 1.0f; });
      const double lc = decayTime(cr, sr, 0.05), lf = decayTime(crFast, sr, 0.05);
      std::snprintf(msg, sizeof(msg), "a crash tuned up is shorter, like a faster ROM clock (%.0f vs %.0f ms)",
                    lf * 1000, lc * 1000);
      check(lf < 0.85 * lc, msg, lf, 0.85 * lc);
      const auto rd = renderVoice(kVoiceRD, 1.0f, 1.6, sr);
      check(rmsIn(rd, sr, 0.5, 0.8) > 0.0 && rmsIn(rd, sr, 0.0, 0.05) > 3.0 * rmsIn(rd, sr, 0.5, 0.8),
            "the ride has a ping over a longer wash");
   }

   // ---- every voice: finite, audible, louder accented
   {
      bool finite = true, audible = true, louder = true;
      for (int v = 0; v < kNumVoices; ++v) {
         const auto a = renderVoice(v, 1.0f, 0.4, sr);
         const auto b = renderVoice(v, 0.0f, 0.4, sr);
         for (float s : a)
            finite = finite && std::isfinite(s);
         const double pa = peakIn(a, sr, 0, 0.4), pb = peakIn(b, sr, 0, 0.4);
         audible = audible && pa > 0.05 && pa <= 1.0;
         if (!(pa > 1.2 * pb)) {
            louder = false;
            std::printf("        voice %d: accented %.3f, plain %.3f\n", v, pa, pb);
         }
      }
      check(finite, "every voice renders finite samples");
      check(audible, "every voice is audible and inside full scale at its default level");
      check(louder, "every voice is louder accented than plain");
   }

   // ---- the drive bus
   {
      // Master: the whole mix through the model, which adds harmonics a kick's
      // clean sweep does not have.
      auto kick = [&](int mode, bool routed) {
         return renderVoice(kVoiceBD, 1.0f, 0.4, sr, [&](DrumParams &p) {
            p.driveMode = mode;
            p.driveModel = 4; // Fuzz
            p.drive = 0.8f;
            p.route[kVoiceBD] = routed;
            p.bdAttack = 0.0f;
         });
      };
      const auto clean = kick(kDriveOff, false), driven = kick(kDriveMaster, false);
      const double hClean = bandPower(clean, sr, 0.1, 0.3, 300.0, 3000.0) /
                            bandPower(clean, sr, 0.1, 0.3, 30.0, 120.0);
      const double hDriven = bandPower(driven, sr, 0.1, 0.3, 300.0, 3000.0) /
                             bandPower(driven, sr, 0.1, 0.3, 30.0, 120.0);
      std::snprintf(msg, sizeof(msg), "Drive Mode Master distorts the mix (%.1f dB more harmonics)",
                    10.0 * std::log10(hDriven / std::max(hClean, 1e-20)));
      check(hDriven > 30.0 * hClean, msg, hDriven, 30.0 * hClean);
      // Selected: only what is routed goes through it.
      const auto selRouted = kick(kDriveSelected, true), selDry = kick(kDriveSelected, false);
      double dRouted = 0.0, dDry = 0.0;
      for (size_t i = 0; i < clean.size(); ++i) {
         dRouted = std::max(dRouted, static_cast<double>(std::fabs(selRouted[i] - clean[i])));
         dDry = std::max(dDry, static_cast<double>(std::fabs(selDry[i] - clean[i])));
      }
      check(dRouted > 0.05, "Selected drives a voice routed to it", dRouted, 0.05);
      check(dDry < 1.0e-6, "and leaves a voice that is not routed untouched", dDry, 1.0e-6);
   }

   // ---- flams and mutes on the engine
   {
      DrumParams p;
      p.gain = 1.0f;
      std::unique_ptr<DrumEngine> e(new DrumEngine());
      e->prepare(sr);
      e->setParams(p);
      e->setMuted(kVoiceSD, true);
      std::vector<float> y(static_cast<size_t>(0.2 * sr));
      e->process(y.data(), static_cast<uint32_t>(0.05 * sr)); // the mute fades in 3 ms
      e->trigger(kVoiceSD, 1.0f);
      e->process(y.data(), static_cast<uint32_t>(y.size()));
      check(peakIn(y, sr, 0.0, 0.2) < 1e-4, "a muted voice is silent");
   }
}

// ----------------------------------------------------- tests through the plugin

// The frames of every note-on the sequencer sent out for one key.
std::vector<uint32_t> onsetsFor(int key) {
   std::vector<uint32_t> out;
   for (const auto &n : gNoteOut)
      if (n.header.type == CLAP_EVENT_NOTE_ON && n.key == key)
         out.push_back(n.header.time);
   return out;
}

Scheduled noteAt(uint64_t frame, int key, double velocity) {
   Scheduled s{};
   s.frame = frame;
   s.isNote = true;
   s.note = makeNote(CLAP_EVENT_NOTE_ON, 0, static_cast<int16_t>(key), velocity);
   return s;
}

// A fresh instance loaded with a bank written as preset text.
const clap_plugin_t *pluginWithPattern(const clap_plugin_entry_t *entry, double sr,
                                       const std::string &lines) {
   const clap_plugin_t *plugin = createPlugin(entry, sr);
   if (!plugin)
      return nullptr;
   std::error_code ec;
   const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "rumpelkiste-selftest";
   std::filesystem::create_directories(dir, ec);
   const std::string path = (dir / "p.rumpelkiste").string();
   FILE *f = std::fopen(path.c_str(), "wb");
   if (!f)
      return plugin;
   std::fprintf(f, "# RumpelKiste preset\nformat = 1\nname = Test\n%s", lines.c_str());
   std::fclose(f);
   const auto *ext = static_cast<const clap_plugin_preset_load_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
   ext->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), nullptr);
   return plugin;
}

void testSequencer(const clap_plugin_entry_t *entry, double sr) {
   std::printf("sequencer (%.0f Hz)\n", sr);
   char msg[160];
   const double stepFrames = 60.0 / 120.0 / 4.0 * sr; // a sixteenth at 120

   // Straight sixteenths: the kick on every beat lands on its frame.
   {
      const clap_plugin_t *p = pluginWithPattern(entry, sr, "p1_bd = x... x... x... x...\n");
      Transport tp;
      tp.bpm = 120.0;
      renderPlugin(p, sr, 2.1, tp);
      const auto on = onsetsFor(36);
      // 2.1 seconds at 120 is four beats and a bit: five kicks.
      bool placed = on.size() == 5;
      for (size_t i = 0; i < on.size(); ++i)
         placed = placed && std::fabs(on[i] - i * 4 * stepFrames) <= 1.0;
      std::snprintf(msg, sizeof(msg), "the kick lands on every beat to the sample (%zu hits)", on.size());
      check(placed, msg, on.empty() ? -1 : on[1], 4 * stepFrames);
      destroyPlugin(p);
   }

   // Shuffle: the second step of each pair is late by (setting - 1) units.
   {
      const clap_plugin_t *p = pluginWithPattern(entry, sr, "p1_ch = xxxx xxxx xxxx xxxx\nshuffle = 7\n");
      Transport tp;
      tp.bpm = 120.0;
      renderPlugin(p, sr, 0.6, tp);
      const auto on = onsetsFor(42);
      const double late = on.size() >= 2 ? on[1] - stepFrames : 0.0;
      std::snprintf(msg, sizeof(msg), "Shuffle 7 puts the off-step half a step late (%.0f frames)", late);
      check(on.size() >= 3 && std::fabs(late - 0.5 * stepFrames) <= 1.5 &&
               std::fabs(on[2] - 2 * stepFrames) <= 1.0,
            msg, late, 0.5 * stepFrames);
      destroyPlugin(p);
   }

   // Scale: eighth-note triplets are three steps to the beat.
   {
      const clap_plugin_t *p = pluginWithPattern(entry, sr, "p1_rs = xxxx xxxx xxxx\nscale = 1/8T\nsteps = 12\n");
      Transport tp;
      tp.bpm = 120.0;
      renderPlugin(p, sr, 0.8, tp);
      const auto on = onsetsFor(37);
      const double want = 60.0 / 120.0 / 3.0 * sr;
      check(on.size() >= 3 && std::fabs(static_cast<double>(on[1]) - want) <= 1.0,
            "Scale 1/8T makes a step a third of a beat", on.size() >= 2 ? on[1] : 0.0, want);
      destroyPlugin(p);
   }

   // Last Step wraps the pattern.
   {
      const clap_plugin_t *p = pluginWithPattern(entry, sr, "p1_bd = x..x ....\nsteps = 3\n");
      Transport tp;
      tp.bpm = 120.0;
      renderPlugin(p, sr, 1.0, tp);
      const auto on = onsetsFor(36);
      // x..|x..|x..: every third step, and step 3 (the second x) never plays.
      // One second at 120 is eight sixteenths: steps 0, 3 and 6.
      bool ok = on.size() == 3;
      for (size_t i = 0; i < on.size(); ++i)
         ok = ok && std::fabs(on[i] - i * 3 * stepFrames) <= 1.0;
      if (!ok)
         for (size_t i = 0; i < on.size(); ++i)
            std::printf("        onset %zu at %.2f steps\n", i, on[i] / stepFrames);
      check(ok, "Last Step 3 plays every third step and nothing past it");
      destroyPlugin(p);
   }

   // A chain: pattern 1 then pattern 2.
   {
      const clap_plugin_t *p = pluginWithPattern(
         entry, sr,
         "p1_bd = x... .... .... ....\np1_chain = Next\np2_sd = x... .... .... ....\np2_chain = "
         "Next\nchain_length = 2\n");
      Transport tp;
      tp.bpm = 120.0;
      renderPlugin(p, sr, 4.2, tp);
      const auto bd = onsetsFor(36), sd = onsetsFor(38);
      const double bar = 16 * stepFrames;
      check(bd.size() == 2 && sd.size() == 1 && std::fabs(sd[0] - bar) <= 1.0 &&
               std::fabs(bd[1] - 2 * bar) <= 1.0,
            "Chain Next plays pattern 2 after pattern 1 and wraps back");
      destroyPlugin(p);
   }

   // A flam: two hits, the flam interval apart, in the audio.
   {
      const clap_plugin_t *p = pluginWithPattern(entry, sr, "p1_sd = f... .... .... ....\nflam = 8\nflam_unit = 4\n");
      Transport tp;
      tp.bpm = 120.0;
      const auto r = renderPlugin(p, sr, 0.2, tp);
      // The second stroke restarts the snare's oscillators: its onset shows as
      // a jump in level 32 ms in.
      const double before = peakIn(r.left, sr, 0.020, 0.030), after = peakIn(r.left, sr, 0.032, 0.040);
      std::snprintf(msg, sizeof(msg), "a flam's second stroke arrives Flam x Flam Unit later (%.3f -> %.3f)",
                    before, after);
      check(after > 1.5 * before, msg, after, 1.5 * before);
      destroyPlugin(p);
   }

   // A seek backwards still fires the step it lands on.
   {
      const clap_plugin_t *p = pluginWithPattern(entry, sr, "p1_bd = x... x... x... x...\n");
      Transport tp;
      tp.bpm = 120.0;
      tp.loopBeats = 2.0; // loop every two beats: the kick on 1 and on 2, forever
      renderPlugin(p, sr, 3.0, tp);
      const auto on = onsetsFor(36);
      // Three seconds at 120 is six beats: six kicks, none dropped at a seam.
      std::snprintf(msg, sizeof(msg), "a looping host drops no step at the seam (%zu kicks in 6 beats)", on.size());
      check(on.size() == 6, msg, static_cast<double>(on.size()), 6);
      destroyPlugin(p);
   }

   // MIDI plays the voices in both modes; a mapped note selects a pattern.
   {
      const clap_plugin_t *p = pluginWithPattern(entry, sr, "mode = MIDI\n");
      Transport tp;
      tp.playing = false;
      const auto r = renderPlugin(p, sr, 0.3, tp, {noteAt(100, 36, 1.0)});
      check(peakIn(r.left, sr, 0.0, 0.3) > 0.05, "key 36 plays the kick in MIDI mode");
      destroyPlugin(p);
      p = pluginWithPattern(entry, sr, "mode = MIDI\n");
      const auto silent = renderPlugin(p, sr, 0.3, tp, {noteAt(100, 60, 1.0)});
      check(peakIn(silent.left, sr, 0.0, 0.3) < 1e-4, "a key with no voice plays nothing");
      destroyPlugin(p);
   }

   // State: a bank and its mutes survive a save and a load.
   {
      const clap_plugin_t *a = pluginWithPattern(entry, sr, "p5_oh = ..x. ..x.\np5_chain = First\nbd_decay = 200\n");
      std::string blob, blob2;
      check(saveState(a, blob), "state saves");
      const clap_plugin_t *b = createPlugin(entry, sr);
      check(loadState(b, blob), "state loads");
      saveState(b, blob2);
      check(blob == blob2, "state round-trips byte for byte", static_cast<double>(blob2.size()),
            static_cast<double>(blob.size()));
      destroyPlugin(a);
      destroyPlugin(b);
   }

   // Determinism: the same preset renders the same samples twice.
   {
      const std::string text = "p1_bd = x... x... x... x...\np1_sd = .... x... .... x...\n"
                               "p1_oh = ..x. ..x. ..x. ..x.\np1_cp = .... x... .... x..x\n";
      const clap_plugin_t *a = pluginWithPattern(entry, sr, text);
      const clap_plugin_t *b = pluginWithPattern(entry, sr, text);
      Transport tp;
      const auto ra = renderPlugin(a, sr, 1.5, tp);
      const auto rb = renderPlugin(b, sr, 1.5, tp);
      check(ra.left == rb.left, "the same events render bit-identical samples");
      destroyPlugin(a);
      destroyPlugin(b);
   }

   // The MIDI file the window drags out.
   {
      uint32_t steps[kMaxSteps] = {0};
      defaultPattern(steps);
      MidiExport spec;
      spec.steps = steps;
      const std::string bytes = patternToMidiFile(spec);
      int noteOns = 0;
      for (size_t i = 0; i + 2 < bytes.size(); ++i)
         if (static_cast<unsigned char>(bytes[i]) == 0x99 && bytes[i + 2] != 0)
            ++noteOns;
      uint32_t want = 0;
      for (int i = 0; i < 16; ++i)
         for (int v = 0; v < kNumVoiceTracks; ++v)
            want += stepLevel(steps[i], v) != kLevelOff ? 1 : 0;
      check(bytes.compare(0, 4, "MThd") == 0 && noteOns == static_cast<int>(want),
            "the dragged MIDI file carries every hit of the pattern", noteOns, want);
   }
}

void testPresets(const clap_plugin_entry_t *entry, const std::vector<PresetEntry> &presets,
                 double sr) {
   std::printf("presets\n");
   check(!presets.empty(), "the factory library is discovered");
   int bad = 0, quiet = 0, hot = 0;
   for (const auto &pr : presets) {
      const clap_plugin_t *p = createPlugin(entry, sr);
      if (!loadPreset(p, pr)) {
         ++bad;
         destroyPlugin(p);
         continue;
      }
      Transport tp;
      tp.bpm = 126.0;
      const auto r = renderPlugin(p, sr, 4.0, tp);
      if (r.sawNonFinite)
         ++bad;
      if (r.peak < 0.05) {
         ++quiet;
         std::printf("        '%s' peaks at %.4f\n", pr.name.c_str(), r.peak);
      }
      if (r.peak > 0.999) {
         ++hot;
         std::printf("        '%s' reaches full scale\n", pr.name.c_str());
      }
      destroyPlugin(p);
   }
   check(bad == 0, "every preset loads and renders finite samples", bad, 0);
   check(quiet == 0, "every preset makes a sound", quiet, 0);
   check(hot == 0, "no preset reaches full scale", hot, 0);
   check(gPresetErrorCount == 0, "no preset reported an error", gPresetErrorCount, 0);
}

int runSelfTest(const clap_plugin_entry_t *entry, const std::vector<PresetEntry> &presets) {
   testParamTable();
   testPatternFormat();
   testGenerator();
   for (double sr : {44100.0, 48000.0, 96000.0})
      testVoices(sr);
   for (double sr : {44100.0, 48000.0})
      testSequencer(entry, sr);
   testPresets(entry, presets, 48000.0);
   std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
   return gFailures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
   std::string pluginPath = "./RumpelKiste.clap";
   std::string presetSel, outPath = "rumpel.wav", outDir = ".", voiceSel;
   double seconds = 8.0, tail = 1.0, bpm = 125.0, sampleRate = 48000.0, accent = 0.0;
   bool doList = false, doAll = false, doSelfTest = false, doDefaults = false;
   std::vector<std::string> paramSpecs;

   for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
      if (a == "--plugin")
         pluginPath = next();
      else if (a == "--preset")
         presetSel = next();
      else if (a == "--out")
         outPath = next();
      else if (a == "--outdir")
         outDir = next();
      else if (a == "--seconds")
         seconds = std::atof(next().c_str());
      else if (a == "--tail")
         tail = std::atof(next().c_str());
      else if (a == "--bpm")
         bpm = std::atof(next().c_str());
      else if (a == "--rate")
         sampleRate = std::atof(next().c_str());
      else if (a == "--param")
         paramSpecs.push_back(next());
      else if (a == "--voice")
         voiceSel = next();
      else if (a == "--accent")
         accent = std::atof(next().c_str());
      else if (a == "--list")
         doList = true;
      else if (a == "--all")
         doAll = true;
      else if (a == "--selftest")
         doSelfTest = true;
      else if (a == "--defaults")
         doDefaults = true;
      else {
         std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
         return 2;
      }
   }

   if (doDefaults) {
      PresetData data;
      data.name = "Defaults";
      data.author = "RumpelKiste";
      data.description = "Every parameter at the value the table gives it.";
      for (uint32_t i = 0; i < kNumParams; ++i)
         data.values.emplace_back(paramTable()[i].id, paramTable()[i].def);
      std::fputs(formatPreset(data).c_str(), stdout);
      return 0;
   }

   // One voice, on a bare engine: what the analysis helpers read.
   if (!voiceSel.empty()) {
      int voice = -1;
      for (int t = 0; t < kNumVoiceTracks; ++t)
         if (squash(trackKey(t)) == squash(voiceSel))
            voice = t;
      if (voice < 0) {
         std::fprintf(stderr, "no such voice: %s (bd sd lt mt ht rs cp ch oh cr rd)\n",
                      voiceSel.c_str());
         return 2;
      }
      const auto x = renderVoice(voice, static_cast<float>(accent), seconds, sampleRate);
      std::vector<float> st;
      for (float s : x) {
         st.push_back(s);
         st.push_back(s);
      }
      return writeWav(outPath, st, 2, static_cast<uint32_t>(sampleRate)) ? 0 : 1;
   }

   void *lib = RD_DLOPEN(pluginPath.c_str());
   if (!lib) {
      std::fprintf(stderr, "cannot load %s: %s\n", pluginPath.c_str(), RD_DLERROR());
      return 1;
   }
   const auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(RD_DLSYM(lib, "clap_entry"));
   if (!entry || !entry->init(pluginPath.c_str())) {
      std::fprintf(stderr, "no usable clap_entry in %s\n", pluginPath.c_str());
      return 1;
   }

   if (doSelfTest) {
      // The user's own library must not leak into what the test sees.
      std::error_code ec;
      const std::string cfg =
         (std::filesystem::temp_directory_path(ec) / "rumpelkiste-selftest-config").string();
      setEnvVar("XDG_CONFIG_HOME", cfg.c_str());
      gQuietDiscovery = true;
   }
   setupIndexer();
   const std::vector<PresetEntry> presets = discoverPresets(entry);

   if (doList) {
      for (const auto &p : presets)
         std::printf("%-28s %s\n", p.name.c_str(),
                     p.locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? "(built in)"
                                                                            : p.location.c_str());
      return 0;
   }

   if (doSelfTest) {
      const int rc = runSelfTest(entry, presets);
      entry->deinit();
      return rc;
   }

   if (!resolveParamOverrides(paramSpecs))
      return 2;

   auto renderOne = [&](const PresetEntry *preset, const std::string &path) {
      const clap_plugin_t *p = createPlugin(entry, sampleRate);
      if (!p)
         return false;
      if (preset && !loadPreset(p, *preset)) {
         destroyPlugin(p);
         return false;
      }
      Transport tp;
      tp.bpm = bpm;
      auto r = renderPlugin(p, sampleRate, seconds, tp);
      // The tail: the transport stops and the last hits ring out.
      Transport stop = tp;
      stop.playing = false;
      const auto t = renderPlugin(p, sampleRate, tail, stop);
      r.left.insert(r.left.end(), t.left.begin(), t.left.end());
      std::vector<float> st;
      float peak = 0.0f;
      for (float s : r.left) {
         st.push_back(s);
         st.push_back(s);
         peak = std::max(peak, std::fabs(s));
      }
      destroyPlugin(p);
      std::printf("%-28s peak %.3f -> %s\n", preset ? preset->name.c_str() : "(default)", peak,
                  path.c_str());
      return writeWav(path, st, 2, static_cast<uint32_t>(sampleRate));
   };

   if (doAll) {
      std::error_code ec;
      std::filesystem::create_directories(outDir, ec);
      bool ok = true;
      for (const auto &p : presets) {
         std::string stem;
         for (char c : p.name)
            stem += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
         ok = renderOne(&p, outDir + "/" + stem + ".wav") && ok;
      }
      return ok ? 0 : 1;
   }

   const PresetEntry *chosen = nullptr;
   for (const auto &p : presets)
      if (!presetSel.empty() &&
          (squash(p.name) == squash(presetSel) || squash(p.loadKey) == squash(presetSel)))
         chosen = &p;
   if (!presetSel.empty() && !chosen) {
      std::fprintf(stderr, "no preset '%s'\n", presetSel.c_str());
      return 2;
   }
   return renderOne(chosen, outPath) ? 0 : 1;
}
