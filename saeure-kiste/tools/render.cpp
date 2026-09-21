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

#include "dsp/delay.h"
#include "dsp/drive.h"
#include "params.h"
#include <sstream>

#include "abl.h"
#include "midifile.h"
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

// One process() call with a given event list, output thrown away. What a test
// uses when it cares about what the plugin *did* with an event rather than
// about what came out of it.
void runOneBlock(const clap_plugin_t *plugin, EventList &events, uint32_t frames = 64) {
   std::vector<float> left(frames, 0.0f), right(frames, 0.0f);
   float *channels[2] = {left.data(), right.data()};
   clap_audio_buffer_t outBuf{};
   outBuf.data32 = channels;
   outBuf.channel_count = 2;

   clap_output_events_t outEvents{};
   outEvents.ctx = nullptr;
   outEvents.try_push = [](const clap_output_events_t *, const clap_event_header_t *) {
      return true;
   };

   clap_process_t proc{};
   proc.frames_count = frames;
   proc.audio_outputs = &outBuf;
   proc.audio_outputs_count = 1;
   proc.in_events = &events.in;
   proc.out_events = &outEvents;
   proc.steady_time = -1;
   plugin->process(plugin, &proc);
}

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

// What the plugin sent out of its note port during the last render, with the
// times made absolute so the test can check when a note landed and not only
// that it did. Filled by the output-event sink both renderers install.
std::vector<clap_event_note_t> gNoteOut;
uint32_t gBlockStart = 0;

void installNoteSink(clap_output_events_t &out) {
   out.ctx = nullptr;
   out.try_push = [](const clap_output_events_t *, const clap_event_header_t *h) {
      if (h && h->space_id == CLAP_CORE_EVENT_SPACE_ID &&
          (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF)) {
         clap_event_note_t ev = *reinterpret_cast<const clap_event_note_t *>(h);
         ev.header.time += gBlockStart;
         gNoteOut.push_back(ev);
      }
      return true;
   };
}

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

// mingw does not define M_PI without _USE_MATH_DEFINES, and one constant is
// cheaper than a define that has to come before every <cmath>.
constexpr double kPi = 3.14159265358979323846;

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
   // A schedule may carry parameter changes as well as notes, which is what it
   // takes to test what happens when the host moves a parameter *between* a
   // note-on and its note-off. `key` carries the parameter id then.
   clap_id paramId = 0;
   double paramValue = 0.0;
};

SeqEvent paramEvent(uint32_t frame, clap_id id, double value) {
   SeqEvent e{frame, CLAP_EVENT_PARAM_VALUE, 0, 0.0, -1};
   e.paramId = id;
   e.paramValue = value;
   return e;
}

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
   installNoteSink(outEvents);
   gNoteOut.clear();

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

      gBlockStart = frame;
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
                            double bpm = 130.0, double loopBeats = 0.0,
                            bool transportPlaying = true) {
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
   installNoteSink(outEvents);
   gNoteOut.clear();

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
         if (e.type == CLAP_EVENT_PARAM_VALUE) {
            clap_event_param_value_t pv = makeParamValue(e.paramId, e.paramValue);
            pv.header.time = at;
            events.params.push_back(pv);
         } else {
            events.notes.push_back(makeNote(e.type, at, e.key, e.velocity, e.noteId));
         }
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
         // A stopped host still hands over its tempo and its playhead; only
         // IS_PLAYING goes away. That is the state a plugin is auditioned in.
         transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
         if (transportPlaying)
            transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
         transport.tempo = bpm;
         double beats = static_cast<double>(frame) / sampleRate * (bpm / 60.0);
         // A looping host: the beat timeline jumps backwards, which is what a
         // DAW does at the end of a loop and what the plugin has to survive.
         if (loopBeats > 0.0)
            beats = std::fmod(beats, loopBeats);
         transport.song_pos_beats =
            static_cast<clap_beattime>(beats * static_cast<double>(CLAP_BEATTIME_FACTOR));
      }

      gBlockStart = frame;
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

// Where the notes are in a render, in milliseconds: a frame of RMS well above
// the one before it and out of something close to silence. Only useful on a
// render voiced for it -- a short gate and a short volume envelope, so a note
// is over before the next step rather than sagging into it.
std::vector<double> onsetTimesMs(const RenderResult &r, double sampleRate) {
   const size_t hop = static_cast<size_t>(sampleRate / 1000.0);
   auto rms = [&](size_t f) {
      const size_t a = f * hop * 2, b = std::min(r.interleaved.size(), (f + 1) * hop * 2);
      if (b <= a)
         return 0.0;
      double acc = 0.0;
      for (size_t i = a; i < b; ++i)
         acc += static_cast<double>(r.interleaved[i]) * r.interleaved[i];
      return std::sqrt(acc / static_cast<double>(b - a));
   };
   std::vector<double> out;
   const size_t frames = r.interleaved.size() / (hop * 2);
   double last = -1.0e9;
   for (size_t f = 1; f + 1 < frames; ++f) {
      if (rms(f) > 0.02 && rms(f - 1) < 0.005 && static_cast<double>(f) - last > 30.0) {
         last = static_cast<double>(f);
         out.push_back(last);
      }
   }
   return out;
}

// ------------------------------------------------------------- MIDI file read
//
// Just enough of a standard MIDI file reader to check what the exporter wrote:
// one track, running status not used by the writer but handled anyway, and
// every channel event kept with the tick it lands on.

struct MidiEv {
   long tick;
   unsigned char status;
   int data1;
   int data2;
};

