// sidmidi-selftest: the built plugin, loaded the way a host loads it.
//
//   sidmidi-selftest SidMidi.clap PROGRAM [seconds-until-ready] [rom-folder] [true1541]
//
// The ROM folder (default: the plugin's own) is handed over as SIDMIDI_ROM_DIR.
//
// 1. Creates an instance, loads PROGRAM through the plugin's own loader (as
//    the Load button or a drop would), lets it start, plays notes through the
//    note port, and checks that sound came out.
// 2. Saves the state, loads it into a second instance, and runs both on: they
//    must produce the same audio sample for sample -- the snapshot puts the
//    machine back exactly where it was.
// 3. Checks the parameters are listed and answer.
//
// Exit code 0 when everything passed.

#include <clap/clap.h>
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "plugin/SidMidiPlugin.h" // the loader extension only

namespace {

int failures = 0;
void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostNoop(const clap_host_t*) {}
clap_host_t host = {CLAP_VERSION_INIT, nullptr, "sidmidi-selftest", "Ravetracer", "", "0.1",
                    hostGetExtension, hostNoop, hostNoop, hostNoop};

struct EventList
{
    std::vector<clap_event_midi_t> events;
    static uint32_t size(const clap_input_events_t* l) { return static_cast<uint32_t>(static_cast<EventList*>(l->ctx)->events.size()); }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i)
    {
        return &static_cast<EventList*>(l->ctx)->events[i].header;
    }
};

struct OutList
{
    std::vector<uint8_t> midi;
    static bool push(const clap_output_events_t* l, const clap_event_header_t* h)
    {
        if (h->type == CLAP_EVENT_MIDI) {
            const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
            static_cast<OutList*>(l->ctx)->midi.push_back(m->data[0]);
        }
        return true;
    }
};

struct Instance
{
    const clap_plugin_t* plugin = nullptr;
    std::vector<float> l, r;
    std::vector<std::vector<float>> aux;
    OutList out;

