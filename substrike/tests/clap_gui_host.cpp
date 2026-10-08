// Minimal CLAP host for automated GUI tests (run under Xvfb, or under wine
// for the Windows build). Loads a .clap, embeds its GUI into a top-level
// window, runs the audio thread and services timer/fd callbacks on the main
// thread. Prints the parameter events the plugin emits and, whenever the
// output starts or stops sounding, its level.
//
// usage: substrike-gui-host <plugin.clap> [seconds] [state-out-file] [state-in-file]
//
//   SUBSTRIKE_TEST_NOTES=1   play the root note every two seconds
//   SUBSTRIKE_TEST_DROP=1    (X11) open a second window, "drop target", that
//                            accepts XDND drops and prints what it receives

#include <clap/clap.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <poll.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Timer
{
    clap_id id;
    uint32_t period;
    std::chrono::steady_clock::time_point next;
};

std::vector<Timer> g_timers;
std::vector<int> g_fds;
std::atomic<bool> g_flushRequested{false};
std::atomic<bool> g_running{true};
uint32_t g_reqW = 0, g_reqH = 0;
std::atomic<bool> g_resizeRequested{false};
const clap_plugin_t* g_plugin = nullptr;
std::mutex g_printMutex;

const void* hostGetExtension(const clap_host_t*, const char* id);
void hostRequestRestart(const clap_host_t*) {}
void hostRequestProcess(const clap_host_t*) {}
void hostRequestCallback(const clap_host_t*) {}

clap_host_t g_host = {CLAP_VERSION_INIT, nullptr, "substrike-test-host", "test", "", "0.1",
                      hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback};

// gui
void guiResizeHintsChanged(const clap_host_t*) {}
bool guiRequestResize(const clap_host_t*, uint32_t w, uint32_t h)
{
    g_reqW = w;
    g_reqH = h;
    g_resizeRequested = true;
    return true;
}
bool guiRequestShow(const clap_host_t*) { return true; }
bool guiRequestHide(const clap_host_t*) { return true; }
void guiClosed(const clap_host_t*, bool) {}
const clap_host_gui_t g_hostGui = {guiResizeHintsChanged, guiRequestResize, guiRequestShow, guiRequestHide, guiClosed};

// timers
bool timerRegister(const clap_host_t*, uint32_t period, clap_id* id)
{
    static clap_id next = 1;
    *id = next++;
    g_timers.push_back({*id, period, std::chrono::steady_clock::now()});
    return true;
}
bool timerUnregister(const clap_host_t*, clap_id id)
{
    for (size_t i = 0; i < g_timers.size(); ++i)
        if (g_timers[i].id == id)
        {
            g_timers.erase(g_timers.begin() + static_cast<long>(i));
            return true;
        }
    return false;
}
const clap_host_timer_support_t g_hostTimer = {timerRegister, timerUnregister};

// fds
bool fdRegister(const clap_host_t*, int fd, clap_posix_fd_flags_t)
{
    g_fds.push_back(fd);
    return true;
}
bool fdModify(const clap_host_t*, int, clap_posix_fd_flags_t) { return true; }
bool fdUnregister(const clap_host_t*, int fd)
{
    for (size_t i = 0; i < g_fds.size(); ++i)
        if (g_fds[i] == fd)
        {
            g_fds.erase(g_fds.begin() + static_cast<long>(i));
            return true;
        }
    return false;
}
const clap_host_posix_fd_support_t g_hostFd = {fdRegister, fdModify, fdUnregister};

// params
void paramsRescan(const clap_host_t*, clap_param_rescan_flags) {}
void paramsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void paramsRequestFlush(const clap_host_t*) { g_flushRequested = true; }
const clap_host_params_t g_hostParams = {paramsRescan, paramsClear, paramsRequestFlush};

void tailChanged(const clap_host_t*) {}
const clap_host_tail_t g_hostTail = {tailChanged};
void stateDirty(const clap_host_t*)
{
    std::lock_guard<std::mutex> lock(g_printMutex);
    std::printf("state: dirty\n");
    std::fflush(stdout);
}
const clap_host_state_t g_hostState = {stateDirty};

