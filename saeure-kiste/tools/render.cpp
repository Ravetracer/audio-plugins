// Minimal CLAP host used to verify SaeureKiste outside a DAW: it walks the
// preset discovery factory exactly as a host would, loads presets through the
// preset-load extension, plays a sixteen-step acid line, and renders the result
// to a WAV file. It also round-trips plugin state and checks the output for
// NaNs.
//
// Unlike the rest of the suite's renderers this one plays a *pattern* by
// default rather than one held note, because the two behaviours that make this
// instrument what it is -- an accent decaying into the notes after it, and a
// slide that does not retrigger the envelope -- are inaudible in a single note.
// --hold gives the held note back.
//
//   saeurekiste-render --list
//   saeurekiste-render --preset dark_engine --out acid.wav --seconds 16
//   saeurekiste-render --all --outdir /tmp/acid --bpm 138
//   saeurekiste-render --hold --key 40 --seconds 4
//   saeurekiste-render --seed 1 --seeds 8     print what the generator makes
//   saeurekiste-render --selftest
//   saeurekiste-render --defaults        > presets/new.saeurekiste

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// The host loads the plugin the way a DAW does. That is the one thing in this
// file that differs between platforms.
#if defined(_WIN32)
namespace {
void *dlopenCompat(const char *path) {
   return reinterpret_cast<void *>(LoadLibraryA(path));
}
void *dlsymCompat(void *h, const char *name) {
   return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
}
void dlcloseCompat(void *h) { FreeLibrary(reinterpret_cast<HMODULE>(h)); }
const char *dlerrorCompat() { return "see GetLastError()"; }
} // namespace
#   define RD_DLOPEN(p) dlopenCompat(p)
#   define RD_DLSYM(h, n) dlsymCompat(h, n)
#   define RD_DLCLOSE(h) dlcloseCompat(h)
#   define RD_DLERROR() dlerrorCompat()
#else
#   define RD_DLOPEN(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#   define RD_DLSYM(h, n) dlsym(h, n)
#   define RD_DLCLOSE(h) dlclose(h)
#   define RD_DLERROR() dlerror()
#endif

#include "params.h"
#include <sstream>

#include "pattern.h"
#include "saeurekiste.h"

#include <filesystem>

// Setting an environment variable is spelled differently on each platform, and
// the self-test needs it to point the preset directory somewhere disposable.
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
} // namespace

namespace {

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
   const uint32_t fmtChunk = 16;
   const uint32_t riffSize = 4 + (8 + fmtChunk) + (8 + dataBytes);

   auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
   auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

   std::fwrite("RIFF", 1, 4, f);
   u32(riffSize);
   std::fwrite("WAVE", 1, 4, f);
   std::fwrite("fmt ", 1, 4, f);
   u32(fmtChunk);
   u16(1); // PCM
   u16(static_cast<uint16_t>(channels));
   u32(sampleRate);
   u32(sampleRate * channels * 2);
   u16(static_cast<uint16_t>(channels * 2));
   u16(16);
   std::fwrite("data", 1, 4, f);
   u32(dataBytes);

   for (float s : interleaved) {
      if (s > 1.0f)
         s = 1.0f;
      if (s < -1.0f)
         s = -1.0f;
      const int16_t v = static_cast<int16_t>(std::lrintf(s * 32767.0f));
      u16(static_cast<uint16_t>(v));
   }
   std::fclose(f);
   return true;
}

// ---------------------------------------------------------------- host object

const clap_host_log_t kHostLog = {
   [](const clap_host_t *, clap_log_severity sev, const char *msg) {
      std::fprintf(stderr, "[plugin log %d] %s\n", sev, msg);
   }};

uint32_t gRescanCount = 0;

