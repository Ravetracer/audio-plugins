// Opens Shapeau's editor in a plain X11 window so its interface can be driven
// and photographed without a DAW. The plugin's GUI is embedded (non-floating),
// so a host has to supply the parent window; that is all this does.
//
//   ./shapeau-guihost [plugin.clap] [seconds]
//
// It processes a drum loop on a thread as well, because the editor reads its
// parameter values, its peaks and its scope back through the same path a host
// would -- and a scope fed a steady tone shows nothing a scope is for.

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
#include <thread>

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
      // A drum loop at 120 BPM, so the editor's scope has something to show:
      // a kick on every beat whose pitch falls from 160 to 50 Hz, a hat on
      // every off-beat, and a quiet bass note under both.
      double phase = 0.0, kickPhase = 0.0;
      uint64_t frame = 0;
      uint32_t noise = 12345u;
      while (gRunning.load()) {
         for (int i = 0; i < 512; ++i, ++frame) {
            const double t = static_cast<double>(frame % 24000) / 48000.0;  // into the beat
            const double off = static_cast<double>((frame + 12000) % 24000) / 48000.0;
            kickPhase = t < 1.0 / 48000.0 ? 0.0 : kickPhase;
            kickPhase += 2.0 * 3.14159265358979 * (50.0 + 110.0 * std::exp(-t * 30.0)) / 48000.0;
            noise = noise * 1664525u + 1013904223u;
            const double white = static_cast<double>(noise >> 8) / 8388608.0 - 1.0;
            const double s = 0.7 * std::exp(-t * 9.0) * std::sin(kickPhase) +
                             0.12 * std::exp(-off * 60.0) * white +
                             0.08 * std::sin(phase);
            left[i] = static_cast<float>(s);
            right[i] = static_cast<float>(s);
            phase += 2.0 * 3.14159265358979 * 55.0 / 48000.0;
         }
         float *inPtrs[2] = {left, right};
         float *outPtrs[2] = {left, right};
         clap_audio_buffer_t inBuf{};
         inBuf.data32 = inPtrs;
         inBuf.channel_count = 2;
         clap_audio_buffer_t outBuf{};
         outBuf.data32 = outPtrs;
         outBuf.channel_count = 2;
         clap_process_t proc{};
         proc.frames_count = 512;
         proc.audio_inputs = &inBuf;
         proc.audio_inputs_count = 1;
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