const void* hostGetExtension(const clap_host_t*, const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &g_hostGui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &g_hostTimer;
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &g_hostFd;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &g_hostParams;
    if (!std::strcmp(id, CLAP_EXT_TAIL))
        return &g_hostTail;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &g_hostState;
    return nullptr;
}

// Event list: optionally a note every two seconds.
clap_event_note_t g_note{};
bool g_hasNote = false;
uint32_t inSize(const clap_input_events_t*) { return g_hasNote ? 1 : 0; }
const clap_event_header_t* inGet(const clap_input_events_t*, uint32_t) { return &g_note.header; }
const clap_input_events_t g_inEvents = {nullptr, inSize, inGet};

bool outPush(const clap_output_events_t*, const clap_event_header_t* ev)
{
    std::lock_guard<std::mutex> lock(g_printMutex);
    if (ev->type == CLAP_EVENT_PARAM_VALUE)
    {
        const auto* v = reinterpret_cast<const clap_event_param_value_t*>(ev);
        std::printf("event: param %u = %.4f\n", v->param_id, v->value);
    }
    else if (ev->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || ev->type == CLAP_EVENT_PARAM_GESTURE_END)
    {
        const auto* g = reinterpret_cast<const clap_event_param_gesture_t*>(ev);
        std::printf("event: gesture %s %u\n", ev->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? "begin" : "end",
                    g->param_id);
    }
    std::fflush(stdout);
    return true;
}
const clap_output_events_t g_outEvents = {nullptr, outPush};

void audioThread()
{
    constexpr uint32_t N = 256;
    std::vector<float> outL(N), outR(N);
    float* outPtr[2] = {outL.data(), outR.data()};
    clap_audio_buffer_t out{outPtr, nullptr, 2, 0, 0};
    g_plugin->start_processing(g_plugin);
    int64_t steady = 0;
    bool sounding = false;
    float peak = 0.0f;
    static const bool notes = std::getenv("SUBSTRIKE_TEST_NOTES") != nullptr;
    auto t = std::chrono::steady_clock::now();
    while (g_running)
    {
        g_hasNote = notes && steady % (2 * 48000) < N;
        if (g_hasNote)
        {
            g_note = {};
            g_note.header = {sizeof(g_note), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
            g_note.note_id = -1;
            g_note.key = 36;
            g_note.velocity = 1.0;
        }
        clap_process_t p{};
        p.steady_time = steady;
        p.frames_count = N;
        p.audio_outputs = &out;
        p.audio_outputs_count = 1;
        p.in_events = &g_inEvents;
        p.out_events = &g_outEvents;
        g_plugin->process(g_plugin, &p);
        float block = 0.0f;
        for (uint32_t i = 0; i < N; ++i)
        {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]))
            {
                std::printf("ERROR: non-finite output\n");
                std::fflush(stdout);
            }
            block = std::max({block, std::fabs(outL[i]), std::fabs(outR[i])});
        }
        if (block > 1e-4f)
        {
            if (!sounding)
            {
                std::lock_guard<std::mutex> lock(g_printMutex);
                std::printf("audio: sounding\n");
                std::fflush(stdout);
            }
            sounding = true;
            peak = std::max(peak, block);
        }
        else if (sounding)
        {
            sounding = false;
            std::lock_guard<std::mutex> lock(g_printMutex);
            std::printf("audio: silent, peak %.3f\n", peak);
            std::fflush(stdout);
            peak = 0.0f;
        }
        steady += N;
        t += std::chrono::microseconds(N * 1000000 / 48000);
        std::this_thread::sleep_until(t);
    }
    g_plugin->stop_processing(g_plugin);
}

int64_t readStream(const clap_istream_t* s, void* buf, uint64_t size)
{
    return static_cast<int64_t>(std::fread(buf, 1, size, static_cast<FILE*>(s->ctx)));
}

#if !defined(_WIN32)
// A bare XDND target: says yes to every position and fetches text/uri-list
// on the drop.
struct DropTarget
{
    Display* dpy = nullptr;
    Window win = 0;
    Atom aware, enter, position, status, drop, finished, selection, uriList, copy, prop;

