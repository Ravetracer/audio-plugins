// resident-selftest: the built plugin, loaded the way a host loads it.
//
//   resident-selftest ReSIDent.clap PROGRAM [seconds-until-ready] [rom-folder] [true1541|lite]
//
// The ROM folder (default: the plugin's own) is handed over as RESIDENT_ROM_DIR.
//
// 1. Creates an instance, loads PROGRAM through the plugin's own loader (as
//    the Load button or a drop would), lets it start, plays notes through the
//    note port, and checks that sound came out.
// 2. Saves the state, loads it into a second instance, and runs both on: they
//    must produce the same audio, bit for bit -- the snapshot puts the
//    machine back exactly where it was.
// 3. Checks the parameters are listed and answer.
// 4. CC maps: a map in a scratch map folder that names the program is picked
//    when it loads; the slots carry its names and value labels; a slot that
//    moves reaches the machine as one message, an unchanged or unused one as
//    none; the map travels with the state.
// 5. Warp: a start warps on its own, the Warp button stops it (and only that
//    one), and the Auto warp setting switches it off.
// 6. Voice outputs: switched on, the main output stays bit-identical, the
//    three voices of SID 1 together come close to SID 1's own output, nothing
//    allocates, and the voices survive a state round trip exactly. Off, the
//    voice ports are silent.
// 7. The SID engine: switched while a note plays (to the other engine and
//    back), the sound goes on and nothing allocates; a project from before
//    the engine setting loads on reSIDfp (exactly, from a reSIDfp project;
//    from a Light one the chips start from their registers and play).
// 8. Counts heap allocations inside process() once the program is running --
//    this executable's operator new replaces the one the plugin's libstdc++
//    would use -- through notes, and while a flood
//    of MIDI, more than the cable queue holds, goes in; the count must stay
//    zero and the program must still play afterwards.
//
// With `lite` every instance runs on the Light engine (SIDLite), switched the
// way the window switches it; on Light the 6581 filter range must change
// nothing.
//
// Exit code 0 when everything passed.
//
//   resident-selftest ReSIDent.clap --media CARTRIDGE DISK START-DISK [rom-folder]
//
// The media: a cartridge with a disk inserted beside it (the cartridge runs
// on untouched, both are in the state), and a disk inserted alone (nothing
// starts) and then started from (its first file runs).

#include <clap/clap.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h> // mkdtemp on macOS

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "plugin/ResidentPlugin.h" // the loader extension only

// Allocation counter: everything that reaches operator new while `counting`
// is set, i.e. inside the plugin's process().
namespace {
bool counting = false;
size_t allocations = 0;
bool lite = false; // every instance on the Light engine
}

