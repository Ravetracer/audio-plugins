// Opens ThreeOhThree's editor in a plain X11 window so the interface can be driven
// and photographed without a DAW. The plugin's GUI is embedded (non-floating),
// so a host has to supply the parent window; that is all this does.
//
//   ./threeohthree-guihost [plugin.clap] [preset.threeohthree] [seconds]
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
#include <string>
#include <thread>
#include <vector>

namespace {

const clap_host_t *gHostPtr = nullptr;
const clap_plugin_t *gPlugin = nullptr;
std::atomic<bool> gRunning{true};

// The plugin asks to be resized when its collapsible panel section opens or
// closes. A DAW obliges by resizing the window it parented the editor into;
// this does the same, on the event loop rather than on the calling thread,
// because that is where the X connection is.
std::atomic<bool> gResizeWanted{false};
std::atomic<uint32_t> gResizeW{0};
std::atomic<uint32_t> gResizeH{0};

void hostGuiResizeHintsChanged(const clap_host_t *) {}

bool hostGuiRequestResize(const clap_host_t *, uint32_t width, uint32_t height) {
   gResizeW.store(width);
   gResizeH.store(height);
   gResizeWanted.store(true);
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
   const std::string pluginPath = argc > 1 ? argv[1] : "./ThreeOhThree.clap";
   const char *presetPath = (argc > 2 && argv[2][0]) ? argv[2] : nullptr;
   const int liveSeconds = argc > 3 ? std::atoi(argv[3]) : 600;

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

   clap_host_t host{};
   host.clap_version = CLAP_VERSION;
   host.name = "threeohthree-guihost";
   host.vendor = "ThreeOhThree";
   host.url = "";
   host.version = "1.0";
   host.get_extension = hostGetExtension;
   host.request_restart = hostRequestRestart;
   host.request_process = hostRequestProcess;
   host.request_callback = hostRequestCallback;
   gHostPtr = &host;

   const clap_plugin_t *plug = fac->create_plugin(fac, &host, desc->id);
   if (!plug || !plug->init(plug)) {
      std::fprintf(stderr, "plugin init failed\n");
      return 1;
   }
   gPlugin = plug;

   const double sr = 48000.0;
   const uint32_t block = 512;
   plug->activate(plug, sr, block, block);
   plug->start_processing(plug);

   if (presetPath) {
      auto *pl = static_cast<const clap_plugin_preset_load_t *>(
         plug->get_extension(plug, CLAP_EXT_PRESET_LOAD));
      if (pl && !pl->from_location(plug, CLAP_PRESET_DISCOVERY_LOCATION_FILE, presetPath, ""))
         std::fprintf(stderr, "preset load failed: %s\n", presetPath);
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

   uint32_t w = 900, h = 648;
   gui->get_size(plug, &w, &h);

#if defined(_WIN32)
   // A plain top-level window standing in for the host's, with the plugin's
   // own window parented into it exactly as a DAW would.
   WNDCLASSEXW wc{};
   wc.cbSize = sizeof(wc);
   wc.lpfnWndProc = DefWindowProcW;
   wc.hInstance = GetModuleHandleW(nullptr);
   wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
   wc.lpszClassName = L"ThreeOhThreeGuiHost";
   RegisterClassExW(&wc);

   RECT wanted{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
   AdjustWindowRect(&wanted, WS_OVERLAPPEDWINDOW, FALSE);
   HWND hostWindow = CreateWindowExW(0, L"ThreeOhThreeGuiHost", L"ThreeOhThree", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, wanted.right - wanted.left,
                               wanted.bottom - wanted.top, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
   if (!hostWindow) {
      std::fprintf(stderr, "could not create the host window\n");
      return 1;
   }
   ShowWindow(hostWindow, SW_SHOW);

   clap_window_t parent{};
   parent.api = CLAP_WINDOW_API_WIN32;
   parent.win32 = hostWindow;
   if (!gui->set_parent(plug, &parent)) {
      std::fprintf(stderr, "set_parent failed\n");
      return 1;
   }
   gui->show(plug);
   std::printf("window %p  %ux%u\n", static_cast<void *>(hostWindow), w, h);
   std::fflush(stdout);
#else
   Display *dpy = XOpenDisplay(nullptr);
   if (!dpy) {
      std::fprintf(stderr, "no X display\n");
      return 1;
   }
   const int screen = DefaultScreen(dpy);
   Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, w, h, 0,
                                    BlackPixel(dpy, screen), BlackPixel(dpy, screen));
   XStoreName(dpy, win, "ThreeOhThree");
   XSelectInput(dpy, win, StructureNotifyMask);
   XMapWindow(dpy, win);
   XFlush(dpy);

   clap_window_t parent{};
   parent.api = RD_WINDOW_API;
   parent.x11 = static_cast<clap_xwnd>(win);
   if (!gui->set_parent(plug, &parent)) {
      std::fprintf(stderr, "set_parent failed\n");
      return 1;
   }
   gui->show(plug);
   XFlush(dpy);
   std::printf("window 0x%lx  %ux%u\n", win, w, h);
   std::fflush(stdout);
#endif

   // The editor publishes its parameter edits through process(), so the audio
   // thread has to keep turning for the interface to behave as it does in a host.
   std::thread audio([&] {
      std::vector<float> l(block), r(block);
      float *chans[2] = {l.data(), r.data()};
      clap_audio_buffer_t ab{};
      ab.data32 = chans;
      ab.channel_count = 2;
      clap_input_events_t in{};
      in.size = [](const clap_input_events_t *) -> uint32_t { return 0; };
      in.get = [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * {
         return nullptr;
      };
      clap_output_events_t out{};
      out.try_push = [](const clap_output_events_t *, const clap_event_header_t *) { return true; };
      while (gRunning.load()) {
         clap_process_t pr{};
         pr.frames_count = block;
         pr.audio_outputs = &ab;
         pr.audio_outputs_count = 1;
         pr.in_events = &in;
         pr.out_events = &out;
         plug->process(plug, &pr);
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
   });

   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(liveSeconds);
   while (std::chrono::steady_clock::now() < deadline) {
#if defined(_WIN32)
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
         TranslateMessage(&msg);
         DispatchMessageW(&msg);
      }
      // The plugin's window is repainted from the host's timer in a DAW; here
      // there is no timer, so the host drives it.
      if (auto *timer = static_cast<const clap_plugin_timer_support_t *>(
             plug->get_extension(plug, CLAP_EXT_TIMER_SUPPORT)))
         timer->on_timer(plug, 0);
#else
      // A resize the plugin asked for, taken the way a DAW takes it: the host
      // window follows the editor rather than the other way round.
      if (gResizeWanted.exchange(false)) {
         const uint32_t nw = gResizeW.load();
         const uint32_t nh = gResizeH.load();
         if (nw && nh && (nw != w || nh != h)) {
            w = nw;
            h = nh;
            XResizeWindow(dpy, win, w, h);
            XFlush(dpy);
            std::printf("host resize %ux%u\n", w, h);
            std::fflush(stdout);
         }
      }
      while (XPending(dpy)) {
         XEvent ev;
         XNextEvent(dpy, &ev);
         // The user resized the host window: offer the new size to the plugin
         // the way a DAW does -- adjust_size to snap it, set_size to take it --
         // and follow the plugin to the size it settled on.
         if (ev.type == ConfigureNotify && gui->can_resize(plug)) {
            uint32_t nw = static_cast<uint32_t>(ev.xconfigure.width);
            uint32_t nh = static_cast<uint32_t>(ev.xconfigure.height);
            if (nw != w || nh != h) {
               gui->adjust_size(plug, &nw, &nh);
               if (gui->set_size(plug, nw, nh)) {
                  w = nw;
                  h = nh;
                  XResizeWindow(dpy, win, w, h);
                  std::printf("resized %ux%u\n", w, h);
                  std::fflush(stdout);
               }
            }
         }
      }
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
   }

   gRunning = false;
   audio.join();
   plug->stop_processing(plug);
   gui->destroy(plug);
   plug->deactivate(plug);
   plug->destroy(plug);
   entry->deinit();
#if !defined(_WIN32)
   XCloseDisplay(dpy);
#endif
   return 0;
}