    void open(Display* d)
    {
        dpy = d;
        win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 300, 200, 0, 0, 0x202020);
        XStoreName(dpy, win, "drop target");
        aware = XInternAtom(dpy, "XdndAware", False);
        enter = XInternAtom(dpy, "XdndEnter", False);
        position = XInternAtom(dpy, "XdndPosition", False);
        status = XInternAtom(dpy, "XdndStatus", False);
        drop = XInternAtom(dpy, "XdndDrop", False);
        finished = XInternAtom(dpy, "XdndFinished", False);
        selection = XInternAtom(dpy, "XdndSelection", False);
        uriList = XInternAtom(dpy, "text/uri-list", False);
        copy = XInternAtom(dpy, "XdndActionCopy", False);
        prop = XInternAtom(dpy, "DROP_TEST", False);
        const long version = 5;
        XChangeProperty(dpy, win, aware, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&version),
                        1);
        XMapWindow(dpy, win);
        XFlush(dpy);
    }

    void reply(Window source, Atom type, long l1, long l2)
    {
        XEvent e{};
        e.xclient.type = ClientMessage;
        e.xclient.display = dpy;
        e.xclient.window = source;
        e.xclient.message_type = type;
        e.xclient.format = 32;
        e.xclient.data.l[0] = static_cast<long>(win);
        e.xclient.data.l[1] = l1;
        e.xclient.data.l[2] = l2;
        e.xclient.data.l[4] = static_cast<long>(copy);
        XSendEvent(dpy, source, False, NoEventMask, &e);
        XFlush(dpy);
    }

    Window source = 0;
    void handle(XEvent& ev)
    {
        if (ev.type == ClientMessage && ev.xclient.window == win)
        {
            const Atom t = ev.xclient.message_type;
            if (t == enter)
            {
                std::printf("drop: enter\n");
                std::fflush(stdout);
            }
            else if (t == position)
                reply(static_cast<Window>(ev.xclient.data.l[0]), status, 1, 0);
            else if (t == drop)
            {
                source = static_cast<Window>(ev.xclient.data.l[0]);
                XConvertSelection(dpy, selection, uriList, prop, win, static_cast<Time>(ev.xclient.data.l[2]));
                XFlush(dpy);
            }
        }
        else if (ev.type == SelectionNotify && ev.xselection.requestor == win)
        {
            Atom type;
            int format;
            unsigned long count, remaining;
            unsigned char* data = nullptr;
            if (ev.xselection.property != None &&
                XGetWindowProperty(dpy, win, prop, 0, 65536, True, AnyPropertyType, &type, &format, &count, &remaining,
                                   &data) == Success &&
                data)
            {
                std::printf("drop: %.*s\n", static_cast<int>(count), reinterpret_cast<char*>(data));
                XFree(data);
            }
            else
                std::printf("drop: no data\n");
            std::fflush(stdout);
            reply(source, finished, 1, static_cast<long>(copy));
        }
    }
};
#endif

int64_t writeStream(const clap_ostream_t* s, const void* buf, uint64_t size)
{
    std::fwrite(buf, 1, size, static_cast<FILE*>(s->ctx));
    return static_cast<int64_t>(size);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s plugin.clap [seconds] [state-out] [state-in]\n", argv[0]);
        return 1;
    }
    const double seconds = argc > 2 ? std::atof(argv[2]) : 5.0;
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib)
    {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(lib, "clap_entry"));
#else
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
#endif
    if (!entry)
    {
        std::fprintf(stderr, "no clap_entry\n");
        return 1;
    }
    entry->init(argv[1]);
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    g_plugin = factory->create_plugin(factory, &g_host, desc->id);
    g_plugin->init(g_plugin);
    if (argc > 4 && argv[4][0])
    {
        auto* state = static_cast<const clap_plugin_state_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_STATE));
        if (FILE* f = std::fopen(argv[4], "rb"))
        {
            clap_istream_t is{f, readStream};
            std::printf("state: load %s\n", state->load(g_plugin, &is) ? "ok" : "FAILED");
            std::fclose(f);
        }
    }
    g_plugin->activate(g_plugin, 48000.0, 1, 4096);

    auto* gui = static_cast<const clap_plugin_gui_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_GUI));
    auto* timer = static_cast<const clap_plugin_timer_support_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_TIMER_SUPPORT));
    auto* fdx = static_cast<const clap_plugin_posix_fd_support_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_POSIX_FD_SUPPORT));
    auto* params = static_cast<const clap_plugin_params_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_PARAMS));