bool readMidiFile(const std::string &bytes, int &division, std::vector<MidiEv> &out) {
   out.clear();
   if (bytes.size() < 22 || bytes.compare(0, 4, "MThd") != 0)
      return false;
   auto u16 = [&](size_t at) {
      return (static_cast<unsigned char>(bytes[at]) << 8) | static_cast<unsigned char>(bytes[at + 1]);
   };
   auto u32 = [&](size_t at) {
      return (static_cast<unsigned long>(static_cast<unsigned char>(bytes[at])) << 24) |
             (static_cast<unsigned long>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
             (static_cast<unsigned long>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
             static_cast<unsigned long>(static_cast<unsigned char>(bytes[at + 3]));
   };
   if (u32(4) != 6 || u16(8) != 0 || u16(10) != 1)
      return false;
   division = static_cast<int>(u16(12));
   if (bytes.compare(14, 4, "MTrk") != 0)
      return false;
   const size_t len = static_cast<size_t>(u32(18));
   size_t i = 22;
   const size_t end = std::min(bytes.size(), 22 + len);
   long tick = 0;
   unsigned char running = 0;
   while (i < end) {
      unsigned long delta = 0;
      while (i < end) {
         const unsigned char b = static_cast<unsigned char>(bytes[i++]);
         delta = (delta << 7) | (b & 0x7F);
         if (!(b & 0x80))
            break;
      }
      tick += static_cast<long>(delta);
      if (i >= end)
         break;
      unsigned char status = static_cast<unsigned char>(bytes[i]);
      if (status & 0x80)
         ++i;
      else
         status = running;
      if (status == 0xFF) {
         const unsigned char type = static_cast<unsigned char>(bytes[i++]);
         unsigned long mlen = 0;
         while (i < end) {
            const unsigned char b = static_cast<unsigned char>(bytes[i++]);
            mlen = (mlen << 7) | (b & 0x7F);
            if (!(b & 0x80))
               break;
         }
         i += mlen;
         if (type == 0x2F)
            break;
         continue;
      }
      running = status;
      const unsigned char high = status & 0xF0;
      const int need = (high == 0xC0 || high == 0xD0) ? 1 : 2;
      MidiEv ev{tick, status, 0, 0};
      ev.data1 = static_cast<unsigned char>(bytes[i++]);
      if (need == 2)
         ev.data2 = static_cast<unsigned char>(bytes[i++]);
      out.push_back(ev);
   }
   return true;
}

// The first step of the pattern the plugin starts with, which is the one the
// note-output test expects to hear first.
uint16_t defaultStep0() {
   uint16_t steps[saeurekiste::kMaxSteps];
   saeurekiste::defaultPattern(steps);
   return steps[0];
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

   bool infoOk = true, textOk = true, roundTripOk = true, idsUnique = true, namesUnique = true;
   std::vector<clap_id> seen;
   std::vector<std::string> seenNames;
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

      // Display names have to be unique as well as ids. `--param`, `--move`
      // and any host with a flat parameter list all address a parameter by the
      // name get_info() gives it, so two parameters sharing one is not
      // cosmetic: `--param mode=Sequencer` would set both of them and say
      // nothing. The delay's Routing chip is called that rather than Mode for
      // exactly this reason.
      for (const std::string &n : seenNames)
         if (strcasecmp(n.c_str(), info.name) == 0) {
            std::fprintf(stderr, "    '%s' is the name of two parameters\n", info.name);
            namesUnique = false;
         }
      seenNames.emplace_back(info.name);

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
   check(namesUnique, "parameter names are unique");
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

      // --- GEN gives a new pattern every press.
      //
      // It used to regenerate from the seed that was already set, so pressing
      // it twice gave the same sixteen steps twice unless a density had moved
      // in between -- a button marked GEN that generated nothing. It now picks
      // a new seed first, which is what every other generator does and what
      // was wanted in the first place.
      //
      // The seed stays a parameter: the pattern GEN just made is still a
      // number that can be written down and stepped back to.
      {
         // From the parameter table, never a literal. This was hard-coded at
         // 9999 and stayed there when the seed's range was widened to the
         // whole of a 32-bit word, so the suite went on testing a range the
         // plugin no longer had -- and missed that GEN divided by zero at the
         // top of the real one.
         const uint32_t kMaxSeed = static_cast<uint32_t>(paramTable()[kParamRandSeed].max);
         uint32_t seed = 1;
         bool inRange = true, everRepeated = false, everStood = false;
         int distinct = 0;
         uint16_t seen[64][kMaxSteps];
         // Sixty-four presses, the salt running as a plain counter the way the
         // plugin's does.
         for (uint32_t press = 0; press < 64; ++press) {
            const uint32_t next = nextGeneratorSeed(seed, press, kMaxSeed);
            if (next > kMaxSeed)
               inRange = false;
            if (next == seed)
               everStood = true;
            seed = next;

            GenSettings gs;
            gs.seed = seed;
            generatePattern(gs, seen[press]);
            for (uint32_t before = 0; before < press; ++before)
               if (std::memcmp(seen[press], seen[before], sizeof(seen[0])) == 0)
                  everRepeated = true;
            ++distinct;
         }
         std::printf("       GEN: %d presses, %s, last seed %u\n", distinct,
                     everRepeated ? "a pattern came back" : "no pattern came back", seed);
         check(inRange, "a generated seed stays inside the parameter's range");
         // The top of the word specifically: there the range is 2^32, which
         // does not fit in the uint32 that counts it.
         {
            bool topOk = true;
            uint32_t at = 0;
            for (uint32_t press = 0; press < 32; ++press) {
               const uint32_t next = nextGeneratorSeed(at, press, 0xFFFFFFFFu);
               if (next == at)
                  topOk = false;
               at = next;
            }
            check(topOk, "GEN works when the seed's range is the whole 32-bit word");
         }
         check(nextGeneratorSeed(0xFFFFFFFFu, 1, 0xFFFFFFFFu) != 0xFFFFFFFFu,
               "and from the very last seed in it");
         check(!everStood, "GEN never hands back the seed that is already set");
         check(!everRepeated, "sixty-four presses of GEN give sixty-four different patterns");

         // And it is still the seed that decides, not the press: setting the
         // same seed again reproduces the line GEN made.
         GenSettings back;
         back.seed = seed;
         uint16_t again[kMaxSteps];
         generatePattern(back, again);
         check(std::memcmp(again, seen[63], sizeof(again)) == 0,
               "a pattern GEN made comes back from its seed");
      }

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

         // --- the octave, which reaches two either way as of 0.3.0.
         {
            bool packHeld = true, textHeld = true;
            PatternData wide;
            for (int i = 0; i < kMaxPatterns; ++i)
               clearPattern(wide.pattern(i));
            for (int o = -kMaxOctave; o <= kMaxOctave; ++o) {
               Step st;
               st.note = 3;
               st.octave = o;
               st.accent = o < 0;
               st.slide = o > 0;
               st.vibrato = o == 0;
               const Step back2 = Step::unpack(st.pack());
               if (back2.octave != o || back2.note != st.note || back2.accent != st.accent ||
                   back2.slide != st.slide || back2.vibrato != st.vibrato)
                  packHeld = false;
               wide.pattern(0)[o + kMaxOctave] = st.pack();
            }
            check(packHeld, "a step survives packing at every octave from -2 to +2");

            // And through the preset text, which writes the octave as a token.
            const std::string wideText = formatPattern(wide.steps);
            PatternData wideBack;
            std::istringstream win(wideText);
            std::string wline;
            while (std::getline(win, wline)) {
               const size_t eq = wline.find('=');
               if (wline.empty() || wline[0] == '#' || eq == std::string::npos)
                  continue;
               std::string key = wline.substr(0, eq);
               std::string value = wline.substr(eq + 1);
               while (!key.empty() && key.back() == ' ')
                  key.pop_back();
               parsePatternLine(key, value, wideBack);
            }
            for (int i = 0; i < 2 * kMaxOctave + 1; ++i)
               if (wide.pattern(0)[i] != wideBack.pattern(0)[i])
                  textHeld = false;
            check(textHeld, "every octave survives the preset text");

            // A state blob written by 0.2.x has no wide bit, and its steps have
            // to read exactly as they did: one octave, in the direction the old
            // two bits gave. This is the compatibility the split field bought.
            bool oldHeld = true;
            for (int dir = 0; dir <= 2; ++dir) {
               const uint16_t legacy = static_cast<uint16_t>(1 | (dir << 4));
               if (Step::unpack(legacy).octave != dir - 1)
                  oldHeld = false;
            }
            check(oldHeld, "a pattern saved before the wide octave reads back unchanged");
         }

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

      // Live mode: the pattern map, end to end through the plugin.
      //
      // The map is learned from the window, which nothing here can reach, so
      // it is written into a state blob instead -- which tests the state
      // format at the same time and is the only way a saved layout ever comes
      // back anyway. The blob's last 128 bytes are the map, one signed byte
      // per MIDI note, appended after everything version 2 wrote.
      {
         const auto *st = static_cast<const clap_plugin_state_t *>(
            plugin->get_extension(plugin, CLAP_EXT_STATE));
         const auto *pr = static_cast<const clap_plugin_params_t *>(
            plugin->get_extension(plugin, CLAP_EXT_PARAMS));
         auto setParam = [&](uint32_t id, double v) {
            EventList ev;
            ev.params.push_back(makeParamValue(id, v));
            ev.build();
            runOneBlock(plugin, ev);
         };
         auto patternNow = [&]() {
            double v = 0.0;
            return pr && pr->get_value(plugin, saeurekiste::kParamPattern, &v)
                      ? static_cast<int>(v)
                      : -1;
         };
         // A note, played, with the map's answer read back from the parameter.
         auto play = [&](int16_t key) {
            EventList ev;
            ev.notes.push_back(makeNote(CLAP_EVENT_NOTE_ON, 0, key, 0.8));
            ev.build();
            runOneBlock(plugin, ev);
            EventList off;
            off.notes.push_back(makeNote(CLAP_EVENT_NOTE_OFF, 0, key, 0.0));
            off.build();
            runOneBlock(plugin, off);
            return patternNow();
         };

         std::string blob;
         clap_ostream_t os{};
         os.ctx = &blob;
         os.write = [](const clap_ostream_t *s, const void *buf, uint64_t size) -> int64_t {
            static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buf), size);
            return static_cast<int64_t>(size);
         };
         check(st && st->save(plugin, &os), "state save, for the pattern map");
         check(blob.size() > 128, "the blob is long enough to hold a map");
         // Kept so the plugin can be put back exactly as it was found. A map
         // left behind here would reach the checks further down, where every
         // parameter is driven to its maximum -- and Mode's maximum is Live,
         // so a mapped note would move the Pattern parameter under them.
         const std::string pristine = blob;

         // Four pads: one for pattern 4, one for pattern 1, and one each way.
         auto bind = [&](int note, int action) {
            blob[blob.size() - 128 + static_cast<size_t>(note)] = static_cast<char>(action);
         };
         bind(60, 3); // pattern 4, zero based
         bind(61, 0); // pattern 1
         bind(62, saeurekiste::kNoteNextPattern);
         bind(63, saeurekiste::kNotePrevPattern);

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
         check(st->load(plugin, &is), "a state blob with a pattern map loads");

         // Live, and on pattern 1, set after the load so the blob carries the
         // map and nothing else this test depends on.
         setParam(saeurekiste::kParamMode, static_cast<double>(saeurekiste::kModeLive));
         setParam(saeurekiste::kParamPattern, 1.0);

         check(play(60) == 4, "a mapped note selects its pattern");
         check(play(62) == 5, "the next-pattern note steps forward");
         check(play(63) == 4, "the prev-pattern note steps back");
         check(play(61) == 1, "and a mapped note reaches pattern 1");
         check(play(63) == 1, "prev stops at the first pattern rather than wrapping");
         setParam(saeurekiste::kParamPattern, static_cast<double>(saeurekiste::kMaxPatterns));
         check(play(62) == saeurekiste::kMaxPatterns,
               "next stops at the last pattern rather than wrapping");
         check(play(64) == saeurekiste::kMaxPatterns,
               "an unmapped note changes no pattern at all");

         // And the map survives the round trip it arrived on.
         std::string again;
         clap_ostream_t os2{};
         os2.ctx = &again;
         os2.write = os.write;
         st->save(plugin, &os2);
         bool mapHeld = true;
         for (int note = 60; note <= 63; ++note)
            if (again[again.size() - 128 + static_cast<size_t>(note)] !=
                blob[blob.size() - 128 + static_cast<size_t>(note)])
               mapHeld = false;
         check(mapHeld, "the pattern map is saved again as it was loaded");

         setParam(saeurekiste::kParamMode, static_cast<double>(saeurekiste::kModeSequencer));
         check(play(60) == saeurekiste::kMaxPatterns,
               "the map does nothing outside Live mode");

         ReadCtx back{&pristine, 0};
         clap_istream_t is3 = is;
         is3.ctx = &back;
         st->load(plugin, &is3);
         check(saeurekiste::kNoteNone ==
                  static_cast<int8_t>(pristine[pristine.size() - 128 + 60]),
               "the plugin starts with nothing mapped");
      }

      // Live mode's other half: the keyboard does not transpose, and the
      // Pattern Oct parameter does. Read off the note port, because that is
      // where the sequencer's own notes go and it is the only place the pitch
      // of a step is visible from outside the plugin.
      {
         const auto *pr = static_cast<const clap_plugin_params_t *>(
            plugin->get_extension(plugin, CLAP_EXT_PARAMS));
         auto lowestSequencedKey = [&](int mode, int octave, int16_t heldKey) {
            gParamOverrides.clear();
            gParamOverrides.emplace_back(saeurekiste::kParamMode, static_cast<double>(mode));
            gParamOverrides.emplace_back(saeurekiste::kParamPatternOctave,
                                         static_cast<double>(octave));
            gParamOverrides.emplace_back(saeurekiste::kParamPattern, 1.0);
            plugin->reset(plugin);
            renderPlugin(plugin, sampleRate, 512, 1.5, 0.2, heldKey, 0.9);
            gParamOverrides.clear();
            // The *first* note out, not the lowest: the key is released
            // before the tail ends, and the steps that fire after that are
            // untransposed again, so a minimum over the whole take would
            // always find them.
            for (const auto &ev : gNoteOut)
               if (ev.header.type == CLAP_EVENT_NOTE_ON)
                  return static_cast<int>(ev.key);
            return -1;
         };
         (void)pr;
         // C3 held. In Sequencer mode that is the machine's own behaviour --
         // the whole pattern moves up by the interval from C2 -- and in Live
         // mode it is not, because the keys belong to the pattern map.
         const int seqHeld = lowestSequencedKey(saeurekiste::kModeSequencer, 0, 48);
         const int liveHeld = lowestSequencedKey(saeurekiste::kModeLive, 0, 48);
         const int seqPlain = lowestSequencedKey(saeurekiste::kModeSequencer, 0, 36);
         check(seqHeld == seqPlain + 12, "a held key still transposes in Sequencer mode");
         check(liveHeld == seqPlain, "a held key does not transpose in Live mode");

         const int liveUp = lowestSequencedKey(saeurekiste::kModeLive, 1, 48);
         const int liveDown = lowestSequencedKey(saeurekiste::kModeLive, -2, 48);
         check(liveUp == liveHeld + 12, "Pattern Oct moves the whole pattern up an octave");
         check(liveDown == liveHeld - 24, "and two octaves down");
         const int seqUp = lowestSequencedKey(saeurekiste::kModeSequencer, 1, 48);
         check(seqUp == seqHeld + 12, "and it adds to the held-key transpose");

         // Back to the defaults. The overrides above are real parameter
         // events, so clearing the override list does not undo them and the
         // checks further down would run in Sequencer mode an octave up.
         EventList restore;
         restore.params.push_back(makeParamValue(saeurekiste::kParamMode, 0.0));
         restore.params.push_back(makeParamValue(saeurekiste::kParamPatternOctave, 0.0));
         restore.params.push_back(makeParamValue(saeurekiste::kParamPattern, 1.0));
         restore.build();
         runOneBlock(plugin, restore);
         plugin->reset(plugin);
      }

      // Where a step lands when the bank is stepped, which is the rule the two
      // pads and the two buttons all go through.
      check(steppedPattern(0, -1, 64) == 0, "stepping back from the first stays there");
      check(steppedPattern(63, 1, 64) == 63, "stepping on from the last stays there");
      check(steppedPattern(5, 1, 64) == 6, "and in between it steps");
      check(steppedPattern(5, -1, 64) == 4, "both ways");
      check(steppedPattern(99, 1, 64) == 63, "a pattern outside the bank is brought into it");

      // The delay. What it has to be held to is where its repeats land, that
      // they decay when the feedback says they should, that the mode that
      // alternates sides actually alternates them, and -- the one that matters
      // -- that feedback past unity stays bounded. [DAFX] eq 2.61 says an IIR
      // comb with |g| > 1 "would grow endlessly"; this stage deliberately
      // allows it and holds the loop up with a soft clipper instead, so the
      // check is that the clipper is really in the path.
      {
         constexpr double kRate = 48000.0;
         constexpr float kTime = 0.1f; // 100 ms, so a repeat is 4800 frames on
         const int lag = static_cast<int>(kTime * kRate);

         // One impulse in, all wet, and nothing fed back: exactly one repeat,
         // and it lands one delay time later.
         auto impulseRun = [&](int mode, float feedback, float width, int frames,
                               std::vector<float> &l, std::vector<float> &r) {
            DelayStage d;
            d.prepare(kRate);
            d.setParams(true, mode, kTime, feedback, 1.0f, width);
            l.assign(static_cast<size_t>(frames), 0.0f);
            r.assign(static_cast<size_t>(frames), 0.0f);
            l[0] = 1.0f;
            r[0] = 1.0f;
            // In blocks, because a host never hands over one long one and the
            // read head, the write head and the tail counter all have to
            // survive the seam.
            for (int at = 0; at < frames; at += 512) {
               const int n = at + 512 <= frames ? 512 : frames - at;
               d.process(l.data() + at, r.data() + at, static_cast<uint32_t>(n));
            }
         };

         std::vector<float> l, r;
         impulseRun(kDelayMono, 0.0f, 1.0f, lag * 3, l, r);
         // Interpolation spreads the impulse over two samples, so the repeat is
         // looked for in a window rather than on one sample.
         double atRepeat = 0.0, elsewhere = 0.0;
         for (int i = 1; i < lag * 3; ++i) {
            const double v = std::fabs(l[i]);
            if (i >= lag - 2 && i <= lag + 2)
               atRepeat = v > atRepeat ? v : atRepeat;
            else
               elsewhere = v > elsewhere ? v : elsewhere;
         }
         check(atRepeat > 0.9, "the repeat arrives one delay time later");
         check(elsewhere < 1.0e-4, "and nothing arrives anywhere else");

         // The same impulse with feedback: each repeat is quieter than the one
         // before it, which is what the equation's g does.
         impulseRun(kDelayMono, 0.5f, 1.0f, lag * 4, l, r);
         double peaks[3] = {0.0, 0.0, 0.0};
         for (int k = 0; k < 3; ++k)
            for (int i = (k + 1) * lag - 2; i <= (k + 1) * lag + 2; ++i)
               peaks[k] = std::fabs(l[i]) > peaks[k] ? std::fabs(l[i]) : peaks[k];
         check(peaks[1] < peaks[0] * 0.8 && peaks[2] < peaks[1] * 0.8,
               "each repeat is quieter than the one before it");

         // Ping-pong: the first repeat is on one side and the second is on the
         // other. A mono source makes this the thing most easily got wrong --
         // cross the inputs as well and both sides stay identical for ever.
         impulseRun(kDelayPingPong, 0.6f, 1.0f, lag * 4, l, r);
         double firstL = 0.0, firstR = 0.0, secondL = 0.0, secondR = 0.0;
         for (int i = lag - 2; i <= lag + 2; ++i) {
            firstL = std::fabs(l[i]) > firstL ? std::fabs(l[i]) : firstL;
            firstR = std::fabs(r[i]) > firstR ? std::fabs(r[i]) : firstR;
         }
         for (int i = 2 * lag - 2; i <= 2 * lag + 2; ++i) {
            secondL = std::fabs(l[i]) > secondL ? std::fabs(l[i]) : secondL;
            secondR = std::fabs(r[i]) > secondR ? std::fabs(r[i]) : secondR;
         }
         check(firstL > 0.5 && firstR < firstL * 0.1, "ping-pong puts the first repeat on one side");
         check(secondR > 0.2 && secondL < secondR * 0.1, "and the second one on the other");

         // Stereo: the two lines run at a ratio of each other, so the right
         // channel's repeat does not land with the left channel's.
         impulseRun(kDelayStereo, 0.0f, 1.0f, lag * 3, l, r);
         int peakL = 0, peakR = 0;
         for (int i = 1; i < lag * 3; ++i) {
            if (std::fabs(l[i]) > std::fabs(l[peakL]))
               peakL = i;
            if (std::fabs(r[i]) > std::fabs(r[peakR]))
               peakR = i;
         }
         check(peakL > peakR + 100, "a stereo delay's two lines do not land together");

         // Width at zero folds the repeats to the middle; the mid/side matrix
         // is the only thing that can do that, so this is the check that it is
         // wired up at all.
         impulseRun(kDelayStereo, 0.0f, 0.0f, lag * 3, l, r);
         bool centred = true;
         for (int i = 0; i < lag * 3; ++i)
            if (std::fabs(l[i] - r[i]) > 1.0e-6f)
               centred = false;
         check(centred, "Width at zero folds the repeats to the centre");

         // And the one that matters. Thirty seconds at the top of the Feedback
         // knob, well past the stability condition the source states, with a
         // note going in the whole time.
         {
            DelayStage d;
            d.prepare(kRate);
            d.setParams(true, kDelayPingPong, kTime, 1.3f, 1.0f, 1.5f);
            std::vector<float> bl(512, 0.0f), br(512, 0.0f);
            float worst = 0.0f;
            bool finite = true;
            for (int block = 0; block < 2812; ++block) { // ~30 s at 48 kHz
               for (int i = 0; i < 512; ++i) {
                  const float x = 0.5f * std::sin(static_cast<float>(block * 512 + i) * 0.01f);
                  bl[i] = x;
                  br[i] = x;
               }
               d.process(bl.data(), br.data(), 512);
               for (int i = 0; i < 512; ++i) {
                  if (!std::isfinite(bl[i]) || !std::isfinite(br[i]))
                     finite = false;
                  const float p = std::fabs(bl[i]) > std::fabs(br[i]) ? std::fabs(bl[i])
                                                                     : std::fabs(br[i]);
                  worst = p > worst ? p : worst;
               }
            }
            std::printf("       feedback at 130 %% peaks at %.3f after 30 s\n",
                        static_cast<double>(worst));
            check(finite, "feedback past unity stays finite");
            check(worst <= 1.001f, "feedback past unity stays bounded by the clipper");
            check(d.ringing(), "and a delay that is still ringing says so");
         }

         // A delay that has been switched off is not ringing, so the host is
         // allowed to let the plugin sleep.
         {
            DelayStage d;
            d.prepare(kRate);
            d.setParams(false, kDelayMono, kTime, 0.5f, 1.0f, 1.0f);
            std::vector<float> bl(512, 0.0f), br(512, 0.0f);
            d.process(bl.data(), br.data(), 512);
            check(!d.ringing(), "a delay that is off is not ringing");
         }

         // The synced times are the note values they are named after.
         check(delayDivisionBeats(kDelayDiv4) == 1.0, "a quarter note is one beat");
         check(delayDivisionBeats(kDelayDiv8) == 0.5, "an eighth is half of one");
         check(delayDivisionBeats(kDelayDiv8Dot) == 0.75, "a dotted eighth is three sixteenths");
         check(std::fabs(delayDivisionBeats(kDelayDiv8T) - 1.0 / 3.0) < 1e-12,
               "an eighth triplet is two thirds of an eighth");
      }

      // A long note is a run of tied steps, and stepsTied() is the whole of
      // what ties two of them together. The grid draws such a run as one bar
      // and a drag across the grid paints one, so a rule that let a glide or a
      // rest through would join two notes that are not one note.
      {
         Step held;
         held.note = 4;
         held.slide = true;
         Step next = held;
         next.slide = false;
         check(stepsTied(held, next), "a slide into the same note is a tie");
         Step other = next;
         other.note = 5;
         check(!stepsTied(held, other), "a slide into another note is a glide, not a tie");
         Step up = next;
         up.octave = 1;
         check(!stepsTied(held, up), "a slide into the same note an octave up is not a tie");
         Step gated = held;
         gated.slide = false;
         check(!stepsTied(gated, next), "two notes without a slide stay two notes");
         const Step rest;
         check(!stepsTied(rest, next), "a rest holds nothing into the step after it");
         check(!stepsTied(held, rest), "a slide into a rest is not a tie");
      }

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
         // level, and it has to reshape the waveform rather than scale it.
         //
         // "Reshapes" is measured, not asserted: scale one take onto the other
         // by least squares and see how much waveform is left over. A clipper
         // leaves a lot; a stage doing nothing leaves nothing, whatever its
         // level. Checking only the level -- which is all this did at first --
         // passed happily with the Muffler in a position where the drive
         // stage's harder clipper erased it completely.
         auto shapeResidual = [](const RenderResult &a, const RenderResult &b) {
            const size_t n = std::min(a.interleaved.size(), b.interleaved.size());
            double num = 0.0, den = 0.0;
            for (size_t i = 0; i < n; ++i) {
               num += static_cast<double>(a.interleaved[i]) * b.interleaved[i];
               den += static_cast<double>(a.interleaved[i]) * a.interleaved[i];
            }
            if (!(den > 0.0))
               return 0.0;
            const double g = num / den;
            double res = 0.0, sig = 0.0;
            for (size_t i = 0; i < n; ++i) {
               const double d = b.interleaved[i] - g * a.interleaved[i];
               res += d * d;
               sig += static_cast<double>(b.interleaved[i]) * b.interleaved[i];
            }
            return sig > 0.0 ? std::sqrt(res / sig) : 0.0;
         };
         // Loud enough for a clipper to have something to work on, which is the
         // only condition under which the Muffler does anything at all.
         gParamOverrides.clear();
         gParamOverrides.emplace_back(kParamOverdrive, 24.0);
         plugin->reset(plugin);
         const RenderResult loudOff = renderSequence(plugin, sampleRate, 512, 2.0, 0.5, flat);
         gParamOverrides.emplace_back(kParamMuffler, 2.0);
         plugin->reset(plugin);
         const RenderResult loudHard = renderSequence(plugin, sampleRate, 512, 2.0, 0.5, flat);
         gParamOverrides.clear();
         check(loudHard.rms > loudOff.rms * 0.9, "the Muffler does not cost level");
         check(loudHard.peak <= loudOff.peak * 1.2f, "the Muffler softens the extremes");
         // 10.4 % on a working build; ~0 with the Muffler ahead of the drive.
         check(shapeResidual(loudOff, loudHard) > 0.03,
               "the Muffler reshapes the waveform rather than only scaling it");

         // Overdrive has to compress rather than simply amplify: the ladder's
         // input pair saturates, so the crest factor falls as it is driven.
         auto crest = [](const RenderResult &r) {
            return r.rms > 0.0 ? static_cast<double>(r.peak) / r.rms : 0.0;
         };
         check(crest(driven) < crest(stock) * 0.95,
               "Overdrive squashes the waveform instead of only raising it");

         // Amp Sustain holds a note up instead of letting it fall away. Nothing
         // covered it at all until the fix was backed out and nothing noticed.
         //
         // gParamOverrides carries *raw* values, and Amp Decay is logarithmic,
         // so its raw range is 0..1 rather than milliseconds. Passing 120 here
         // clamped to the maximum and neither take decayed -- this check failed
         // on correct code until the conversion was put in.
         gParamOverrides.clear();
         gParamOverrides.emplace_back(kParamAmpDecay,
                                      realToParam(*paramById(kParamAmpDecay), 120.0));
         plugin->reset(plugin);
         const RenderResult dies = renderPlugin(plugin, sampleRate, 512, 1.5, 0.2, 40, 1.0);
         gParamOverrides.emplace_back(kParamAmpSustain, 0.9);
         plugin->reset(plugin);
         const RenderResult holds = renderPlugin(plugin, sampleRate, 512, 1.5, 0.2, 40, 1.0);
         gParamOverrides.clear();
         auto tailRms = [](const RenderResult &r) {
            const size_t from = r.interleaved.size() / 2;
            double sum = 0.0;
            size_t n = 0;
            for (size_t i = from; i < r.interleaved.size(); ++i, ++n)
               sum += static_cast<double>(r.interleaved[i]) * r.interleaved[i];
            return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
         };
         check(tailRms(holds) > tailRms(dies) * 2.0,
               "Amp Sustain holds a note up instead of letting it fall away");

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

      // --- a Mode change must not leave a key behind.
      //
      // In Sequencer mode a MIDI note does not sound, it transposes, so it is
      // held in the plugin's own stack rather than in the engine's. That stack
      // is only maintained while the mode says Sequencer: switch to MIDI with a
      // key down -- which is exactly what loading a preset does, since nineteen
      // of the twenty-seven are in MIDI mode and eight are in Sequencer -- and
      // the note-off goes to the engine instead, leaving the stack holding a
      // key that is no longer down. Switch back and the plugin believes a key
      // is held: the pattern runs on its own, with no transport and nothing
      // pressed, until the plugin is reloaded.
      //
      // Reported from a Windows session: a preset was saved, presets were
      // browsed, and the sequencer ones started playing by themselves.
      {
         gParamOverrides.clear();
         gParamOverrides.emplace_back(modeId, 1.0); // Sequencer, on the first block
         plugin->reset(plugin);

         const uint32_t sr = static_cast<uint32_t>(sampleRate);
         std::vector<SeqEvent> script;
         // The key goes down, and the pattern runs off it -- no transport.
         script.push_back({0, CLAP_EVENT_NOTE_ON, 36, 1.0, 1});
         // The host switches the mode while it is still down.
         script.push_back(paramEvent(sr / 2, modeId, 0.0)); // MIDI
         // ... and only then does the key come up.
         script.push_back({sr * 3 / 4, CLAP_EVENT_NOTE_OFF, 36, 1.0, 1});
         // Back to Sequencer, with nothing held and no transport.
         script.push_back(paramEvent(sr * 3 / 2, modeId, 1.0));

         const RenderResult run =
            renderSequence(plugin, sampleRate, 512, 3.0, 0.0, script, false);
         gParamOverrides.clear();
         plugin->reset(plugin);

         auto rmsOver = [&](double from, double to) {
            const size_t a = std::min(run.interleaved.size(),
                                      static_cast<size_t>(from * sampleRate) * 2);
            const size_t b = std::min(run.interleaved.size(),
                                      static_cast<size_t>(to * sampleRate) * 2);
            if (b <= a)
               return 0.0;
            double acc = 0.0;
            for (size_t i = a; i < b; ++i)
               acc += static_cast<double>(run.interleaved[i]) * run.interleaved[i];
            return std::sqrt(acc / static_cast<double>(b - a));
         };
         // The control: while the key really is down, the pattern really runs.
         check(rmsOver(0.05, 0.45) > 1e-3, "a held key runs the pattern without a transport");
         // The claim: after it came up, nothing does.
         const double after = rmsOver(2.0, 3.0);
         std::printf("       level a second after a Mode change with a key down: %.6f\n", after);
         check(after < 1e-5, "a Mode change with a key down does not leave the sequencer running");
      }

      // The three parameters the two tests below voice the sequencer with.
      uint32_t rateId = CLAP_INVALID_ID, gateId = CLAP_INVALID_ID;
      uint32_t ampDecayId = CLAP_INVALID_ID;
      {
         const uint32_t countR = params->count(plugin);
         for (uint32_t i = 0; i < countR; ++i) {
            clap_param_info_t info{};
            if (!params->get_info(plugin, i, &info))
               continue;
            if (std::strcmp(info.name, "Rate") == 0)
               rateId = info.id;
            else if (std::strcmp(info.name, "Gate") == 0)
               gateId = info.id;
            else if (std::strcmp(info.name, "Amp Decay") == 0)
               ampDecayId = info.id;
         }
         check(rateId != CLAP_INVALID_ID && gateId != CLAP_INVALID_ID &&
                  ampDecayId != CLAP_INVALID_ID,
               "the Rate, Gate and Amp Decay parameters exist");
      }

      // --- a key press starts the pattern over.
      //
      // With no transport the pattern free-runs off a held key, and pressing
      // another one used to leave it exactly where it was: a key was a
      // transposition and nothing else. What a player expects, and what the
      // machine's own keyboard did, is that the key starts the pattern at step
      // one.
      //
      // Timed rather than measured by level, so it cannot pass by accident:
      // the key is pressed 80 % of the way through a step, where nothing is
      // due. A restart puts a note onset right there. Without one the next
      // onset is the step boundary, a fifth of a step later.
      {
         // The slowest rate, a gate that closes early and a short volume
         // envelope: a note is well over before the next step, so an onset is
         // a rise out of near-silence rather than a bump in a sustain. The
         // machine's own 1.5 second amplifier sag leaves no gap to see one in.
         auto overrides = [&]() {
            gParamOverrides.clear();
            gParamOverrides.emplace_back(modeId, 1.0);  // Sequencer
            gParamOverrides.emplace_back(rateId, 4.0);  // 1/8
            gParamOverrides.emplace_back(gateId, 0.3);
            gParamOverrides.emplace_back(ampDecayId, 0.2);
         };

         // The step length is not assumed: the plugin free-runs at whatever
         // tempo it last saw, so it is measured off a run with no key press in
         // it and the press is placed against that.
         overrides();
         plugin->reset(plugin);
         const std::vector<SeqEvent> oneKey = {
            {0, CLAP_EVENT_NOTE_ON, 36, 1.0, 1},
            {static_cast<uint32_t>(1.9 * sampleRate), CLAP_EVENT_NOTE_OFF, 36, 0.0, 1}};
         const RenderResult plain = renderSequence(plugin, sampleRate, 64, 2.0, 0.0, oneKey, false);
         const std::vector<double> grid = onsetTimesMs(plain, sampleRate);
         check(grid.size() >= 4, "the free-running pattern plays a line to time");

         // The shortest gap between two notes is one step; the longer ones are
         // the pattern's rests.
         double step = 1.0e9;
         for (size_t i = 1; i < grid.size(); ++i)
            step = std::min(step, grid[i] - grid[i - 1]);
         // The last onset before one second, and 80 % of a step past it.
         double anchor = grid.front();
         for (double t : grid)
            if (t <= 1000.0)
               anchor = t;
         const double pressAt = anchor + 0.8 * step;

         overrides();
         plugin->reset(plugin);
         std::vector<SeqEvent> script;
         script.push_back({0, CLAP_EVENT_NOTE_ON, 36, 1.0, 1});
         script.push_back({static_cast<uint32_t>(pressAt / 1000.0 * sampleRate),
                           CLAP_EVENT_NOTE_ON, 36, 1.0, 2});
         // Both keys released before the render ends. A test that leaves one
         // down leaves the sequencer running into whatever runs next.
         script.push_back({static_cast<uint32_t>(1.9 * sampleRate), CLAP_EVENT_NOTE_OFF, 36, 0.0, 1});
         script.push_back({static_cast<uint32_t>(1.9 * sampleRate), CLAP_EVENT_NOTE_OFF, 36, 0.0, 2});
         const RenderResult run = renderSequence(plugin, sampleRate, 64, 2.0, 0.0, script, false);
         gParamOverrides.clear();

         double onsetMs = -1.0;
         for (double t : onsetTimesMs(run, sampleRate)) {
            if (t >= pressAt - 2.0) {
               onsetMs = t - pressAt;
               break;
            }
         }
         std::printf("       step %.0f ms, key at %.0f ms, next note %.0f ms after it\n", step,
                     pressAt, onsetMs);
         check(onsetMs >= 0.0, "a key press is followed by a note at all");
         // Half way between the two answers: a restart lands on the key, and
         // carrying on lands a fifth of a step later.
         check(onsetMs >= 0.0 && onsetMs < 0.1 * step,
               "a key press starts the pattern over");
      }

      // --- releasing the key and pressing it again starts the pattern over.
      //
      // The restart above is the easy half: a second key while the first one is
      // still down, with the sequencer already running. The reported bug is the
      // other half -- let go, press again, and the pattern carried on from the
      // step it had stopped on instead of from step one.
      //
      // Two things caused it, and both need a stopped host that still publishes
      // a beats timeline, which is what a DAW hands over when it is not
      // rolling. The clock is read once per block and before the restart is
      // consumed, so a press that is not on the block's first sample found the
      // sequencer still stopped and the restart was dropped; and the free-run
      // start path only rewound the position when the host offered no beats
      // timeline at all, so with one present it resumed where it left off.
      //
      // Checked against a reference run rather than by eye: what comes out of
      // the note port after the second press has to be, note for note and
      // sample for sample, what comes out after the first one. The keys alone
      // would not do it -- eight of the sixteen default steps are the root, so
      // resuming in the wrong place lands on the right key about half the time.
      {
         auto seqOverrides = [&]() {
            gParamOverrides.clear();
            gParamOverrides.emplace_back(modeId, 1.0); // Sequencer
            gParamOverrides.emplace_back(rateId, 4.0); // 1/8
            gParamOverrides.emplace_back(gateId, 0.3);
            gParamOverrides.emplace_back(ampDecayId, 0.2);
         };
         // (frame, key) for the note-ons at or after `from`, relative to it.
         auto onsAfter = [](uint32_t from, size_t want) {
            std::vector<std::pair<uint32_t, int>> out;
            for (const clap_event_note_t &ev : gNoteOut) {
               if (ev.header.type != CLAP_EVENT_NOTE_ON || ev.header.time < from)
                  continue;
               out.emplace_back(ev.header.time - from, static_cast<int>(ev.key));
               if (out.size() == want)
                  break;
            }
            return out;
         };

         seqOverrides();
         plugin->reset(plugin);
         const std::vector<SeqEvent> once = {
            {0, CLAP_EVENT_NOTE_ON, 36, 1.0, 1},
            {static_cast<uint32_t>(2.9 * sampleRate), CLAP_EVENT_NOTE_OFF, 36, 0.0, 1}};
         renderSequence(plugin, sampleRate, 512, 3.0, 0.0, once, true, 130.0, 0.0, false);
         const std::vector<std::pair<uint32_t, int>> reference = onsAfter(0, 8);

         // The second press is deliberately off a block boundary as well as off
         // a step boundary: the restart has to land on the sample the key
         // arrived on, not at the top of the next block.
         seqOverrides();
         plugin->reset(plugin);
         const uint32_t pressFrame = static_cast<uint32_t>(1.3 * sampleRate) + 173;
         const std::vector<SeqEvent> twice = {
            {0, CLAP_EVENT_NOTE_ON, 36, 1.0, 1},
            {static_cast<uint32_t>(0.7 * sampleRate), CLAP_EVENT_NOTE_OFF, 36, 0.0, 1},
            {pressFrame, CLAP_EVENT_NOTE_ON, 36, 1.0, 2},
            {static_cast<uint32_t>(3.9 * sampleRate), CLAP_EVENT_NOTE_OFF, 36, 0.0, 2}};
         renderSequence(plugin, sampleRate, 512, 4.0, 0.0, twice, true, 130.0, 0.0, false);
         const std::vector<std::pair<uint32_t, int>> again = onsAfter(pressFrame, 8);
         gParamOverrides.clear();

         std::printf("       key again at frame %u: %zu notes follow it, first at +%u (key %d); "
                     "the first press gave +%u (key %d)\n",
                     pressFrame, again.size(), again.empty() ? 0u : again.front().first,
                     again.empty() ? -1 : again.front().second,
                     reference.empty() ? 0u : reference.front().first,
                     reference.empty() ? -1 : reference.front().second);
         check(reference.size() == 8, "the reference run plays a line out of the note port");
         check(again.size() == 8, "pressing the key again plays a line at all");
         check(!again.empty() && again.front().first == 0,
               "the note after a fresh key press lands on the key, not on the next block");
         check(again == reference,
               "a released and pressed key starts the pattern over, note for note");
      }

      // --- a stopped host still has a tempo, and the sequencer has to use it.
      //
      // Reported from a Windows session: changing the project tempo did
      // nothing, the pattern went on at the speed it had when the plugin was
      // instantiated. The tempo was read inside the `IS_PLAYING` branch, so a
      // pattern auditioned off a held key -- which is the whole point of being
      // able to run one without putting the song into play -- ran at whatever
      // tempo the plugin had last seen while the host was rolling, or at 120
      // if it never had been.
      //
      // Two tempos, because one measurement only proves the step length is
      // some number.
      {
         const double kTempos[] = {100.0, 160.0};
         bool followed = true;
         for (double bpm : kTempos) {
            gParamOverrides.clear();
            gParamOverrides.emplace_back(modeId, 1.0); // Sequencer
            gParamOverrides.emplace_back(rateId, 4.0); // 1/8: two steps to the beat
            gParamOverrides.emplace_back(gateId, 0.3);
            gParamOverrides.emplace_back(ampDecayId, 0.2);
            plugin->reset(plugin);
            // A held key and a transport that is stopped but carries the
            // tempo, which is what a DAW hands over when it is not rolling.
            const std::vector<SeqEvent> held = {
               {0, CLAP_EVENT_NOTE_ON, 36, 1.0, 1},
               {static_cast<uint32_t>(1.9 * sampleRate), CLAP_EVENT_NOTE_OFF, 36, 0.0, 1}};
            const RenderResult run =
               renderSequence(plugin, sampleRate, 64, 2.0, 0.0, held, true, bpm, 0.0, false);
            gParamOverrides.clear();

            const std::vector<double> grid = onsetTimesMs(run, sampleRate);
            double measured = 1.0e9;
            for (size_t i = 1; i < grid.size(); ++i)
               measured = std::min(measured, grid[i] - grid[i - 1]);
            const double expected = 60000.0 / bpm / 2.0;
            std::printf("       %.0f BPM: step %.0f ms, expected %.0f ms\n", bpm, measured,
                        expected);
            if (!(grid.size() >= 4 && std::fabs(measured - expected) < 0.06 * expected))
               followed = false;
         }
         check(followed, "the sequencer runs at the host's tempo with the transport stopped");
      }

      // --- the note output port.
      //
      // The sequencer's notes go out as well as into the engine, so a producer
      // can record the line onto another track and edit it as MIDI. What the
      // port carries has to be what the instrument is actually doing: the
      // transposed key, an accent as a velocity above the accent threshold, and
      // a slide as an overlap -- the same convention the plugin's own MIDI mode
      // reads back.
      {
         gParamOverrides.clear();
         gParamOverrides.emplace_back(modeId, 1.0); // Sequencer
         plugin->reset(plugin);
         const RenderResult run =
            renderSequence(plugin, sampleRate, 512, 4.0, 0.0, {}, true, 130.0);
         (void)run;

         std::vector<clap_event_note_t> notes = gNoteOut;
         bool ordered = true;
         uint32_t prev = 0;
         for (const clap_event_note_t &ev : notes) {
            if (ev.header.time < prev)
               ordered = false;
            prev = ev.header.time;
         }
         // The host stopping is what releases the last note, and a render that
         // ends in the middle of one would look like a leak that is not there.
         renderSequence(plugin, sampleRate, 512, 0.05, 0.0, {}, true, 130.0, 0.0, false);
         notes.insert(notes.end(), gNoteOut.begin(), gNoteOut.end());
         gParamOverrides.clear();
         int ons = 0, offs = 0, accents = 0, plains = 0, maxHeld = 0, overlaps = 0;
         int held = 0;
         bool balanced = true;
         int perKey[128] = {0};
         for (const clap_event_note_t &ev : notes) {
            if (ev.key < 0 || ev.key > 127) {
               balanced = false;
               continue;
            }
            if (ev.header.type == CLAP_EVENT_NOTE_ON) {
               ++ons;
               ++held;
               ++perKey[ev.key];
               if (held > maxHeld)
                  maxHeld = held;
               if (held > 1)
                  ++overlaps;
               if (ev.velocity > 0.99)
                  ++accents;
               else
                  ++plains;
            } else {
               ++offs;
               --held;
               --perKey[ev.key];
               if (perKey[ev.key] < 0)
                  balanced = false;
            }
         }
         for (int k = 0; k < 128; ++k)
            if (perKey[k] != 0)
               balanced = false;

         std::printf("       note out: %d on, %d off, %d accented, %d overlapping\n", ons, offs,
                     accents, overlaps);
         check(ons > 8, "the sequencer sends its notes out of the note port");
         check(ordered, "the notes it sends are in time order");
         check(balanced, "every note it sends is released, and only once");
         check(accents > 0 && plains > 0,
               "an accented step goes out louder than an unaccented one");
         check(maxHeld == 2, "a slid step overlaps the next one, and nothing overlaps three deep");

         // The first note is the pattern's first step, at C2 plus whatever the
         // step says, and it lands on the first sample of the bar.
         const saeurekiste::Step first = saeurekiste::Step::unpack(defaultStep0());
         const int expected = 36 + first.note + 12 * first.octave;
         check(!notes.empty() && notes.front().header.type == CLAP_EVENT_NOTE_ON &&
                  notes.front().key == expected && notes.front().header.time == 0,
               "the first note out is step one of the pattern, on the first sample");

         // Nothing at all in MIDI mode: those notes came from the host.
         gParamOverrides.clear();
         gParamOverrides.emplace_back(modeId, 0.0);
         plugin->reset(plugin);
         renderPlugin(plugin, sampleRate, 512, 0.5, 0.5, 60, 1.0);
         gParamOverrides.clear();
         check(gNoteOut.empty(), "MIDI mode sends nothing out of the note port");
      }

      // --- the MIDI file the window drags into the host.
      //
      // The same three conventions as the note port, written to a file: an
      // accent is a velocity, a slide is an overlap, a vibrato is CC1 around
      // the note. What is checked here is that the file says what the pattern
      // says, tick for tick -- a dropped file that is a bar out of step or a
      // slide short is worse than no feature.
      {
         using namespace saeurekiste;
         uint16_t pat[kMaxSteps];
         clearPattern(pat);
         Step a;                     // plain
         a.note = 0;
         uint16_t &s0 = pat[0];
         s0 = a.pack();
         Step b;                     // accented, an octave up
         b.note = 7;
         b.octave = 2;
         b.accent = true;
         pat[1] = b.pack();
         Step c;                     // slides into the next step
         c.note = 3;
         c.slide = true;
         pat[2] = c.pack();
         Step d;                     // vibrato, two octaves down
         d.note = 5;
         d.octave = -2;
         d.vibrato = true;
         pat[3] = d.pack();

         MidiExport spec;
         spec.steps = pat;
         spec.length = 8;
         spec.stepsPerBeat = 4.0; // 1/16
         spec.gate = 0.5;
         spec.swingPercent = 50.0;
         spec.tempoBpm = 130.0;
         spec.accentVelocity = 100.0;
         spec.patternNumber = 3;

         const std::string bytes = patternToMidiFile(spec);
         int division = 0;
         std::vector<MidiEv> evs;
         check(!bytes.empty() && readMidiFile(bytes, division, evs),
               "the pattern writes a standard MIDI file that parses back");
         check(division == 960, "the file is 960 ticks to the quarter note");

         const long step = division / 4; // a sixteenth
         int ons = 0, offs = 0;
         long onAt[4] = {-1, -1, -1, -1}, offAt[4] = {-1, -1, -1, -1};
         int vel[4] = {0, 0, 0, 0};
         const int wantKey[4] = {36, 36 + 7 + 24, 36 + 3, 36 + 5 - 24};
         bool ccUp = false, ccDown = false;
         for (const MidiEv &e : evs) {
            const unsigned char high = e.status & 0xF0;
            if (high == 0xB0 && e.data1 == 1) {
               if (e.data2 > 0 && e.tick == 3 * step)
                  ccUp = true;
               if (e.data2 == 0 && e.tick > 3 * step)
                  ccDown = true;
               continue;
            }
            const bool on = high == 0x90 && e.data2 > 0;
            const bool off = high == 0x80 || (high == 0x90 && e.data2 == 0);
            for (int k = 0; k < 4; ++k) {
               if (e.data1 != wantKey[k])
                  continue;
               if (on && onAt[k] < 0) {
                  onAt[k] = e.tick;
                  vel[k] = e.data2;
               } else if (off && offAt[k] < 0) {
                  offAt[k] = e.tick;
               }
            }
            ons += on ? 1 : 0;
            offs += off ? 1 : 0;
         }

         check(ons == 4 && offs == 4, "every step with a note in it is written once, and released");
         bool onGrid = true;
         for (int k = 0; k < 4; ++k)
            if (onAt[k] != k * step)
               onGrid = false;
         check(onGrid, "every note lands on its own step");
         check(vel[1] == 127 && vel[0] < 100 && vel[3] < 100,
               "an accented step is written louder than the accent threshold, a plain one below");
         check(offAt[0] == onAt[0] + step / 2, "an ordinary note holds for its share of the step");
         check(offAt[2] > onAt[3], "a slid step is still held when the next note starts");
         check(ccUp && ccDown, "a vibrato step is bracketed by CC1");

         // Swing moves the odd steps and nothing else.
         spec.swingPercent = 66.666666;
         std::vector<MidiEv> swung;
         check(readMidiFile(patternToMidiFile(spec), division, swung),
               "a swung pattern writes a file too");
         long firstOdd = -1, firstEven = -1;
         for (const MidiEv &e : swung) {
            if ((e.status & 0xF0) != 0x90 || e.data2 == 0)
               continue;
            if (e.data1 == wantKey[1] && firstOdd < 0)
               firstOdd = e.tick;
            if (e.data1 == wantKey[0] && firstEven < 0)
               firstEven = e.tick;
         }
         check(firstEven == 0, "swing leaves the even steps where they are");
         check(std::labs(firstOdd - (step + step / 3)) <= 1,
               "swing puts an odd step exactly where a triplet would be");

         // An empty pattern is not a file.
         uint16_t empty[kMaxSteps];
         clearPattern(empty);
         spec.steps = empty;
         check(patternToMidiFile(spec).empty(), "an empty pattern writes no file at all");
      }
   }

   // --- reading ABL's .pat text pattern format.
   //
   // The corpus this was written against is not in the repository -- it is
   // somebody else's pattern library -- so the cases here are written out by
   // hand. The one that earns its place is the pair: the same four steps in
   // ABL2's four columns and in ABL3's six have to come out identical, which
   // is the only thing holding ABL3's column order in place.
   {
      using namespace saeurekiste;
      const char kAbl2[] =
         "; ABL2 Meta tag: 4\r\n"
         "; Tune: 0.500000 Cutoff: 0.500000 Resonance: 0.250000 Envmod: 0.000000 "
         "Decay: 0.000000 Accent: 0.000000 Waveform: 1.000000 Volume: 0.750000 \r\n"
         "c-3 1 0 0\r\n"
         "d#4 1 1 0\r\n"
         "a-2 1 0 1\r\n"
         "c-3 0 0 0\r\n";
      // pitch, down, up, slide, accent, gate.
      const char kAbl3[] =
         "; ABL3 Meta tag: 4\n"
         "; Tune: 0.500000 Cutoff: 0.500000 Resonance: 0.250000 Envmod: 0.000000 "
         "Decay: 0.000000 Accent: 0.000000 Waveform: 1.000000 Volume: 0.750000 \n"
         "0 0 0 0 0 1\n"
         "3 0 1 1 0 1\n"
         "9 1 0 0 1 1\n"
         "0 0 0 0 0 0\n";

      AblImport two, three;
      std::string err;
      check(parseAblPattern(kAbl2, sizeof(kAbl2) - 1, "Two", two, err), "an ABL2 file parses");
      check(parseAblPattern(kAbl3, sizeof(kAbl3) - 1, "Three", three, err), "an ABL3 file parses");

      const uint16_t *a = two.bank.pattern(0);
      const uint16_t *b = three.bank.pattern(0);
      bool same = two.steps == 4 && three.steps == 4;
      for (int i = 0; i < kMaxSteps; ++i)
         same = same && a[i] == b[i];
      check(same, "the same four steps in ABL2's four columns and ABL3's six read identically");

      const Step s0 = Step::unpack(a[0]);
      const Step s1 = Step::unpack(a[1]);
      const Step s2 = Step::unpack(a[2]);
      const Step s3 = Step::unpack(a[3]);
      check(s0.note == 0 && s0.octave == 0, "ABL's c-3 is this plugin's C at the middle octave");
      check(s1.note == 3 && s1.octave == 1 && s1.slide && !s1.accent,
            "a sharp an octave up keeps its octave and its slide");
      check(s2.note == 9 && s2.octave == -1 && s2.accent && !s2.slide,
            "a note an octave down keeps its octave and its accent");
      check(s3.note < 0, "a step with its gate off is a rest");
      check(two.patternCount == 1 && two.clipped == 0, "one pattern, and nothing had to be moved");

      // The knobs. Tune and Volume are the two that are not a plain sweep of
      // this plugin's range, so they are the two worth asserting.
      double tuning = 1.0, volume = 0.0, waveform = -1.0;
      for (const auto &kv : two.preset.values) {
         if (kv.first == kParamTuning)
            tuning = kv.second;
         else if (kv.first == kParamVolume)
            volume = kv.second;
         else if (kv.first == kParamWaveform)
            waveform = kv.second;
      }
      check(std::fabs(tuning) < 1e-9, "ABL's centred Tune imports as no detune at all");
      check(std::fabs(volume - paramTable()[kParamVolume].def) < 1e-9,
            "ABL's default Volume imports as this plugin's default");
      check(waveform == 1.0, "ABL's Waveform 1.0 is the square wave");

      // A file may hold several patterns, and they go into the bank in order.
      const char kBank[] =
         "; ABL2 Meta tag: 1\n"
         "c-3 1 0 0\n"
         "; ABL2 Meta tag: 2\n"
         "e-3 1 0 0\n"
         "g-3 1 0 0\n";
      AblImport bank;
      check(parseAblPattern(kBank, sizeof(kBank) - 1, "Bank", bank, err) &&
               bank.patternCount == 2 && bank.steps == 2,
            "a file holding two patterns fills two slots of the bank");
      check(Step::unpack(bank.bank.pattern(1)[1]).note == 7,
            "the second pattern's second step is its own");

      // Everything past +-2 octaves has to move as a whole, because a step
      // cannot carry more than that on its own.
      const char kHigh[] =
         "; ABL2 Meta tag: 2\n"
         "c-6 1 0 0\n"
         "c-6 1 0 0\n";
      AblImport high;
      check(parseAblPattern(kHigh, sizeof(kHigh) - 1, "High", high, err) && high.clipped == 0,
            "a line three octaves up is moved rather than flattened");
      double shift = 0.0;
      for (const auto &kv : high.preset.values)
         if (kv.first == kParamPatternOctave)
            shift = kv.second;
      check(shift == 3.0 && Step::unpack(high.bank.pattern(0)[0]).octave == 0,
            "and what it is moved by is Pattern Oct");

      // And the whole thing has to survive being written out and read back,
      // because that is the only form it ever reaches the plugin in.
      PresetData back;
      PatternData backPattern;
      const std::string text = ablPresetText(two);
      check(parsePreset(text.c_str(), text.size(), back, &backPattern, err),
            "an imported preset parses as a preset");
      bool round = backPattern.present;
      for (int i = 0; i < kMaxSteps; ++i)
         round = round && backPattern.pattern(0)[i] == a[i];
      check(round, "and reads back as the pattern that went in");

      // Not every file with that extension is one of these.
      AblImport nope;
      check(!parseAblPattern("<?xml version=\"1.0\"?>\n", 22, "Xml", nope, err),
            "XML with no pattern in it is refused rather than read as an empty one");

      // The third shape: a .pat that is a Reason JukeboxPatch. Same four steps
      // again, under the property names the six columns were named from, so
      // this holds the XML reader against the text one the same way.
      const char kXml[] =
         "<?xml version=\"1.0\"?>\n"
         "<JukeboxPatch version=\"1.0\" >\n"
         " <Properties deviceProductID=\"se.audiorealism.abl3\" >\n"
         "  <Object name=\"custom_properties\" >\n"
         "   <Value property=\"tuning\" type=\"number\" >0.500000</Value>\n"
         "   <Value property=\"waveform\" type=\"number\" >1.000000</Value>\n"
         "   <Value property=\"resonance\" type=\"number\" >0.250000</Value>\n"
         "   <Value property=\"dpatternlength\" type=\"number\" >4.000000</Value>\n"
         "   <Value property=\"dpitch0\" type=\"number\" >0.000000</Value>\n"
         "   <Value property=\"dpitch1\" type=\"number\" >3.000000</Value>\n"
         "   <Value property=\"dpitch2\" type=\"number\" >9.000000</Value>\n"
         "   <Value property=\"dpitch3\" type=\"number\" >0.000000</Value>\n"
         "   <Value property=\"ddown2\" type=\"number\" >1.000000</Value>\n"
         "   <Value property=\"dup1\" type=\"number\" >1.000000</Value>\n"
         "   <Value property=\"dslide1\" type=\"number\" >1.000000</Value>\n"
         "   <Value property=\"daccent2\" type=\"number\" >1.000000</Value>\n"
         "   <Value property=\"dgate0\" type=\"number\" >1.000000</Value>\n"
         "   <Value property=\"dgate1\" type=\"number\" >1.000000</Value>\n"
         "   <Value property=\"dgate2\" type=\"number\" >1.000000</Value>\n"
         "   <Value property=\"dgate3\" type=\"number\" >0.000000</Value>\n"
         "  </Object>\n"
         " </Properties>\n"
         "</JukeboxPatch>\n";
      AblImport patch;
      check(parseAblPattern(kXml, sizeof(kXml) - 1, "Patch", patch, err),
            "a Reason JukeboxPatch carrying a pattern is read as one");
      bool sameXml = patch.steps == 4;
      for (int i = 0; i < kMaxSteps; ++i)
         sameXml = sameXml && patch.bank.pattern(0)[i] == a[i];
      check(sameXml, "and reads as the same four steps the two text forms do");
      double xmlWave = -1.0;
      for (const auto &kv : patch.preset.values)
         if (kv.first == kParamWaveform)
            xmlWave = kv.second;
      check(xmlWave == 1.0, "its knobs are read too, under their own property names");

      // "decay" and "dpatternlength" both begin with a d and neither is a step
      // field; a prefix test that got that wrong would put them in the grid.
      const char kDecay[] =
         "<Value property=\"decay\" type=\"number\" >1.000000</Value>\n"
         "<Value property=\"dpitch0\" type=\"number\" >0.000000</Value>\n"
         "<Value property=\"dgate0\" type=\"number\" >1.000000</Value>\n";
      AblImport decayPatch;
      double decayRaw = -1.0;
      check(parseAblPattern(kDecay, sizeof(kDecay) - 1, "Decay", decayPatch, err) &&
               decayPatch.steps == 1,
            "a property whose name merely starts with d is a knob, not a step");
      for (const auto &kv : decayPatch.preset.values)
         if (kv.first == kParamDecay)
            decayRaw = kv.second;
      check(decayRaw == 1.0, "and it lands on the knob it names");

      // A .param sidecar wins over the header it sits beside.
      const char kParam[] =
         "\"Tuning\" = 0.50000000\n"
         "\"Cutoff\" = 1.00000000\n"
         "\"Reso Trim\" = 0.50000000\n";
      AblImport sided;
      check(parseAblPattern(kAbl2, sizeof(kAbl2) - 1, "Sided", sided, err), "the .pat parses");
      double before = -1.0;
      for (const auto &kv : sided.preset.values)
         if (kv.first == kParamCutoff)
            before = kv.second;
      check(applyAblParams(kParam, sizeof(kParam) - 1, sided), "its .param sidecar applies");
      double after = -1.0;
      int cutoffEntries = 0;
      for (const auto &kv : sided.preset.values)
         if (kv.first == kParamCutoff) {
            after = kv.second;
            ++cutoffEntries;
         }
      check(before == 0.5 && after == 1.0 && cutoffEntries == 1,
            "the sidecar replaces the header's value rather than sitting behind it");

      // And the folder walk, on a real filesystem: two directories of patterns
      // under one root have to come out as two shelves, and a second import of
      // the same root must not mix itself into the first.
      const std::string root = (std::filesystem::temp_directory_path() /
                                ("saeurekiste-abl-" + std::to_string(
#if defined(_WIN32)
                                    static_cast<unsigned long>(GetCurrentProcessId())
#else
                                    static_cast<unsigned long>(getpid())
#endif
                                    ))).string();
      std::error_code ec;
      std::filesystem::create_directories(root + "/src/Acid", ec);
      std::filesystem::create_directories(root + "/src/Techno", ec);
      std::filesystem::create_directories(root + "/out", ec);
      if (!ec) {
         std::string werr;
         check(writePresetFile(root + "/src/Acid/One.pat", kAbl2, werr) &&
                  writePresetFile(root + "/src/Acid/One.param", kParam, werr) &&
                  writePresetFile(root + "/src/Acid/Two.pat", kAbl3, werr) &&
                  writePresetFile(root + "/src/Techno/Three.pat", kXml, werr) &&
                  writePresetFile(root + "/src/Techno/notes.txt", "ignore me", werr),
               "the folder under test is laid out");

         std::string first;
         std::string ferr;
         check(importAblFolder(root + "/src", root + "/out", first, ferr) == 3,
               "three pattern files under two folders import as three presets -- and a "
               ".param sidecar is not one of them");
         check(first == "Acid", "and the browser is pointed at the first folder made");
         check(std::filesystem::exists(root + "/out/Acid/One." + std::string(kPresetExtension)) &&
                  std::filesystem::exists(root + "/out/Techno/Three." +
                                          std::string(kPresetExtension)),
               "each source folder became a shelf of its own, and only the .pat files were read");

         // The sidecar beside One.pat has to have reached the file on disk.
         PresetData sideBack;
         std::string sideErr;
         double sideCutoff = -1.0;
         if (parsePresetFile(root + "/out/Acid/One." + std::string(kPresetExtension), sideBack,
                             sideErr))
            for (const auto &kv : sideBack.values)
               if (kv.first == kParamCutoff)
                  sideCutoff = kv.second;
         check(std::fabs(sideCutoff - 1.0) < 1e-6,
               "a .param beside a .pat is picked up by the walk, not only by hand");

         // The same root again: a second shelf rather than a merge, so the two
         // imports cannot end up half of each.
         std::string second;
         check(importAblFolder(root + "/src", root + "/out", second, ferr) == 3 &&
                  second == "Acid_2",
               "importing the same folder twice gives a second shelf rather than a mixture");

         std::string dirErr;
         std::string none;
         check(importAblFolder(root + "/out/Techno", root + "/out", none, dirErr) == 0 &&
                  !dirErr.empty(),
               "a folder with no pattern files in it says so");

         std::error_code rmec;
         std::filesystem::remove_all(root, rmec);
      }
   }

   // --- parameter events must be reflected by get_value, which is how a host
   // reads back what automation did.
   {
      RenderResult r;
      plugin->reset(plugin);
      const std::vector<double> atMax = driveAllParams(plugin, sampleRate, Extreme::Max, &r);
      check(!r.sawNonFinite, "all parameters at maximum: output stays finite");
      check(r.peak <= 1.001f, "all parameters at maximum: output stays bounded");

      // That sweep renders silence, and it is worth knowing why rather than
      // trusting it: at maximum, Mode is Sequencer and Pattern points at slot
      // 64, which is empty. So it proves the plugin survives the extremes, not
      // that the DSP was ever asked to do anything. The same extremes with
      // Mode alone turned down are played over MIDI, by the note the renderer
      // holds, and those do drive it.
      {
         gParamOverrides.clear();
         const uint32_t countM = params->count(plugin);
         for (uint32_t i = 0; i < countM; ++i) {
            clap_param_info_t info{};
            if (!params->get_info(plugin, i, &info))
               continue;
            gParamOverrides.emplace_back(
               info.id, std::strcmp(info.name, "Mode") == 0 ? info.min_value : info.max_value);
         }
         plugin->reset(plugin);
         const RenderResult loud = renderPlugin(plugin, sampleRate, 512, 0.6, 0.6, 60, 1.0);
         gParamOverrides.clear();

         check(!loud.sawNonFinite, "every parameter at maximum, played over MIDI: stays finite");
         check(loud.peak <= 1.001f, "every parameter at maximum, played over MIDI: stays bounded");
         check(loud.peak > 0.0005f, "every parameter at maximum, played over MIDI: makes sound");
      }

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

   // --- the bank and the chain, end to end through a real preset file.
   //
   // Every check here exists because backing a fix out showed the suite did not
   // notice. The round trip above compares two outputs of the *same* writer, so
   // a writer that dropped every pattern but the first produced two identical
   // blobs and passed; the chain had unit tests on chainPatternAt() and nothing
   // connecting it to the sequencer; and a preset load that stopped resetting to
   // defaults went entirely unremarked.
   if (state) {
      using namespace saeurekiste;
      const std::string dir =
         (std::filesystem::temp_directory_path() /
          ("saeurekiste-bank-" + std::to_string(
#if defined(_WIN32)
              static_cast<unsigned long>(GetCurrentProcessId())
#else
              static_cast<unsigned long>(getpid())
#endif
              ))).string();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const std::string path = dir + "/two-patterns." + std::string(kPresetExtension);

      // Pattern 1 is a bar of C; pattern 2 is a bar of G an octave up and
      // accented, so the two cannot be confused in the audio.
      std::string text = "# preset\nformat = 1\nname = Two Patterns\n";
      text += "mode = Sequencer\nseq_rate = 1/16\nseq_steps = 16\nchain_length = 2\n";
      text += "cutoff = 900\nresonance = 0.4\n";
      auto row = [](const char *key, const char *tok) {
         std::string l = key;
         while (l.size() < 13) l += ' ';
         l += "=";
         for (int i = 0; i < kMaxSteps; ++i) { l += " "; l += tok; }
         return l + "\n";
      };
      text += row("seq_pitch", "C") + row("seq_octave", ".") + row("seq_slide", ".") +
              row("seq_accent", ".") + row("seq_vibrato", ".");
      text += row("seq2_pitch", "G") + row("seq2_octave", "+") + row("seq2_slide", ".") +
              row("seq2_accent", "x") + row("seq2_vibrato", ".");
      std::string werr;
      const bool wrote = writePresetFile(path, text, werr);
      check(wrote, "the two-pattern test preset writes");

      const auto *pl = static_cast<const clap_plugin_preset_load_t *>(
         plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
      if (wrote && pl) {
         check(pl->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), ""),
               "a preset carrying two patterns loads");

         // 1. the bank actually reaches the state blob, compared against the
         //    preset's own words rather than against another save -- so a
         //    writer that drops the bank cannot agree with itself.
         PresetData pd;
         PatternData pat;
         std::string perr;
         check(parsePresetFile(path, pd, &pat, perr) && pat.present,
               "the test preset parses back with its bank");
         std::string blob;
         clap_ostream_t bos{};
         bos.ctx = &blob;
         bos.write = [](const clap_ostream_t *st, const void *buf, uint64_t size) -> int64_t {
            static_cast<std::string *>(st->ctx)->append(static_cast<const char *>(buf), size);
            return static_cast<int64_t>(size);
         };
         state->save(plugin, &bos);
         const size_t bankAt = sizeof(uint32_t) * 3 + static_cast<size_t>(kNumParams) * 12;
         const size_t pat2At = bankAt + static_cast<size_t>(kMaxSteps) * sizeof(uint16_t);
         bool carried = blob.size() >= pat2At + kMaxSteps * sizeof(uint16_t);
         for (int i = 0; carried && i < kMaxSteps; ++i) {
            uint16_t got = 0;
            std::memcpy(&got, blob.data() + pat2At + i * sizeof(uint16_t), sizeof(got));
            if (got != pat.pattern(1)[i])
               carried = false;
         }
         check(carried, "the state blob carries pattern 2, not just pattern 1");

         // 2. the chain is wired to the sequencer. Stay repeats one bar; Next
         //    alternates two very different ones, so the audio must differ.
         const ParamDesc &cm = *paramById(kParamChainMode);
         gParamOverrides.clear();
         gParamOverrides.emplace_back(cm.id, static_cast<double>(kChainStay));
         plugin->reset(plugin);
         const RenderResult stay =
            renderSequence(plugin, sampleRate, 512, 4.0, 0.5, {}, true, 130.0);
         gParamOverrides.clear();
         gParamOverrides.emplace_back(cm.id, static_cast<double>(kChainNext));
         plugin->reset(plugin);
         const RenderResult next =
            renderSequence(plugin, sampleRate, 512, 4.0, 0.5, {}, true, 130.0);
         gParamOverrides.clear();
         check(stay.peak > 0.001f && next.peak > 0.001f, "both chain renders make sound");
         check(stay.interleaved != next.interleaved,
               "the sequencer plays the chain, not just the selected pattern");

         // 3. a preset load starts from the defaults. Drive a parameter the
         //    minimal preset below never mentions, then load it.
         const std::string bare = dir + "/bare." + std::string(kPresetExtension);
         check(writePresetFile(bare, "# preset\nformat = 1\nname = Bare\ncutoff = 700\n", werr),
               "the minimal test preset writes");
         const ParamDesc &res = *paramById(kParamResonance);
         gParamOverrides.clear();
         plugin->reset(plugin);
         const auto *pex = static_cast<const clap_plugin_params_t *>(
            plugin->get_extension(plugin, CLAP_EXT_PARAMS));
         {
            // flush(), not a zero-frame process(): flush is the call CLAP
            // provides for moving a parameter while the plugin is not running,
            // and a process() with no frames does not deliver the event.
            EventList ev;
            ev.params.push_back(makeParamValue(res.id, res.max));
            ev.build();
            static clap_output_events_t noOut{};
            noOut.try_push = [](const clap_output_events_t *, const clap_event_header_t *) {
               return true;
            };
            pex->flush(plugin, &ev.in, &noOut);
         }
         double before = 0.0;
         pex->get_value(plugin, res.id, &before);
         check(std::fabs(before - res.max) < 1e-9, "the probe parameter moved off its default");
         check(pl->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, bare.c_str(), ""),
               "the minimal preset loads");
         double after = 0.0;
         pex->get_value(plugin, res.id, &after);
         check(std::fabs(after - res.def) < 1e-9,
               "a preset resets a parameter it does not mention to its default");
      }
      std::error_code rmec;
      std::filesystem::remove_all(dir, rmec);
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

   // --- long patterns.
   //
   // Steps reaches 128 as of 0.5.0. Three things had to move with it and each
   // one is checked here rather than assumed: the generator, the preset text
   // and the state blob's bank.
   {
      using namespace saeurekiste;

      GenSettings g;
      g.length = 64;
      uint16_t longPat[kMaxSteps];
      generatePattern(g, longPat);
      const uint16_t rest = Step().pack();
      bool tailClear = true;
      for (int i = 64; i < kMaxSteps; ++i)
         if (longPat[i] != rest)
            tailClear = false;
      check(tailClear, "the generator leaves nothing past the pattern's length");
      int filled = 0;
      for (int i = 16; i < 64; ++i)
         if (Step::unpack(longPat[i]).note >= 0)
            ++filled;
      check(filled > 16, "a sixty-four step generation fills the steps past the first sixteen");

      // Turning Steps up extends the line rather than replacing it: the draws
      // are per step and in order, so the first sixteen are the sixteen a
      // shorter generation from the same seed gave.
      GenSettings shortSettings = g;
      shortSettings.length = 16;
      uint16_t shortPat[kMaxSteps];
      generatePattern(shortSettings, shortPat);
      check(std::memcmp(shortPat, longPat, 16 * sizeof(uint16_t)) == 0,
            "a longer generation starts with exactly the steps the shorter one had");

      // The preset text carries what the pattern uses and no more, so a
      // sixteen-step line is still written in sixteen columns -- which is what
      // keeps every preset in the factory library byte for byte what it was.
      auto columnsOf = [](const std::string &text, const char *key) {
         std::istringstream in(text);
         std::string line;
         while (std::getline(in, line)) {
            if (line.compare(0, std::strlen(key), key) != 0)
               continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
               return 0;
            int n = 0;
            std::istringstream fields(line.substr(eq + 1));
            std::string token;
            while (fields >> token)
               ++n;
            return n;
         }
         return -1;
      };

      PatternData shortData;
      std::memcpy(shortData.pattern(0), shortPat, sizeof(shortPat));
      for (int p = 1; p < kMaxPatterns; ++p)
         clearPattern(shortData.pattern(p));
      check(columnsOf(formatPattern(shortData.steps), "seq_pitch") == 16,
            "a sixteen-step pattern is still written in sixteen columns");

      PatternData longData;
      std::memcpy(longData.pattern(0), longPat, sizeof(longPat));
      for (int p = 1; p < kMaxPatterns; ++p)
         clearPattern(longData.pattern(p));
      const std::string longText = formatPattern(longData.steps);
      check(columnsOf(longText, "seq_pitch") == patternUsedLength(longPat),
            "a long pattern is written in as many columns as it uses");

      // And it reads back as the same steps.
      PatternData readBack;
      {
         std::istringstream in(longText);
         std::string line;
         while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
               continue;
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            while (!key.empty() && key.back() == ' ')
               key.pop_back();
            while (!value.empty() && value.front() == ' ')
               value.erase(value.begin());
            parsePatternLine(key, value, readBack);
         }
      }
      check(readBack.present &&
               std::memcmp(readBack.pattern(0), longPat, sizeof(longPat)) == 0,
            "a long pattern survives the preset text");
   }

   // --- the sequencer plays past step sixteen.
   //
   // The pattern below is sixteen rests followed by sixteen notes, so at Steps
   // 16 it is silence and at Steps 32 it is a bar of notes. Nothing else in
   // the suite would notice a sequencer that still stopped at sixteen.
   if (state) {
      using namespace saeurekiste;
      const std::string dir =
         (std::filesystem::temp_directory_path() /
          ("saeurekiste-long-" + std::to_string(
#if defined(_WIN32)
              static_cast<unsigned long>(GetCurrentProcessId())
#else
              static_cast<unsigned long>(getpid())
#endif
              ))).string();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const std::string path = dir + "/long." + std::string(kPresetExtension);

      std::string text = "# preset\nformat = 1\nname = Long\n";
      text += "mode = Sequencer\nseq_rate = 1/16\nseq_steps = 32\ncutoff = 1200\n";
      auto row = [](const char *key, const char *early, const char *late) {
         std::string l = key;
         while (l.size() < 13)
            l += ' ';
         l += "=";
         for (int i = 0; i < 32; ++i) {
            l += " ";
            l += i < 16 ? early : late;
         }
         return l + "\n";
      };
      text += row("seq_pitch", ".", "C") + row("seq_octave", ".", ".") +
              row("seq_slide", ".", ".") + row("seq_accent", ".", "x") +
              row("seq_vibrato", ".", ".");
      std::string werr;
      const bool wrote = writePresetFile(path, text, werr);
      check(wrote, "the long test preset writes");

      const auto *pl = static_cast<const clap_plugin_preset_load_t *>(
         plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
      if (wrote && pl) {
         check(pl->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), ""),
               "a thirty-two step preset loads");
         plugin->reset(plugin);
         const RenderResult full =
            renderSequence(plugin, sampleRate, 512, 4.0, 0.5, {}, true, 130.0);
         gParamOverrides.clear();
         gParamOverrides.emplace_back(kParamSeqSteps, 16.0);
         plugin->reset(plugin);
         const RenderResult half =
            renderSequence(plugin, sampleRate, 512, 4.0, 0.5, {}, true, 130.0);
         gParamOverrides.clear();
         check(full.peak > 0.01f, "the steps past sixteen are played");
         check(half.peak < 0.0005f,
               "and only the steps the length reaches: at Steps 16 the same pattern is silent");
      }
      std::error_code rmec;
      std::filesystem::remove_all(dir, rmec);
   }

   // --- a project saved before patterns grew still opens.
   //
   // A version 1 blob carries sixteen words per pattern where this build
   // writes 128. The version is what says which, so the migration is built by
   // hand here from a blob this build wrote: nothing else can produce one.
   if (state) {
      using namespace saeurekiste;
      std::string current;
      clap_ostream_t os{};
      os.ctx = &current;
      os.write = [](const clap_ostream_t *s, const void *buf, uint64_t size) -> int64_t {
         static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buf), size);
         return static_cast<int64_t>(size);
      };
      plugin->reset(plugin);
      state->save(plugin, &os);

      const size_t headerSize = sizeof(uint32_t) * 3;
      const size_t paramsSize = static_cast<size_t>(kNumParams) * 12;
      const size_t bankAt = headerSize + paramsSize;
      const size_t patternWords = static_cast<size_t>(kMaxSteps) * sizeof(uint16_t);
      bool sane = current.size() >= bankAt + kMaxPatterns * patternWords;
      check(sane, "the blob this build writes is the shape the migration test assumes");

      if (sane) {
         // The same blob with sixteen words per pattern and version 1 on it.
         std::string old = current.substr(0, headerSize + paramsSize);
         const uint32_t one = 1;
         std::memcpy(&old[sizeof(uint32_t)], &one, sizeof(one));
         std::vector<uint16_t> wanted(kMaxPatterns * 16, 0);
         for (int p = 0; p < kMaxPatterns; ++p) {
            const size_t from = bankAt + static_cast<size_t>(p) * patternWords;
            old.append(current, from, 16 * sizeof(uint16_t));
            std::memcpy(&wanted[static_cast<size_t>(p) * 16], current.data() + from,
                        16 * sizeof(uint16_t));
         }
         old.push_back('\0'); // the collapsible section's byte

         struct ReadCtx {
            const std::string *data;
            size_t pos;
         } rc{&old, 0};
         clap_istream_t is{};
         is.ctx = &rc;
         is.read = [](const clap_istream_t *s, void *buf, uint64_t size) -> int64_t {
            auto *c = static_cast<ReadCtx *>(s->ctx);
            const size_t n = std::min<size_t>(size, c->data->size() - c->pos);
            std::memcpy(buf, c->data->data() + c->pos, n);
            c->pos += n;
            return static_cast<int64_t>(n);
         };
         check(state->load(plugin, &is), "a version 1 state blob still loads");

         std::string after;
         clap_ostream_t os2{};
         os2.ctx = &after;
         os2.write = [](const clap_ostream_t *s, const void *buf, uint64_t size) -> int64_t {
            static_cast<std::string *>(s->ctx)->append(static_cast<const char *>(buf), size);
            return static_cast<int64_t>(size);
         };
         state->save(plugin, &os2);

         bool carried = after.size() >= bankAt + kMaxPatterns * patternWords;
         const uint16_t rest = saeurekiste::Step().pack();
         for (int p = 0; carried && p < kMaxPatterns; ++p) {
            const size_t at = bankAt + static_cast<size_t>(p) * patternWords;
            for (int i = 0; i < kMaxSteps; ++i) {
               uint16_t got = 0;
               std::memcpy(&got, after.data() + at + i * sizeof(uint16_t), sizeof(got));
               const uint16_t want = i < 16 ? wanted[static_cast<size_t>(p) * 16 + i] : rest;
               if (got != want)
                  carried = false;
            }
         }
         check(carried,
               "an old blob's sixteen steps land in the first sixteen, and the rest are rests");
      }
   }

   // --- the drive stage's models.
   //
   // What is asserted here is *harmonic structure*, not samples. The first
   // version of this stage was fourteen shapes that all sounded the same, and
   // the test that was supposed to notice compared sample buffers -- "no two
   // types render the same audio" -- which two indistinguishable signals pass
   // with ease. A distortion is told apart by what it does to the spectrum, so
   // that is what is measured: how much of the output is harmonics at all, and
   // how much of that is even rather than odd.
   {
      using namespace saeurekiste;

      // One 1 kHz sine through the stage, then a Goertzel at each harmonic.
      // The first quarter of the run is thrown away so the models with
      // filters in them have settled.
      struct Spectrum {
         double fundamental = 0.0;
         double even = 0.0;   // harmonics 2, 4, 6, 8, 10
         double odd = 0.0;    // harmonics 3, 5, 7, 9
         double thd = 0.0;    // sqrt(sum of harmonics^2) / fundamental
         double evenRatio = 0.0;
         double highRatio = 0.0; // harmonics 6..10 as a share of all of them
         // Harmonics two to ten, scaled to unit length: the *distribution* of
         // what the model adds, with both the level and the amount of
         // distortion taken out of it. Two models that produce this same
         // vector at the same THD are producing the same sound, whatever
         // their sample buffers look like.
         double shape[9] = {0.0};
         float peak = 0.0f;
         bool finite = true;
      };

      auto measure = [](int model, double drive, double bias, double amplitude,
                        double freq = 1000.0) {
         const double rate = 48000.0;
         const int total = 24000;
         const int skip = total / 4;
         DriveStage stage;
         stage.prepare(rate);
         stage.setParams(model, static_cast<float>(drive), static_cast<float>(bias), 1.0f);

         std::vector<float> out;
         out.reserve(total - skip);
         Spectrum s;
         for (int n = 0; n < total; ++n) {
            const float x =
               static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * n / rate));
            const float y = stage.tick(x);
            if (!std::isfinite(y))
               s.finite = false;
            if (n >= skip) {
               out.push_back(y);
               s.peak = std::max(s.peak, std::fabs(y));
            }
         }

         auto magnitudeAt = [&](double hz) {
            const double w = 2.0 * kPi * hz / rate;
            const double coeff = 2.0 * std::cos(w);
            double s1 = 0.0, s2 = 0.0;
            for (const float v : out) {
               const double s0 = v + coeff * s1 - s2;
               s2 = s1;
               s1 = s0;
            }
            const double real = s1 - s2 * std::cos(w);
            const double imag = s2 * std::sin(w);
            return std::sqrt(real * real + imag * imag) / (0.5 * out.size());
         };

         s.fundamental = magnitudeAt(freq);
         double harmonics = 0.0;
         double high = 0.0;
         for (int k = 2; k <= 10; ++k) {
            const double m = magnitudeAt(freq * k);
            s.shape[k - 2] = m;
            harmonics += m * m;
            if (k % 2 == 0)
               s.even += m * m;
            else
               s.odd += m * m;
            if (k >= 6)
               high += m * m;
         }
         double norm = 0.0;
         for (const double m : s.shape)
            norm += m * m;
         norm = std::sqrt(norm);
         if (norm > 1e-12)
            for (double &m : s.shape)
               m /= norm;
         s.thd = s.fundamental > 1e-9 ? std::sqrt(harmonics) / s.fundamental : 0.0;
         const double all = s.even + s.odd;
         s.evenRatio = all > 1e-18 ? s.even / all : 0.0;
         // How far up the series the model reaches, which is what tells a soft
         // clipper from a hard one at the same total distortion.
         s.highRatio = harmonics > 1e-18 ? high / harmonics : 0.0;
         return s;
      };

      // 1. every model distorts, stays finite and stays bounded.
      std::vector<Spectrum> loud;
      bool finite = true, bounded = true, distorts = true;
      for (int model = 0; model < kNumDriveModels; ++model) {
         const Spectrum s = measure(model, 0.7, 0.0, 0.8);

         // Finiteness is checked across the whole of Drive and both ends of
         // Bias rather than at one setting, because the settings that break a
         // model are its extremes and 0.7 is neither. A model whose feedback
         // resistance goes to zero at the bottom of the knob divides by it
         // there and nowhere else; this suite passed such a model until the
         // sweep was added, because it only ever asked at 0.7.
         for (int i = 0; i <= 8 && finite; ++i)
            for (int b = -1; b <= 1 && finite; ++b) {
               const Spectrum e = measure(model, i / 8.0, b, 0.8);
               if (!e.finite || !std::isfinite(e.peak) || !std::isfinite(e.thd)) {
                  finite = false;
                  std::printf("  [dbg] model %d not finite at drive %.3f bias %d\n", model,
                              i / 8.0, b);
               }
            }

         loud.push_back(s);
         if (!s.finite)
            finite = false;
         // The stage on its own is allowed past unity: what it hands over goes
         // through the Muffler, the DC blocker and the master, and the models
         // are matched to each other on *loudness* rather than on peak, which
         // is what a comparison between two distortions needs. The bound that
         // matters is the plugin's, and that one is checked below at 1.001.
         if (s.peak > 1.5f) {
            bounded = false;
            std::printf("  [dbg] model %d peak %.3f\n", model, s.peak);
         }
         if (s.thd < 0.05)
            distorts = false;
      }
      check(finite, "every drive model stays finite");
      check(bounded, "every drive model stays bounded");
      check(distorts, "every drive model actually distorts at 70 % drive");

      // 2. no two models sound the same, asked properly.
      //
      //    Comparing them at the same *knob position* is not the question a
      //    player asks: any two clippers turned up far enough are the same
      //    square wave, and the first version of this stage was fourteen
      //    shapes that met exactly there. The question is whether two models
      //    differ when they are doing the same *amount* of distortion -- so
      //    each one's drive is searched for the setting that gives it 25 %
      //    THD, and the spectra are compared there. Two models with the same
      //    spectrum at the same THD are one model with two names.
      auto driveForThd = [&](int model, double target) {
         double lo = 0.0, hi = 1.0;
         for (int i = 0; i < 18; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (measure(model, mid, 0.0, 0.8).thd < target)
               lo = mid;
            else
               hi = mid;
         }
         return 0.5 * (lo + hi);
      };

      // How much of the distortion survives when the signal is quiet, which is
      // the dimension a steady sine cannot see and the one that separates a
      // model with a linear region from a model without one. It is also the
      // difference a player hears first, because every note on this instrument
      // decays through it.
      // And how much of it survives when the signal is *low*, which is the
      // other dimension one sine at one frequency cannot see. A model whose
      // gain is the same at every frequency distorts a bass note exactly as
      // hard as a mid one; a model built around a frequency-dependent gain
      // stage does not, and on this instrument -- where the fundamental is
      // down at 40 to 200 Hz and the harmonics are not -- that is the
      // difference a player hears before any other. 80 Hz against the 1 kHz
      // everything else here is measured at.
      std::vector<Spectrum> matched;
      std::vector<double> dynamics;
      std::vector<double> tilts;
      for (int model = 0; model < kNumDriveModels; ++model) {
         const double d = driveForThd(model, 0.25);
         const Spectrum s = measure(model, d, 0.0, 0.8);
         const Spectrum quiet = measure(model, d, 0.0, 0.2);
         const Spectrum low = measure(model, d, 0.0, 0.8, 80.0);
         const double ratio = s.thd > 1e-9 ? quiet.thd / s.thd : 0.0;
         const double tilt = s.thd > 1e-9 ? low.thd / s.thd : 0.0;
         matched.push_back(s);
         dynamics.push_back(ratio);
         tilts.push_back(tilt);
         std::printf("       model %d: drive %.3f, THD at 1 kHz %.3f, at 80 Hz %.3f "
                     "(tilt %.2f), quiet %.2f\n",
                     model, d, s.thd, low.thd, tilt, ratio);
      }

      double closest = 1e9;
      int likeA = -1, likeB = -1;
      for (size_t a = 0; a < matched.size(); ++a)
         for (size_t b = a + 1; b < matched.size(); ++b) {
            double d = 0.0;
            for (int k = 0; k < 9; ++k) {
               const double diff = matched[a].shape[k] - matched[b].shape[k];
               d += diff * diff;
            }
            const double dynamic = dynamics[a] - dynamics[b];
            const double tilt = tilts[a] - tilts[b];
            d = std::sqrt(d + dynamic * dynamic + tilt * tilt);
            if (d < closest) {
               closest = d;
               likeA = static_cast<int>(a);
               likeB = static_cast<int>(b);
            }
         }
      std::printf("       closest pair at matched distortion: models %d and %d, distance %.3f\n",
                  likeA, likeB, closest);

      // Zero would be two models that add exactly the same harmonics in the
      // same proportions and behave the same way as the signal decays -- one
      // model with two names, which is what this stage was rebuilt to get rid
      // of.
      //
      // The closest pair in the set as it stands is Soft Clip against
      // Overdrive, at 0.17, and it is worth knowing why: their harmonic
      // distributions really are close -- a rational tanh and Schetzen's
      // piecewise curve are both smooth symmetric soft clippers -- and what
      // separates them is the dynamic term. [DAFX] eq 4.14 is exactly linear
      // below a third of full scale, so it stops distorting as a note decays
      // and the tanh does not. That is audible on an instrument whose every
      // note decays, and it is the whole reason both are in the set.
      check(closest > 0.15, "no two drive models produce the same spectrum at the same THD");

      // 2c. Crunch and Lead against the service notes they were built from.
      //
      // The SD-2's service notes end with measured output waveforms: a 200 Hz
      // square at 20 mV peak to peak into the input, photographed at the
      // output for both modes. The two pictures differ in a way that is worth
      // holding the models to, and it is not a spectrum -- it is the *shape*
      // of the half-cycle. CRUNCH keeps a tall leading spike and then sags
      // towards the next edge, because its clipper sits at 1.6 V and lets go
      // as soon as the signal falls. LEAD is flat and ringing: three stages
      // and two clippers leave nothing of the envelope at all.
      //
      // Crest factor measures exactly that. A signal that spikes and sags has
      // a high one; a flattened one approaches the 1.0 of a square.
      {
         // Driven at the level the notes specify rather than at this
         // instrument's, because the shape they photographed is the shape at
         // *their* input: 20 mV peak to peak, which is 10 mV of peak against
         // each channel's own volts-per-unit.
         auto crest = [](int model, float amplitude) {
            const double rate = 48000.0;
            DriveStage stage;
            stage.prepare(rate);
            stage.setParams(model, 0.5f, 0.0f, 1.0f);
            double peak = 0.0, sum = 0.0;
            int n = 0;
            for (int i = 0; i < 24000; ++i) {
               // The service notes' own test signal, at this instrument's
               // scale: a 200 Hz square.
               const double phase = std::fmod(200.0 * i / rate, 1.0);
               const float y = stage.tick(phase < 0.5 ? amplitude : -amplitude);
               if (i >= 6000) {
                  peak = std::max(peak, std::fabs(static_cast<double>(y)));
                  sum += static_cast<double>(y) * y;
                  ++n;
               }
            }
            return n > 0 && sum > 0.0 ? peak / std::sqrt(sum / n) : 0.0;
         };
         const double crunch = crest(kDriveCrunch, 0.010f / 0.12f);
         const double lead = crest(kDriveLead, 0.010f / 0.02f);
         std::printf("       crest factor on the notes' 200 Hz square: Crunch %.2f, Lead %.2f\n",
                     crunch, lead);
         // Where Crunch's spike comes from, and why this is a test of the
         // circuit rather than of a preference: C28 and R37 give the gain leg
         // a time constant of 3.2 ms, and half a cycle of a 200 Hz square is
         // 2.5 ms. So the stage's gain is still falling when the next edge
         // arrives -- it starts a half-cycle at 383 and has not finished
         // getting back to 1. Lead's leg is C10 with R28, 0.39 ms, which is
         // over and done with long before the edge, and its two clippers
         // flatten what is left.
         check(crunch > 1.5, "Crunch keeps the spike the service notes photograph");
         check(lead < 1.25, "and Lead flattens it, as its own picture does");
         check(crunch > lead * 1.4, "and the two are not the same shape");
      }

      // 2b. Germanium against the analysis it was built from.
      //
      // [ESmash] measures the pedal's response and reports a mid hump around
      // 1.5 kHz. Nothing in the model is a 1.5 kHz anything: the hump is what
      // the 47 n shelf climbing from 720 Hz and the 741 running out of gain at
      // 4.7 kHz produce between them. So it is the one claim on that page that
      // is a *check* rather than an input, and it is worth spending a test on
      // -- take either filter out and it goes away.
      //
      // Measured small-signal, at an amplitude far below where the diodes do
      // anything, so this is the stage's frequency response and not its
      // distortion.
      {
         const double freqs[] = {60.0, 120.0, 250.0, 500.0, 1000.0, 1500.0,
                                 2000.0, 3000.0, 5000.0, 9000.0};
         const int count = static_cast<int>(sizeof(freqs) / sizeof(freqs[0]));
         double mag[10] = {0.0};
         double peak = 0.0;
         int peakAt = -1;
         for (int i = 0; i < count; ++i) {
            const Spectrum sp = measure(kDriveGermanium, 1.0, 0.0, 0.004, freqs[i]);
            mag[i] = sp.fundamental;
            if (sp.fundamental > peak) {
               peak = sp.fundamental;
               peakAt = i;
            }
         }
         std::printf("       Germanium small-signal response, 60 Hz to 9 kHz:");
         for (int i = 0; i < count; ++i)
            std::printf(" %.0f", peak > 0.0 ? 20.0 * std::log10(mag[i] / peak) : 0.0);
         std::printf(" dB (peak at %.0f Hz)\n", peakAt >= 0 ? freqs[peakAt] : 0.0);

         check(peakAt >= 0 && freqs[peakAt] >= 1000.0 && freqs[peakAt] <= 2500.0,
               "Germanium peaks in the mid, where the published analysis measures a hump");
         // The two sides of it, which is what makes it a hump rather than a
         // shelf: the bass is well down, and so is the top.
         check(peak > 0.0 && mag[0] < peak * 0.5,
               "and its bass is at least 6 dB below that peak -- the 47 nF leg");
         // 6 dB rather than 3: without the gain-dependent bandwidth limit the
         // model still rolls off at 9 kHz -- C5 and the oversampler's own
         // filter see to about 4 dB of it -- so a 3 dB bar passes a model with
         // the 741 taken out of it. 6 dB does not. Confirmed by removing it.
         check(peak > 0.0 && mag[count - 1] < peak * 0.5,
               "and its top end is 6 dB down -- the 741 running out of gain");
      }

      // 3. the documented character of each one, which is the thing a player
      //    is being promised.
      //
      //    Overdrive keeps a linear region: [DAFX] eq 4.14 is exactly 2x below
      //    a third of full scale, so a quiet signal comes through clean and a
      //    loud one does not. No other model here does that.
      const Spectrum odQuiet = measure(kDriveOverdrive, 0.0, 0.0, 0.2);
      const Spectrum odLoud = measure(kDriveOverdrive, 0.9, 0.0, 0.8);
      check(odQuiet.thd < 0.01 && odLoud.thd > 0.2,
            "Overdrive is clean below its knee and distorted above it");

      //    Tube, Fuzz and Rectifier are asymmetric and put even harmonics in;
      //    Soft Clip and Hard Clip are odd-symmetric and do not.
      const double evenTube = loud[kDriveTube].evenRatio;
      const double evenRect = loud[kDriveRectifier].evenRatio;
      const double evenSoft = loud[kDriveSoftClip].evenRatio;
      const double evenOver = loud[kDriveOverdrive].evenRatio;
      check(evenSoft < 0.15 && evenOver < 0.15,
            "the symmetric models generate almost no even harmonics");
      check(evenTube > 0.35, "the tube model generates even harmonics");
      check(evenRect > 0.5, "the rectifier is mostly even harmonics");

      //    And the rectifier really does double the fundamental: full-wave
      //    rectification moves the energy to twice the input frequency
      //    ([DAFX] 4.3.3, figure 4.36).
      const Spectrum rect = measure(kDriveRectifier, 1.0, 1.0, 0.8);
      check(rect.even > rect.fundamental * rect.fundamental,
            "the rectifier at full drive puts more energy an octave up than at the note");

      //    Valve Stack has a tone stack in it -- [Pirkle] 19.13 puts a low
      //    shelf and a high shelf between the third stage and the fourth -- so
      //    it is the one model whose *frequency* response differs from the
      //    others. Measured where it shows: a 100 Hz note against a 1 kHz one.
      {
         const Spectrum valveLow = measure(kDriveValveStack, 0.5, 0.0, 0.8);
         const Spectrum softLow = measure(kDriveSoftClip, 0.5, 0.0, 0.8);
         auto lowVsMid = [&](int model) {
            const double rate = 48000.0;
            DriveStage stage;
            stage.prepare(rate);
            stage.setParams(model, 0.5f, 0.0f, 1.0f);
            double low = 0.0, mid = 0.0;
            for (int n = 0; n < 8000; ++n) {
               const float x = static_cast<float>(0.5 * std::sin(2.0 * kPi * 100.0 * n / rate));
               const float y = stage.tick(x);
               if (n >= 2000)
                  low += static_cast<double>(y) * y;
            }
            stage.reset();
            for (int n = 0; n < 8000; ++n) {
               const float x = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * n / rate));
               const float y = stage.tick(x);
               if (n >= 2000)
                  mid += static_cast<double>(y) * y;
            }
            return low / (mid > 1e-12 ? mid : 1e-12);
         };
         const double valveTilt = lowVsMid(kDriveValveStack);
         const double softTilt = lowVsMid(kDriveSoftClip);
         (void)valveLow;
         (void)softLow;
         check(valveTilt < softTilt * 0.8,
               "the valve stack's tone stack thins the bottom, which no other model does");
      }

      //    Crush quantises, so its output takes a small number of distinct
      //    values -- which no analogue model here does.
      {
         DriveStage stage;
         stage.prepare(48000.0);
         stage.setParams(kDriveCrush, 1.0f, 0.0f, 1.0f);
         std::vector<float> seen;
         for (int n = 0; n < 2000; ++n) {
            const float y = stage.shapeOnly(static_cast<float>(-1.0 + 2.0 * n / 1999.0));
            if (std::find_if(seen.begin(), seen.end(), [&](float v) {
                   return std::fabs(v - y) < 1e-6f;
                }) == seen.end())
               seen.push_back(y);
         }
         check(seen.size() > 2 && seen.size() < 40,
               "Crush quantises its input to a handful of levels");
      }

      // 4. Bias moves the operating point, which is what puts even harmonics
      //    into a model that is symmetric at the centre.
      const Spectrum centred = measure(kDriveOverdrive, 0.7, 0.0, 0.8);
      const Spectrum biased = measure(kDriveOverdrive, 0.7, 0.9, 0.8);
      check(biased.evenRatio > centred.evenRatio + 0.2,
            "Bias turns a symmetric model into an asymmetric one");

      // 5. Soft Clip is the model this instrument always had, and Bias does
      //    not touch it -- which is what keeps every preset written before the
      //    models existed sounding exactly as it did.
      {
         DriveStage a, b;
         a.prepare(48000.0);
         b.prepare(48000.0);
         a.setParams(kDriveSoftClip, 0.2f, 0.0f, 1.0f);
         b.setParams(kDriveSoftClip, 0.2f, 1.0f, 1.0f);
         bool same = true;
         for (int n = 0; n < 512; ++n) {
            const float x = static_cast<float>(std::sin(n * 0.07));
            if (a.tick(x) != b.tick(x))
               same = false;
         }
         check(same, "Bias leaves Soft Clip exactly as it was");
      }

      // 6. the mix control really is a bypass at zero, whatever model is set.
      {
         DriveStage a, b;
         a.prepare(48000.0);
         b.prepare(48000.0);
         a.setParams(kDriveFuzz, 1.0f, 0.5f, 0.0f);
         b.setParams(kDriveRectifier, 1.0f, -0.5f, 0.0f);
         bool bypassed = true;
         for (int n = 0; n < 512; ++n) {
            const float x = static_cast<float>(std::sin(n * 0.07));
            if (a.tick(x) != x || b.tick(x) != x)
               bypassed = false;
         }
         check(bypassed, "Dist Mix at zero bypasses the stage whatever model is set");
      }
   }

   // --- the whole plugin with each drive model in it: finite, bounded, and
   // still an instrument.
   {
      using namespace saeurekiste;
      bool finite = true, bounded = true, audible = true;
      for (int model = 0; model < kNumDriveModels; ++model) {
         gParamOverrides.clear();
         gParamOverrides.emplace_back(kParamMode, static_cast<double>(kModeMidi));
         gParamOverrides.emplace_back(kParamDistType, static_cast<double>(model));
         gParamOverrides.emplace_back(kParamDrive, 1.0);
         gParamOverrides.emplace_back(kParamDistMix, 1.0);
         gParamOverrides.emplace_back(kParamVolume, 0.0);
         plugin->reset(plugin);
         const RenderResult r = renderPlugin(plugin, sampleRate, 512, 0.6, 0.6, 45, 1.0);
         if (r.sawNonFinite)
            finite = false;
         if (r.peak > 1.001f)
            bounded = false;
         if (r.peak < 0.01f)
            audible = false;
      }
      gParamOverrides.clear();
      check(finite, "the plugin stays finite with every drive model");
      check(bounded, "the plugin stays bounded with every drive model");
      check(audible, "the plugin makes sound with every drive model");
   }

   // --- preset packs. A folder of presets as one file, and back again.
   {
      using namespace saeurekiste;
      std::vector<plugincore::PresetPackEntry> entries;
      plugincore::PresetPackEntry one;
      one.name = "First";
      one.text = "# preset\nformat = 1\nname = First\ncutoff = 700\n"
                 "seq_pitch     = C   .   G   .\n";
      plugincore::PresetPackEntry two;
      two.name = "Second";
      two.text = "# preset\nformat = 1\nname = Second\ncutoff = 900\n";
      entries.push_back(one);
      entries.push_back(two);

      const std::string packText =
         plugincore::formatPresetPack(presetContext(), "Live Set", entries);
      std::string packName;
      std::vector<plugincore::PresetPackEntry> back;
      std::string packErr;
      check(plugincore::parsePresetPack(presetContext(), packText, packName, back, packErr),
            "a pack parses back in");
      check(packName == "Live Set", "a pack keeps its name");
      check(back.size() == entries.size(), "a pack keeps every preset");
      bool sameText = back.size() == entries.size();
      for (size_t i = 0; sameText && i < back.size(); ++i)
         if (back[i].name != entries[i].name || back[i].text != entries[i].text)
            sameText = false;
      // Byte for byte, because a pack carries each preset's text rather than a
      // re-serialised copy -- which is what keeps this plugin's pattern lines,
      // which the shared format knows nothing about, intact across the trip.
      check(sameText, "a pack keeps each preset's text exactly, pattern lines and all");

      std::string ignored;
      std::vector<plugincore::PresetPackEntry> nothing;
      std::string rejectErr;
      check(!plugincore::parsePresetPack(presetContext(),
                                         "# preset\nformat = 1\nname = Not A Pack\n", ignored,
                                         nothing, rejectErr),
            "a preset is not mistaken for a pack");

      // A folder is one level under the user preset directory, and its name is
      // sanitised the same way a preset's is.
      const std::string inFolder = plugincore::userPresetPathIn(presetContext(), "Live Set!",
                                                                "My Line");
      check(inFolder.empty() || inFolder.find("Live_Set") != std::string::npos,
            "a folder name reaches the path, sanitised");
      check(inFolder.empty() || inFolder.find("My_Line") != std::string::npos,
            "and the preset's name is still in it");
   }

   plugin->deactivate(plugin);
   plugin->destroy(plugin);

   std::printf("\nselftest: %d failure(s)\n", failures);
   return failures == 0 ? 0 : 1;
}

} // namespace
