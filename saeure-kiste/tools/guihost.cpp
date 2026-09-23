// Opens SaeureKiste's editor in a plain X11 window so the interface can be driven
// and photographed without a DAW. The plugin's GUI is embedded (non-floating),
// so a host has to supply the parent window; that is all this does.
//
//   ./saeurekiste-guihost [plugin.clap] [preset.saeurekiste] [seconds]
//                         [--also <preset.saeurekiste|-> ...]
//                         [--note <key>[@<seconds>[+<hold>]] ...]
//                         [--scale <factor>]
//
// --scale asks the editor for a GUI scale before the window is made, the way a
// host on a HiDPI screen would. Its real use here is the manual: a screenshot
// taken at 2 is legible when it is printed, and one taken at 1 is not.
//
// --note plays one MIDI note into the first instance, `seconds` after the
// window opens (2 by default), held for `hold` seconds (a quarter of a second
// by default), and may be repeated. A long hold runs the sequencer, which is
// how a chain moving through the bank is watched. It exists because some of
// the interface only answers to notes -- Live mode's pattern map is learned by
// playing the note you want a pad to send -- and that half of the window
// cannot be driven, photographed or tested without one.
//
// Each --also opens another instance of the same plugin in its own window,
// loaded with its own preset ("-" for the defaults). One process, one module,
// several instances: that is what a DAW does with two tracks of the same
// plugin, and it is the only arrangement in which state accidentally shared
// between instances can be seen at all.
//
// It processes audio on a timer thread as well, because the editor reads its
// parameter values back through the same path a host would.

#include <clap/clap.h>

#if defined(_WIN32)
#   define RD_WINDOW_API CLAP_WINDOW_API_WIN32
#else
#   define RD_WINDOW_API CLAP_WINDOW_API_X11
#endif

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <X11/Xlib.h>
#   include <dlfcn.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> gRunning{true};

// One plugin instance and the host window it is parented into. Everything a
// DAW keeps per track: its own clap_host_t, so a callback can tell which
// instance made it, and its own size and resize request.
struct Instance {
   const clap_plugin_t *plug = nullptr;
   const clap_plugin_gui_t *gui = nullptr;
   clap_host_t host{};
   std::string title;
   uint32_t w = 900;
   uint32_t h = 648;
   std::atomic<bool> resizeWanted{false};
   std::atomic<uint32_t> resizeW{0};
   std::atomic<uint32_t> resizeH{0};
#if defined(_WIN32)
   HWND window = nullptr;
#else
   Window window = 0;
#endif
};

Instance *instanceOf(const clap_host_t *host) {
   return host ? static_cast<Instance *>(host->host_data) : nullptr;
}

void hostGuiResizeHintsChanged(const clap_host_t *) {}

// The plugin asks to be resized when its collapsible panel section opens or
// closes. A DAW obliges by resizing the window it parented the editor into;
// this does the same, on the event loop rather than on the calling thread,
// because that is where the X connection is.
bool hostGuiRequestResize(const clap_host_t *host, uint32_t width, uint32_t height) {
   Instance *inst = instanceOf(host);
   if (!inst)
      return false;
   inst->resizeW.store(width);
   inst->resizeH.store(height);
   inst->resizeWanted.store(true);
   return true;
}

bool hostGuiRequestShow(const clap_host_t *) { return false; }
bool hostGuiRequestHide(const clap_host_t *) { return false; }
void hostGuiClosed(const clap_host_t *, bool) {}

const clap_host_gui_t gHostGui = {hostGuiResizeHintsChanged, hostGuiRequestResize,
                                  hostGuiRequestShow, hostGuiRequestHide, hostGuiClosed};

const void *hostGetExtension(const clap_host_t *, const char *id) {
   if (id && std::strcmp(id, CLAP_EXT_GUI) == 0)
      return &gHostGui;
   return nullptr;
}
void hostRequestRestart(const clap_host_t *) {}
void hostRequestProcess(const clap_host_t *) {}
void hostRequestCallback(const clap_host_t *) {}

} // namespace