    void create(const clap_plugin_factory_t* f)
    {
        plugin = f->create_plugin(f, &host, "de.ravetracer.sidmidi");
        plugin->init(plugin);
        // Offline: warp by a fixed amount, so every run is the same run.
        if (auto* render = static_cast<const clap_plugin_render_t*>(plugin->get_extension(plugin, CLAP_EXT_RENDER)))
            render->set(plugin, CLAP_RENDER_OFFLINE);
        plugin->activate(plugin, 48000.0, 1, 512);
        plugin->start_processing(plugin);
        l.resize(512);
        r.resize(512);
        aux.assign(8, std::vector<float>(512));
    }
    // Runs `blocks` of 512 frames; `notes` adds MIDI at given block numbers.
    std::vector<float> run(int blocks, const std::vector<std::pair<int, std::vector<uint8_t>>>& midi = {})
    {
        std::vector<float> all;
        for (int b = 0; b < blocks; ++b) {
            EventList ev;
            for (const auto& m : midi) {
                if (m.first != b)
                    continue;
                clap_event_midi_t e{};
                e.header = {sizeof(e), 100, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
                for (size_t k = 0; k < 3 && k < m.second.size(); ++k)
                    e.data[k] = m.second[k];
                ev.events.push_back(e);
            }
            clap_input_events_t in{&ev, EventList::size, EventList::get};
            clap_output_events_t outEv{&out, OutList::push};
            float* main[2] = {l.data(), r.data()};
            clap_audio_buffer_t bufs[9]{};
            bufs[0].data32 = main;
            bufs[0].channel_count = 2;
            float* auxPtr[8];
            for (int i = 0; i < 8; ++i) {
                auxPtr[i] = aux[static_cast<size_t>(i)].data();
                bufs[i + 1].data32 = &auxPtr[i];
                bufs[i + 1].channel_count = 1;
            }
            clap_process_t p{};
            p.frames_count = 512;
            p.audio_outputs = bufs;
            p.audio_outputs_count = 9;
            p.in_events = &in;
            p.out_events = &outEv;
            p.steady_time = -1;
            plugin->process(plugin, &p);
            all.insert(all.end(), l.begin(), l.end());
        }
        return all;
    }
    void destroy()
    {
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
};

double rmsDb(const std::vector<float>& v)
{
    double mean = 0;
    for (float x : v)
        mean += x;
    mean /= std::max<size_t>(1, v.size());
    double s = 0;
    for (float x : v)
        s += (x - mean) * (x - mean);
    return 10.0 * std::log10(s / std::max<size_t>(1, v.size()) + 1e-20);
}

struct Buffer
{
    std::vector<uint8_t> data;
    size_t at = 0;
    static int64_t write(const clap_ostream_t* s, const void* p, uint64_t n)
    {
        auto* b = static_cast<Buffer*>(s->ctx);
        b->data.insert(b->data.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
        return static_cast<int64_t>(n);
    }
    static int64_t read(const clap_istream_t* s, void* p, uint64_t n)
    {
        auto* b = static_cast<Buffer*>(s->ctx);
        const uint64_t k = std::min<uint64_t>(n, b->data.size() - b->at);
        std::memcpy(p, b->data.data() + b->at, k);
        b->at += k;
        return static_cast<int64_t>(k);
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: sidmidi-selftest SidMidi.clap PROGRAM [seconds-until-ready]\n");
        return 2;
    }
    const double ready = argc > 3 ? std::atof(argv[3]) : 3.0;
    if (argc > 4)
        setenv("SIDMIDI_ROM_DIR", argv[4], 1);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    check(entry && entry->init(argv[1]), "clap_entry found and initialised");
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(factory && factory->get_plugin_count(factory) == 1, "one plugin in the factory");

    Instance a;
    a.create(factory);
    auto* params = static_cast<const clap_plugin_params_t*>(a.plugin->get_extension(a.plugin, CLAP_EXT_PARAMS));
    check(params && params->count(a.plugin) == 17, "17 parameters");
    double v = -1;
    check(params && params->get_value(a.plugin, 1, &v) && v == 0.0, "volume answers 0 dB");

    // Load the program through the plugin itself.
    if (argc > 5 && std::string(argv[5]) == "true1541") {
        // Switch to the true drive through a state whose settings say so
        // ("drive_mode = 0" and "= 1" have the same length).
        auto* st = static_cast<const clap_plugin_state_t*>(a.plugin->get_extension(a.plugin, CLAP_EXT_STATE));
        Buffer b0;
        clap_ostream_t o0{&b0, Buffer::write};
        st->save(a.plugin, &o0);
        const std::string from = "drive_mode = 0", to = "drive_mode = 1";
        auto it = std::search(b0.data.begin(), b0.data.end(), from.begin(), from.end());
        check(it != b0.data.end(), "settings carry the drive mode");
        if (it != b0.data.end())
            std::copy(to.begin(), to.end(), it);
        clap_istream_t i0{&b0, Buffer::read};
        check(st->load(a.plugin, &i0), "switched to the true 1541");
    }
    auto* loader = static_cast<const sidmidi::ExtLoader*>(a.plugin->get_extension(a.plugin, sidmidi::kExtLoader));
    check(loader && loader->load_file(a.plugin, argv[2]), (std::string("program loads: ") + argv[2]).c_str());

    const int readyBlocks = static_cast<int>(ready * 48000 / 512) + 200; // + the warp
    a.run(readyBlocks);
    const auto silent = a.run(40);
    const auto note = a.run(80, {{0, {0x90, 60, 100}}, {60, {0x80, 60, 0}}});
    std::printf("      quiet %.1f dB, note %.1f dB\n", rmsDb(silent), rmsDb(note));
    check(rmsDb(note) > -50.0 && rmsDb(note) > rmsDb(silent) + 20.0, "a MIDI note makes the C64 play");

    // State round trip.
    auto* state = static_cast<const clap_plugin_state_t*>(a.plugin->get_extension(a.plugin, CLAP_EXT_STATE));
    Buffer buf;
    clap_ostream_t os{&buf, Buffer::write};
    check(state && state->save(a.plugin, &os), "state saves");
    std::printf("      state: %zu bytes\n", buf.data.size());
    Instance b;
    b.create(factory);
    clap_istream_t is{&buf, Buffer::read};
    auto* stateB = static_cast<const clap_plugin_state_t*>(b.plugin->get_extension(b.plugin, CLAP_EXT_STATE));
    check(stateB && stateB->load(b.plugin, &is), "state loads into a second instance");
    const std::vector<std::pair<int, std::vector<uint8_t>>> phrase = {
        {2, {0x90, 64, 110}}, {30, {0x80, 64, 0}}, {35, {0x90, 67, 90}}, {70, {0x80, 67, 0}}};
    const auto outA = a.run(100, phrase);
    const auto outB = b.run(100, phrase);
    double maxDiff = 0;
    for (size_t i = 0; i < outA.size(); ++i)
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(outA[i] - outB[i])));
    std::printf("      after the round trip: max difference %.3g, level %.1f dB\n", maxDiff, rmsDb(outB));
    for (size_t seg = 0; maxDiff >= 1e-3 && seg < outA.size(); seg += outA.size() / 10) {
        double d = 0;
        for (size_t i = seg; i < std::min(outA.size(), seg + outA.size() / 10); ++i)
            d = std::max(d, static_cast<double>(std::fabs(outA[i] - outB[i])));
        std::printf("        from sample %6zu: %.3g\n", seg, d);
    }
    // reSIDfp restores its chips to within a sample's last bit or two (it
    // recomputes a cached filter value rather than storing it): -60 dB is the
    // bar, far below hearing and far above what a wrong restore produces.
    check(maxDiff < 1e-3 && rmsDb(outB) > -50.0, "the restored machine plays on like the original (within -60 dB)");

    a.destroy();
    b.destroy();
    entry->deinit();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
