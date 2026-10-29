// Opens Shapeau's editor in a plain X11 window so its interface can be driven
// and photographed without a DAW. The plugin's GUI is embedded (non-floating),
// so a host has to supply the parent window; that is all this does.
//
//   ./shapeau-guihost [plugin.clap] [seconds] [--scale s] [--sidechain]
//                     [--param Module/Name=value ...]
//
// It processes a drum loop on a thread as well, because the editor reads its
// parameter values, its peaks and its scope back through the same path a host
// would -- and a scope fed a steady tone shows nothing a scope is for. With
// --sidechain the input is a bass line and the kick goes to the sidechain
// instead, the way a duck is set up in a DAW. --param sets a parameter by the
// name a host lists it under, as text: Volume/Trigger=Audio, Trigger/Threshold=-30.

#include <clap/clap.h>

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <X11/Xlib.h>
#   include <dlfcn.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> gRunning{true};

const clap_host_log_t kHostLog = {[](const clap_host_t *, clap_log_severity, const char *) {}};
const clap_host_params_t kHostParams = {
   [](const clap_host_t *, clap_param_rescan_flags) {},
   [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
   [](const clap_host_t *) {},
};
const clap_host_thread_check_t kHostThreadCheck = {
   [](const clap_host_t *) { return true; },
   [](const clap_host_t *) { return false; },
};
const clap_host_gui_t kHostGui = {
   [](const clap_host_t *) {},                                                  // resize hints changed
   [](const clap_host_t *, uint32_t, uint32_t) { return true; },                // request resize
   [](const clap_host_t *) { return true; },                                    // request show
   [](const clap_host_t *) { return true; },                                    // request hide
   [](const clap_host_t *, bool) {},                                            // closed
};

clap_host_t gHost = {
   CLAP_VERSION_INIT,
   nullptr,
   "shapeau-guihost",
   "Ravetracer",
   "https://github.com/Ravetracer/audio-plugins",
   "0.1.0",
   [](const clap_host_t *, const char *id) -> const void * {
      if (std::strcmp(id, CLAP_EXT_LOG) == 0)
         return &kHostLog;
      if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
         return &kHostParams;
      if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
         return &kHostThreadCheck;
      if (std::strcmp(id, CLAP_EXT_GUI) == 0)
         return &kHostGui;
      return nullptr;
   },
   [](const clap_host_t *) {},
   [](const clap_host_t *) {},
   [](const clap_host_t *) {},
};

void *openLibrary(const char *path) {
#if defined(_WIN32)
   return static_cast<void *>(LoadLibraryA(path));
#else
   return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *findSymbol(void *handle, const char *name) {
#if defined(_WIN32)
   return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
   return dlsym(handle, name);
#endif
}

} // namespace

int main(int argc, char **argv) {
   std::string path = argc > 1 ? argv[1] : "build/Shapeau.clap";
   double seconds = argc > 2 ? std::atof(argv[2]) : 8.0;
   // --scale asks the editor for a GUI scale before the window is made, the way
   // a host on a high-density screen would; screenshots for print and the web
   // are taken at 1.5.
   double scale = 0.0;
   bool sidechain = false;
   std::vector<std::string> settings;
   for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--scale" && i + 1 < argc)
         scale = std::atof(argv[++i]);
      else if (a == "--sidechain")
         sidechain = true;
      else if (a == "--param" && i + 1 < argc)
         settings.push_back(argv[++i]);
   }

   void *handle = openLibrary(path.c_str());
   if (!handle) {
      std::fprintf(stderr, "cannot load %s\n", path.c_str());
      return 1;
   }
   const auto *entry =
      static_cast<const clap_plugin_entry_t *>(findSymbol(handle, "clap_entry"));
   if (!entry || !entry->init(path.c_str())) {
      std::fprintf(stderr, "%s is not a CLAP plugin\n", path.c_str());
      return 1;
   }
   const auto *factory =
      static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
   const clap_plugin_descriptor_t *desc =
      factory ? factory->get_plugin_descriptor(factory, 0) : nullptr;
   if (!desc) {
      std::fprintf(stderr, "no plugin in %s\n", path.c_str());
      return 1;
   }
   const clap_plugin_t *plugin = factory->create_plugin(factory, &gHost, desc->id);
   if (!plugin) {
      std::fprintf(stderr, "cannot instantiate %s\n", desc->id);
      return 1;
   }
   plugin->init(plugin);
   plugin->activate(plugin, 48000.0, 1, 512);
   plugin->start_processing(plugin);

   // Parameters by the name a host shows them under, and their value as text,
   // through the same flush a host uses.
   const auto *params =
      static_cast<const clap_plugin_params_t *>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
   for (const std::string &setting : settings) {
      const size_t eq = setting.find('=');
      if (!params || eq == std::string::npos)
         continue;
      const std::string name = setting.substr(0, eq), text = setting.substr(eq + 1);
      uint32_t id = UINT32_MAX;
      for (uint32_t i = 0; i < params->count(plugin); ++i) {
         clap_param_info_t info{};
         if (params->get_info(plugin, i, &info) &&
             strcasecmp((std::string(info.module) + "/" + info.name).c_str(), name.c_str()) == 0)
            id = info.id;
      }
      double value = 0.0;
      if (id == UINT32_MAX || !params->text_to_value(plugin, id, text.c_str(), &value)) {
         std::fprintf(stderr, "cannot set %s\n", setting.c_str());
         continue;
      }
      clap_event_param_value_t ev{};
      ev.header.size = sizeof(ev);
      ev.header.type = CLAP_EVENT_PARAM_VALUE;
      ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      ev.param_id = id;
      ev.note_id = -1;
      ev.port_index = -1;
      ev.channel = -1;
      ev.key = -1;
      ev.value = value;
      const clap_event_header_t *header = &ev.header;
      clap_input_events_t in{&header, [](const clap_input_events_t *) { return 1u; },
                             [](const clap_input_events_t *list, uint32_t) {
                                return *static_cast<const clap_event_header_t *const *>(list->ctx);
                             }};
      clap_output_events_t out{nullptr, [](const clap_output_events_t *,
                                           const clap_event_header_t *) { return true; }};
      params->flush(plugin, &in, &out);
   }

   const auto *gui =
      static_cast<const clap_plugin_gui_t *>(plugin->get_extension(plugin, CLAP_EXT_GUI));
   if (!gui) {
      std::fprintf(stderr, "this build has no editor\n");
      return 1;
   }

   Display *display = XOpenDisplay(nullptr);
   if (!display) {
      std::fprintf(stderr, "cannot open a display\n");
      return 1;
   }

   if (!gui->create(plugin, CLAP_WINDOW_API_X11, false)) {
      std::fprintf(stderr, "the editor could not be created\n");
      return 1;
   }
   if (scale > 0.0)
      gui->set_scale(plugin, scale);
   uint32_t width = 1200, height = 800;
   gui->get_size(plugin, &width, &height);

   const Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display), 40, 40, width,
                                             height, 0, 0, 0x101418);
   XStoreName(display, parent, "Shapeau");
   XSelectInput(display, parent, ExposureMask | StructureNotifyMask);
   XMapWindow(display, parent);
   // The plugin reparents into this window on its *own* X connection, and two
   // connections have no ordering between them: without this round trip the
   // server can see ReparentWindow before CreateWindow and answer BadWindow.
   XSync(display, False);

   clap_window_t window{};
   window.api = CLAP_WINDOW_API_X11;
   window.x11 = parent;
   if (!gui->set_parent(plugin, &window)) {
      std::fprintf(stderr, "the editor could not be embedded\n");
      return 1;
   }
   gui->show(plugin);

   std::thread audio([&] {
      float left[512] = {0}, right[512] = {0};
      float sideL[512] = {0}, sideR[512] = {0};
      // A drum loop at 120 BPM, so the editor's scope has something to show:
      // a kick on every beat whose pitch falls from 160 to 50 Hz, a hat on
      // every off-beat, and a quiet bass note under both. With --sidechain
      // the kick -- with an extra one on the last sixteenth of the bar, which
      // a duck in Sync cannot follow and one set to Audio can -- is on the
      // sidechain, and the input is an eighth-note bass line and the hats.
      double phase = 0.0, kickPhase = 0.0;
      uint64_t frame = 0;
      uint32_t noise = 12345u;
      while (gRunning.load()) {
         for (int i = 0; i < 512; ++i, ++frame) {
            const uint64_t inBar = frame % 96000;
            uint64_t sinceKick = frame % 24000;
            if (sidechain && inBar >= 90000)
               sinceKick = inBar - 90000;
            const double t = static_cast<double>(sinceKick) / 48000.0; // since the kick
            const double off = static_cast<double>((frame + 12000) % 24000) / 48000.0;
            kickPhase = t < 1.0 / 48000.0 ? 0.0 : kickPhase;
            kickPhase += 2.0 * 3.14159265358979 * (50.0 + 110.0 * std::exp(-t * 30.0)) / 48000.0;
            noise = noise * 1664525u + 1013904223u;
            const double white = static_cast<double>(noise >> 8) / 8388608.0 - 1.0;
            const double kick = 0.7 * std::exp(-t * 9.0) * std::sin(kickPhase);
            const double hat = 0.12 * std::exp(-off * 60.0) * white;
            if (sidechain) {
               const double eighth = static_cast<double>(frame % 12000) / 48000.0;
               const double bass = 0.45 * std::min(1.0, eighth * 400.0) *
                                   (std::sin(phase) + 0.3 * std::sin(2.0 * phase));
               left[i] = right[i] = static_cast<float>(bass + hat);
               sideL[i] = sideR[i] = static_cast<float>(kick);
            } else {
               left[i] = right[i] = static_cast<float>(kick + hat + 0.08 * std::sin(phase));
            }
            phase += 2.0 * 3.14159265358979 * 55.0 / 48000.0;
         }
         float *inPtrs[2] = {left, right};
         float *sidePtrs[2] = {sideL, sideR};
         float *outPtrs[2] = {left, right};
         clap_audio_buffer_t inBufs[2] = {};
         inBufs[0].data32 = inPtrs;
         inBufs[0].channel_count = 2;
         inBufs[1].data32 = sidePtrs;
         inBufs[1].channel_count = 2;
         clap_audio_buffer_t outBuf{};
         outBuf.data32 = outPtrs;
         outBuf.channel_count = 2;
         clap_process_t proc{};
         proc.frames_count = 512;
         proc.audio_inputs = inBufs;
         proc.audio_inputs_count = sidechain ? 2 : 1;
         proc.audio_outputs = &outBuf;
         proc.audio_outputs_count = 1;
         plugin->process(plugin, &proc);
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
   });

   const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
   while (std::chrono::steady_clock::now() < deadline) {
      while (XPending(display)) {
         XEvent ev;
         XNextEvent(display, &ev);
         if (ev.type == DestroyNotify)
            gRunning.store(false);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
   }

   gRunning.store(false);
   audio.join();

   gui->hide(plugin);
   gui->destroy(plugin);
   XDestroyWindow(display, parent);
   XCloseDisplay(display);
   plugin->stop_processing(plugin);
   plugin->deactivate(plugin);
   plugin->destroy(plugin);
   entry->deinit();
   return 0;
}