#if defined(_WIN32)
    if (!gui)
    {
        std::fprintf(stderr, "no gui\n");
        return 1;
    }
    gui->create(g_plugin, CLAP_WINDOW_API_WIN32, false);
    gui->set_scale(g_plugin, 1.0);
    uint32_t w = 0, h = 0;
    gui->get_size(g_plugin, &w, &h);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SubstrikeTestHost";
    RegisterClassW(&wc);
    RECT frame{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    HWND top = CreateWindowW(L"SubstrikeTestHost", L"Substrike test host", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
                             0, 0, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
                             wc.hInstance, nullptr);
    clap_window_t win{CLAP_WINDOW_API_WIN32, {}};
    win.win32 = top;
#else
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy || !gui)
    {
        std::fprintf(stderr, "no display or no gui\n");
        return 1;
    }
    gui->create(g_plugin, CLAP_WINDOW_API_X11, false);
    gui->set_scale(g_plugin, 1.0);
    uint32_t w = 0, h = 0;
    gui->get_size(g_plugin, &w, &h);
    Window top = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, w, h, 0, 0, 0);
    XStoreName(dpy, top, "Substrike test host");
    XSelectInput(dpy, top, StructureNotifyMask);
    XMapWindow(dpy, top);
    XFlush(dpy);
    clap_window_t win{CLAP_WINDOW_API_X11, {}};
    win.x11 = top;
    DropTarget dropTarget;
    if (std::getenv("SUBSTRIKE_TEST_DROP"))
    {
        dropTarget.open(dpy);
        XMoveWindow(dpy, dropTarget.win, static_cast<int>(w) + 20, 0);
        XSelectInput(dpy, dropTarget.win, StructureNotifyMask);
    }
#endif
    gui->set_parent(g_plugin, &win);
    gui->show(g_plugin);
    std::printf("gui: %ux%u, timers %zu, fds %zu\n", w, h, g_timers.size(), g_fds.size());
    std::fflush(stdout);

    std::thread audio(audioThread);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < end)
    {
#if defined(_WIN32)
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        (void)fdx;
#else
        std::vector<pollfd> pfds;
        for (int fd : g_fds)
            pfds.push_back({fd, POLLIN, 0});
        poll(pfds.data(), pfds.size(), 5);
        for (auto& p : pfds)
            if (p.revents & POLLIN)
                fdx->on_fd(g_plugin, p.fd, CLAP_POSIX_FD_READ);
#endif
        const auto now = std::chrono::steady_clock::now();
        for (auto& t : g_timers)
            if (now >= t.next)
            {
                t.next = now + std::chrono::milliseconds(t.period);
                timer->on_timer(g_plugin, t.id);
            }
        if (g_resizeRequested.exchange(false))
        {
#if defined(_WIN32)
            RECT r{0, 0, static_cast<LONG>(g_reqW), static_cast<LONG>(g_reqH)};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(top, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
#else
            XResizeWindow(dpy, top, g_reqW, g_reqH);
#endif
            gui->set_size(g_plugin, g_reqW, g_reqH);
            std::printf("resize: %ux%u\n", g_reqW, g_reqH);
        }
#if !defined(_WIN32)
        while (XPending(dpy))
        {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (dropTarget.win)
                dropTarget.handle(ev);
        }
#endif
    }
    g_running = false;
    audio.join();

    if (argc > 3)
    {
        auto* state = static_cast<const clap_plugin_state_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_STATE));
        FILE* f = std::fopen(argv[3], "wb");
        clap_ostream_t os{f, writeStream};
        state->save(g_plugin, &os);
        std::fclose(f);
    }
    (void)params;
    gui->destroy(g_plugin);
    g_plugin->deactivate(g_plugin);
    g_plugin->destroy(g_plugin);
    entry->deinit();
#if defined(_WIN32)
    DestroyWindow(top);
#else
    XDestroyWindow(dpy, top);
    XCloseDisplay(dpy);
#endif
    std::printf("done\n");
    return 0;
}