void* operator new(std::size_t n)
{
    if (counting)
        ++allocations;
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
clap_host_t host = {CLAP_VERSION_INIT, nullptr, "resident-selftest", "Ravetracer", "", "0.1",
                    hostGetExtension, hostNoop, hostNoop, hostNoop};

struct EventList
{
    std::vector<clap_event_param_value_t> params; // at time 0, before the MIDI
    std::vector<clap_event_midi_t> events;
    static uint32_t size(const clap_input_events_t* l)
    {
        const auto* e = static_cast<EventList*>(l->ctx);
        return static_cast<uint32_t>(e->params.size() + e->events.size());
    }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i)
    {
        auto* e = static_cast<EventList*>(l->ctx);
        if (i < e->params.size())
            return &e->params[i].header;
        return &e->events[i - e->params.size()].header;
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
    std::vector<std::vector<float>> aux;          // SID 1-8, then SID 1 voice 1 ... SID 8 voice 3
    std::vector<std::vector<float>> auxAll;       // everything the last run() wrote to them
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
        if (lite)
            setEngine(1);
        l.resize(512);
        r.resize(512);
        aux.assign(8 + 24, std::vector<float>(512));
    }
    // Runs `blocks` of 512 frames; `notes` adds MIDI at given block numbers,
    // `params` parameter values (id, value) in the first block.
    std::vector<float> run(int blocks, const std::vector<std::pair<int, std::vector<uint8_t>>>& midi = {},
                           const std::vector<std::pair<clap_id, double>>& params = {})
    {
        std::vector<float> all;
        auxAll.assign(aux.size(), {});
        for (int b = 0; b < blocks; ++b) {
            EventList ev;
            for (const auto& pv : params) {
                if (b != 0)
                    break;
                clap_event_param_value_t e{};
                e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                e.param_id = pv.first;
                e.note_id = -1;
                e.port_index = e.channel = e.key = -1;
                e.value = pv.second;
                ev.params.push_back(e);
            }
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
            clap_audio_buffer_t bufs[1 + 8 + 24]{};
            bufs[0].data32 = main;
            bufs[0].channel_count = 2;
            float* auxPtr[8 + 24];
            for (int i = 0; i < 8 + 24; ++i) {
                auxPtr[i] = aux[static_cast<size_t>(i)].data();
                bufs[i + 1].data32 = &auxPtr[i];
                bufs[i + 1].channel_count = 1;
            }
            clap_process_t p{};
            p.frames_count = 512;
            p.audio_outputs = bufs;
            p.audio_outputs_count = 1 + 8 + 24;
            p.in_events = &in;
            p.out_events = &outEv;
            p.steady_time = -1;
            counting = true;
            plugin->process(plugin, &p);
            counting = false;
            all.insert(all.end(), l.begin(), l.end());
            for (size_t i = 0; i < aux.size(); ++i)
                auxAll[i].insert(auxAll[i].end(), aux[i].begin(), aux[i].end());
        }
        return all;
    }
    void setEngine(int engine)
    {
        auto* lo = static_cast<const resident::ExtLoader*>(plugin->get_extension(plugin, resident::kExtLoader));
        lo->set_sid_engine(plugin, engine);
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

int mediaTest(const clap_plugin_factory_t* factory, const char* cartPath, const char* diskPath, const char* startDisk)
{
    using resident::ExtLoader;
    auto loaderOf = [](const Instance& i) {
        return static_cast<const ExtLoader*>(i.plugin->get_extension(i.plugin, resident::kExtLoader));
    };
    const std::vector<std::pair<int, std::vector<uint8_t>>> note = {{2, {0x90, 64, 110}}, {50, {0x80, 64, 0}}};

    // X and Y run the cartridge the same way; X gets a disk halfway.
    Instance x, y;
    x.create(factory);
    y.create(factory);
    const ExtLoader* lx = loaderOf(x);
    const ExtLoader* ly = loaderOf(y);
    check(lx && lx->load_file(x.plugin, cartPath) && ly->load_file(y.plugin, cartPath),
          (std::string("cartridge loads: ") + cartPath).c_str());
    x.run(400);
    y.run(400);
    check(lx->attach_disk(x.plugin, diskPath), (std::string("disk inserts: ") + diskPath).c_str());
    check(lx->has_cartridge(x.plugin), "inserting a disk leaves the cartridge in");
    const int files = lx->disk_files(x.plugin);
    std::printf("      the disk lists %d files\n", files);
    check(files > 0, "the inserted disk's directory reads");
    const auto outX = x.run(80, note);
    const auto outY = y.run(80, note);
    double diff = 0;
    for (size_t i = 0; i < outX.size(); ++i)
        diff = std::max(diff, static_cast<double>(std::fabs(outX[i] - outY[i])));
    std::printf("      the cartridge plays at %.1f dB; with the disk inserted, max difference %.3g\n", rmsDb(outX), diff);
    check(rmsDb(outX) > -50.0 && diff == 0.0, "the cartridge runs on untouched by the disk");

    // Both travel with the project.
    auto* st = static_cast<const clap_plugin_state_t*>(x.plugin->get_extension(x.plugin, CLAP_EXT_STATE));
    Buffer buf;
    clap_ostream_t os{&buf, Buffer::write};
    check(st && st->save(x.plugin, &os), "the state saves");
    Instance z;
    z.create(factory);
    clap_istream_t is{&buf, Buffer::read};
    auto* stZ = static_cast<const clap_plugin_state_t*>(z.plugin->get_extension(z.plugin, CLAP_EXT_STATE));
    check(stZ && stZ->load(z.plugin, &is), "the state loads");
    const ExtLoader* lz = loaderOf(z);
    check(lz->has_cartridge(z.plugin) && lz->disk_files(z.plugin) == files,
          "the project keeps the cartridge and the disk");
    check(rmsDb(z.run(80, note)) > -50.0, "the restored cartridge plays");

    // A disk inserted alone starts nothing; Start from disk runs its first file.
    Instance w;
    w.create(factory);
    const ExtLoader* lw = loaderOf(w);
    w.run(300); // to READY
    check(lw->attach_disk(w.plugin, startDisk), (std::string("disk inserts: ") + startDisk).c_str());
    w.run(1500);
    const double idle = rmsDb(w.run(80, note));
    check(lw->start_from_disk(w.plugin, "*"), "Start from disk");
    w.run(1600);
    const double started = rmsDb(w.run(80, note));
    std::printf("      a note with the disk only inserted: %.1f dB; after starting its first file: %.1f dB\n", idle,
                started);
    check(idle < -60.0, "an inserted disk starts nothing");
    check(started > -50.0, "Start from disk runs the program");

    for (Instance* i : {&x, &y, &z, &w})
        i->destroy();
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: resident-selftest ReSIDent.clap PROGRAM [seconds-until-ready]\n");
        return 2;
    }
    const bool media = std::string(argv[2]) == "--media";
    const double ready = argc > 3 && !media ? std::atof(argv[3]) : 3.0;
    const int romArg = media ? 6 : 4;
    lite = !media && argc > 5 && std::string(argv[5]) == "lite";
    if (argc > romArg)
        setenv("RESIDENT_ROM_DIR", argv[romArg], 1);
    // A CC map for the program under test, in a scratch folder (this runs in
    // the build container, whose /tmp goes with it).
    std::string program = argv[2];
    program = program.substr(program.find_last_of('/') + 1);
    program = program.substr(0, program.find('.'));
    char mapDir[] = "/tmp/resident-selftest-XXXXXX";
    if (mkdtemp(mapDir)) {
        setenv("RESIDENT_CCMAP_DIR", mapDir, 1);
        const std::string map = "name = Selftest\nprograms = " + program +
                                "\nchannel = 3\n1 = 74 Cutoff | 0 Closed, 64 Open\n2 = 7@2 Volume\n";
        if (FILE* f = std::fopen((std::string(mapDir) + "/selftest.ccmap").c_str(), "w")) {
            std::fputs(map.c_str(), f);
            std::fclose(f);
        }
    }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    check(entry && entry->init(argv[1]), "clap_entry found and initialised");
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(factory && factory->get_plugin_count(factory) == 1, "one plugin in the factory");
    if (media) {
        if (argc < 6) {
            std::fprintf(stderr, "usage: resident-selftest ReSIDent.clap --media CARTRIDGE DISK START-DISK [rom-folder]\n");
            return 2;
        }
        mediaTest(factory, argv[3], argv[4], argv[5]);
        entry->deinit();
        std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
        return failures ? 1 : 0;
    }

    Instance a;
    a.create(factory);
    auto* params = static_cast<const clap_plugin_params_t*>(a.plugin->get_extension(a.plugin, CLAP_EXT_PARAMS));
    check(params && params->count(a.plugin) == resident::pid::kCount, "17 parameters plus the CC slots");
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
    auto* loader = static_cast<const resident::ExtLoader*>(a.plugin->get_extension(a.plugin, resident::kExtLoader));
    check(loader && loader->load_file(a.plugin, argv[2]), (std::string("program loads: ") + argv[2]).c_str());

    const int readyBlocks = static_cast<int>(ready * 48000 / 512) + 200; // + the warp
    a.run(readyBlocks);
    // Loading allocates (the virtual drive's DOS works in strings and
    // vectors, and a file operation is rare); playing must not. Count from here.
    allocations = 0;
    const auto silent = a.run(40);
    const auto note = a.run(80, {{0, {0x90, 60, 100}}, {60, {0x80, 60, 0}}});
    std::printf("      quiet %.1f dB, note %.1f dB\n", rmsDb(silent), rmsDb(note));
    check(rmsDb(note) > -50.0 && rmsDb(note) > rmsDb(silent) + 20.0, "a MIDI note makes the C64 play");
    check(allocations == 0, ("no allocation in process() so far (" + std::to_string(allocations) + ")").c_str());

    // CC map: picked by the program's name, slots named after it.
    {
        auto infoName = [&](clap_id id) {
            for (uint32_t i = 0; params && i < params->count(a.plugin); ++i) {
                clap_param_info_t info{};
                if (params->get_info(a.plugin, i, &info) && info.id == id)
                    return std::string(info.name);
            }
            return std::string("?");
        };
        check(infoName(1000) == "Cutoff" && infoName(1001) == "Volume" && infoName(1002) == "Slot 3",
              ("the program's CC map names the slots (" + infoName(1000) + ", " + infoName(1001) + ", " +
               infoName(1002) + ")").c_str());
        char text[64] = {};
        double back = -1;
        check(params->value_to_text(a.plugin, 1000, 70.0, text, sizeof(text)) && std::string(text) == "Open" &&
                  params->text_to_value(a.plugin, 1000, "closed", &back) && back == 0.0,
              "a slot's values carry the map's labels both ways");
        const uint32_t m0 = loader->midi_in_count(a.plugin);
        a.run(2, {}, {{1000, 100.0}});
        const uint32_t m1 = loader->midi_in_count(a.plugin);
        a.run(2, {}, {{1000, 100.4}, {1002, 50.0}});
        const uint32_t m2 = loader->midi_in_count(a.plugin);
        std::printf("      slot moves reached the machine: %u, then %u\n", m1 - m0, m2 - m1);
        check(m1 - m0 == 1, "a moved slot sends its controller once");
        check(m2 == m1, "an unchanged value and an unused slot send nothing");
        check(allocations == 0, "the CC slots allocate nothing in process()");

        // Heard: CC 7 is the volume in Cynthcart and Station64 alike. A 6581
        // steps its DC level when the volume changes (what C64 digis play
        // with), so the step settles before the note is measured.
        a.run(40, {}, {{1001, 0.0}});
        const auto off = a.run(60, {{2, {0x90, 60, 100}}, {40, {0x80, 60, 0}}});
        a.run(40, {}, {{1001, 127.0}});
        const auto on = a.run(60, {{2, {0x90, 60, 100}}, {40, {0x80, 60, 0}}});
        std::printf("      Volume slot at 0: %.1f dB, at 127: %.1f dB\n", rmsDb(off), rmsDb(on));
        check(rmsDb(on) > rmsDb(off) + 20.0, "the program hears the slot (Volume 0 is quiet, 127 is not)");
    }

    // A flood: 3000 messages (9000 bytes) in one block, more than the
    // cable queue holds. The rest is dropped, nothing allocates, and once the
    // cable has drained (about 1.3 s) the program still answers a note.
    {
        std::vector<std::pair<int, std::vector<uint8_t>>> flood;
        for (int i = 0; i < 3000; ++i)
            flood.push_back({0, {0x80, static_cast<uint8_t>(i & 127), 0}});
        const size_t before = allocations;
        a.run(200, flood);
        const auto after = a.run(80, {{0, {0x90, 60, 100}}, {60, {0x80, 60, 0}}});
        std::printf("      after a 9000-byte flood: note %.1f dB, %zu allocations\n", rmsDb(after),
                    allocations - before);
        check(allocations == before, "a MIDI flood allocates nothing in process()");
        check(rmsDb(after) > -50.0, "the C64 still plays after a MIDI flood");
    }

    // State round trip.
    auto* state = static_cast<const clap_plugin_state_t*>(a.plugin->get_extension(a.plugin, CLAP_EXT_STATE));
    Buffer buf;
    clap_ostream_t os{&buf, Buffer::write};
    check(state && state->save(a.plugin, &os), "state saves");
    std::printf("      state: %zu bytes\n", buf.data.size());
    // A plays on first, then B loads the state: loading a project must not
    // change how any other instance sounds (reSIDfp's 6581 model is shared
    // by every chip in the process).
    const std::vector<std::pair<int, std::vector<uint8_t>>> phrase = {
        {2, {0x90, 64, 110}}, {30, {0x80, 64, 0}}, {35, {0x90, 67, 90}}, {70, {0x80, 67, 0}}};
    const auto outA = a.run(100, phrase);
    Instance b;
    b.create(factory);
    clap_istream_t is{&buf, Buffer::read};
    auto* stateB = static_cast<const clap_plugin_state_t*>(b.plugin->get_extension(b.plugin, CLAP_EXT_STATE));
    check(stateB && stateB->load(b.plugin, &is), "state loads into a second instance");
    {
        auto* paramsB = static_cast<const clap_plugin_params_t*>(b.plugin->get_extension(b.plugin, CLAP_EXT_PARAMS));
        clap_param_info_t info{};
        bool named = false;
        double v = -1;
        for (uint32_t i = 0; paramsB && i < paramsB->count(b.plugin); ++i)
            if (paramsB->get_info(b.plugin, i, &info) && info.id == 1000)
                named = std::string(info.name) == "Cutoff";
        check(named && paramsB->get_value(b.plugin, 1000, &v) && v == 100.0,
              "the CC map and the slot values travel with the state");
    }
    const auto outB = b.run(100, phrase);
    double maxDiff = 0;
    for (size_t i = 0; i < outA.size(); ++i)
        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(outA[i] - outB[i])));
    std::printf("      after the round trip: max difference %.3g, level %.1f dB\n", maxDiff, rmsDb(outB));
    for (size_t seg = 0; maxDiff > 0 && seg < outA.size(); seg += outA.size() / 10) {
        double d = 0;
        for (size_t i = seg; i < std::min(outA.size(), seg + outA.size() / 10); ++i)
            d = std::max(d, static_cast<double>(std::fabs(outA[i] - outB[i])));
        std::printf("        from sample %6zu: %.3g\n", seg, d);
    }
    // Exact: the snapshot carries every chip's dither position too (the
    // vendored reSIDfp keeps it per chip). Both instances ran in this
    // process one after the other, so this also proves that one instance's
    // chips do not move the other's.
    check(maxDiff == 0.0 && rmsDb(outB) > -50.0, "the restored machine plays on exactly like the original");

    // The 6581 filter range is per chip: C gets SID 1 dark (in the
    // settings, "range" is the eighth field of the sid1 line), D the
    // original state again. C sounds different; D still plays exactly like
    // A, with C in the same process.
    {
        Buffer darkBuf = buf, origBuf = buf;
        darkBuf.at = origBuf.at = 0;
        const std::string from = " 0.487179 ", to = " 0.100000 ";
        auto it = std::search(darkBuf.data.begin(), darkBuf.data.end(), from.begin(), from.end());
        check(it != darkBuf.data.end(), "the settings carry SID 1's filter range");
        if (it != darkBuf.data.end())
            std::copy(to.begin(), to.end(), it);
        Instance c, d;
        c.create(factory);
        clap_istream_t isC{&darkBuf, Buffer::read};
        auto* stC = static_cast<const clap_plugin_state_t*>(c.plugin->get_extension(c.plugin, CLAP_EXT_STATE));
        check(stC && stC->load(c.plugin, &isC), "a state with another filter range loads");
        d.create(factory);
        clap_istream_t isD{&origBuf, Buffer::read};
        auto* stD = static_cast<const clap_plugin_state_t*>(d.plugin->get_extension(d.plugin, CLAP_EXT_STATE));
        check(stD && stD->load(d.plugin, &isD), "the original state loads once more");
        const auto outC = c.run(100, phrase);
        const auto outD = d.run(100, phrase);
        double diffC = 0, diffD = 0;
        for (size_t i = 0; i < outA.size(); ++i) {
            diffC = std::max(diffC, static_cast<double>(std::fabs(outA[i] - outC[i])));
            diffD = std::max(diffD, static_cast<double>(std::fabs(outA[i] - outD[i])));
        }
        std::printf("      SID 1 range 0.1: max difference %.3g; the original beside it: %.3g\n", diffC, diffD);
        if (lite)
            check(diffC == 0.0, "the Light engine has no filter range: it changes nothing");
        else
            check(diffC > 1e-3, "the filter range changes the sound");
        check(diffD == 0.0, "another instance's filter range leaves this one untouched");
        c.destroy();
        d.destroy();
    }

    // The SID engine. A project from before the setting has no sid_engine
    // line (renamed here, so the blob keeps its length): it loads on reSIDfp.
    // From a reSIDfp project it plays on exactly; a Light project's chip
    // states are not reSIDfp's, so its chips start from their registers.
    {
        Buffer oldBuf = buf;
        oldBuf.at = 0;
        const std::string from = "sid_engine = ", to = "xid_engine = ";
        auto it = std::search(oldBuf.data.begin(), oldBuf.data.end(), from.begin(), from.end());
        check(it != oldBuf.data.end(), "the settings carry sid_engine");
        if (it != oldBuf.data.end())
            std::copy(to.begin(), to.end(), it);
        Instance o;
        const bool wasLite = lite;
        lite = false; // a host loading an old project switches nothing
        o.create(factory);
        lite = wasLite;
        clap_istream_t isO{&oldBuf, Buffer::read};
        auto* stO = static_cast<const clap_plugin_state_t*>(o.plugin->get_extension(o.plugin, CLAP_EXT_STATE));
        check(stO && stO->load(o.plugin, &isO), "a project without sid_engine loads");
        const auto outO = o.run(100, phrase);
        double dO = 0;
        for (size_t i = 0; i < outA.size(); ++i)
            dO = std::max(dO, static_cast<double>(std::fabs(outA[i] - outO[i])));
        std::printf("      a project without sid_engine: max difference %.3g, level %.1f dB\n", dO, rmsDb(outO));
        if (lite)
            check(rmsDb(outO) > -50.0 && std::fabs(rmsDb(outO) - rmsDb(outA)) < 6.0,
                  "a Light project read without the setting plays on reSIDfp, without a thump");
        else
            check(dO == 0.0, "a project without sid_engine plays on reSIDfp exactly");
        o.destroy();
    }
    // Switched while a note plays: to the other engine and back. The note
    // goes on (its oscillators start over), nothing allocates. A twin plays
    // the same without switching: the levels must stay close to its own
    // (the program's envelope may fall meanwhile).
    {
        Instance s, twin;
        for (Instance* i : {&s, &twin}) {
            i->create(factory);
            auto* li = static_cast<const resident::ExtLoader*>(i->plugin->get_extension(i->plugin, resident::kExtLoader));
            li->load_file(i->plugin, argv[2]);
            i->run(readyBlocks);
        }
        const size_t before = allocations;
        const auto pre = s.run(20, {{0, {0x90, 60, 100}}});
        twin.run(20, {{0, {0x90, 60, 100}}});
        s.setEngine(lite ? 0 : 1);
        const auto other = s.run(30);
        const auto otherTwin = twin.run(30);
        s.setEngine(lite ? 1 : 0);
        const auto back = s.run(30, {{29, {0x80, 60, 0}}});
        const auto backTwin = twin.run(30, {{29, {0x80, 60, 0}}});
        std::printf("      engine switch during a note: %.1f dB, on the other engine %.1f dB (twin %.1f), "
                    "back %.1f dB (twin %.1f)\n",
                    rmsDb(pre), rmsDb(other), rmsDb(otherTwin), rmsDb(back), rmsDb(backTwin));
        check(rmsDb(other) > -50.0 && rmsDb(back) > -50.0, "a note plays on through engine switches");
        // A fresh chip on a running machine steps its output's DC; unsettled,
        // that thump was 10 dB louder than the note.
        check(std::fabs(rmsDb(other) - rmsDb(otherTwin)) < 6.0 && std::fabs(rmsDb(back) - rmsDb(backTwin)) < 6.0,
              "no thump when the engine changes (within 6 dB of a twin that does not switch)");
        check(allocations == before, "process() allocates nothing around an engine switch");
        s.destroy();
        twin.destroy();
    }

    // Warp. A load starts in warp on its own; the Warp button stops it, and
    // the stop holds until that warp would have ended -- the next load warps
    // again. With Auto warp off ("warp_boot = 0", "warp_disk = 0") it does not.
    {
        Instance e;
        e.create(factory);
        auto* le = static_cast<const resident::ExtLoader*>(e.plugin->get_extension(e.plugin, resident::kExtLoader));
        le->load_file(e.plugin, argv[2]);
        e.run(2);
        check(le->warping(e.plugin), "a load warps on its own");
        le->set_warp(e.plugin, false);
        const bool stopped = !le->warping(e.plugin);
        e.run(2); // in real time now: about 20 ms, far from the end of the start
        check(stopped && !le->warping(e.plugin), "the Warp button stops an automatic warp");
        le->load_file(e.plugin, argv[2]);
        e.run(2);
        check(le->warping(e.plugin), "the next load warps again");

        auto* stE = static_cast<const clap_plugin_state_t*>(e.plugin->get_extension(e.plugin, CLAP_EXT_STATE));
        Buffer offBuf;
        clap_ostream_t osE{&offBuf, Buffer::write};
        stE->save(e.plugin, &osE);
        for (const std::string from : {"warp_boot = 1", "warp_disk = 1"}) {
            const std::string to = from.substr(0, from.size() - 1) + "0";
            auto it = std::search(offBuf.data.begin(), offBuf.data.end(), from.begin(), from.end());
            check(it != offBuf.data.end(), ("the settings carry " + from.substr(0, 9)).c_str());
            if (it != offBuf.data.end())
                std::copy(to.begin(), to.end(), it);
        }
        clap_istream_t isE{&offBuf, Buffer::read};
        check(stE->load(e.plugin, &isE), "a state with Auto warp off loads");
        le->load_file(e.plugin, argv[2]);
        e.run(2);
        check(!le->warping(e.plugin), "with Auto warp off a load runs in real time");
        e.destroy();
    }

    // Voice outputs.
    {
        auto withVoices = [&](Instance& i, bool on) {
            i.create(factory);
            auto* st = static_cast<const clap_plugin_state_t*>(i.plugin->get_extension(i.plugin, CLAP_EXT_STATE));
            Buffer sb;
            clap_ostream_t so{&sb, Buffer::write};
            st->save(i.plugin, &so);
            const std::string from = "voice_outputs = 0", to = "voice_outputs = 1";
            auto it = std::search(sb.data.begin(), sb.data.end(), from.begin(), from.end());
            check(it != sb.data.end(), "the settings carry voice_outputs");
            if (on && it != sb.data.end())
                std::copy(to.begin(), to.end(), it);
            clap_istream_t si{&sb, Buffer::read};
            st->load(i.plugin, &si);
            auto* li = static_cast<const resident::ExtLoader*>(i.plugin->get_extension(i.plugin, resident::kExtLoader));
            li->load_file(i.plugin, argv[2]);
            i.run(readyBlocks);
        };
        Instance on, off;
        withVoices(on, true);
        withVoices(off, false);
        const size_t before = allocations;
        const auto mainOn = on.run(100, phrase);
        const auto onAux = on.auxAll;
        check(allocations == before, "voice outputs allocate nothing in process()");
        const auto mainOff = off.run(100, phrase);
        double dMain = 0, offVoices = 0;
        for (size_t k = 0; k < mainOn.size(); ++k)
            dMain = std::max(dMain, static_cast<double>(std::fabs(mainOn[k] - mainOff[k])));
        for (size_t v = 8; v < off.auxAll.size(); ++v)
            for (float x : off.auxAll[v])
                offVoices = std::max(offVoices, static_cast<double>(std::fabs(x)));
        check(dMain == 0.0, "voice outputs leave the main output bit-identical");
        check(offVoices == 0.0, "with voice outputs off the voice ports are silent");

        const auto& sid1 = onAux[0];
        std::vector<float> sum(sid1.size(), 0.0f);
        double voiceDb[3];
        for (int v = 0; v < 3; ++v) {
            const auto& vo = onAux[8 + static_cast<size_t>(v)];
            for (size_t k = 0; k < sum.size(); ++k)
                sum[k] += vo[k];
            voiceDb[v] = rmsDb(vo);
        }
        std::vector<float> resid(sum.size());
        for (size_t k = 0; k < sum.size(); ++k)
            resid[k] = sum[k] - sid1[k];
        std::printf("      SID 1 %.1f dB; voices %.1f / %.1f / %.1f dB; their sum %.1f dB, minus SID 1 %.1f dB\n",
                    rmsDb(sid1), voiceDb[0], voiceDb[1], voiceDb[2], rmsDb(sum), rmsDb(resid));
        check(std::max({voiceDb[0], voiceDb[1], voiceDb[2]}) > -50.0, "a played note comes out of a voice output");
        check(rmsDb(resid) < rmsDb(sid1) - 10.0, "the three voices together come close to their SID's output");

        // The voices travel with the state, exactly.
        auto* stOn = static_cast<const clap_plugin_state_t*>(on.plugin->get_extension(on.plugin, CLAP_EXT_STATE));
        Buffer vb;
        clap_ostream_t vo{&vb, Buffer::write};
        stOn->save(on.plugin, &vo);
        Instance again;
        again.create(factory);
        clap_istream_t vi{&vb, Buffer::read};
        auto* stAgain = static_cast<const clap_plugin_state_t*>(again.plugin->get_extension(again.plugin, CLAP_EXT_STATE));
        check(stAgain->load(again.plugin, &vi), "a state with voice outputs loads");
        on.run(100, phrase);
        again.run(100, phrase);
        double dVoice = 0;
        for (size_t v = 8; v < 11; ++v)
            for (size_t k = 0; k < on.auxAll[v].size(); ++k)
                dVoice = std::max(dVoice, static_cast<double>(std::fabs(on.auxAll[v][k] - again.auxAll[v][k])));
        std::printf("      voices after the round trip: max difference %.3g\n", dVoice);
        check(dVoice == 0.0, "the voices play on exactly after a state round trip");
        on.destroy();
        off.destroy();
        again.destroy();
    }

    a.destroy();
    b.destroy();
    entry->deinit();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