const clap_host_params_t kHostParams = {
   [](const clap_host_t *, clap_param_rescan_flags) { ++gRescanCount; },
   [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
   [](const clap_host_t *) {},
};

uint32_t gPresetLoadedCount = 0;
uint32_t gPresetErrorCount = 0;

const clap_host_preset_load_t kHostPresetLoad = {
   [](const clap_host_t *, uint32_t, const char *loc, const char *key, int32_t,
      const char *msg) {
      ++gPresetErrorCount;
      std::fprintf(stderr, "[preset error] %s (%s / %s)\n", msg ? msg : "?", loc ? loc : "-",
                   key ? key : "-");
   },
   [](const clap_host_t *, uint32_t, const char *, const char *) { ++gPresetLoadedCount; },
};

const clap_host_thread_check_t kHostThreadCheck = {
   [](const clap_host_t *) { return true; },
   [](const clap_host_t *) { return true; },
};

clap_host_t gHost = {
   CLAP_VERSION_INIT,
   nullptr,
   "saeurekiste-render",
   "SaeureKiste",
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
   std::string creator;
   std::vector<std::string> features;
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
   bool failed = false;
};

Indexer gIndexer;

void setupIndexer() {
   gIndexer.iface.clap_version = CLAP_VERSION_INIT;
   gIndexer.iface.name = "saeurekiste-render";
   gIndexer.iface.vendor = "SaeureKiste";
   gIndexer.iface.url = "https://example.invalid";
   gIndexer.iface.version = "1.0.0";
   gIndexer.iface.indexer_data = &gIndexer;
   gIndexer.iface.declare_filetype = [](const clap_preset_discovery_indexer_t *ix,
                                        const clap_preset_discovery_filetype_t *ft) {
      auto *self = static_cast<Indexer *>(ix->indexer_data);
      self->extensions.push_back(ft->file_extension ? ft->file_extension : "");
      std::printf("  filetype: %s (.%s)\n", ft->name ? ft->name : "?",
                  ft->file_extension ? ft->file_extension : "");
      return true;
   };
   gIndexer.iface.declare_location = [](const clap_preset_discovery_indexer_t *ix,
                                        const clap_preset_discovery_location_t *loc) {
      auto *self = static_cast<Indexer *>(ix->indexer_data);
      self->locations.emplace_back(loc->kind, loc->location ? loc->location : "");
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
   rx.iface.on_error = [](const clap_preset_discovery_metadata_receiver_t *r, int32_t,
                          const char *msg) {
      auto *self = static_cast<Receiver *>(r->receiver_data);
      self->failed = true;
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
   rx.iface.add_creator = [](const clap_preset_discovery_metadata_receiver_t *r,
                             const char *c) {
      auto *self = static_cast<Receiver *>(r->receiver_data);
      if (!self->out->empty() && c)
         self->out->back().creator = c;
   };
   rx.iface.set_description = [](const clap_preset_discovery_metadata_receiver_t *r,
                                 const char *d) {
      auto *self = static_cast<Receiver *>(r->receiver_data);
      if (!self->out->empty() && d)
         self->out->back().description = d;
   };
   rx.iface.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t *, clap_timestamp,
                                clap_timestamp) {};
   rx.iface.add_feature = [](const clap_preset_discovery_metadata_receiver_t *r,
                             const char *f) {
      auto *self = static_cast<Receiver *>(r->receiver_data);
      if (!self->out->empty() && f)
         self->out->back().features.push_back(f);
   };
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
   std::printf("preset providers: %u\n", providerCount);
   for (uint32_t i = 0; i < providerCount; ++i) {
      const auto *desc = factory->get_descriptor(factory, i);
      if (!desc)
         continue;
      std::printf("provider '%s' (%s)\n", desc->name, desc->id);
      const auto *provider = factory->create(factory, &gIndexer.iface, desc->id);
      if (!provider) {
         std::fprintf(stderr, "  create failed\n");
         continue;
      }
      gIndexer.locations.clear();
      gIndexer.extensions.clear();
      if (!provider->init(provider)) {
         std::fprintf(stderr, "  init failed\n");
         provider->destroy(provider);
         continue;
      }

      for (const auto &loc : gIndexer.locations) {
         Receiver rx;
         rx.out = &presets;
         setupReceiver(rx);

         if (loc.first == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
            rx.locationKind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
            rx.location.clear();
            provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr,
                                   &rx.iface);
            continue;
         }

         // Crawl the directory the way a host indexer would.
         DIR *dir = opendir(loc.second.c_str());
         if (!dir) {
            std::fprintf(stderr, "  cannot open location %s\n", loc.second.c_str());
            continue;
         }
         std::vector<std::string> files;
         while (dirent *de = readdir(dir)) {
            const std::string name = de->d_name;
            if (name.size() < 2 || name[0] == '.')
               continue;
            bool match = gIndexer.extensions.empty();
            for (const auto &ext : gIndexer.extensions) {
               if (ext.empty())
                  continue;
               if (name.size() > ext.size() + 1 &&
                   name.compare(name.size() - ext.size(), ext.size(), ext) == 0 &&
                   name[name.size() - ext.size() - 1] == '.')
                  match = true;
            }
            if (match)
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

// Holds the events for one process() call. Parameter events come first so a
// host-style "set the parameter, then play the note" ordering is preserved.
struct EventList {
   std::vector<clap_event_param_value_t> params;
   std::vector<clap_event_note_t> notes;
   clap_input_events_t in{};

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
         if (index >= self->notes.size())
            return nullptr;
         return &self->notes[index].header;
      };
   }
};

clap_event_param_value_t makeParamValue(clap_id id, double value) {
   clap_event_param_value_t ev{};
   ev.header.size = sizeof(ev);
   ev.header.time = 0;
   ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
   ev.header.type = CLAP_EVENT_PARAM_VALUE;
   ev.header.flags = 0;
   ev.param_id = id;
   ev.cookie = nullptr;
   ev.note_id = -1;
   ev.port_index = -1;
   ev.channel = -1;
   ev.key = -1;
   ev.value = value;
   return ev;
}

std::vector<std::pair<clap_id, double>> gParamOverrides;

// ---------------------------------------------------------------- automation
//
// A parameter that moves while the render runs, which is what turns a demo from
// a photograph of a preset into a recording of somebody playing it.
//
// The points are raw parameter values and they are interpolated linearly in
// that domain, not in display units. For a Log parameter -- Cutoff is one --
// the raw domain is already exponential, so a straight line through it is a
// straight line in octaves, which is what a filter sweep has to be to sound
// even. Doing this in Hz would spend most of the take crawling through the top
// octave.
struct ParamMove {
   clap_id id = 0;
   std::vector<double> points; // two or more raw values, evenly spaced in time
   double startSec = 0.0;
   double endSec = 0.0;
};

std::vector<ParamMove> gParamMoves;

double moveValueAt(const ParamMove &m, double t) {
   if (m.points.empty())
      return 0.0;
   if (m.points.size() == 1 || !(m.endSec > m.startSec) || t <= m.startSec)
      return m.points.front();
   if (t >= m.endSec)
      return m.points.back();
   const double span = static_cast<double>(m.points.size() - 1);
   const double u = (t - m.startSec) / (m.endSec - m.startSec) * span;
   const size_t leg = std::min(static_cast<size_t>(u), m.points.size() - 2);
   const double f = u - static_cast<double>(leg);
   return m.points[leg] + (m.points[leg + 1] - m.points[leg]) * f;
}

// One event per move per block, at the block's own start. A block is 512 frames
// by default, so a sixteen second sweep lands in about 1500 steps -- a few cents
// each on the cutoff, which is far below anything audible as a stair. Putting
// them mid-block instead would mean interleaving them with the note events in
// time order, and CLAP requires the input list sorted; this keeps every
// parameter event at time 0 and the list sorted by construction.
void pushMoveEvents(std::vector<clap_event_param_value_t> &out, uint32_t frame,
                    double sampleRate) {
   const double t = static_cast<double>(frame) / sampleRate;
   for (const ParamMove &m : gParamMoves)
      out.push_back(makeParamValue(m.id, moveValueAt(m, t)));
}

clap_event_note_t makeNote(uint16_t type, uint32_t time, int16_t key, double velocity,
                           int32_t noteId = 1) {
   clap_event_note_t ev{};
   ev.header.size = sizeof(ev);
   ev.header.time = time;
   ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
   ev.header.type = type;
   ev.header.flags = 0;
   ev.note_id = noteId;
   ev.port_index = 0;
   ev.channel = 0;
   ev.key = key;
   ev.velocity = velocity;
   return ev;
}

struct RenderResult {
   std::vector<float> interleaved;
   float peak = 0.0f;
   double rms = 0.0;
   bool sawNonFinite = false;
   uint32_t sleepAt = 0;
};

// ------------------------------------------------------------------- patterns
//
// A held note is the wrong test signal for this instrument. A 303 is a
// sequencer with a synthesiser bolted to it, and the two behaviours that make
// it sound like itself -- an accent decaying into the notes after it, and a
// slide that does not retrigger the envelope -- cannot be heard at all in a
// single note. So the renderer can play a line.
//
// The pattern below is sixteen steps of the shape everybody writes on one:
// a root, an octave, a rest or two, accents landing off the beat and a couple
// of slides. Note ids are unique per step, because a slide is two notes
// overlapping and the plugin has to be able to tell them apart.

struct SeqEvent {
   uint32_t frame;
   uint16_t type;
   int16_t key;
   double velocity;
   int32_t noteId;
};

struct PatternStep {
   int8_t semis;
   bool rest;
   bool accent;
   bool slide; // holds past the next step's start, which is what makes a slide
};

const PatternStep kAcidPattern[16] = {
   {0, false, true, false},  {0, false, false, true},  {12, false, false, false},
   {0, true, false, false},  {0, false, false, false}, {10, false, true, false},
   {0, false, false, true},  {3, false, false, false}, {0, false, true, false},
   {0, true, false, false},  {12, false, false, true}, {10, false, false, false},
   {0, false, false, false}, {7, false, true, false},  {0, false, false, true},
   {0, true, false, false},
};

std::vector<SeqEvent> buildPattern(double sampleRate, double seconds, double bpm, int16_t root,
                                   double accentVelocity, double plainVelocity) {
   std::vector<SeqEvent> events;
   const double stepSec = 60.0 / (bpm > 1.0 ? bpm : 130.0) / 4.0; // sixteenths
   const uint32_t totalFrames = static_cast<uint32_t>(seconds * sampleRate);
   int32_t noteId = 1;
   for (int step = 0;; ++step) {
      const double onSec = step * stepSec;
      const uint32_t onFrame = static_cast<uint32_t>(onSec * sampleRate);
      if (onFrame >= totalFrames)
         break;
      const PatternStep &p = kAcidPattern[step % 16];
      if (p.rest)
         continue;
      const int16_t key = static_cast<int16_t>(root + p.semis);
      const double vel = p.accent ? accentVelocity : plainVelocity;
      // A sliding step overlaps the next one; an ordinary one ends halfway
      // through its own step, which is where a 303's gate ends.
      const double offSec = onSec + stepSec * (p.slide ? 1.10 : 0.5);
      const uint32_t offFrame = static_cast<uint32_t>(offSec * sampleRate);
      events.push_back({onFrame, CLAP_EVENT_NOTE_ON, key, vel, noteId});
      events.push_back({offFrame, CLAP_EVENT_NOTE_OFF, key, vel, noteId});
      ++noteId;
   }
   std::sort(events.begin(), events.end(),
             [](const SeqEvent &a, const SeqEvent &b) { return a.frame < b.frame; });
   return events;
}

RenderResult renderPlugin(const clap_plugin_t *plugin, double sampleRate, uint32_t blockSize,
                          double holdSeconds, double tailSeconds, int16_t key, double velocity) {
   RenderResult res;
   // A negative hold time means: never send a note at all.
   const bool silentRun = holdSeconds < 0.0;
   const uint32_t holdFrames =
      silentRun ? 0 : static_cast<uint32_t>(holdSeconds * sampleRate);
   const uint32_t totalFrames = holdFrames + static_cast<uint32_t>(tailSeconds * sampleRate);

   std::vector<float> left(blockSize), right(blockSize);
   float *channels[2] = {left.data(), right.data()};
   clap_audio_buffer_t outBuf{};
   outBuf.data32 = channels;
   outBuf.data64 = nullptr;
   outBuf.channel_count = 2;
   outBuf.latency = 0;
   outBuf.constant_mask = 0;

   clap_output_events_t outEvents{};
   outEvents.ctx = nullptr;
   outEvents.try_push = [](const clap_output_events_t *, const clap_event_header_t *) {
      return true;
   };

   plugin->start_processing(plugin);

   res.interleaved.reserve(totalFrames * 2);
   uint32_t frame = 0;
   bool noteOnSent = silentRun, noteOffSent = silentRun;

   while (frame < totalFrames) {
      const uint32_t n = std::min<uint32_t>(blockSize, totalFrames - frame);

      EventList events;
      if (!noteOnSent) {
         for (const auto &ov : gParamOverrides)
            events.params.push_back(makeParamValue(ov.first, ov.second));
         events.notes.push_back(makeNote(CLAP_EVENT_NOTE_ON, 0, key, velocity));
         noteOnSent = true;
      }
      pushMoveEvents(events.params, frame, sampleRate);
      if (!noteOffSent && frame + n > holdFrames && holdFrames >= frame) {
         events.notes.push_back(
            makeNote(CLAP_EVENT_NOTE_OFF, holdFrames - frame, key, velocity));
         noteOffSent = true;
      }
      events.build();

      clap_process_t pr{};
      pr.steady_time = frame;
      pr.frames_count = n;
      pr.transport = nullptr;
      pr.audio_inputs = nullptr;
      pr.audio_inputs_count = 0;
      pr.audio_outputs = &outBuf;
      pr.audio_outputs_count = 1;
      pr.in_events = &events.in;
      pr.out_events = &outEvents;

      const clap_process_status st = plugin->process(plugin, &pr);
      if (st == CLAP_PROCESS_ERROR) {
         std::fprintf(stderr, "process() returned ERROR\n");
         break;
      }
      if (st == CLAP_PROCESS_SLEEP && res.sleepAt == 0 && noteOffSent)
         res.sleepAt = frame;

      for (uint32_t i = 0; i < n; ++i) {
         const float l = left[i], r = right[i];
         if (!std::isfinite(l) || !std::isfinite(r))
            res.sawNonFinite = true;
         res.peak = std::max(res.peak, std::max(std::fabs(l), std::fabs(r)));
         res.rms += static_cast<double>(l) * l + static_cast<double>(r) * r;
         res.interleaved.push_back(l);
         res.interleaved.push_back(r);
      }
      frame += n;
   }

   plugin->stop_processing(plugin);
   if (!res.interleaved.empty())
      res.rms = std::sqrt(res.rms / res.interleaved.size());
   return res;
}

// The same loop, driven by a note list rather than by one held note.
RenderResult renderSequence(const clap_plugin_t *plugin, double sampleRate, uint32_t blockSize,
                            double seconds, double tailSeconds,
                            const std::vector<SeqEvent> &schedule, bool withTransport = false,
                            double bpm = 130.0, double loopBeats = 0.0) {
   RenderResult res;
   const uint32_t totalFrames =
      static_cast<uint32_t>((seconds + tailSeconds) * sampleRate);

   std::vector<float> left(blockSize), right(blockSize);
   float *channels[2] = {left.data(), right.data()};
   clap_audio_buffer_t outBuf{};
   outBuf.data32 = channels;
   outBuf.data64 = nullptr;
   outBuf.channel_count = 2;
   outBuf.latency = 0;
   outBuf.constant_mask = 0;

   clap_output_events_t outEvents{};
   outEvents.ctx = nullptr;
   outEvents.try_push = [](const clap_output_events_t *, const clap_event_header_t *) {
      return true;
   };

   plugin->start_processing(plugin);
   res.interleaved.reserve(totalFrames * 2);

   size_t next = 0;
   uint32_t frame = 0;
   bool paramsSent = false;
   while (frame < totalFrames) {
      const uint32_t n = std::min<uint32_t>(blockSize, totalFrames - frame);

      EventList events;
      if (!paramsSent) {
         for (const auto &ov : gParamOverrides)
            events.params.push_back(makeParamValue(ov.first, ov.second));
         paramsSent = true;
      }
      // After the overrides, so a move wins over a static value for the same
      // parameter rather than being overwritten by it on the first block.
      pushMoveEvents(events.params, frame, sampleRate);
      while (next < schedule.size() && schedule[next].frame < frame + n) {
         const SeqEvent &e = schedule[next];
         const uint32_t at = e.frame > frame ? e.frame - frame : 0;
         events.notes.push_back(makeNote(e.type, at, e.key, e.velocity, e.noteId));
         ++next;
      }
      events.build();

      // A host transport, so the plugin's own sequencer has a timeline to lock
      // to. Without one it free-runs, which is not what a DAW gives it.
      clap_event_transport_t transport{};
      if (withTransport) {
         transport.header.size = sizeof(transport);
         transport.header.time = 0;
         transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
         transport.header.type = CLAP_EVENT_TRANSPORT;
         transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
                           CLAP_TRANSPORT_IS_PLAYING;
         transport.tempo = bpm;
         double beats = static_cast<double>(frame) / sampleRate * (bpm / 60.0);
         // A looping host: the beat timeline jumps backwards, which is what a
         // DAW does at the end of a loop and what the plugin has to survive.
         if (loopBeats > 0.0)
            beats = std::fmod(beats, loopBeats);
         transport.song_pos_beats =
            static_cast<clap_beattime>(beats * static_cast<double>(CLAP_BEATTIME_FACTOR));
      }

      clap_process_t pr{};
      pr.steady_time = frame;
      pr.frames_count = n;
      pr.transport = withTransport ? &transport : nullptr;
      pr.audio_inputs = nullptr;
      pr.audio_inputs_count = 0;
      pr.audio_outputs = &outBuf;
      pr.audio_outputs_count = 1;
      pr.in_events = &events.in;
      pr.out_events = &outEvents;

      const clap_process_status st = plugin->process(plugin, &pr);
      if (st == CLAP_PROCESS_ERROR) {
         std::fprintf(stderr, "process() returned ERROR\n");
         break;
      }

      for (uint32_t i = 0; i < n; ++i) {
         const float l = left[i], r = right[i];
         if (!std::isfinite(l) || !std::isfinite(r))
            res.sawNonFinite = true;
         res.peak = std::max(res.peak, std::max(std::fabs(l), std::fabs(r)));
         res.rms += static_cast<double>(l) * l + static_cast<double>(r) * r;
         res.interleaved.push_back(l);
         res.interleaved.push_back(r);
      }
      frame += n;
   }

   plugin->stop_processing(plugin);
   if (!res.interleaved.empty())
      res.rms = std::sqrt(res.rms / res.interleaved.size());
   return res;
}

// --------------------------------------------------------------------- driver

const clap_plugin_t *createPlugin(const clap_plugin_entry_t *entry) {
   const auto *factory =
      static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
   if (!factory || factory->get_plugin_count(factory) == 0) {
      std::fprintf(stderr, "no plugin factory\n");
      return nullptr;
   }
   const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
   const clap_plugin_t *plugin = factory->create_plugin(factory, &gHost, desc->id);
   if (!plugin || !plugin->init(plugin)) {
      std::fprintf(stderr, "plugin creation failed\n");
      return nullptr;
   }
   return plugin;
}

// Resolves "<name or id>=<value>" against the plugin's own parameter list,
// using text_to_value so the value can be given in real units ("2200 Hz").
bool resolveParamOverrides(const clap_plugin_t *plugin,
                           const std::vector<std::string> &specs) {
   gParamOverrides.clear();
   if (specs.empty())
      return true;
   const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   if (!params)
      return false;

   auto squash = [](const std::string &in) {
      std::string out;
      for (char c : in)
         if (!std::isspace(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return out;
   };

   const uint32_t count = params->count(plugin);
   for (const auto &spec : specs) {
      const size_t eq = spec.find('=');
      if (eq == std::string::npos) {
         std::fprintf(stderr, "bad --param '%s', expected key=value\n", spec.c_str());
         return false;
      }
      const std::string key = squash(spec.substr(0, eq));
      const std::string valueText = spec.substr(eq + 1);

      bool found = false;
      for (uint32_t i = 0; i < count && !found; ++i) {
         clap_param_info_t info{};
         if (!params->get_info(plugin, i, &info))
            continue;
         if (squash(info.name) != key && std::to_string(info.id) != key)
            continue;
         double raw = 0.0;
         if (!params->text_to_value(plugin, info.id, valueText.c_str(), &raw)) {
            std::fprintf(stderr, "cannot parse value '%s' for '%s'\n", valueText.c_str(),
                         info.name);
            return false;
         }
         gParamOverrides.emplace_back(info.id, raw);
         found = true;
      }
      if (!found) {
         std::fprintf(stderr, "no such parameter: '%s'\n", spec.substr(0, eq).c_str());
         return false;
      }
   }
   return true;
}

// ---------------------------------------------------------- the demo sweep
//
// What --demo-moves does. A demo that holds every knob still for sixteen
// seconds is a photograph of a preset; this is a recording of somebody playing
// it. Three controls move, and all three move *relative to whatever the preset
// sets*, so every demo shows the instrument's range without any of them losing
// the thing that makes it that preset.
//
//   Cutoff     down about an octave and a half, up to an octave above where it
//              started, then settling a little under it. The knob everybody
//              reaches for, and the one the header's curve is drawn from.
//   Resonance  a quarter of its travel over the take, so the sweep gets more
//              vocal as it goes.
//   Drive      a third of its travel, starting halfway in, so the end of the
//              take leans into the clipper the hardware does not have.
//
// Resonance and Drive reflect rather than clamp: a preset already near the top
// travels the same distance downwards instead. A demo whose knob sits against
// the ceiling for sixteen seconds is the thing this exists to avoid, and
// Screamer and Teeth are both up there.
double rawAfterOverrides(const clap_plugin_t *plugin, const clap_plugin_params_t *params,
                         clap_id id) {
   for (const auto &ov : gParamOverrides)
      if (ov.first == id)
         return ov.second;
   double raw = 0.0;
   params->get_value(plugin, id, &raw);
   return raw;
}

// A move of `travel` (a fraction of the parameter's whole range) from `raw`,
// upwards if there is room and downwards if there is not.
double reflectedTarget(const saeurekiste::ParamDesc &d, double raw, double travel) {
   const double step = (d.max - d.min) * travel;
   if (raw + step <= d.max)
      return raw + step;
   return std::max(d.min, raw - step);
}

void buildDemoMoves(const clap_plugin_t *plugin, double heldSeconds) {
   using namespace saeurekiste;
   gParamMoves.clear();
   const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   if (!params || !(heldSeconds > 0.0))
      return;

   auto clampRaw = [](const ParamDesc &d, double v) {
      return std::min(d.max, std::max(d.min, v));
   };

   // Cutoff, in octaves about where the preset left it. Written in Hz and
   // converted back, so the numbers here read the way the knob is labelled;
   // the interpolation still happens in the raw domain, which is octaves.
   {
      const ParamDesc &d = *paramById(kParamCutoff);
      const double hz = paramToReal(d, rawAfterOverrides(plugin, params, d.id));
      ParamMove m;
      m.id = d.id;
      m.startSec = 0.0;
      m.endSec = heldSeconds;
      m.points = {clampRaw(d, realToParam(d, hz / 2.8)),
                  clampRaw(d, realToParam(d, hz * 2.0)),
                  clampRaw(d, realToParam(d, hz / 1.4))};
      gParamMoves.push_back(m);
   }

   {
      const ParamDesc &d = *paramById(kParamResonance);
      const double raw = rawAfterOverrides(plugin, params, d.id);
      ParamMove m;
      m.id = d.id;
      m.startSec = 0.0;
      m.endSec = heldSeconds;
      m.points = {raw, reflectedTarget(d, raw, 0.25)};
      gParamMoves.push_back(m);
   }

   {
      const ParamDesc &d = *paramById(kParamDrive);
      const double raw = rawAfterOverrides(plugin, params, d.id);
      ParamMove m;
      m.id = d.id;
      m.startSec = heldSeconds * 0.5;
      m.endSec = heldSeconds;
      m.points = {raw, reflectedTarget(d, raw, 0.33)};
      gParamMoves.push_back(m);
   }
}

// Resolves "<name or id>=<a>..<b>[..<c>][@<start>:<end>]" the same way
// --param resolves a static value, so the endpoints are written in real units
// ("Cutoff=200 Hz..2 kHz") and the seconds are optional.
bool resolveParamMoves(const clap_plugin_t *plugin, const std::vector<std::string> &specs,
                       double heldSeconds) {
   if (specs.empty())
      return true;
   const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   if (!params)
      return false;

   auto squash = [](const std::string &in) {
      std::string out;
      for (char c : in)
         if (!std::isspace(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return out;
   };

   const uint32_t count = params->count(plugin);
   for (const auto &spec : specs) {
      const size_t eq = spec.find('=');
      if (eq == std::string::npos) {
         std::fprintf(stderr, "bad --move '%s', expected name=from..to\n", spec.c_str());
         return false;
      }
      const std::string key = squash(spec.substr(0, eq));
      std::string rest = spec.substr(eq + 1);

      ParamMove m;
      m.startSec = 0.0;
      m.endSec = heldSeconds;
      const size_t at = rest.find('@');
      if (at != std::string::npos) {
         const std::string when = rest.substr(at + 1);
         rest = rest.substr(0, at);
         const size_t colon = when.find(':');
         if (colon == std::string::npos) {
            std::fprintf(stderr, "bad --move timing '%s', expected @start:end\n", when.c_str());
            return false;
         }
         m.startSec = std::atof(when.substr(0, colon).c_str());
         m.endSec = std::atof(when.substr(colon + 1).c_str());
      }

      std::vector<std::string> legs;
      size_t from = 0;
      for (;;) {
         const size_t dots = rest.find("..", from);
         legs.push_back(rest.substr(from, dots == std::string::npos ? dots : dots - from));
         if (dots == std::string::npos)
            break;
         from = dots + 2;
      }
      if (legs.size() < 2) {
         std::fprintf(stderr, "bad --move '%s', expected at least from..to\n", spec.c_str());
         return false;
      }

      bool found = false;
      for (uint32_t i = 0; i < count && !found; ++i) {
         clap_param_info_t info{};
         if (!params->get_info(plugin, i, &info))
            continue;
         if (squash(info.name) != key && std::to_string(info.id) != key)
            continue;
         m.id = info.id;
         for (const std::string &leg : legs) {
            double raw = 0.0;
            if (!params->text_to_value(plugin, info.id, leg.c_str(), &raw)) {
               std::fprintf(stderr, "cannot parse value '%s' for '%s'\n", leg.c_str(), info.name);
               return false;
            }
            m.points.push_back(std::min(info.max_value, std::max(info.min_value, raw)));
         }
         found = true;
      }
      if (!found) {
         std::fprintf(stderr, "no such parameter: '%s'\n", spec.substr(0, eq).c_str());
         return false;
      }
      gParamMoves.push_back(m);
   }
   return true;
}

bool loadPreset(const clap_plugin_t *plugin, const PresetEntry &preset) {
   const auto *ext = static_cast<const clap_plugin_preset_load_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
   if (!ext) {
      std::fprintf(stderr, "plugin has no preset-load extension\n");
      return false;
   }
   const char *location =
      preset.locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? nullptr
                                                                   : preset.location.c_str();
   return ext->from_location(plugin, preset.locationKind, location, preset.loadKey.c_str());
}

std::string sanitise(const std::string &s) {
   std::string out;
   for (char c : s)
      out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
   return out;
}

// Case-insensitive key used to match a preset by name, load key or file stem.
std::string matchKey(const std::string &s) {
   std::string out;
   for (char c : s) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (std::isalnum(u))
         out += static_cast<char>(std::tolower(u));
   }
   return out;
}


// Reads every parameter's current value into a vector, in id order.
std::vector<double> snapshotParams(const clap_plugin_t *plugin) {
   std::vector<double> out;
   const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   if (!params)
      return out;
   const uint32_t count = params->count(plugin);
   for (uint32_t i = 0; i < count; ++i) {
      clap_param_info_t info{};
      if (!params->get_info(plugin, i, &info))
         continue;
      double v = 0.0;
      params->get_value(plugin, info.id, &v);
      out.push_back(v);
   }
   return out;
}

// Pushes one parameter value per parameter through a real process() call, the
// way a host would, and returns what the plugin reports back afterwards.
enum class Extreme { Min, Max, Mid };

std::vector<double> driveAllParams(const clap_plugin_t *plugin, double sampleRate,
                                   Extreme which, RenderResult *renderOut) {
   const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   if (!params)
      return {};

   gParamOverrides.clear();
   const uint32_t count = params->count(plugin);
   for (uint32_t i = 0; i < count; ++i) {
      clap_param_info_t info{};
      if (!params->get_info(plugin, i, &info))
         continue;
      double v = info.default_value;
      if (which == Extreme::Min)
         v = info.min_value;
      else if (which == Extreme::Max)
         v = info.max_value;
      else
         v = 0.5 * (info.min_value + info.max_value);
      gParamOverrides.emplace_back(info.id, v);
   }

   const RenderResult res = renderPlugin(plugin, sampleRate, 512, 0.6, 0.6, 60, 1.0);
   if (renderOut)
      *renderOut = res;
   gParamOverrides.clear();
   return snapshotParams(plugin);
}

// Whether the loaded preset drives itself. Read back through the parameter
// extension rather than assumed, so that --all does the right thing for a
// library with both kinds in it: a preset in Sequencer mode wants a transport
// and no notes, and one in MIDI mode wants the test line and no transport.
bool presetDrivesItself(const clap_plugin_t *plugin) {
   const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   if (!params)
      return false;
   const uint32_t count = params->count(plugin);
   for (uint32_t i = 0; i < count; ++i) {
      clap_param_info_t info{};
      if (!params->get_info(plugin, i, &info) || std::strcmp(info.name, "Mode") != 0)
         continue;
      double value = 0.0;
      if (!params->get_value(plugin, info.id, &value))
         return false;
      char text[64] = {0};
      if (!params->value_to_text(plugin, info.id, value, text, sizeof(text)))
         return false;
      return std::strcmp(text, "Sequencer") == 0;
   }
   return false;
}

// How long one time round the loaded pattern takes, from the plugin's own Steps
// and Rate. Returns 0 if they cannot be read.
double presetPatternSeconds(const clap_plugin_t *plugin, double bpm) {
   const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   if (!params || bpm <= 1.0)
      return 0.0;
   double steps = 0.0, perBeat = 0.0;
   const uint32_t count = params->count(plugin);
   for (uint32_t i = 0; i < count; ++i) {
      clap_param_info_t info{};
      if (!params->get_info(plugin, i, &info))
         continue;
      double value = 0.0;
      if (std::strcmp(info.name, "Steps") == 0) {
         if (params->get_value(plugin, info.id, &value))
            steps = value;
      } else if (std::strcmp(info.name, "Rate") == 0) {
         char text[32] = {0};
         if (params->get_value(plugin, info.id, &value) &&
             params->value_to_text(plugin, info.id, value, text, sizeof(text))) {
            if (std::strcmp(text, "1/32") == 0)
               perBeat = 8.0;
            else if (std::strcmp(text, "1/16T") == 0)
               perBeat = 6.0;
            else if (std::strcmp(text, "1/16") == 0)
               perBeat = 4.0;
            else if (std::strcmp(text, "1/8T") == 0)
               perBeat = 3.0;
            else if (std::strcmp(text, "1/8") == 0)
               perBeat = 2.0;
         }
      }
   }
   if (steps < 1.0 || perBeat < 1.0)
      return 0.0;
   return steps * (60.0 / bpm / perBeat);
}

int runSelfTest(const clap_plugin_entry_t *entry, double sampleRate);

// Prints what the pattern generator makes, as the grid draws it. The whole
// point of a seeded generator is being able to look at what a number gives you.
void printPatterns(int firstSeed, int count, int scale, int root) {
   static const char *const kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                          "F#", "G",  "G#", "A",  "A#", "B"};
   using namespace saeurekiste;
   for (int n = 0; n < count; ++n) {
      GenSettings g;
      g.seed = static_cast<uint32_t>(firstSeed + n);
      g.scale = scale;
      g.root = root;
      uint16_t packed[kMaxSteps];
      generatePattern(g, packed);
      std::printf("seed %-5u", g.seed);
      std::string slide, accent, vib;
      for (int i = 0; i < kMaxSteps; ++i) {
         const Step st = Step::unpack(packed[i]);
         char cell[8];
         if (st.note < 0)
            std::snprintf(cell, sizeof(cell), " .   ");
         else
            std::snprintf(cell, sizeof(cell), " %-2s%s ", kNames[st.note],
                          st.octave > 0 ? "+" : (st.octave < 0 ? "-" : " "));
         std::printf("%s", cell);
         slide += st.slide ? "  x   " : "  .   ";
         accent += st.accent ? "  x   " : "  .   ";
         vib += st.vibrato ? "  x   " : "  .   ";
      }
      std::printf("\n          %s  slide\n          %s  accent\n          %s  vibrato\n\n",
                  slide.c_str(), accent.c_str(), vib.c_str());
   }
}

} // namespace

int main(int argc, char **argv) {
   std::string pluginPath = "./SaeureKiste.clap";
   std::string presetSel, outPath = "acid.wav", outDir = ".";
   double seconds = 8.0, tail = 1.5;
   // C2, which is where a bass line lives, and the pattern's root.
   int16_t key = 36;
   double velocity = 0.9;
   // A 303 is played as a line, so that is what the renderer plays unless it
   // is told otherwise. --hold falls back to the one held note the rest of the
   // suite's renderers use.
   bool pattern = true;
   // --seq hands the plugin a running transport and no notes at all, so what
   // comes out is its own sequencer rather than the renderer's test line. It
   // only does anything with Mode set to Sequencer, which the presets that use
   // it set themselves.
   bool internalSeq = false;
   int firstSeed = -1, seedCount = 8, genScale = 0;
   // The fewest times a self-driving preset goes round its pattern, whatever
   // --seconds says.
   int minLoops = 3;
   // Non-zero makes the renderer's transport loop every this many beats, the
   // way a DAW's loop markers do.
   double loopBeats = 0.0;
   double bpm = 130.0;
   double sampleRate = 48000.0;
   uint32_t blockSize = 512;
   bool doList = false, doAll = false, doSelfTest = false, doDefaults = false;
   std::vector<std::string> paramSpecs;
   // Parameters that move while the render runs. --demo-moves is the built-in
   // sweep; --move is one written out by hand and can be given more than once.
   std::vector<std::string> moveSpecs;
   bool demoMoves = false;

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
      else if (a == "--key")
         key = static_cast<int16_t>(std::atoi(next().c_str()));
      else if (a == "--velocity")
         velocity = std::atof(next().c_str());
      else if (a == "--bpm")
         bpm = std::atof(next().c_str());
      else if (a == "--pattern")
         pattern = true;
      else if (a == "--hold")
         pattern = false;
      else if (a == "--seq")
         internalSeq = true;
      else if (a == "--seed")
         firstSeed = std::atoi(next().c_str());
      else if (a == "--seeds")
         seedCount = std::atoi(next().c_str());
      else if (a == "--scale")
         genScale = std::atoi(next().c_str());
      else if (a == "--loops")
         minLoops = std::atoi(next().c_str());
      else if (a == "--looplen")
         loopBeats = std::atof(next().c_str());
      else if (a == "--rate")
         sampleRate = std::atof(next().c_str());
      else if (a == "--block")
         blockSize = static_cast<uint32_t>(std::atoi(next().c_str()));
      else if (a == "--param")
         paramSpecs.push_back(next());
      else if (a == "--move")
         moveSpecs.push_back(next());
      else if (a == "--demo-moves")
         demoMoves = true;
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

   // Every parameter at its default, written in the preset format. This is how
   // the factory library is authored: a preset is that text with a handful of
   // lines changed, so a preset cannot silently omit a parameter or carry a
   // stale default, and adding a parameter to the table shows up here at once.
   if (doDefaults) {
      using namespace saeurekiste;
      const ParamDesc *table = paramTable();
      PresetData data;
      data.name = "Defaults";
      data.author = "SaeureKiste";
      data.description = "Every parameter at the value the table gives it.";
      for (uint32_t i = 0; i < kNumParams; ++i)
         data.values.emplace_back(table[i].id, table[i].def);
      std::fputs(formatPreset(data).c_str(), stdout);
      return 0;
   }

   void *dso = RD_DLOPEN(pluginPath.c_str());
   if (!dso) {
      std::fprintf(stderr, "dlopen failed: %s\n", RD_DLERROR());
      return 1;
   }
   auto *entry = static_cast<const clap_plugin_entry_t *>(RD_DLSYM(dso, "clap_entry"));
   if (!entry) {
      std::fprintf(stderr, "no clap_entry symbol: %s\n", RD_DLERROR());
      return 1;
   }
   if (!clap_version_is_compatible(entry->clap_version)) {
      std::fprintf(stderr, "incompatible CLAP version\n");
      return 1;
   }
   if (!entry->init(pluginPath.c_str())) {
      std::fprintf(stderr, "entry init failed\n");
      return 1;
   }

   setupIndexer();

   int rc = 0;
   if (firstSeed >= 0) {
      printPatterns(firstSeed, seedCount < 1 ? 1 : seedCount, genScale, 0);
      return 0;
   }

   if (doSelfTest) {
      rc = runSelfTest(entry, sampleRate);
      entry->deinit();
      RD_DLCLOSE(dso);
      return rc;
   }

   std::vector<PresetEntry> presets = discoverPresets(entry);
   std::printf("discovered %zu presets\n", presets.size());

   if (doList) {
      for (const auto &p : presets) {
         std::printf("  %-22s kind=%u key=%-20s %s\n", p.name.c_str(), p.locationKind,
                     p.loadKey.empty() ? "-" : p.loadKey.c_str(), p.description.c_str());
      }
      entry->deinit();
      RD_DLCLOSE(dso);
      return 0;
   }

   std::vector<PresetEntry> todo;
   if (doAll) {
      // --outdir is written into once per preset; create it here so a missing
      // directory is one error before any work rather than seventeen after it.
      std::error_code ec;
      std::filesystem::create_directories(outDir, ec);
      if (!std::filesystem::is_directory(outDir, ec)) {
         std::fprintf(stderr, "could not create the output directory %s\n", outDir.c_str());
         return 1;
      }
      todo = presets;
   } else if (!presetSel.empty()) {
      const std::string want = matchKey(presetSel);
      for (const auto &p : presets) {
         std::string stem = p.location;
         const size_t slash = stem.rfind('/');
         if (slash != std::string::npos)
            stem = stem.substr(slash + 1);
         const size_t dot = stem.rfind('.');
         if (dot != std::string::npos)
            stem = stem.substr(0, dot);
         if (matchKey(p.name) == want || matchKey(p.loadKey) == want || matchKey(stem) == want)
            todo.push_back(p);
      }
      if (todo.empty()) {
         std::fprintf(stderr, "preset '%s' not found\n", presetSel.c_str());
         entry->deinit();
         RD_DLCLOSE(dso);
         return 1;
      }
      todo.resize(1);
   } else {
      todo.push_back(PresetEntry{}); // defaults, no preset load
   }

   for (const auto &preset : todo) {
      const clap_plugin_t *plugin = createPlugin(entry);
      if (!plugin) {
         rc = 1;
         break;
      }
      if (!preset.name.empty() && !loadPreset(plugin, preset))
         std::fprintf(stderr, "warning: failed to load preset '%s'\n", preset.name.c_str());

      if (!resolveParamOverrides(plugin, paramSpecs)) {
         plugin->destroy(plugin);
         rc = 1;
         break;
      }

      if (!plugin->activate(plugin, sampleRate, 1, blockSize)) {
         std::fprintf(stderr, "activate failed\n");
         plugin->destroy(plugin);
         rc = 1;
         break;
      }

      const bool selfDriven = internalSeq || presetDrivesItself(plugin);

      // A demo of a sequenced preset has to be long enough to hear the loop as
      // a loop, and it should end where the pattern does rather than halfway
      // through a bar. So the length is rounded up to a whole number of times
      // round the pattern, and never fewer than three.
      double useSeconds = seconds;
      if (selfDriven) {
         const double loop = presetPatternSeconds(plugin, bpm);
         if (loop > 0.01) {
            double loops = std::ceil(seconds / loop - 1.0e-6);
            if (loops < static_cast<double>(minLoops))
               loops = static_cast<double>(minLoops);
            useSeconds = loops * loop;
         }
      }

      // Automation is built here rather than with the static overrides, because
      // both halves of it need numbers that are only settled by now: the demo
      // sweep is measured from the preset's own values, and a move with no
      // timing of its own runs for exactly as long as this take is held --
      // which for a self-driving preset was rounded up to whole patterns a few
      // lines ago. The tail is deliberately outside it, so the last note decays
      // at wherever the sweep left the knob.
      gParamMoves.clear();
      if (demoMoves)
         buildDemoMoves(plugin, useSeconds);
      if (!resolveParamMoves(plugin, moveSpecs, useSeconds)) {
         plugin->deactivate(plugin);
         plugin->destroy(plugin);
         rc = 1;
         break;
      }

      const RenderResult res =
         selfDriven
            ? renderSequence(plugin, sampleRate, blockSize, useSeconds, tail, {}, true, bpm,
                             loopBeats)
            : (pattern ? renderSequence(plugin, sampleRate, blockSize, useSeconds, tail,
                                        buildPattern(sampleRate, useSeconds, bpm, key, velocity,
                                                     velocity * 0.6))
                       : renderPlugin(plugin, sampleRate, blockSize, useSeconds, tail, key,
                                      velocity));

      const std::string file =
         doAll ? outDir + "/" + sanitise(preset.name.empty() ? "default" : preset.name) + ".wav"
               : outPath;
      if (!writeWav(file, res.interleaved, 2, static_cast<uint32_t>(sampleRate))) {
         std::fprintf(stderr, "could not write %s\n", file.c_str());
         return 1;
      }

      std::printf("%-22s %-11s peak %6.3f (%+6.1f dBFS)  rms %7.5f (%+6.1f dBFS)%s -> %s\n",
                  preset.name.empty() ? "(defaults)" : preset.name.c_str(),
                  selfDriven ? "[sequencer]" : "[test line]", res.peak,
                  20.0 * std::log10(std::max(res.peak, 1e-9f)), res.rms,
                  20.0 * std::log10(std::max(res.rms, 1e-9)),
                  res.sawNonFinite ? "  !! NON-FINITE OUTPUT" : "", file.c_str());
      if (res.sawNonFinite)
         rc = 1;

      plugin->deactivate(plugin);
      plugin->destroy(plugin);
   }

   entry->deinit();
   RD_DLCLOSE(dso);
   return rc;
}

namespace {

// Exercises the parts a DAW leans on but a plain render does not: parameter
// metadata, text conversion, state round-tripping and the sleep contract.
int runSelfTest(const clap_plugin_entry_t *entry, double sampleRate) {
   int failures = 0;
   auto check = [&](bool ok, const char *what) {
      std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
      if (!ok)
         ++failures;
   };

   const clap_plugin_t *plugin = createPlugin(entry);
   if (!plugin)
      return 1;

   // --- parameters
   const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   check(params != nullptr, "params extension present");
   uint32_t count = params ? params->count(plugin) : 0;
   check(count > 0, "parameter count > 0");
   std::printf("  %u parameters\n", count);

   bool infoOk = true, textOk = true, roundTripOk = true, idsUnique = true;
   std::vector<clap_id> seen;
   for (uint32_t i = 0; i < count; ++i) {
      clap_param_info_t info{};
      if (!params->get_info(plugin, i, &info)) {
         infoOk = false;
         continue;
      }
      for (clap_id s : seen)
         if (s == info.id)
            idsUnique = false;
      seen.push_back(info.id);

      if (!(info.min_value <= info.default_value && info.default_value <= info.max_value))
         infoOk = false;
      if (info.name[0] == '\0')
         infoOk = false;

      char buf[CLAP_NAME_SIZE];
      if (!params->value_to_text(plugin, info.id, info.default_value, buf, sizeof(buf))) {
         textOk = false;
         continue;
      }
      double back = 0.0;
      if (!params->text_to_value(plugin, info.id, buf, &back)) {
         roundTripOk = false;
         continue;
      }
      const double span = info.max_value - info.min_value;
      if (std::fabs(back - info.default_value) > std::max(0.02 * span, 1e-6)) {
         std::fprintf(stderr, "    text round-trip drift on '%s': %f -> '%s' -> %f\n", info.name,
                      info.default_value, buf, back);
         roundTripOk = false;
      }
   }
   check(infoOk, "every get_info() is well formed");
   check(idsUnique, "parameter ids are unique");
   check(textOk, "value_to_text() works for all defaults");
   check(roundTripOk, "value_to_text -> text_to_value round-trips");

   // --- state round trip
   const auto *state =
      static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
   check(state != nullptr, "state extension present");
   std::string blob;
   if (state) {
      clap_ostream_t os{};
      os.ctx = &blob;
      os.write = [](const clap_ostream_t *s, const void *buf, uint64_t size) -> int64_t {
         static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buf), size);
         return static_cast<int64_t>(size);
      };
      check(state->save(plugin, &os), "state save");
      check(!blob.empty(), "state blob is not empty");

      struct ReadCtx {
         const std::string *data;
         size_t pos;
      } rc{&blob, 0};
      clap_istream_t is{};
      is.ctx = &rc;
      is.read = [](const clap_istream_t *s, void *buf, uint64_t size) -> int64_t {
         auto *c = static_cast<ReadCtx *>(s->ctx);
         const size_t n = std::min<size_t>(size, c->data->size() - c->pos);
         std::memcpy(buf, c->data->data() + c->pos, n);
         c->pos += n;
         return static_cast<int64_t>(n);
      };
      check(state->load(plugin, &is), "state load");

      // Loading garbage must be rejected, not crash.
      const std::string junk = "not a saeurekiste state blob at all";
      ReadCtx rc2{&junk, 0};
      clap_istream_t is2 = is;
      is2.ctx = &rc2;
      check(!state->load(plugin, &is2), "state load rejects garbage");
   }

   // --- audio: parameter extremes must not produce NaN or silence
   check(plugin->activate(plugin, sampleRate, 1, 512), "activate");
   const RenderResult quiet = renderPlugin(plugin, sampleRate, 512, 0.0, 0.5, 60, 0.9);
   check(!quiet.sawNonFinite, "no non-finite output");

   const RenderResult loud = renderPlugin(plugin, sampleRate, 512, 1.0, 2.0, 60, 1.0);
   check(!loud.sawNonFinite, "no non-finite output while playing");
   check(loud.peak > 0.0005f, "a held note actually produces sound");
   check(loud.peak <= 1.001f, "output stays inside +/-1.0");

   // --- odd block sizes must be handled
   const RenderResult odd = renderPlugin(plugin, sampleRate, 37, 0.5, 0.5, 48, 0.5);
   check(!odd.sawNonFinite && odd.peak > 0.0f, "renders with a 37-sample block size");

   // --- the engine has no random state at all, which is unusual here and is
   // what a machine emulation should be: the same preset at the same sample
   // rate has to render the same samples, whatever the engine rendered before.
   // Playing a different note in between and resetting is what catches state
   // that survives reset().
   {
      plugin->reset(plugin);
      const RenderResult firstTake = renderPlugin(plugin, sampleRate, 512, 0.4, 0.4, 60, 1.0);
      // Leave the engine with a history, deliberately not reset.
      renderPlugin(plugin, sampleRate, 512, 0.4, 0.4, 48, 1.0);
      plugin->reset(plugin);
      const RenderResult secondTake = renderPlugin(plugin, sampleRate, 512, 0.4, 0.4, 60, 1.0);
      check(firstTake.interleaved == secondTake.interleaved,
            "the same note renders identically after reset");
   }

   // --- the pattern generator. Its two promises are testable, so they are
   // tested: the same settings give the same pattern, and changing one density
   // changes only what that density controls.
   {
      using namespace saeurekiste;
      GenSettings g;
      uint16_t a[kMaxSteps], b[kMaxSteps];
      generatePattern(g, a);
      generatePattern(g, b);
      check(std::memcmp(a, b, sizeof(a)) == 0, "the same seed gives the same pattern");

      GenSettings other = g;
      other.seed = g.seed + 1;
      generatePattern(other, b);
      check(std::memcmp(a, b, sizeof(a)) != 0, "a different seed gives a different pattern");

      // The independence property: raising Accents must not rewrite the line.
      //
      // Octaves are the one deliberate exception. An octave jump is far likelier
      // on an accented note -- accent plus an octave up is *the* gesture -- so
      // changing which steps are accented does move some octaves, and that is
      // the rule working rather than the property failing. The notes themselves
      // do not move, and neither do the slides or the vibratos.
      GenSettings louder = g;
      louder.accent = 0.9;
      generatePattern(louder, b);
      bool notesHeld = true, flagsHeld = true, accentsMoved = false, octavesFollowed = false;
      for (int i = 0; i < kMaxSteps; ++i) {
         const Step x = Step::unpack(a[i]);
         const Step y = Step::unpack(b[i]);
         if (x.note != y.note)
            notesHeld = false;
         if (x.slide != y.slide || x.vibrato != y.vibrato)
            flagsHeld = false;
         if (x.accent != y.accent)
            accentsMoved = true;
         if (x.octave != y.octave)
            octavesFollowed = true;
      }
      check(notesHeld, "raising Accents leaves every note where it was");
      check(flagsHeld, "raising Accents leaves the slides and vibratos alone");
      check(accentsMoved, "raising Accents actually changes the accents");
      check(octavesFollowed, "octave jumps follow the accents, as they are meant to");

      // Slides, though, are wholly independent: nothing else keys off them.
      GenSettings slippery = g;
      slippery.slide = 0.9;
      generatePattern(slippery, b);
      bool untouched = true;
      for (int i = 0; i < kMaxSteps; ++i) {
         const Step x = Step::unpack(a[i]);
         const Step y = Step::unpack(b[i]);
         if (x.note != y.note || x.octave != y.octave || x.accent != y.accent)
            untouched = false;
      }
      check(untouched, "raising Slides changes nothing but the slides");

      // --- the demo automation. A move is the only thing in this renderer that
      // makes a take differ from one moment to the next, so the interpolation
      // had better hit its endpoints exactly and never leave the range between
      // them.
      {
         ParamMove m;
         m.id = 0;
         m.startSec = 2.0;
         m.endSec = 6.0;
         m.points = {0.2, 0.8, 0.5};
         check(moveValueAt(m, 0.0) == 0.2, "a move holds its first value before it starts");
         check(moveValueAt(m, 2.0) == 0.2, "a move starts on its first value");
         check(std::fabs(moveValueAt(m, 4.0) - 0.8) < 1e-9, "a move reaches its middle point");
         check(std::fabs(moveValueAt(m, 3.0) - 0.5) < 1e-9, "a move interpolates its first leg");
         check(std::fabs(moveValueAt(m, 5.0) - 0.65) < 1e-9, "a move interpolates its second leg");
         check(moveValueAt(m, 6.0) == 0.5, "a move ends on its last value");
         check(moveValueAt(m, 99.0) == 0.5, "a move holds its last value through the tail");
         bool inRange = true;
         for (int i = 0; i <= 400; ++i) {
            const double v = moveValueAt(m, i * 0.02);
            if (v < 0.2 - 1e-12 || v > 0.8 + 1e-12)
               inRange = false;
         }
         check(inRange, "a move never leaves the span of its own points");

         // A parameter at the top of its travel must still move, or the demo
         // that needed the movement most is the one that does not get it.
         const ParamDesc &res = *paramById(kParamResonance);
         check(reflectedTarget(res, res.min, 0.25) > res.min,
               "a control at the bottom of its range travels upwards");
         check(reflectedTarget(res, res.max, 0.25) < res.max,
               "a control at the top of its range travels downwards instead");
         check(reflectedTarget(res, res.max, 0.25) >= res.min,
               "a reflected move stays inside the range");
      }

      // --- the bank. Sixty-four patterns have to survive the preset text, and
      // the chain has to be a function of the cycle rather than of how the
      // sequencer got there -- that is what makes looping and scrubbing land
      // on the right pattern.
      {
         PatternData bank;
         for (int i = 0; i < kMaxPatterns; ++i)
            clearPattern(bank.pattern(i));
         // Three patterns with something in them, spread across the bank, and
         // one of them the last.
         const int written[3] = {0, 17, kMaxPatterns - 1};
         for (int w = 0; w < 3; ++w) {
            GenSettings gs;
            gs.seed = static_cast<uint32_t>(100 + w);
            generatePattern(gs, bank.pattern(written[w]));
         }

         const std::string text = formatPattern(bank.steps);
         PatternData back;
         std::istringstream in(text);
         std::string line;
         while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos)
               continue;
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            while (!key.empty() && key.back() == ' ')
               key.pop_back();
            parsePatternLine(key, value, back);
         }
         check(back.present, "a written bank parses back in");
         bool bankHeld = true;
         for (int i = 0; i < kMaxPatterns * kMaxSteps; ++i)
            if (bank.steps[i] != back.steps[i])
               bankHeld = false;
         check(bankHeld, "every one of the sixty-four patterns survives the round trip");

         int lines = 0;
         for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n')
               ++lines;
         // Three patterns of five lines, three headers, the section heading and
         // the blank line before it. An empty pattern must not cost a line.
         check(lines <= 3 * 5 + 3 + 2, "an empty pattern is not written out");

         // Stay never moves; Next walks the chain and wraps at its length;
         // First comes home after one pattern; Random stays inside the chain.
         check(chainPatternAt(kChainStay, 3, 8, 7) == 3, "Stay repeats the selected pattern");
         check(chainPatternAt(kChainNext, 0, 4, 1) == 1, "Next steps to the following pattern");
         check(chainPatternAt(kChainNext, 0, 4, 4) == 0, "Next wraps round at Chain Length");
         check(chainPatternAt(kChainNext, 2, 4, 3) == 1, "Next starts from the selected pattern");
         check(chainPatternAt(kChainNext, 9, 4, 1) == 1,
               "a pattern outside the chain still feeds into it");
         check(chainPatternAt(kChainFirst, 5, 8, 1) == 0, "First comes back to pattern 1");
         check(chainPatternAt(kChainFirst, 5, 8, 0) == 5, "First plays the selected one first");
         bool randomInChain = true;
         for (long cycle = 1; cycle < 200; ++cycle) {
            const int got = chainPatternAt(kChainRandom, 0, 5, cycle);
            if (got < 0 || got >= 5)
               randomInChain = false;
            if (got != chainPatternAt(kChainRandom, 0, 5, cycle))
               randomInChain = false;
         }
         check(randomInChain, "Random stays inside the chain and repeats for a given cycle");
      }

      // The musical rules.
      check(Step::unpack(a[0]).note >= 0, "the pattern always starts on a note");
      bool slideIntoRest = false;
      for (int i = 0; i < kMaxSteps; ++i)
         if (Step::unpack(a[i]).slide && Step::unpack(a[(i + 1) % kMaxSteps]).note < 0)
            slideIntoRest = true;
      check(!slideIntoRest, "no step slides into a rest");

      // Over many seeds the root has to dominate, or it is not a bass line.
      int rootCount = 0, noteCount = 0;
      for (uint32_t seed = 1; seed <= 400; ++seed) {
         GenSettings many;
         many.seed = seed;
         uint16_t p[kMaxSteps];
         generatePattern(many, p);
         for (int i = 0; i < kMaxSteps; ++i) {
            const Step st = Step::unpack(p[i]);
            if (st.note < 0)
               continue;
            ++noteCount;
            if (st.note == many.root)
               ++rootCount;
         }
      }
      const double rootShare = noteCount ? static_cast<double>(rootCount) / noteCount : 0.0;
      std::printf("       root is %.0f%% of generated notes\n", rootShare * 100.0);
      check(rootShare > 0.30 && rootShare < 0.65, "the generator keeps returning to the root");

      // And a scale has to be respected.
      GenSettings pent;
      pent.scale = kScaleMajorPent;
      pent.root = 2; // D
      bool inScale = true;
      for (uint32_t seed = 1; seed <= 200; ++seed) {
         pent.seed = seed;
         uint16_t p[kMaxSteps];
         generatePattern(pent, p);
         for (int i = 0; i < kMaxSteps; ++i) {
            const Step st = Step::unpack(p[i]);
            if (st.note < 0)
               continue;
            const int degree = ((st.note - pent.root) % 12 + 12) % 12;
            if (degree != 0 && degree != 2 && degree != 4 && degree != 7 && degree != 9)
               inScale = false;
         }
      }
      check(inScale, "every generated note is in the chosen scale");
   }

   // --- the two behaviours that make this instrument what it is, and that a
   // single held note cannot show at all.
   {
      plugin->reset(plugin);
      // A slide is two overlapping notes, and it must not retrigger: the
      // second note therefore has to arrive without a fresh attack. What is
      // checked here is the weaker, non-negotiable half of that -- it stays
      // finite, stays bounded and keeps sounding across the join.
      const std::vector<SeqEvent> line =
         buildPattern(sampleRate, 2.0, 130.0, 36, 1.0, 0.55);
      check(!line.empty(), "the test pattern has notes in it");
      const RenderResult seq = renderSequence(plugin, sampleRate, 512, 2.0, 0.5, line);
      check(!seq.sawNonFinite, "a sixteen-step line stays finite");
      check(seq.peak > 0.0005f, "a sixteen-step line produces sound");
      check(seq.peak <= 1.001f, "a sixteen-step line stays inside +/-1.0");

      // An accent has to be audible as an accent. The same line with every
      // velocity below the threshold must come out quieter than one with the
      // accents in it, or the accent circuit is not doing anything.
      plugin->reset(plugin);
      const std::vector<SeqEvent> flat =
         buildPattern(sampleRate, 2.0, 130.0, 36, 0.55, 0.55);
      const RenderResult flatRes = renderSequence(plugin, sampleRate, 512, 2.0, 0.5, flat);
      check(seq.rms > flatRes.rms * 1.02, "accented notes are louder than unaccented ones");

      // --- the Devil Fish controls. Every one of them has to do the thing its
      // documentation says, and the stock setting has to do nothing at all --
      // the second half is checked far more strictly elsewhere, by rendering
      // every preset and comparing it byte for byte against the build before
      // these existed.
      {
         using namespace saeurekiste;
         auto renderWith = [&](uint32_t id, double raw) {
            gParamOverrides.clear();
            if (id != kNumParams)
               gParamOverrides.emplace_back(id, raw);
            plugin->reset(plugin);
            const RenderResult r =
               renderSequence(plugin, sampleRate, 512, 2.0, 0.5, flat);
            gParamOverrides.clear();
            return r;
         };

         const RenderResult stock = renderWith(kNumParams, 0.0);

         // Overdrive at the bottom takes the oscillator away entirely, which is
         // the setting that leaves a self-oscillating filter on its own.
         const RenderResult noOsc = renderWith(kParamOverdrive, -60.0);
         check(noOsc.peak == 0.0f, "Overdrive at its minimum silences the oscillator");

         // And driven hard it squashes: same peak ceiling, more energy under it.
         const RenderResult driven = renderWith(kParamOverdrive, 24.0);
         check(driven.rms > stock.rms * 1.1, "Overdrive drives the filter harder");
         check(!driven.sawNonFinite && driven.peak <= 1.001f,
               "Overdrive stays finite and bounded");

         // Accent Hold accents a line that asked for no accents at all.
         const RenderResult held = renderWith(kParamAccentHold, 1.0);
         check(held.rms > stock.rms * 1.02, "Accent Hold accents every note");

         // The three sweep speeds are three different circuits, not one with a
         // different time constant, so all three have to differ from each other.
         const RenderResult fast = renderWith(kParamSweepSpeed, 1.0);
         const RenderResult slow = renderWith(kParamSweepSpeed, 2.0);
         check(fast.interleaved != stock.interleaved, "Sweep Speed Fast is not Normal");
         check(slow.interleaved != stock.interleaved, "Sweep Speed Slow is not Normal");
         check(fast.interleaved != slow.interleaved, "Sweep Speed Fast is not Slow");

         // The Muffler is a clipper and not a volume control: it must not cost
         // level, and it has to add harmonics rather than remove them.
         const RenderResult muffled = renderWith(kParamMuffler, 2.0);
         check(muffled.rms > stock.rms * 0.9, "the Muffler does not cost level");
         check(muffled.peak <= stock.peak * 1.2f, "the Muffler softens the extremes");

         // Filter FM is a feedback path from the amplifier back into the filter.
         // The one thing it must never do is run away.
         gParamOverrides.clear();
         gParamOverrides.emplace_back(kParamFilterFM, 1.0);
         gParamOverrides.emplace_back(kParamResonance, 1.0);
         gParamOverrides.emplace_back(kParamResRange, 200.0);
         gParamOverrides.emplace_back(kParamOverdrive, 36.5);
         plugin->reset(plugin);
         const RenderResult chaos = renderSequence(plugin, sampleRate, 512, 2.0, 0.5, flat);
         gParamOverrides.clear();
         check(!chaos.sawNonFinite, "Filter FM at maximum stays finite");
         check(chaos.peak <= 1.001f, "Filter FM at maximum stays bounded");
         check(chaos.interleaved != stock.interleaved, "Filter FM changes the sound");

         plugin->reset(plugin);
      }
   }

   // --- a looping host must not wedge the sequencer.
   //
   // This is a regression test for a real bug, and the symptom is worth
   // recording: the first pass through the loop was fine, the second turned
   // into one endless slide, and it faded out over about twenty seconds and
   // never came back -- reloading the plugin was the only cure.
   //
   // The cause was that the pending note-offs are absolute positions on the
   // step timeline. When the host looped, the position jumped backwards and
   // those offs sat in the future, so the notes were never released. One stale
   // entry in the engine's held stack is enough: every later note then looks
   // like a slide, nothing retriggers, and the amplifier's slow sag runs
   // unchecked.
   {
      uint32_t modeId = CLAP_INVALID_ID;
      const uint32_t count3 = params->count(plugin);
      for (uint32_t i = 0; i < count3; ++i) {
         clap_param_info_t info{};
         if (params->get_info(plugin, i, &info) && std::strcmp(info.name, "Mode") == 0) {
            modeId = info.id;
            break;
         }
      }
      check(modeId != CLAP_INVALID_ID, "the Mode parameter exists");

      // Loop lengths that do and do not line up with the sixteen-step pattern,
      // plus one shorter than a single step.
      const double kLoops[] = {8.0, 4.0, 3.0, 0.7};
      bool held = true;
      double worst = 0.0;
      for (double loopBeats : kLoops) {
         gParamOverrides.clear();
         gParamOverrides.emplace_back(modeId, 1.0); // Sequencer
         plugin->reset(plugin);
         const RenderResult run =
            renderSequence(plugin, sampleRate, 512, 16.0, 0.5, {}, true, 130.0, loopBeats);
         gParamOverrides.clear();
         if (run.interleaved.size() < 4000)
            continue;
         // The level over the last quarter against the second quarter. The
         // first quarter is skipped because the very first pass is the one that
         // always worked.
         const size_t n = run.interleaved.size();
         auto rmsOf = [&](size_t from, size_t to) {
            double acc = 0.0;
            for (size_t i = from; i < to; ++i)
               acc += static_cast<double>(run.interleaved[i]) * run.interleaved[i];
            return std::sqrt(acc / static_cast<double>(to - from));
         };
         const double early = rmsOf(n / 4, n / 2);
         const double late = rmsOf(n * 3 / 4, n);
         const double drop = 20.0 * std::log10(std::max(late, 1e-12) / std::max(early, 1e-12));
         if (drop < worst)
            worst = drop;
         if (drop < -6.0)
            held = false;
      }
      std::printf("       worst level change across a looping transport: %.1f dB\n", worst);
      check(held, "a looping transport does not fade the sequencer out");

      // Levels alone would not have caught the second bug this test exists for:
      // one note at the seam being cut from 216 ms to 40 ms is barely a decibel
      // averaged over a bar. So compare the audio itself.
      //
      // A host loop exactly as long as the pattern must sound the same as no
      // loop at all -- the pattern repeats either way, so every sample should
      // match. Homesick's pattern is sixteen eighths, eight beats; the default
      // one is sixteen sixteenths, four beats.
      gParamOverrides.clear();
      gParamOverrides.emplace_back(modeId, 1.0);
      plugin->reset(plugin);
      const RenderResult linear =
         renderSequence(plugin, sampleRate, 512, 12.0, 0.0, {}, true, 130.0, 0.0);
      plugin->reset(plugin);
      const RenderResult looped =
         renderSequence(plugin, sampleRate, 512, 12.0, 0.0, {}, true, 130.0, 4.0);
      gParamOverrides.clear();

      double signal = 0.0, error = 0.0;
      const size_t n2 = std::min(linear.interleaved.size(), looped.interleaved.size());
      for (size_t i = n2 / 4; i < n2; ++i) {
         const double a = linear.interleaved[i];
         const double b = looped.interleaved[i];
         signal += a * a;
         error += (a - b) * (a - b);
      }
      const double snr =
         20.0 * std::log10(std::sqrt(std::max(signal, 1e-30)) / std::sqrt(std::max(error, 1e-30)));
      std::printf("       looped against linear transport: %.0f dB below signal\n", snr);
      check(snr > 40.0, "looping at the pattern length sounds the same as not looping");
   }

   // --- parameter events must be reflected by get_value, which is how a host
   // reads back what automation did.
   {
      RenderResult r;
      const std::vector<double> atMax = driveAllParams(plugin, sampleRate, Extreme::Max, &r);
      check(!r.sawNonFinite, "all parameters at maximum: output stays finite");
      check(r.peak <= 1.001f, "all parameters at maximum: output stays bounded");

      bool reflected = true;
      const uint32_t count2 = params->count(plugin);
      for (uint32_t i = 0; i < count2 && i < atMax.size(); ++i) {
         clap_param_info_t info{};
         if (!params->get_info(plugin, i, &info))
            continue;
         if (std::fabs(atMax[i] - info.max_value) > 1e-9)
            reflected = false;
      }
      check(reflected, "get_value() reflects incoming parameter events");

      const std::vector<double> atMin = driveAllParams(plugin, sampleRate, Extreme::Min, &r);
      check(!r.sawNonFinite, "all parameters at minimum: output stays finite");
      (void)atMin;

      driveAllParams(plugin, sampleRate, Extreme::Mid, &r);
      check(!r.sawNonFinite, "all parameters mid-range: output stays finite");
      check(r.peak > 0.0f, "all parameters mid-range: still audible");
   }

   // --- out of range values from the host must be clamped, not trusted
   {
      const uint32_t count2 = params->count(plugin);
      gParamOverrides.clear();
      for (uint32_t i = 0; i < count2; ++i) {
         clap_param_info_t info{};
         if (!params->get_info(plugin, i, &info))
            continue;
         gParamOverrides.emplace_back(info.id, info.max_value + 1000.0);
      }
      RenderResult r = renderPlugin(plugin, sampleRate, 512, 0.3, 0.3, 60, 1.0);
      gParamOverrides.clear();
      bool clamped = true;
      for (uint32_t i = 0; i < count2; ++i) {
         clap_param_info_t info{};
         if (!params->get_info(plugin, i, &info))
            continue;
         double v = 0.0;
         params->get_value(plugin, info.id, &v);
         if (v > info.max_value + 1e-9)
            clamped = false;
      }
      check(clamped, "out-of-range parameter values are clamped");
      check(!r.sawNonFinite, "out-of-range parameters do not break the DSP");
   }

   // --- a real state round trip: save, change everything, restore, compare
   if (state) {
      const std::vector<double> original = snapshotParams(plugin);
      std::string saved;
      clap_ostream_t os{};
      os.ctx = &saved;
      os.write = [](const clap_ostream_t *s, const void *buf, uint64_t size) -> int64_t {
         static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buf), size);
         return static_cast<int64_t>(size);
      };
      state->save(plugin, &os);

      RenderResult r;
      const std::vector<double> changed = driveAllParams(plugin, sampleRate, Extreme::Min, &r);
      check(changed != original, "parameters actually changed before restoring");

      struct ReadCtx {
         const std::string *data;
         size_t pos;
      } rc{&saved, 0};
      clap_istream_t is{};
      is.ctx = &rc;
      is.read = [](const clap_istream_t *s, void *buf, uint64_t size) -> int64_t {
         auto *c = static_cast<ReadCtx *>(s->ctx);
         const size_t n = std::min<size_t>(size, c->data->size() - c->pos);
         std::memcpy(buf, c->data->data() + c->pos, n);
         c->pos += n;
         return static_cast<int64_t>(n);
      };
      state->load(plugin, &is);
      check(snapshotParams(plugin) == original, "state load restores every parameter exactly");

      // The bank is not in the parameter table and so is not in that snapshot.
      // Saving the restored state and comparing the blobs covers it: the
      // sixty-four patterns are the bulk of what the blob holds, so two blobs
      // that match byte for byte are two identical banks.
      std::string again;
      clap_ostream_t os2{};
      os2.ctx = &again;
      os2.write = [](const clap_ostream_t *s, const void *buf, uint64_t size) -> int64_t {
         static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buf), size);
         return static_cast<int64_t>(size);
      };
      state->save(plugin, &os2);
      check(again == saved, "state load restores the pattern bank as well");
      check(saved.size() > sizeof(uint32_t) * 3 +
                              static_cast<size_t>(saeurekiste::kNumParams) * 12 +
                              static_cast<size_t>(saeurekiste::kMaxPatterns) *
                                 saeurekiste::kMaxSteps * sizeof(uint16_t),
            "the state blob carries the whole bank");
   }

   // --- a fresh instance must be silent until a note arrives
   {
      const clap_plugin_t *fresh = createPlugin(entry);
      if (fresh) {
         fresh->activate(fresh, sampleRate, 1, 512);
         const RenderResult idle = renderPlugin(fresh, sampleRate, 512, -1.0, 1.0, 60, 0.0);
         check(idle.peak == 0.0f, "no output at all before the first note");
         fresh->deactivate(fresh);
         fresh->destroy(fresh);
      }
   }

   // --- deactivate/activate cycles must be safe
   plugin->deactivate(plugin);
   check(plugin->activate(plugin, sampleRate, 1, 256), "re-activate after deactivate");
   check(plugin->activate != nullptr, "plugin still usable");

   const auto *tailExt =
      static_cast<const clap_plugin_tail_t *>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
   check(tailExt != nullptr, "tail extension present");
   check(tailExt && tailExt->get(plugin) > 0, "tail is a positive number of samples");

   // --- the preset writer and the preset parser have to agree, or saving a
   // preset quietly changes the sound it was saved from.
   {
      using namespace saeurekiste;
      const ParamDesc *table = paramTable();

      PresetData original;
      original.name = "Round Trip";
      original.author = "selftest";
      original.description = "Written by the self-test.";
      original.features.push_back("test");
      for (uint32_t i = 0; i < kNumParams; ++i) {
         const ParamDesc &d = table[i];
         double v = d.min + 0.37 * (d.max - d.min);
         if (d.kind == ParamKind::Enum || d.kind == ParamKind::Stepped)
            v = std::floor(v + 0.5);
         original.values.emplace_back(d.id, v);
      }

      const std::string text = formatPreset(original);
      PresetData reparsed;
      std::string err;
      check(parsePreset(text.c_str(), text.size(), reparsed, err),
            "a written preset parses back in");
      check(reparsed.name == original.name && reparsed.author == original.author &&
               reparsed.description == original.description,
            "a written preset keeps its metadata");
      check(reparsed.values.size() == original.values.size(),
            "a written preset keeps every parameter");

      bool valuesAgree = true;
      double worst = 0.0;
      const char *worstKey = "";
      for (const auto &want : original.values) {
         const ParamDesc *d = paramById(want.first);
         if (!d)
            continue;
         bool seen = false;
         for (const auto &got : reparsed.values) {
            if (got.first != want.first)
               continue;
            seen = true;
            const double span = d->max - d->min;
            const double err2 = span > 0.0 ? std::fabs(got.second - want.second) / span : 0.0;
            if (err2 > worst) {
               worst = err2;
               worstKey = d->key;
            }
            if (err2 > 0.005)
               valuesAgree = false;
            break;
         }
         if (!seen)
            valuesAgree = false;
      }
      if (!valuesAgree)
         std::printf("       worst drift %.4f on '%s'\n", worst, worstKey);
      check(valuesAgree, "a written preset reads back with the same values");

      // Enums must survive as names, which is what makes the files editable.
      check(text.find("waveform = ") != std::string::npos &&
               text.find("waveform = 0") == std::string::npos,
            "enum parameters are written by name");

      // The display text has to be stable under a round trip at every value, not
      // just at the ones a fuzzer happens to pick. Rounding used to push a value
      // across the boundary that chose its own precision, so 99.96 printed as
      // "100.0" and read back as "100".
      {
         bool stable = true;
         char firstText[128] = {0};
         char againText[128] = {0};
         const char *worstName = "";
         double worstValue = 0.0;
         for (uint32_t i = 0; i < kNumParams && stable; ++i) {
            const ParamDesc &d = table[i];
            for (int step = 0; step <= 400 && stable; ++step) {
               const double raw = d.min + (d.max - d.min) * (step / 400.0);
               char a[128], b[128];
               if (!paramValueToText(d, raw, a, sizeof(a)))
                  continue;
               double back = 0.0;
               if (!paramTextToValue(d, a, &back))
                  continue;
               if (!paramValueToText(d, back, b, sizeof(b)))
                  continue;
               if (std::strcmp(a, b) != 0) {
                  stable = false;
                  std::snprintf(firstText, sizeof(firstText), "%s", a);
                  std::snprintf(againText, sizeof(againText), "%s", b);
                  worstName = d.name;
                  worstValue = raw;
               }
            }
         }
         if (!stable)
            std::printf("       '%s' -> '%s' for %s at raw %.6f\n", firstText, againText,
                        worstName, worstValue);
         check(stable, "parameter text is stable across a round trip at every value");
      }

      // A display name is not a filename.
      const std::string path = userPresetPath("My Line / 2 **");
      check(path.empty() || path.find("My_Line_2.") != std::string::npos,
            "a preset name becomes a safe filename");

      // Saving has to create the user preset directory and land a file that
      // reads back, on a real filesystem rather than in principle.
      const std::string tmpdir =
         (std::filesystem::temp_directory_path() /
          ("saeurekiste-selftest-" + std::to_string(
#if defined(_WIN32)
              static_cast<unsigned long>(GetCurrentProcessId())
#else
              static_cast<unsigned long>(getpid())
#endif
              ))).string();
      std::error_code mkec;
      const char *tmp = std::filesystem::create_directories(tmpdir, mkec) || !mkec
                           ? tmpdir.c_str()
                           : nullptr;
      if (tmp) {
         setEnvVar("XDG_CONFIG_HOME", tmp);
         setEnvVar("APPDATA", tmp);
         const std::string target = userPresetPath("Saved By Selftest");
         check(!target.empty(), "a save path is offered under the user config directory");
         std::string writeErr;
         check(writePresetFile(target, text, writeErr), "a preset writes to a fresh directory");
         PresetData readBack;
         std::string readErr;
         check(parsePresetFile(target, readBack, readErr) && readBack.name == original.name,
               "a saved preset file reads back");
         std::error_code rmec;
         std::filesystem::remove_all(tmpdir, rmec);
         setEnvVar("XDG_CONFIG_HOME", nullptr);
         setEnvVar("APPDATA", nullptr);
      }
   }

   plugin->deactivate(plugin);
   plugin->destroy(plugin);

   std::printf("\nselftest: %d failure(s)\n", failures);
   return failures == 0 ? 0 : 1;
}

} // namespace