int main(int argc, char **argv) {
   std::string pluginPath = "./SaeureKiste.clap";
   int liveSeconds = 600;
   // Notes to play into the first instance: the key, when, and for how long.
   struct TimedNote {
      int key = 60;
      double at = 2.0;
      double hold = 0.25;
   };
   std::vector<TimedNote> notes;
   double scale = 0.0; // 0 = leave the editor at whatever it defaults to
   // One entry per instance: the preset it opens with, or empty for the
   // defaults. The first is the positional argument, the rest come from --also.
   std::vector<std::string> presets(1);

   {
      int positional = 0;
      for (int i = 1; i < argc; ++i) {
         const std::string arg = argv[i];
         if (arg == "--scale") {
            if (i + 1 >= argc) {
               std::fprintf(stderr, "--scale wants a factor\n");
               return 1;
            }
            scale = std::atof(argv[++i]);
         } else if (arg == "--note") {
            if (i + 1 >= argc) {
               std::fprintf(stderr, "--note wants a key, optionally key@seconds+hold\n");
               return 1;
            }
            const std::string spec = argv[++i];
            TimedNote n;
            const size_t at = spec.find('@');
            n.key = std::atoi(spec.substr(0, at).c_str());
            if (at != std::string::npos)
               n.at = std::atof(spec.c_str() + at + 1);
            const size_t plus = spec.find('+', at == std::string::npos ? 0 : at);
            if (plus != std::string::npos)
               n.hold = std::atof(spec.c_str() + plus + 1);
            notes.push_back(n);
         } else if (arg == "--also") {
            if (i + 1 >= argc) {
               std::fprintf(stderr, "--also wants a preset path, or - for the defaults\n");
               return 1;
            }
            const std::string p = argv[++i];
            presets.push_back(p == "-" ? std::string() : p);
         } else if (positional == 0) {
            pluginPath = arg;
            ++positional;
         } else if (positional == 1) {
            presets[0] = arg;
            ++positional;
         } else if (positional == 2) {
            liveSeconds = std::atoi(arg.c_str());
            ++positional;
         }
      }
   }

#if defined(_WIN32)
   HMODULE lib = LoadLibraryA(pluginPath.c_str());
   if (!lib) {
      std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
      return 1;
   }
   auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(lib, "clap_entry"));
#else
   void *lib = dlopen(pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
      return 1;
   }
   auto *entry = static_cast<const clap_plugin_entry_t *>(dlsym(lib, "clap_entry"));
#endif
   if (!entry || !entry->init(pluginPath.c_str())) {
      std::fprintf(stderr, "no usable clap_entry\n");
      return 1;
   }

   auto *fac = static_cast<const clap_plugin_factory_t *>(
      entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
   const clap_plugin_descriptor_t *desc = fac->get_plugin_descriptor(fac, 0);

   const double sr = 48000.0;
   const uint32_t block = 512;

#if !defined(_WIN32)
   Display *dpy = XOpenDisplay(nullptr);
   if (!dpy) {
      std::fprintf(stderr, "no X display\n");
      return 1;
   }
   const int screen = DefaultScreen(dpy);
#else
   WNDCLASSEXW wc{};
   wc.cbSize = sizeof(wc);
   wc.lpfnWndProc = DefWindowProcW;
   wc.hInstance = GetModuleHandleW(nullptr);
   wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
   wc.lpszClassName = L"SaeureKisteGuiHost";
   RegisterClassExW(&wc);
#endif

   // unique_ptr rather than a plain vector: an Instance holds atomics, so it
   // cannot be moved, and a growing vector would move it.
   std::vector<std::unique_ptr<Instance>> instances;

   for (size_t i = 0; i < presets.size(); ++i) {
      auto inst = std::make_unique<Instance>();
      // A distinct title per instance. The verification recipe picks a window
      // out of `xdotool search` by name, and two windows with the same name is
      // exactly how the wrong one gets grabbed.
      inst->title = presets.size() > 1 ? "SaeureKiste #" + std::to_string(i + 1) : "SaeureKiste";

      inst->host.clap_version = CLAP_VERSION;
      inst->host.host_data = inst.get();
      inst->host.name = "saeurekiste-guihost";
      inst->host.vendor = "SaeureKiste";
      inst->host.url = "";
      inst->host.version = "1.0";
      inst->host.get_extension = hostGetExtension;
      inst->host.request_restart = hostRequestRestart;
      inst->host.request_process = hostRequestProcess;
      inst->host.request_callback = hostRequestCallback;

      const clap_plugin_t *plug = fac->create_plugin(fac, &inst->host, desc->id);
      if (!plug || !plug->init(plug)) {
         std::fprintf(stderr, "plugin init failed\n");
         return 1;
      }
      inst->plug = plug;
      plug->activate(plug, sr, block, block);
      plug->start_processing(plug);

      if (!presets[i].empty()) {
         auto *pl = static_cast<const clap_plugin_preset_load_t *>(
            plug->get_extension(plug, CLAP_EXT_PRESET_LOAD));
         if (pl && !pl->from_location(plug, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                      presets[i].c_str(), ""))
            std::fprintf(stderr, "preset load failed: %s\n", presets[i].c_str());
      }

      auto *gui = static_cast<const clap_plugin_gui_t *>(plug->get_extension(plug, CLAP_EXT_GUI));
      if (!gui || !gui->is_api_supported(plug, RD_WINDOW_API, false)) {
         std::fprintf(stderr, "plugin has no embedded GUI for %s\n", RD_WINDOW_API);
         return 1;
      }
      if (!gui->create(plug, RD_WINDOW_API, false)) {
         std::fprintf(stderr, "gui create failed\n");
         return 1;
      }
      inst->gui = gui;
      // Before get_size, so the host window is made at the scaled size rather
      // than resized under a window that has already been placed.
      if (scale > 0.0)
         gui->set_scale(plug, scale);
      gui->get_size(plug, &inst->w, &inst->h);

#if defined(_WIN32)
      // A plain top-level window standing in for the host's, with the plugin's
      // own window parented into it exactly as a DAW would.
      RECT wanted{0, 0, static_cast<LONG>(inst->w), static_cast<LONG>(inst->h)};
      AdjustWindowRect(&wanted, WS_OVERLAPPEDWINDOW, FALSE);
      const std::wstring wtitle(inst->title.begin(), inst->title.end());
      HWND hostWindow = CreateWindowExW(
         0, L"SaeureKisteGuiHost", wtitle.c_str(), WS_OVERLAPPEDWINDOW,
         CW_USEDEFAULT, CW_USEDEFAULT, wanted.right - wanted.left, wanted.bottom - wanted.top,
         nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!hostWindow) {
         std::fprintf(stderr, "could not create the host window\n");
         return 1;
      }
      inst->window = hostWindow;
      ShowWindow(hostWindow, SW_SHOW);

      clap_window_t parent{};
      parent.api = CLAP_WINDOW_API_WIN32;
      parent.win32 = hostWindow;
      if (!gui->set_parent(plug, &parent)) {
         std::fprintf(stderr, "set_parent failed\n");
         return 1;
      }
      gui->show(plug);
      std::printf("window %p  %ux%u  %s\n", static_cast<void *>(hostWindow), inst->w, inst->h,
                  inst->title.c_str());
      std::fflush(stdout);
#else
      // Side by side rather than stacked, so two of them can be photographed
      // without moving either.
      const int x = static_cast<int>(i) * 40;
      const int y = static_cast<int>(i) * 40;
      Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), x, y, inst->w, inst->h, 0,
                                       BlackPixel(dpy, screen), BlackPixel(dpy, screen));
      XStoreName(dpy, win, inst->title.c_str());
      XSelectInput(dpy, win, StructureNotifyMask);
      XMapWindow(dpy, win);
      XFlush(dpy);
      inst->window = win;

      clap_window_t parent{};
      parent.api = RD_WINDOW_API;
      parent.x11 = static_cast<clap_xwnd>(win);
      if (!gui->set_parent(plug, &parent)) {
         std::fprintf(stderr, "set_parent failed\n");
         return 1;
      }
      gui->show(plug);
      XFlush(dpy);
      std::printf("window 0x%lx  %ux%u  %s\n", win, inst->w, inst->h, inst->title.c_str());
      std::fflush(stdout);
