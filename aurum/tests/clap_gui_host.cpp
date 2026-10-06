// Minimal CLAP host for automated GUI tests (run under Xvfb).
// Loads a .clap, embeds its X11 GUI into a top-level window, processes noise
// bursts on an audio thread and services timer/fd callbacks on the main
// thread. Prints parameter events emitted by the plugin.
//
// usage: clap_gui_host <plugin.clap> [seconds] [state-out-file]

#include <X11/Xlib.h>
#include <clap/clap.h>
#include <dlfcn.h>
#include <poll.h>

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

clap_host_t g_host = {CLAP_VERSION_INIT, nullptr, "aurum-test-host", "test", "", "0.1",
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
void stateDirty(const clap_host_t*) {}
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

// Event lists: optionally a MIDI CC ramp (AURUM_TEST_CC=<cc>) between 3 and 4.5 s.
clap_event_midi_t g_midi{};
bool g_hasMidi = false;
uint32_t inSize(const clap_input_events_t*) { return g_hasMidi ? 1 : 0; }
const clap_event_header_t* inGet(const clap_input_events_t*, uint32_t) { return &g_midi.header; }
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
    std::vector<float> inL(N), inR(N), outL(N), outR(N);
    float* inPtr[2] = {inL.data(), inR.data()};
    float* outPtr[2] = {outL.data(), outR.data()};
    clap_audio_buffer_t in{inPtr, nullptr, 2, 0, 0};
    clap_audio_buffer_t out{outPtr, nullptr, 2, 0, 0};
    g_plugin->start_processing(g_plugin);
    uint32_t seed = 1;
    int64_t steady = 0;
    auto t = std::chrono::steady_clock::now();
    while (g_running)
    {
        const bool burst = (steady / 48000) % 3 == 0 && (steady % 48000) < 9600;
        for (uint32_t i = 0; i < N; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const float n = burst ? ((seed >> 9) / 4194304.0f - 1.0f) * 0.25f : 0.0f;
            inL[i] = inR[i] = n;
        }
        static const char* ccEnv = std::getenv("AURUM_TEST_CC");
        g_hasMidi = false;
        if (ccEnv && steady > 3 * 48000 && steady < 4.5 * 48000 && (steady / N) % 8 == 0)
        {
            g_midi.header = {sizeof(g_midi), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            g_midi.port_index = 0;
            g_midi.data[0] = 0xB0;
            g_midi.data[1] = static_cast<uint8_t>(std::atoi(ccEnv));
            g_midi.data[2] = static_cast<uint8_t>((steady - 3 * 48000) * 127 / (1.5 * 48000));
            g_hasMidi = true;
        }
        clap_process_t p{};
        p.steady_time = steady;
        p.frames_count = N;
        p.audio_inputs = &in;
        p.audio_outputs = &out;
        p.audio_inputs_count = 1;
        p.audio_outputs_count = 1;
        p.in_events = &g_inEvents;
        p.out_events = &g_outEvents;
        g_plugin->process(g_plugin, &p);
        for (uint32_t i = 0; i < N; ++i)
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]))
            {
                std::printf("ERROR: non-finite output\n");
                std::fflush(stdout);
            }
        steady += N;
        t += std::chrono::microseconds(N * 1000000 / 48000);
        std::this_thread::sleep_until(t);
    }
    g_plugin->stop_processing(g_plugin);
}

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
        std::fprintf(stderr, "usage: %s plugin.clap [seconds] [state-out]\n", argv[0]);
        return 1;
    }
    const double seconds = argc > 2 ? std::atof(argv[2]) : 5.0;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    entry->init(argv[1]);
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    g_plugin = factory->create_plugin(factory, &g_host, desc->id);
    g_plugin->init(g_plugin);
    g_plugin->activate(g_plugin, 48000.0, 1, 4096);

    auto* gui = static_cast<const clap_plugin_gui_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_GUI));
    auto* timer = static_cast<const clap_plugin_timer_support_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_TIMER_SUPPORT));
    auto* fdx = static_cast<const clap_plugin_posix_fd_support_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_POSIX_FD_SUPPORT));
    auto* params = static_cast<const clap_plugin_params_t*>(g_plugin->get_extension(g_plugin, CLAP_EXT_PARAMS));

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
    XStoreName(dpy, top, "Aurum test host");
    XSelectInput(dpy, top, StructureNotifyMask);
    XMapWindow(dpy, top);
    XFlush(dpy);
    clap_window_t win{CLAP_WINDOW_API_X11, {}};
    win.x11 = top;
    gui->set_parent(g_plugin, &win);
    gui->show(g_plugin);
    std::printf("gui: %ux%u, timers %zu, fds %zu\n", w, h, g_timers.size(), g_fds.size());
    std::fflush(stdout);

    std::thread audio(audioThread);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < end)
    {
        std::vector<pollfd> pfds;
        for (int fd : g_fds)
            pfds.push_back({fd, POLLIN, 0});
        poll(pfds.data(), pfds.size(), 5);
        for (auto& p : pfds)
            if (p.revents & POLLIN)
                fdx->on_fd(g_plugin, p.fd, CLAP_POSIX_FD_READ);
        const auto now = std::chrono::steady_clock::now();
        for (auto& t : g_timers)
            if (now >= t.next)
            {
                t.next = now + std::chrono::milliseconds(t.period);
                timer->on_timer(g_plugin, t.id);
            }
        if (g_resizeRequested.exchange(false))
        {
            XResizeWindow(dpy, top, g_reqW, g_reqH);
            gui->set_size(g_plugin, g_reqW, g_reqH);
            std::printf("resize: %ux%u\n", g_reqW, g_reqH);
        }
        while (XPending(dpy))
        {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }
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
    XDestroyWindow(dpy, top);
    XCloseDisplay(dpy);
    std::printf("done\n");
    return 0;
}