#endif
      instances.push_back(std::move(inst));
   }

   // The editor publishes its parameter edits through process(), so the audio
   // thread has to keep turning for the interface to behave as it does in a host.
   // One thread for all of them, which is what a host does per audio buffer.
   std::thread audio([&] {
      std::vector<float> l(block), r(block);
      float *chans[2] = {l.data(), r.data()};
      clap_audio_buffer_t ab{};
      ab.data32 = chans;
      ab.channel_count = 2;
      // The block's events, which are empty for every instance but the first
      // and empty for that one too unless a --note is due.
      std::vector<clap_event_note_t> pending;
      clap_input_events_t in{};
      in.ctx = &pending;
      in.size = [](const clap_input_events_t *l) -> uint32_t {
         return static_cast<uint32_t>(
            static_cast<std::vector<clap_event_note_t> *>(l->ctx)->size());
      };
      in.get = [](const clap_input_events_t *l, uint32_t i) -> const clap_event_header_t * {
         auto *v = static_cast<std::vector<clap_event_note_t> *>(l->ctx);
         return i < v->size() ? &(*v)[i].header : nullptr;
      };
      clap_input_events_t empty{};
      empty.size = [](const clap_input_events_t *) -> uint32_t { return 0; };
      empty.get = [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * {
         return nullptr;
      };
      clap_output_events_t out{};
      out.try_push = [](const clap_output_events_t *, const clap_event_header_t *) { return true; };

      const auto started = std::chrono::steady_clock::now();
      // Each note is played once, at its own time and for its own length.
      std::vector<bool> sent(notes.size(), false), released(notes.size(), false);

      while (gRunning.load()) {
         pending.clear();
         const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started).count();
         for (size_t i = 0; i < notes.size(); ++i) {
            clap_event_note_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.note_id = -1;
            ev.port_index = 0;
            ev.channel = 0;
            ev.key = static_cast<int16_t>(notes[i].key);
            if (!sent[i] && now >= notes[i].at) {
               ev.header.type = CLAP_EVENT_NOTE_ON;
               ev.velocity = 0.8;
               pending.push_back(ev);
               sent[i] = true;
               std::printf("note on  %d at %.2fs\n", notes[i].key, now);
               std::fflush(stdout);
            } else if (sent[i] && !released[i] && now >= notes[i].at + notes[i].hold) {
               ev.header.type = CLAP_EVENT_NOTE_OFF;
               ev.velocity = 0.0;
               pending.push_back(ev);
               released[i] = true;
            }
         }
         bool first = true;
         for (auto &inst : instances) {
            clap_process_t pr{};
            pr.frames_count = block;
            pr.audio_outputs = &ab;
            pr.audio_outputs_count = 1;
            pr.in_events = first ? &in : &empty;
            pr.out_events = &out;
            inst->plug->process(inst->plug, &pr);
            first = false;
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
   });

   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(liveSeconds);
   while (std::chrono::steady_clock::now() < deadline) {
      for (auto &instPtr : instances) {
         Instance &inst = *instPtr;
#if defined(_WIN32)
         // A resize the plugin asked for, taken the way a DAW takes it. The same
         // job as the X11 branch below, but the host window's frame has to be
         // added back: SetWindowPos sizes the whole window and the plugin asked
         // for a client area.
         if (inst.resizeWanted.exchange(false)) {
            const uint32_t nw = inst.resizeW.load();
            const uint32_t nh = inst.resizeH.load();
            if (nw && nh && (nw != inst.w || nh != inst.h)) {
               inst.w = nw;
               inst.h = nh;
               RECT want{0, 0, static_cast<LONG>(inst.w), static_cast<LONG>(inst.h)};
               AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
               SetWindowPos(inst.window, nullptr, 0, 0, want.right - want.left,
                            want.bottom - want.top, SWP_NOMOVE | SWP_NOZORDER);
               std::printf("host resize %ux%u  %s\n", inst.w, inst.h, inst.title.c_str());
               std::fflush(stdout);
            }
         }
         // The plugin's window is repainted from the host's timer in a DAW; here
         // there is no timer, so the host drives it.
         if (auto *timer = static_cast<const clap_plugin_timer_support_t *>(
                inst.plug->get_extension(inst.plug, CLAP_EXT_TIMER_SUPPORT)))
            timer->on_timer(inst.plug, 0);
#else
         // A resize the plugin asked for, taken the way a DAW takes it: the host
         // window follows the editor rather than the other way round.
         if (inst.resizeWanted.exchange(false)) {
            const uint32_t nw = inst.resizeW.load();
            const uint32_t nh = inst.resizeH.load();
            if (nw && nh && (nw != inst.w || nh != inst.h)) {
               inst.w = nw;
               inst.h = nh;
               XResizeWindow(dpy, inst.window, inst.w, inst.h);
               XFlush(dpy);
               std::printf("host resize %ux%u  %s\n", inst.w, inst.h, inst.title.c_str());
               std::fflush(stdout);
            }
         }
#endif
      }

#if defined(_WIN32)
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
         TranslateMessage(&msg);
         DispatchMessageW(&msg);
      }
#else
      while (XPending(dpy)) {
         XEvent ev;
         XNextEvent(dpy, &ev);
         if (ev.type != ConfigureNotify)
            continue;
         // The user resized the host window: offer the new size to the plugin
         // the way a DAW does -- adjust_size to snap it, set_size to take it --
         // and follow the plugin to the size it settled on.
         for (auto &instPtr : instances) {
            Instance &inst = *instPtr;
            if (ev.xconfigure.window != inst.window || !inst.gui->can_resize(inst.plug))
               continue;
            uint32_t nw = static_cast<uint32_t>(ev.xconfigure.width);
            uint32_t nh = static_cast<uint32_t>(ev.xconfigure.height);
            if (nw == inst.w && nh == inst.h)
               continue;
            inst.gui->adjust_size(inst.plug, &nw, &nh);
            if (inst.gui->set_size(inst.plug, nw, nh)) {
               inst.w = nw;
               inst.h = nh;
               XResizeWindow(dpy, inst.window, inst.w, inst.h);
               std::printf("resized %ux%u  %s\n", inst.w, inst.h, inst.title.c_str());
               std::fflush(stdout);
            }
         }
      }
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
   }

   gRunning = false;
   audio.join();
   for (auto &instPtr : instances) {
      Instance &inst = *instPtr;
      inst.plug->stop_processing(inst.plug);
      inst.gui->destroy(inst.plug);
      inst.plug->deactivate(inst.plug);
      inst.plug->destroy(inst.plug);
   }
   entry->deinit();
#if !defined(_WIN32)
   XCloseDisplay(dpy);
#endif
   return 0;
}
