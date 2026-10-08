// Minimal CLAP host for Substrike: loads the built .clap the way a DAW does,
// plays one or more hits, and writes the result as a 32-bit float WAV. It also
// carries the self-test, which goes through the same CLAP interface, so it
// checks exactly what gets installed.
//
//   substrike-render --plugin build/Substrike.clap --out kick.wav
//   substrike-render --param "L1 Pitch Start=1.2 kHz" --param "L1 Body Decay=1.5 s"
//   substrike-render --param "L2 On=On" --param "L2 Output=Aux" --aux 2
//   substrike-render --hits 4 --bpm 140 --seconds 2 --key 38 --lead 1000   (the first hit 1000 samples in)
//   substrike-render --list-params
//   substrike-render --state preset.substrike --save-state out.substrike
//   substrike-render --preset techno-concrete        (a factory preset, by load key)
//   substrike-render --preset a --then-preset b --at 9600   (switch presets mid-render)
//   substrike-render --list-presets
//   substrike-render --all-presets --outdir renders --seconds 4
//   substrike-render --selftest

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <dlfcn.h>
#endif

#include <clap/clap.h>

#if defined(_WIN32)
namespace {
void* dlopenCompat(const char* path) { return reinterpret_cast<void*>(LoadLibraryA(path)); }
void* dlsymCompat(void* h, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
}
void dlcloseCompat(void* h) { FreeLibrary(reinterpret_cast<HMODULE>(h)); }
const char* dlerrorCompat() { return "see GetLastError()"; }
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

namespace {

// ------------------------------------------------------------------- host

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostNoop(const clap_host_t*) {}

const clap_host_t kHost = {
    CLAP_VERSION_INIT, nullptr, "substrike-render", "Ravetracer", "", "1.0",
    hostGetExtension,  hostNoop, hostNoop,          hostNoop,
};

// An input event list over a vector of owned events, sorted by time.
struct EventList
{
    std::vector<std::vector<uint8_t>> events;

    template <typename T> void add(const T& ev)
    {
        std::vector<uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &ev, sizeof(T));
        events.push_back(std::move(bytes));
    }
    const clap_event_header_t* at(uint32_t i) const
    {
        return reinterpret_cast<const clap_event_header_t*>(events[i].data());
    }
    void sort()
    {
        std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
            return reinterpret_cast<const clap_event_header_t*>(a.data())->time <
                   reinterpret_cast<const clap_event_header_t*>(b.data())->time;
        });
    }

    static uint32_t size(const clap_input_events_t* l) { return static_cast<uint32_t>(self(l)->events.size()); }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) { return self(l)->at(i); }
    static const EventList* self(const clap_input_events_t* l) { return static_cast<const EventList*>(l->ctx); }

    clap_input_events_t input() const { return {const_cast<EventList*>(this), size, get}; }
};

bool outPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }
const clap_output_events_t kOutEvents = {nullptr, outPush};

clap_event_note_t noteOn(uint32_t time, int16_t key, double velocity)
{
    clap_event_note_t ev{};
    ev.header.size = sizeof(ev);
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_NOTE_ON;
    ev.note_id = -1;
    ev.port_index = 0;
    ev.channel = 0;
    ev.key = key;
    ev.velocity = velocity;
    return ev;
}

clap_event_midi_t midiNoteOn(uint32_t time, uint8_t key, uint8_t velocity)
{
    clap_event_midi_t ev{};
    ev.header.size = sizeof(ev);
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_MIDI;
    ev.port_index = 0;
    ev.data[0] = 0x90;
    ev.data[1] = key;
    ev.data[2] = velocity;
    return ev;
}

clap_event_param_value_t paramValue(clap_id id, double value)
{
    clap_event_param_value_t ev{};
    ev.header.size = sizeof(ev);
    ev.header.time = 0;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.param_id = id;
    ev.note_id = -1;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.value = value;
    return ev;
}

// ---------------------------------------------------------------- plugin

struct Module
{
    void* dso = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
};

const clap_plugin_t* createPlugin(const Module& m)
{
    const clap_plugin_descriptor_t* desc = m.factory->get_plugin_descriptor(m.factory, 0);
    if (!desc)
        return nullptr;
    const clap_plugin_t* p = m.factory->create_plugin(m.factory, &kHost, desc->id);
    if (!p || !p->init(p))
    {
        std::fprintf(stderr, "plugin creation failed\n");
        return nullptr;
    }
    return p;
}

const clap_plugin_params_t* paramsOf(const clap_plugin_t* p)
{
    return static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
}

std::string squash(const std::string& s)
{
    std::string out;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool findParam(const clap_plugin_t* p, const std::string& name, clap_param_info_t& info)
{
    const clap_plugin_params_t* params = paramsOf(p);
    for (uint32_t i = 0; i < params->count(p); ++i)
        if (params->get_info(p, i, &info) && squash(info.name) == squash(name))
            return true;
    return false;
}

// "Name=text" -> a parameter event, through the plugin's own text parser.
bool resolveParam(const clap_plugin_t* p, const std::string& spec, EventList& out)
{
    const size_t eq = spec.find('=');
    clap_param_info_t info{};
    if (eq == std::string::npos || !findParam(p, spec.substr(0, eq), info))
    {
        std::fprintf(stderr, "unknown parameter in '%s' (see --list-params)\n", spec.c_str());
        return false;
    }
    double v = 0.0;
    if (!paramsOf(p)->text_to_value(p, info.id, spec.substr(eq + 1).c_str(), &v))
    {
        std::fprintf(stderr, "cannot read the value in '%s'\n", spec.c_str());
        return false;
    }
    out.add(paramValue(info.id, v));
    return true;
}

// Flushes parameter events into an inactive plugin.
void flushParams(const clap_plugin_t* p, const EventList& events)
{
    const clap_input_events_t in = events.input();
    paramsOf(p)->flush(p, &in, &kOutEvents);
}

constexpr int kLanes = 8;

// Sets "Name=text" parameters one at a time: a slot's A-F are named after
// what its type makes them, so they can only be found once the type is set.
bool applyParams(const clap_plugin_t* p, const std::vector<std::string>& specs)
{
    for (const std::string& spec : specs)
    {
        EventList one;
        if (!resolveParam(p, spec, one))
            return false;
        flushParams(p, one);
    }
    return true;
}

// Creates a plugin and sets "Name=text" parameters on it.
const clap_plugin_t* configured(const Module& m, const std::vector<std::string>& specs)
{
    const clap_plugin_t* p = createPlugin(m);
    if (!p || !applyParams(p, specs))
    {
        std::fprintf(stderr, "configuration failed\n");
        std::exit(1);
    }
    return p;
}

// Main output plus one stereo aux port per lane, as the plugin declares them.
struct Render
{
    std::vector<float> left, right;
    std::vector<float> auxL[kLanes], auxR[kLanes];
    bool sawNonFinite = false;
    bool sleptAtEnd = false;
    float peak = 0.0f;
};

// A tempo for the host's transport in render(); 0 leaves the transport out.
double gTransportTempo = 0.0;

// Called before the block that starts at `pos`, to change the plugin
// between blocks (a preset switch, as a browser does it).
using BetweenBlocks = std::function<void(const clap_plugin_t*, uint32_t pos)>;

// Plays `events` (note events with absolute sample times) over `frames`
// samples in blocks of `block`.
Render render(const clap_plugin_t* p, double rate, uint32_t block, uint32_t frames, const EventList& events,
              const BetweenBlocks& between = {})
{
    Render r;
    r.left.assign(frames, 0.0f);
    r.right.assign(frames, 0.0f);
    for (int a = 0; a < kLanes; ++a)
    {
        r.auxL[a].assign(frames, 0.0f);
        r.auxR[a].assign(frames, 0.0f);
    }
    if (!p->activate(p, rate, 1, block) || !p->start_processing(p))
    {
        std::fprintf(stderr, "activate failed\n");
        return r;
    }
    size_t next = 0;
    clap_process_status status = CLAP_PROCESS_CONTINUE;
    for (uint32_t pos = 0; pos < frames; pos += block)
    {
        const uint32_t n = std::min(block, frames - pos);
        if (between)
            between(p, pos);
        EventList blockEvents;
        while (next < events.events.size() && events.at(static_cast<uint32_t>(next))->time < pos + n)
        {
            std::vector<uint8_t> copy = events.events[next++];
            reinterpret_cast<clap_event_header_t*>(copy.data())->time -= pos;
            blockEvents.events.push_back(std::move(copy));
        }
        float* chans[1 + kLanes][2];
        clap_audio_buffer_t out[1 + kLanes]{};
        for (int b = 0; b <= kLanes; ++b)
        {
            chans[b][0] = (b ? r.auxL[b - 1].data() : r.left.data()) + pos;
            chans[b][1] = (b ? r.auxR[b - 1].data() : r.right.data()) + pos;
            out[b].data32 = chans[b];
            out[b].channel_count = 2;
        }
        clap_process_t proc{};
        proc.steady_time = pos;
        proc.frames_count = n;
        proc.audio_outputs = out;
        proc.audio_outputs_count = 1 + kLanes;
        const clap_input_events_t in = blockEvents.input();
        proc.in_events = &in;
        proc.out_events = &kOutEvents;
        clap_event_transport_t transport{};
        if (gTransportTempo > 0.0)
        {
            transport.header.size = sizeof(transport);
            transport.header.type = CLAP_EVENT_TRANSPORT;
            transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_IS_PLAYING;
            transport.tempo = gTransportTempo;
            proc.transport = &transport;
        }
        status = p->process(p, &proc);
        if (status == CLAP_PROCESS_ERROR)
        {
            std::fprintf(stderr, "process returned an error\n");
            break;
        }
    }
    r.sleptAtEnd = status == CLAP_PROCESS_SLEEP;
    p->stop_processing(p);
    p->deactivate(p);
    for (uint32_t i = 0; i < frames; ++i)
    {
        if (!std::isfinite(r.left[i]) || !std::isfinite(r.right[i]))
            r.sawNonFinite = true;
        else
            r.peak = std::max({r.peak, std::fabs(r.left[i]), std::fabs(r.right[i])});
        for (int a = 0; a < kLanes; ++a)
            if (!std::isfinite(r.auxL[a][i]) || !std::isfinite(r.auxR[a][i]))
                r.sawNonFinite = true;
    }
    return r;
}

// ------------------------------------------------------------------- WAV

bool writeWav(const std::string& path, const std::vector<float>& left, const std::vector<float>& right, uint32_t rate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
    {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    const uint32_t frames = static_cast<uint32_t>(left.size());
    const uint32_t dataBytes = frames * 2 * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(4 + (8 + 16) + (8 + dataBytes));
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32(rate);
    u32(rate * 2 * 4);
    u16(2 * 4);
    u16(32);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
    {
        std::fwrite(&left[i], 4, 1, f);
        std::fwrite(&right[i], 4, 1, f);
    }
    std::fclose(f);
    return true;
}

// -------------------------------------------------------------- analysis

// Mean frequency over [from, to) seconds from zero crossings in both
// directions, with sub-sample interpolation.
double measureFrequency(const std::vector<float>& x, double rate, double from, double to)
{
    const size_t a = static_cast<size_t>(from * rate), b = std::min(x.size(), static_cast<size_t>(to * rate));
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (size_t i = std::max<size_t>(a, 1); i < b; ++i)
        if ((x[i - 1] < 0.0f && x[i] >= 0.0f) || (x[i - 1] > 0.0f && x[i] <= 0.0f))
        {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    if (crossings < 3)
        return 0.0;
    return 0.5 * (crossings - 1) * rate / (last - first);
}

// Level in dB of the component at `freq` over [from, to) samples, Hann
// windowed (Goertzel), relative to a full-scale sine.
double toneDb(const std::vector<float>& x, double rate, double freq, size_t from, size_t to)
{
    to = std::min(to, x.size());
    const size_t n = to > from ? to - from : 0;
    if (n < 2)
        return -300.0;
    const double w = 2.0 * 3.14159265358979323846 * freq / rate;
    const double coef = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0, win = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double h = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / (n - 1));
        win += h;
        const double s0 = x[from + i] * h + coef * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coef * s1 * s2;
    return 20.0 * std::log10(std::sqrt(std::max(power, 1e-300)) * 2.0 / win);
}

// The magnitude in dB of the plain DFT at `freq` over [from, to): for an
// impulse response, the chain's gain at that frequency.
double responseDb(const std::vector<float>& x, double rate, double freq, size_t from, size_t to)
{
    double re = 0.0, im = 0.0;
    const double w = 2.0 * 3.14159265358979323846 * freq / rate;
    for (size_t i = from; i < std::min(to, x.size()); ++i)
    {
        re += x[i] * std::cos(w * static_cast<double>(i - from));
        im -= x[i] * std::sin(w * static_cast<double>(i - from));
    }
    return 10.0 * std::log10(std::max(re * re + im * im, 1e-300));
}

double peakDb(const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = from; i < std::min(to, x.size()); ++i)
        m = std::max(m, std::fabs(x[i]));
    return 20.0 * std::log10(std::max(m, 1e-15f));
}

float maxStep(const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i)
        m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

// A state as text, through the plugin's state extension.
bool loadStateText(const clap_plugin_t* pl, const std::string& text)
{
    struct Reader
    {
        const std::string* s;
        size_t pos;
    } reader{&text, 0};
    clap_istream_t is{&reader, [](const clap_istream_t* st, void* buf, uint64_t size) -> int64_t {
                          auto* r = static_cast<Reader*>(st->ctx);
                          const size_t n = std::min<size_t>(size, r->s->size() - r->pos);
                          std::memcpy(buf, r->s->data() + r->pos, n);
                          r->pos += n;
                          return static_cast<int64_t>(n);
                      }};
    const auto* st = static_cast<const clap_plugin_state_t*>(pl->get_extension(pl, CLAP_EXT_STATE));
    return st->load(pl, &is);
}

std::string saveStateText(const clap_plugin_t* pl)
{
    std::string blob;
    clap_ostream_t os{&blob, [](const clap_ostream_t* st, const void* buf, uint64_t size) -> int64_t {
                          static_cast<std::string*>(st->ctx)->append(static_cast<const char*>(buf), size);
                          return static_cast<int64_t>(size);
                      }};
    const auto* st = static_cast<const clap_plugin_state_t*>(pl->get_extension(pl, CLAP_EXT_STATE));
    st->save(pl, &os);
    return blob;
}

// ---------------------------------------------------------------- presets

// A preset as the plugin's discovery provider describes it.
struct FoundPreset
{
    std::string name, loadKey, description;
    std::vector<std::string> features;
    bool factory = false;
};

// Asks the plugin's preset discovery for its factory presets, the way a
// host's indexer does.
std::vector<FoundPreset> discoverPresets(const Module& m, std::string* error = nullptr)
{
    std::vector<FoundPreset> out;
    const auto* factory = static_cast<const clap_preset_discovery_factory_t*>(
        m.entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!factory || factory->count(factory) < 1)
    {
        if (error)
            *error = "no preset discovery factory";
        return out;
    }
    struct Indexer
    {
        std::vector<std::pair<uint32_t, std::string>> locations;
        int filetypes = 0;
    } ix;
    clap_preset_discovery_indexer_t indexer{};
    indexer.clap_version = CLAP_VERSION;
    indexer.name = "substrike-render";
    indexer.indexer_data = &ix;
    indexer.declare_filetype = [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_filetype_t*) {
        ++static_cast<Indexer*>(i->indexer_data)->filetypes;
        return true;
    };
    indexer.declare_location = [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_location_t* l) {
        static_cast<Indexer*>(i->indexer_data)->locations.emplace_back(l->kind, l->location ? l->location : "");
        return true;
    };
    indexer.declare_soundpack = [](const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) {
        return true;
    };
    indexer.get_extension = [](const clap_preset_discovery_indexer_t*, const char*) -> const void* { return nullptr; };

    const auto* desc = factory->get_descriptor(factory, 0);
    const clap_preset_discovery_provider_t* provider = factory->create(factory, &indexer, desc->id);
    if (!provider || !provider->init(provider))
    {
        if (error)
            *error = "the preset provider did not start";
        return out;
    }
    struct Receiver
    {
        std::vector<FoundPreset>* out;
        std::string error;
    } rx{&out, {}};
    clap_preset_discovery_metadata_receiver_t r{};
    r.receiver_data = &rx;
    r.on_error = [](const clap_preset_discovery_metadata_receiver_t* x, int32_t, const char* msg) {
        static_cast<Receiver*>(x->receiver_data)->error = msg ? msg : "error";
    };
    r.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* x, const char* name, const char* key) {
        FoundPreset f;
        f.name = name ? name : "";
        f.loadKey = key ? key : "";
        static_cast<Receiver*>(x->receiver_data)->out->push_back(f);
        return true;
    };
    r.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t*, const clap_universal_plugin_id_t*) {};
    r.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
    r.set_flags = [](const clap_preset_discovery_metadata_receiver_t* x, uint32_t flags) {
        static_cast<Receiver*>(x->receiver_data)->out->back().factory =
            (flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0;
    };
    r.add_creator = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
    r.set_description = [](const clap_preset_discovery_metadata_receiver_t* x, const char* d) {
        static_cast<Receiver*>(x->receiver_data)->out->back().description = d ? d : "";
    };
    r.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
    r.add_feature = [](const clap_preset_discovery_metadata_receiver_t* x, const char* f) {
        static_cast<Receiver*>(x->receiver_data)->out->back().features.push_back(f ? f : "");
    };
    r.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};
    for (const auto& [kind, location] : ix.locations)
        if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
            provider->get_metadata(provider, kind, nullptr, &r);
    provider->destroy(provider);
    if (error)
        *error = ix.filetypes != 1 ? "no file type declared" : rx.error;
    return out;
}

// Loads a factory preset through the plugin's preset-load extension.
bool loadFactoryPreset(const clap_plugin_t* p, const std::string& key)
{
    const auto* ext = static_cast<const clap_plugin_preset_load_t*>(p->get_extension(p, CLAP_EXT_PRESET_LOAD));
    return ext && ext->from_location(p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str());
}

bool readTextFile(const std::string& path, std::string& text)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    std::fclose(f);
    return true;
}

// ------------------------------------------------------------- self-test

int runSelfTest(const Module& m)
{
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
        if (!ok)
            ++failures;
    };
    const double rate = 48000.0;

    // --- parameters
    const clap_plugin_t* p = createPlugin(m);
    if (!p)
        return 1;
    const clap_plugin_params_t* params = paramsOf(p);
    check(params != nullptr, "params extension present");
    if (!params)
        return 1;
    const uint32_t count = params->count(p);
    std::printf("  %u parameters\n", count);
    bool infoOk = true, unique = true, textOk = true, defaultsOk = true;
    std::vector<clap_param_info_t> infos;
    for (uint32_t i = 0; i < count; ++i)
    {
        clap_param_info_t info{};
        if (!params->get_info(p, i, &info))
        {
            infoOk = false;
            continue;
        }
        for (const clap_param_info_t& o : infos)
            if (o.id == info.id || squash(o.name) == squash(info.name))
            {
                unique = false;
                std::printf("    duplicate: %s / %s\n", o.name, info.name);
            }
        infos.push_back(info);
        double v = 0.0;
        if (!params->get_value(p, info.id, &v) || v != info.default_value || v < info.min_value ||
            v > info.max_value)
            defaultsOk = false;
        // Display text must read back to a value that displays the same.
        // Eleven even steps and then a dense sweep, which finds the places
        // where rounding carries over into the next unit.
        for (int s = 0; s <= 10 + 2000; ++s)
        {
            const double frac = s <= 10 ? s / 10.0 : (s - 11) / 1999.0;
            double x = info.min_value + (info.max_value - info.min_value) * frac;
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                x = std::round(x);
            char text[256], again[256];
            double back = 0.0;
            if (!params->value_to_text(p, info.id, x, text, sizeof(text)) ||
                !params->text_to_value(p, info.id, text, &back) ||
                !params->value_to_text(p, info.id, back, again, sizeof(again)) || std::strcmp(text, again) != 0)
            {
                textOk = false;
                std::printf("    text round trip: %s at %g -> '%s' -> %g\n", info.name, x, text, back);
                break;
            }
        }
    }
    check(count > 0 && infoOk, "parameter info for every index");
    check(unique, "parameter ids and names unique");
    check(defaultsOk, "values start at their defaults, inside their ranges");
    check(textOk, "value -> text -> value round trip");

    // --- state round trip with every parameter moved off its default
    {
        EventList moved;
        for (const clap_param_info_t& info : infos)
        {
            double v = info.min_value + (info.max_value - info.min_value) * 0.37;
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round(v) == info.default_value ? info.max_value : std::round(v);
            moved.add(paramValue(info.id, v));
        }
        flushParams(p, moved);
        const auto* state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
        std::string blob;
        clap_ostream_t os{&blob, [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
                              static_cast<std::string*>(s->ctx)->append(static_cast<const char*>(buf), size);
                              return static_cast<int64_t>(size);
                          }};
        check(state && state->save(p, &os), "state saves");
        const clap_plugin_t* q = createPlugin(m);
        struct Reader
        {
            const std::string* s;
            size_t pos;
        } reader{&blob, 0};
        clap_istream_t is{&reader, [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
                              auto* r = static_cast<Reader*>(s->ctx);
                              // Small reads, to exercise the plugin's loop.
                              const size_t n = std::min<size_t>({size, 100, r->s->size() - r->pos});
                              std::memcpy(buf, r->s->data() + r->pos, n);
                              r->pos += n;
                              return static_cast<int64_t>(n);
                          }};
        const auto* qstate = static_cast<const clap_plugin_state_t*>(q->get_extension(q, CLAP_EXT_STATE));
        check(qstate && qstate->load(q, &is), "state loads");
        bool same = true;
        for (size_t i = 0; i < infos.size(); ++i)
        {
            double a = 0.0, b = 0.0;
            paramsOf(p)->get_value(p, infos[i].id, &a);
            paramsOf(q)->get_value(q, infos[i].id, &b);
            if (std::fabs(a - b) > 1e-7)
            {
                same = false;
                std::printf("    %s: %.9g -> %.9g\n", infos[i].name, a, b);
            }
        }
        check(same, "state round trip restores every parameter");
        const std::string foreign = "[Something Else]\nformat=1\n";
        reader = {&foreign, 0};
        check(!qstate->load(q, &is), "foreign state is refused");
        q->destroy(q);
    }
    p->destroy(p);

    // --- the default hit
    const uint32_t lead = 1000;
    EventList hit;
    hit.add(noteOn(lead, 36, 1.0));
    {
        p = createPlugin(m);
        const Render r = render(p, rate, 512, static_cast<uint32_t>(rate), hit);
        check(!r.sawNonFinite, "default hit is finite");
        check(r.peak > 0.3f && r.peak <= 1.0f, "default hit peak " + std::to_string(r.peak) + " in (0.3, 1]");
        bool silentLead = true;
        for (uint32_t i = 0; i < lead; ++i)
            silentLead &= r.left[i] == 0.0f && r.right[i] == 0.0f;
        check(silentLead, "silence before the note");
        check(r.left == r.right, "a centred lane is identical in both channels");
        const double f = measureFrequency(r.left, rate, 0.30, 0.45);
        check(std::fabs(f / 48.0 - 1.0) < 0.015, "body settles on Pitch End (48 Hz): " + std::to_string(f) + " Hz");
        const double fEarly = measureFrequency(r.left, rate, lead / rate, lead / rate + 0.008);
        check(fEarly > 200.0, "body starts high (" + std::to_string(fEarly) + " Hz in the first 8 ms)");
        // Hold 30 ms + decay 450 ms: silent and asleep well before one second.
        bool silentEnd = true;
        for (size_t i = lead + static_cast<size_t>(0.49 * rate); i < r.left.size(); ++i)
            silentEnd &= r.left[i] == 0.0f;
        check(silentEnd && r.sleptAtEnd, "silent and asleep after the decay");
        check(maxStep(r.left, 0, r.left.size()) < 0.05f, "no step in the waveform");

        // Block size must not change a single sample.
        const Render odd = render(p, rate, 37, static_cast<uint32_t>(rate), hit);
        check(odd.left == r.left && odd.right == r.right, "bit-identical with a block size of 37");
        const Render again = render(p, rate, 512, static_cast<uint32_t>(rate), hit);
        check(again.left == r.left, "bit-identical on a second activation");

        EventList midi;
        midi.add(midiNoteOn(lead, 36, 127));
        const Render viaMidi = render(p, rate, 512, static_cast<uint32_t>(rate), midi);
        check(viaMidi.left == r.left, "a MIDI note-on plays the same hit");
        p->destroy(p);
    }

    // --- retrigger: the old hit fades, the new one starts clean
    {
        p = createPlugin(m);
        const uint32_t at = lead + static_cast<uint32_t>(0.1 * rate);
        EventList two;
        two.add(noteOn(lead, 36, 1.0));
        two.add(noteOn(at, 36, 1.0));
        const Render r = render(p, rate, 512, static_cast<uint32_t>(rate), two);
        check(maxStep(r.left, at - 10, at + 400) < 0.05f, "retrigger without a click");
        // Once the 3 ms fade is over, the second hit is a single hit, shifted.
        EventList one;
        one.add(noteOn(at, 36, 1.0));
        const Render single = render(p, rate, 512, static_cast<uint32_t>(rate), one);
        bool same = true;
        for (size_t i = at + 200; i < r.left.size(); ++i)
            same &= std::fabs(r.left[i] - single.left[i]) < 1e-6f;
        check(same, "after the fade the second hit equals a single hit");
        // And during the fade the old hit is still there, not cut.
        float under = 0.0f;
        for (size_t i = at + 10; i < at + 100; ++i)
            under = std::max(under, std::fabs(r.left[i] - single.left[i]));
        check(under > 0.01f, "the old hit fades out under the new one");
        p->destroy(p);
    }

    // --- key tracking and the root note
    {
        p = createPlugin(m);
        clap_param_info_t kt{}, root{};
        check(findParam(p, "L1 Key Track", kt) && findParam(p, "Root Note", root), "key track and root found");
        double full = 0.0, c3 = 0.0;
        params = paramsOf(p);
        params->text_to_value(p, kt.id, "100", &full);
        params->text_to_value(p, root.id, "C3", &c3);
        EventList set;
        set.add(paramValue(kt.id, full));
        set.add(paramValue(root.id, c3));
        flushParams(p, set);
        EventList up;
        up.add(noteOn(lead, 60, 1.0)); // C4, an octave above the root
        const Render r = render(p, rate, 512, static_cast<uint32_t>(rate), up);
        const double f = measureFrequency(r.left, rate, 0.30, 0.45);
        check(std::fabs(f / 96.0 - 1.0) < 0.015, "an octave up with full key tracking: " + std::to_string(f) + " Hz");
        p->destroy(p);
    }

    // --- other sample rates
    for (double sr : {44100.0, 96000.0})
    {
        p = createPlugin(m);
        EventList h;
        h.add(noteOn(100, 36, 1.0));
        const Render r = render(p, sr, 256, static_cast<uint32_t>(sr), h);
        const double f = measureFrequency(r.left, sr, 0.30, 0.45);
        check(!r.sawNonFinite && std::fabs(f / 48.0 - 1.0) < 0.015,
              "at " + std::to_string(static_cast<int>(sr)) + " Hz: finite, " + std::to_string(f) + " Hz");
        p->destroy(p);
    }


    // --- 0.1.0 compatibility: its ids are still there, and its state loads
    {
        p = createPlugin(m);
        params = paramsOf(p);
        const clap_id old[] = {1, 2, 1000, 1001, 1002, 1003, 1100, 1101, 1102, 1103, 1104, 1110, 1111, 1112, 1113, 1120};
        bool all = true;
        for (clap_id id : old)
        {
            double v;
            all &= params->get_value(p, id, &v);
        }
        check(all, "every 0.1.0 parameter id still exists");
        const std::string v010 = "[Substrike]\nformat=1\n[Parameters]\noutput=-6\nroot_note=D1\nl1.on=On\n"
                                 "l1.body.pitch_end=60\nl1.body.decay=900\n";
        struct Reader
        {
            const std::string* s;
            size_t pos;
        } reader{&v010, 0};
        clap_istream_t is{&reader, [](const clap_istream_t* st, void* buf, uint64_t size) -> int64_t {
                              auto* r = static_cast<Reader*>(st->ctx);
                              const size_t n = std::min<size_t>(size, r->s->size() - r->pos);
                              std::memcpy(buf, r->s->data() + r->pos, n);
                              r->pos += n;
                              return static_cast<int64_t>(n);
                          }};
        const auto* state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
        char end[64] = "", decay[64] = "";
        double v = 0.0;
        const bool loaded = state->load(p, &is);
        params->get_value(p, 1101, &v);
        params->value_to_text(p, 1101, v, end, sizeof(end));
        params->get_value(p, 1112, &v);
        params->value_to_text(p, 1112, v, decay, sizeof(decay));
        check(loaded && std::string(end) == "60.0 Hz" && std::string(decay) == "900 ms",
              std::string("a 0.1.0 state loads (") + end + ", " + decay + ")");
        p->destroy(p);
    }

    // --- curves are state: they change the sound, save, load, and a state
    // without them goes back to the default curves
    {
        auto loadText = loadStateText;
        auto saveText = saveStateText;
        EventList one;
        one.add(noteOn(lead, 36, 1.0));
        p = createPlugin(m);
        const Render plain = render(p, rate, 512, static_cast<uint32_t>(rate), one);
        // The pitch holds at Pitch Start for 80 % of the sweep, then drops.
        const std::string curves = "[Substrike]\nformat=1\n[Parameters]\n[Curves]\n"
                                   "l1.pitch=0,1,0;0.8,1,0;1,0,0\nl1.amp=0,1,0;0.5,0.6,0.3;1,0,0\n";
        check(loadText(p, curves), "a state with curves loads");
        const Render curved = render(p, rate, 512, static_cast<uint32_t>(rate), one);
        const double held = measureFrequency(curved.left, rate, lead / rate + 0.04, lead / rate + 0.08);
        const double fell = measureFrequency(plain.left, rate, lead / rate + 0.04, lead / rate + 0.08);
        check(std::fabs(held / 350.0 - 1.0) < 0.03 && fell < 150.0,
              "a drawn pitch curve plays: " + std::to_string(held) + " Hz held, " + std::to_string(fell) +
                  " Hz without it");
        const std::string saved = saveText(p);
        check(saved.find("l1.pitch=0,1,0;0.8,1,0;1,0,0\n") != std::string::npos &&
                  saved.find("l1.amp=0,1,0;0.5,0.6,0.3;1,0,0\n") != std::string::npos,
              "curves are saved as written");
        const clap_plugin_t* q = createPlugin(m);
        check(loadText(q, saved), "the saved state loads");
        const Render back = render(q, rate, 512, static_cast<uint32_t>(rate), one);
        check(back.left == curved.left, "a saved curve plays back bit-identically");
        q->destroy(q);
        check(loadText(p, "[Substrike]\nformat=1\n"), "a state without curves loads");
        const Render reset = render(p, rate, 512, static_cast<uint32_t>(rate), one);
        check(reset.left == plain.left, "a state without curves restores the default ones");
        p->destroy(p);
    }

    // --- ports: main plus one aux per lane
    {
        p = createPlugin(m);
        const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
        bool ok = ports && ports->count(p, false) == 1 + kLanes && ports->count(p, true) == 0;
        for (uint32_t i = 0; ok && i < 1 + kLanes; ++i)
        {
            clap_audio_port_info_t info{};
            ok &= ports->get(p, i, false, &info) && info.channel_count == 2 && info.id == i &&
                  ((i == 0) == ((info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0));
        }
        check(ok, "a stereo main output and eight stereo lane outputs");
        p->destroy(p);
    }

    auto silent = [](const std::vector<float>& x) {
        return std::all_of(x.begin(), x.end(), [](float v) { return v == 0.0f; });
    };
    auto auxSilent = [&](const Render& r, int except) {
        bool ok = true;
        for (int a = 0; a < kLanes; ++a)
            if (a != except)
                ok &= silent(r.auxL[a]) && silent(r.auxR[a]);
        return ok;
    };
    auto rendered = [&](const std::vector<std::string>& specs, const EventList& ev, double seconds = 1.0,
                        uint32_t block = 512) {
        const clap_plugin_t* q = configured(m, specs);
        Render r = render(q, rate, block, static_cast<uint32_t>(seconds * rate), ev);
        q->destroy(q);
        return r;
    };

    // --- every source and every variant of it: finite, audible, and asleep
    //     once it has died away
    {
        std::vector<std::vector<std::string>> variants;
        for (const char* w : {"Sine", "Triangle", "Saw", "Square", "Additive"})
            variants.push_back({std::string("L1 Wave=") + w, "L1 FM Amount=80", "L1 Feedback=100", "L1 Shape=60",
                                "L1 Drift=100", "L1 Pitch Start=9k", "L1 Stretch=100", "L1 Tilt=100"});
        for (const char* t : {"Impulse", "Noise", "Blip", "Zap"})
            for (const char* f : {"Off", "Low Pass", "Band Pass", "High Pass"})
                variants.push_back({"L1 Source=Click", std::string("L1 Click Type=") + t,
                                    std::string("L1 Click Filter=") + f, "L1 Click Reso=100", "L1 Click Sweep=8"});
        for (const char* c : {"White", "Pink", "Brown", "Crackle"})
            variants.push_back({"L1 Source=Noise", std::string("L1 Noise Color=") + c, "L1 Noise Filter Env=8",
                                "L1 Noise Reso=100", "L1 Noise Width=100"});
        for (const char* e : {"Impulse", "Mallet", "Noise"})
            for (const char* md : {"Membrane", "Harmonic", "Odd", "Bar"})
                variants.push_back({"L1 Source=Resonator", std::string("L1 Resonator Exciter=") + e,
                                    std::string("L1 Resonator Model=") + md, "L1 Resonator Modes=8",
                                    "L1 Resonator Brightness=100", "L1 Resonator Drop=24", "L1 Resonator Tune=400"});
        int bad = 0;
        for (const auto& v : variants)
        {
            const Render r = rendered(v, hit, 3.0);
            // Noise through a resonant filter may peak past full scale; what
            // this looks for is a blow-up.
            if (r.sawNonFinite || r.peak < 0.01f || r.peak > 2.0f || !r.sleptAtEnd || !silent(r.auxL[0]))
            {
                ++bad;
                std::string what;
                for (const std::string& s : v)
                    what += s + " ";
                std::printf("    %s-> peak %g%s%s\n", what.c_str(), r.peak, r.sawNonFinite ? ", non-finite" : "",
                            r.sleptAtEnd ? "" : ", never sleeps");
            }
        }
        check(bad == 0, std::to_string(variants.size()) + " source variants finite, under +6 dBFS and asleep");
    }

    // The default hit, with the output at unity so lanes and aux ports compare.
    const Render unity = rendered({"Output=0"}, hit);

    // --- note filter
    {
        EventList c1, d1;
        c1.add(noteOn(lead, 36, 1.0));
        d1.add(noteOn(lead, 38, 1.0));
        // MIDI 36 and 38, named with C4 = 60.
        const Render other = rendered({"Output=0", "L1 Note=D2"}, c1);
        const Render own = rendered({"Output=0", "L1 Note=D2"}, d1);
        check(silent(other.left) && other.sleptAtEnd, "a lane set to D2 ignores C2");
        check(own.left == unity.left, "a lane set to D2 plays D2");
    }

    // --- trigger delay and polarity
    {
        const uint32_t d = static_cast<uint32_t>(0.010 * rate);
        bool exact = true;
        for (uint32_t blk : {512u, 37u})
        {
            const Render late = rendered({"Output=0", "L1 Delay=10"}, hit, 1.0, blk);
            for (size_t i = 0; i < late.left.size(); ++i)
                exact &= late.left[i] == (i < d ? 0.0f : unity.left[i - d]);
        }
        check(exact, "a 10 ms delay moves the hit by exactly 480 samples");
        // And in absolute terms: started at 90 degrees, a hit's first sample
        // is its peak, on the note's sample or exactly 480 later.
        auto onset = [&](const Render& r) {
            for (size_t i = 0; i < r.left.size(); ++i)
                if (r.left[i] != 0.0f)
                    return static_cast<long>(i);
            return -1L;
        };
        const long now = onset(rendered({"L1 Body Phase=90"}, hit));
        const long later = onset(rendered({"L1 Body Phase=90", "L1 Delay=10"}, hit));
        check(now == lead && later == lead + d,
              "the hit starts on the note's sample (" + std::to_string(now) + "), delayed on sample " +
                  std::to_string(later));
        const Render inv = rendered({"Output=0", "L1 Invert=On"}, hit);
        bool negated = true;
        for (size_t i = 0; i < inv.left.size(); ++i)
            negated &= inv.left[i] == -unity.left[i];
        check(negated, "invert negates the lane exactly");
    }

    // --- output routing
    {
        const Render toAux = rendered({"Output=0", "L1 Output=Aux"}, hit);
        check(silent(toAux.left) && toAux.auxL[0] == unity.left && auxSilent(toAux, 0),
              "Output Aux: only lane 1's port carries the hit");
        const Render both = rendered({"Output=0", "L1 Output=Main+Aux"}, hit);
        check(both.left == unity.left && both.auxL[0] == unity.left && auxSilent(both, 0),
              "Output Main+Aux: main and lane 1's port carry the same hit");
        check(auxSilent(unity, -1), "Output Main: every aux port is silent");
        const Render lane3 = rendered({"Output=0", "L3 On=On", "L3 Output=Aux"}, hit);
        check(lane3.left == unity.left && !silent(lane3.auxL[2]) && auxSilent(lane3, 2),
              "lane 3 reaches the third aux port and leaves the main output alone");
        const Render quiet = rendered({"Output=-12", "L1 Output=Main+Aux"}, hit);
        check(quiet.auxL[0] == unity.left, "the master level does not touch the aux ports");
    }

    // --- pitch link and transpose
    {
        auto settles = [&](const std::vector<std::string>& specs, double expect, const std::string& what) {
            const Render r = rendered(specs, hit);
            const double f = measureFrequency(r.left, rate, 0.30, 0.45);
            check(std::fabs(f / expect - 1.0) < 0.015, what + ": " + std::to_string(f) + " Hz");
        };
        const std::vector<std::string> l2 = {"L1 On=Off", "L2 On=On", "L2 Source=Body", "L2 Pitch End=200",
                                             "L2 Transpose=12"};
        auto with = [&](std::vector<std::string> v, const std::string& extra) {
            v.push_back(extra);
            return v;
        };
        settles(l2, 400.0, "own pitch, an octave up");
        settles(with(l2, "L2 Pitch Link=Lane 1"), 96.0, "linked to lane 1 (48 Hz), an octave up");
        auto linked = with(with(l2, "L2 Pitch Link=Lane 1"), "L1 Transpose=7");
        settles(linked, 96.0 * std::exp2(7.0 / 12.0), "and lane 1 transposed by a fifth");
        settles(with(l2, "L2 Pitch Link=Lane 2"), 400.0, "a link to itself is its own pitch");
    }

    // --- resonator tuning: key tracking and pitch link
    {
        const std::vector<std::string> res = {"L1 Source=Resonator",     "L1 Resonator Model=Harmonic",
                                              "L1 Resonator Modes=2",    "L1 Resonator Brightness=0",
                                              "L1 Resonator Damping=100", "L1 Resonator Tune=100",
                                              "L1 Resonator Decay=2 s",  "L1 Resonator Key Track=100"};
        Render r = rendered(res, hit);
        double f = measureFrequency(r.left, rate, 0.30, 0.45);
        check(std::fabs(f / 100.0 - 1.0) < 0.01, "resonator rings at Tune: " + std::to_string(f) + " Hz");
        EventList up;
        up.add(noteOn(lead, 48, 1.0));
        r = rendered(res, up);
        f = measureFrequency(r.left, rate, 0.30, 0.45);
        check(std::fabs(f / 200.0 - 1.0) < 0.01, "an octave up with full key tracking: " + std::to_string(f) + " Hz");
        r = rendered({"L1 On=Off", "L4 On=On", "L4 Resonator Model=Harmonic", "L4 Resonator Modes=2",
                      "L4 Resonator Brightness=0", "L4 Resonator Damping=100", "L4 Resonator Decay=2 s",
                      "L4 Pitch Link=Lane 1"},
                     hit);
        f = measureFrequency(r.left, rate, 0.30, 0.45);
        check(std::fabs(f / 48.0 - 1.0) < 0.01, "linked to lane 1, it rings at its Pitch End: " + std::to_string(f) +
                                                    " Hz");
        r = rendered({"L1 Source=Resonator", "L1 Resonator Drop=12", "L1 Resonator Drop Time=200",
                      "L1 Resonator Model=Harmonic", "L1 Resonator Modes=2", "L1 Resonator Brightness=0",
                      "L1 Resonator Damping=100", "L1 Resonator Tune=100", "L1 Resonator Decay=2 s"},
                     hit);
        const double early = measureFrequency(r.left, rate, lead / rate, lead / rate + 0.02);
        f = measureFrequency(r.left, rate, 0.30, 0.45);
        check(early > 150.0 && std::fabs(f / 100.0 - 1.0) < 0.01,
              "a drop starts high (" + std::to_string(early) + " Hz) and lands on Tune");
    }

    // --- variation: off repeats every hit exactly, on does not
    {
        const uint32_t gap = static_cast<uint32_t>(0.5 * rate);
        EventList two;
        two.add(noteOn(lead, 36, 1.0));
        two.add(noteOn(lead + gap, 36, 1.0));
        auto repeats = [&](const Render& r) {
            for (uint32_t i = 0; i < gap; ++i)
                if (r.left[lead + i] != r.left[lead + gap + i] || r.right[lead + i] != r.right[lead + gap + i])
                    return false;
            return true;
        };
        for (const char* src : {"Noise", "Click"})
        {
            const std::vector<std::string> base = {std::string("L1 Source=") + src};
            check(repeats(rendered(base, two, 1.5)), std::string(src) + " without variation repeats bit for bit");
            check(!repeats(rendered({base[0], "L1 Variation=50"}, two, 1.5)),
                  std::string(src) + " with variation does not");
        }
        check(repeats(rendered({"L1 Drift=100"}, two, 1.5)), "a drifting body without variation repeats bit for bit");
    }

    // --- every lane at once: block size still changes nothing
    {
        const std::vector<std::string> all = {"L2 On=On",       "L3 On=On",       "L4 On=On",     "L2 Delay=3",
                                              "L3 Delay=17",    "L4 Delay=0.5",   "L3 Variation=40",
                                              "L4 Variation=40", "L1 Drift=50",   "L5 On=On",     "L5 Pitch Link=Lane 1",
                                              "L5 Transpose=19", "L5 Wave=Saw",   "L5 Level=-20", "L3 Output=Main+Aux",
                                              "L3 Noise Filter Env=5"};
        EventList many;
        for (int h = 0; h < 6; ++h)
            many.add(noteOn(lead + static_cast<uint32_t>(h * 0.071 * rate), static_cast<int16_t>(36 + h), 0.9));
        const Render a = rendered(all, many, 1.5, 512);
        const Render b = rendered(all, many, 1.5, 37);
        check(!a.sawNonFinite && a.left == b.left && a.right == b.right && a.auxL[2] == b.auxL[2],
              "five lanes with delays and variation: bit-identical with a block size of 37");
    }


    // --- Tune: the whole kick, every lane, once
    {
        const Render up = rendered({"Output=0", "Tune=+12"}, hit);
        const Render down = rendered({"Output=0", "Tune=-2.5"}, hit);
        const double fu = measureFrequency(up.left, rate, 0.30, 0.45);
        const double fd = measureFrequency(down.left, rate, 0.30, 0.45);
        check(std::fabs(fu / 96.0 - 1.0) < 0.015 && std::fabs(fd / (48.0 * std::exp2(-2.5 / 12.0)) - 1.0) < 0.015,
              "Tune moves the body: +12 st to " + std::to_string(fu) + " Hz, -2.5 st to " + std::to_string(fd) + " Hz");
        // A resonator linked to lane 1 follows the tuned pitch, shifted once.
        const Render linked = rendered({"Output=0", "L1 Output=Aux", "L4 On=On", "L4 Pitch Link=Lane 1",
                                        "L4 Resonator Decay=2 s", "Tune=+7"},
                                       hit, 1.5);
        const double fl = measureFrequency(linked.left, rate, 0.5, 0.8);
        check(std::fabs(fl / (48.0 * std::exp2(7.0 / 12.0)) - 1.0) < 0.015,
              "a lane linked to lane 1 is tuned once with it: " + std::to_string(fl) + " Hz");
    }

    // ======================================================== effect slots

    // --- a slot's A-F are named, shown and defaulted by its type
    {
        p = configured(m, {"L1 Slot 1 Type=Clipper"});
        params = paramsOf(p);
        auto text = [&](const char* name) -> std::string {
            clap_param_info_t info{};
            if (!findParam(p, name, info))
                return "(missing)";
            double v = 0.0;
            char t[256];
            params->get_value(p, info.id, &v);
            params->value_to_text(p, info.id, v, t, sizeof(t));
            return t;
        };
        clap_param_info_t none{};
        check(text("L1 Slot 1 Drive") == "+6.0 dB" && text("L1 Slot 1 Knee") == "30 %" &&
                  text("L1 Slot 1 Ceiling") == "+0.0 dB" && !findParam(p, "L1 Slot 1 A", none),
              "a Clipper slot's letters are Drive, Knee and Ceiling, at their defaults");
        EventList set;
        clap_param_info_t type{};
        findParam(p, "L1 Slot 1 Type", type);
        double filter = 0.0;
        params->text_to_value(p, type.id, "Filter", &filter);
        set.add(paramValue(type.id, filter));
        flushParams(p, set);
        check(text("L1 Slot 1 Mode") == "LP 24" && text("L1 Slot 1 Cutoff") == "2.00 kHz" &&
                  !(type.flags & CLAP_PARAM_IS_AUTOMATABLE),
              "switched to Filter they become Mode and Cutoff, reset to its defaults; Type is not automatable");
        p->destroy(p);
    }

    // --- every type's letters read back what they show
    {
        static const char* kTypes[] = {"Distortion", "Clipper", "Wavefolder", "Bitcrush", "Filter", "EQ",
                                       "Compressor", "Transient", "Gate",  "Reverb",   "Delay",  "Warp",
                                       "Smear",      "Ring Mod",  "Stereo", "Utility", "Limiter"};
        bool ok = true;
        for (const char* type : kTypes)
        {
            p = configured(m, {std::string("L1 Slot 1 Type=") + type});
            params = paramsOf(p);
            for (int k = 0; k < 6; ++k)
            {
                const clap_id id = 1604 + static_cast<clap_id>(k);
                for (int s = 0; s <= 2000; ++s)
                {
                    const double x = s / 2000.0;
                    char text[256], again[256];
                    double back = 0.0;
                    if (!params->value_to_text(p, id, x, text, sizeof(text)) ||
                        !params->text_to_value(p, id, text, &back) ||
                        !params->value_to_text(p, id, back, again, sizeof(again)) || std::strcmp(text, again) != 0)
                    {
                        ok = false;
                        std::printf("    %s letter %c at %g: '%s' -> %g\n", type, 'A' + k, x, text, back);
                        break;
                    }
                }
            }
            p->destroy(p);
        }
        check(ok, "every slot type's A-F round-trip through their text");
    }

    // --- state: letters are saved by name and unit, and come back
    {
        const std::vector<std::string> setup = {
            "L1 Slot 1 Type=Distortion", "L1 Slot 1 Model=Germanium", "L1 Slot 1 Drive=73",
            "L1 Slot 2 Type=Filter",     "L1 Slot 2 Mode=Notch",      "L1 Slot 2 Cutoff=345",
            "L1 Slot 2 Band=Mid+High",   "L1 Slot 3 Type=Gate",       "L1 Slot 3 Mode=Hit",
            "L1 Slot 3 Range=-inf",      "Master Slot 6 Type=EQ",     "Master Slot 6 Tilt=-4.5",
            "Mono Below=120",            "Output Clip=Soft",          "Quality=4x"};
        p = configured(m, setup);
        const auto* state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
        std::string blob;
        clap_ostream_t os{&blob, [](const clap_ostream_t* st, const void* buf, uint64_t size) -> int64_t {
                              static_cast<std::string*>(st->ctx)->append(static_cast<const char*>(buf), size);
                              return static_cast<int64_t>(size);
                          }};
        state->save(p, &os);
        check(blob.find("l1.slot1.model=Germanium\n") != std::string::npos &&
                  blob.find("l1.slot2.cutoff=345\n") != std::string::npos &&
                  blob.find("master.slot6.tilt=-4.5\n") != std::string::npos,
              "slot letters are saved under their names, in their units");
        struct Reader
        {
            const std::string* s;
            size_t pos;
        } reader{&blob, 0};
        clap_istream_t is{&reader, [](const clap_istream_t* st, void* buf, uint64_t size) -> int64_t {
                              auto* r = static_cast<Reader*>(st->ctx);
                              const size_t n = std::min<size_t>(size, r->s->size() - r->pos);
                              std::memcpy(buf, r->s->data() + r->pos, n);
                              r->pos += n;
                              return static_cast<int64_t>(n);
                          }};
        const clap_plugin_t* q = createPlugin(m);
        static_cast<const clap_plugin_state_t*>(q->get_extension(q, CLAP_EXT_STATE))->load(q, &is);
        bool same = true;
        const clap_plugin_params_t* pp = paramsOf(p);
        const clap_plugin_params_t* qp = paramsOf(q);
        for (uint32_t i = 0; i < pp->count(p); ++i)
        {
            clap_param_info_t a{}, b{};
            pp->get_info(p, i, &a);
            qp->get_info(q, i, &b);
            double va = 0.0, vb = 0.0;
            char ta[256], tb[256];
            pp->get_value(p, a.id, &va);
            qp->get_value(q, b.id, &vb);
            pp->value_to_text(p, a.id, va, ta, sizeof(ta));
            qp->value_to_text(q, b.id, vb, tb, sizeof(tb));
            if (std::strcmp(a.name, b.name) != 0 || std::strcmp(ta, tb) != 0)
            {
                same = false;
                std::printf("    %s = %s -> %s = %s\n", a.name, ta, b.name, tb);
            }
        }
        check(same, "a state with slots in it loads back to the same names and values");
        q->destroy(q);
        p->destroy(p);
    }

    // --- every type at its defaults and at both ends of every letter, on a
    //     full band and on a split one: finite, bounded, asleep at the end
    {
        static const char* kTypes[] = {"Distortion", "Clipper", "Wavefolder", "Bitcrush", "Filter", "EQ",
                                       "Compressor", "Transient", "Gate",  "Reverb",   "Delay",  "Warp",
                                       "Smear",      "Ring Mod",  "Stereo", "Utility", "Limiter"};
        int bad = 0, runs = 0;
        for (const char* type : kTypes)
            for (const char* band : {"Full", "Low+Mid"})
                for (const char* end : {"", "0", "1"})
                {
                    p = configured(m, {"L2 On=On", std::string("L1 Slot 1 Type=") + type,
                                       std::string("L1 Slot 1 Band=") + band, std::string("Master Slot 1 Type=") + type});
                    if (*end)
                    {
                        EventList set;
                        for (clap_id id : {1604u, 1605u, 1606u, 1607u, 1608u, 1609u, 9004u, 9005u, 9006u, 9007u,
                                           9008u, 9009u})
                            set.add(paramValue(id, *end == '0' ? 0.0 : 1.0));
                        flushParams(p, set);
                    }
                    // The time effects' tails take longer, the master's
                    // copy of the effect included.
                    const bool echoes = std::strcmp(type, "Delay") == 0 || std::strcmp(type, "Warp") == 0;
                    const bool timed = echoes || std::strcmp(type, "Reverb") == 0;
                    const Render r = render(p, rate, 512, static_cast<uint32_t>((timed ? 12.0 : 4.0) * rate), hit);
                    p->destroy(p);
                    ++runs;
                    // Every output and makeup gain is at its top at "1", on
                    // the lane and on the master, so only the other two
                    // settings have a level to keep. A delay adds its echoes
                    // to the hit, on the lane and again on the master, so it
                    // may reach twice as high. At the top of their ranges the
                    // delays and the reverb ring for longer than the render.
                    const bool rings = *end == '1' && timed;
                    const float limit = echoes ? 4.0f : 2.0f;
                    if (r.sawNonFinite || (*end != '1' && r.peak > limit) || (!r.sleptAtEnd && !rings))
                    {
                        ++bad;
                        std::printf("    %s on %s, letters at %s: peak %g%s%s\n", type, band, *end ? end : "defaults",
                                    r.peak, r.sawNonFinite ? ", non-finite" : "", r.sleptAtEnd ? "" : ", never sleeps");
                    }
                }
        check(bad == 0, std::to_string(runs) + " slot settings finite, asleep at the end, and under +6 dBFS unless turned up");
    }

    // --- mix 0 and bypass leave the lane untouched, to the bit
    {
        const Render dry = rendered({"Output=0", "L1 Slot 1 Type=Distortion", "L1 Slot 1 Drive=100",
                                     "L1 Slot 1 Mix=0"},
                                    hit);
        const Render bypassed = rendered({"Output=0", "L1 Slot 1 Type=Wavefolder", "L1 Slot 1 Drive=30",
                                          "L1 Slot 1 Bypass=On"},
                                         hit);
        check(dry.left == unity.left && bypassed.left == unity.left, "mix 0 % and bypass are bit-transparent");
    }

    // --- the band split is flat, and a band can be taken out on its own
    {
        // A single-sample click, so the response is the chain's own.
        const std::vector<std::string> impulse = {"Output=0", "L1 Source=Click", "L1 Click Type=Impulse",
                                                  "L1 Click Filter=Off"};
        auto with = [&](std::vector<std::string> v, std::initializer_list<const char*> extra) {
            for (const char* e : extra)
                v.push_back(e);
            return v;
        };
        auto response = [&](const Render& r, double f) { return responseDb(r.left, rate, f, lead, lead + 24000); };
        // Crossovers close together, where the low band's allpass matters.
        const Render flat = rendered(with(impulse, {"L1 Slot 1 Type=EQ", "L1 Slot 1 Band=Mid",
                                                    "L1 Crossover Low=400", "L1 Crossover High=800"}),
                                     hit);
        double worst = 0.0;
        for (double f : {40.0, 300.0, 400.0, 560.0, 800.0, 1200.0, 9000.0})
            worst = std::max(worst, std::fabs(response(flat, f)));
        check(worst < 0.01, "low + mid + high around a neutral slot is flat (worst " + std::to_string(worst) + " dB)");

        // A gate that nothing here can open: the click's band parts stay well
        // under full scale.
        const std::vector<std::string> mute = {"L1 Slot 1 Type=Gate", "L1 Slot 1 Threshold=0", "L1 Slot 1 Range=-inf"};
        const Render shut = rendered(with(impulse, {"L1 Slot 1 Type=Gate", "L1 Slot 1 Threshold=0",
                                                    "L1 Slot 1 Range=-inf", "L1 Slot 1 Band=Mid"}),
                                     hit);
        check(response(shut, 600.0) < -30.0, "a gate in Gate mode ignores the hits and stays shut");
        auto muted = [&](const char* band) {
            std::vector<std::string> v = impulse;
            v.insert(v.end(), mute.begin(), mute.end());
            v.push_back(std::string("L1 Slot 1 Band=") + band);
            return rendered(v, hit);
        };
        const Render noLow = muted("Low"), noHigh = muted("High");
        check(response(noLow, 40.0) < -30.0 && std::fabs(response(noLow, 9000.0)) < 0.5 &&
                  response(noHigh, 9000.0) < -30.0 && std::fabs(response(noHigh, 40.0)) < 0.5,
              "a closed gate on Low removes the lows only, on High the highs only (" +
                  std::to_string(response(noLow, 40.0)) + " / " + std::to_string(response(noHigh, 9000.0)) + " dB)");
    }

    // --- oversampling: a hard clip on a 1.9 kHz tone aliases less at 4x
    {
        auto aliasing = [&](const char* quality) {
            const Render r = rendered({"Output=0", std::string("Quality=") + quality, "L1 Pitch Start=1900",
                                       "L1 Pitch End=1900", "L1 Body Hold=1 s", "L1 Slot 1 Type=Clipper",
                                       "L1 Slot 1 Drive=24", "L1 Slot 1 Knee=0", "L1 Slot 1 Ceiling=-6"},
                                      hit);
            const size_t a = lead + 4800, b = a + 16384;
            // 1900 Hz and every rate here are multiples of 100 Hz, so every
            // alias lands on a multiple of 100 Hz: sum the ones that are not
            // harmonics, below 19 kHz.
            double power = 0.0;
            for (int k = 1; k <= 190; ++k)
                if (k % 19 != 0)
                    power += std::pow(10.0, toneDb(r.left, rate, 100.0 * k, a, b) / 10.0);
            return 10.0 * std::log10(power) - toneDb(r.left, rate, 1900.0, a, b);
        };
        const double x1 = aliasing("1x"), x2 = aliasing("2x"), x4 = aliasing("4x");
        // At 2x a hard clip still folds its upper harmonics inside the doubled
        // rate; 4x is what clears them.
        check(x2 < x1 - 6.0 && x4 < x2 - 15.0,
              "aliasing of a clipped tone: 1x " + std::to_string(x1) + ", 2x " + std::to_string(x2) + ", 4x " +
                  std::to_string(x4) + " dB");
    }

    // --- the effects do what they say
    {
        // A held body: a steady 48 Hz at full scale from 0.3 s on.
        const size_t a = static_cast<size_t>(0.30 * rate), b = static_cast<size_t>(0.45 * rate);
        const Render held = rendered({"Output=0", "L1 Body Hold=1 s"}, hit);
        const double clean = toneDb(held.left, rate, 48.0, a, b);
        Render r = rendered({"Output=0", "L1 Body Hold=1 s", "L1 Slot 1 Type=EQ", "L1 Slot 1 Mid=12", "L1 Slot 1 Mid Freq=48",
                             "L1 Slot 1 Mid Q=1"},
                            hit);
        const double eq = toneDb(r.left, rate, 48.0, a, b) - clean;
        check(std::fabs(eq - 12.0) < 0.3, "EQ: +12 dB at 48 Hz lifts the body's tail by " + std::to_string(eq) + " dB");

        r = rendered({"Output=0", "L1 Body Hold=1 s", "L1 Slot 1 Type=Compressor", "L1 Slot 1 Threshold=-30",
                      "L1 Slot 1 Ratio=20", "L1 Slot 1 Attack=0.05", "L1 Slot 1 Knee=0"},
                     hit);
        const double squash = peakDb(held.left, a, b) - peakDb(r.left, a, b);
        check(squash > 20.0, "Compressor: 20:1 at -30 dB takes " + std::to_string(squash) + " dB off the tail");

        r = rendered({"Output=0", "L1 Slot 1 Type=Gate", "L1 Slot 1 Mode=Hit", "L1 Slot 1 Attack=0",
                      "L1 Slot 1 Hold=20", "L1 Slot 1 Release=10", "L1 Slot 1 Range=-inf"},
                     hit);
        const double gated = peakDb(r.left, lead + static_cast<size_t>(0.08 * rate), lead + static_cast<size_t>(0.2 * rate));
        check(gated < -80.0 && peakDb(r.left, lead, lead + 500) > -10.0,
              "Gate in Hit mode cuts the body after hold and release (" + std::to_string(gated) + " dB at 80 ms)");

        const Render punch = rendered({"Output=0", "L1 Slot 1 Type=Transient", "L1 Slot 1 Attack=100"}, hit);
        const auto contrast = [&](const Render& x) {
            return peakDb(x.left, lead, lead + 240) - peakDb(x.left, lead + 4800, lead + 7200);
        };
        check(contrast(punch) > contrast(unity) + 3.0,
              "Transient at +100 % attack lifts the onset against the body by " +
                  std::to_string(contrast(punch) - contrast(unity)) + " dB");

        r = rendered({"Output=0", "L1 Source=Noise", "L1 Noise Filter=Off", "L1 Slot 1 Type=Filter",
                      "L1 Slot 1 Cutoff=100"},
                     hit);
        const Render open = rendered({"Output=0", "L1 Source=Noise", "L1 Noise Filter=Off"}, hit);
        const double cut = toneDb(open.left, rate, 5000.0, lead, lead + 4096) - toneDb(r.left, rate, 5000.0, lead, lead + 4096);
        check(cut > 60.0, "Filter LP 24 at 100 Hz takes " + std::to_string(cut) + " dB off noise at 5 kHz");

        r = rendered({"Output=0", "Quality=1x", "L1 Slot 1 Type=Bitcrush", "L1 Slot 1 Bits=3"}, hit);
        bool steps = true;
        for (float x : r.left)
            steps &= std::fabs(x * 4.0f - std::round(x * 4.0f)) < 1e-5f;
        check(steps && !silent(r.left), "Bitcrush at 3 bits leaves only multiples of 1/4");

        static const char* kModels[] = {"Soft Clip", "Overdrive", "Tube", "Valve Stack", "Fuzz",
                                        "Rectifier", "Crush",     "Germanium", "Crunch",  "Lead"};
        const double cleanHarm = std::max(toneDb(held.left, rate, 96.0, a, b), toneDb(held.left, rate, 144.0, a, b)) - clean;
        int flat = 0;
        for (const char* model : kModels)
        {
            r = rendered({"Output=0", "L1 Body Hold=1 s", "L1 Slot 1 Type=Distortion",
                          std::string("L1 Slot 1 Model=") + model, "L1 Slot 1 Drive=100"},
                         hit);
            const double harm =
                std::max(toneDb(r.left, rate, 96.0, a, b), toneDb(r.left, rate, 144.0, a, b)) - toneDb(r.left, rate, 48.0, a, b);
            if (r.sawNonFinite || harm < cleanHarm + 20.0)
            {
                ++flat;
                std::printf("    %s: harmonics %.1f dB under the fundamental\n", model, -harm);
            }
        }
        check(flat == 0, "every distortion model puts harmonics on the body's tail");

        r = rendered({"Output=0", "L1 Body Hold=1 s", "L1 Slot 1 Type=Wavefolder", "L1 Slot 1 Drive=24",
                      "L1 Slot 1 Shape=100"},
                     hit);
        check(!r.sawNonFinite && r.peak < 1.2f && toneDb(r.left, rate, 144.0, a, b) - toneDb(r.left, rate, 48.0, a, b) > -20.0,
              "Wavefolder folds rather than clips: bounded, with strong odd harmonics");
    }

    // --- a hit reaches the slots on its own sample, delay included
    {
        // The body starts at its peak; the gate's 5 ms attack has to start on
        // the same sample, so the first sample is a 240th of it.
        const std::vector<std::string> gate = {"Output=0", "L1 Body Phase=90", "L1 Slot 1 Type=Gate",
                                               "L1 Slot 1 Mode=Hit", "L1 Slot 1 Attack=5", "L1 Slot 1 Range=-inf",
                                               "L1 Delay=10"};
        const Render r = rendered(gate, hit);
        long first = -1;
        for (size_t i = 0; i < r.left.size() && first < 0; ++i)
            if (r.left[i] != 0.0f)
                first = static_cast<long>(i);
        const float start = first >= 0 ? std::fabs(r.left[static_cast<size_t>(first)]) : 0.0f;
        check(first == static_cast<long>(lead + static_cast<uint32_t>(0.010 * rate)) && start < 0.01f,
              "a Hit gate opens on the delayed hit's sample (" + std::to_string(first) + ", first sample " +
                  std::to_string(start) + ")");
    }

    // --- a chain rings past its voices, then the lane goes to sleep
    {
        const std::vector<std::string> ring = {"L1 Source=Click", "L1 Click Type=Impulse", "L1 Click Filter=Off",
                                               "L1 Slot 1 Type=Filter", "L1 Slot 1 Mode=LP 12",
                                               "L1 Slot 1 Cutoff=60", "L1 Slot 1 Reso=100"};
        const Render r = rendered(ring, hit, 3.0);
        const Render odd = rendered(ring, hit, 3.0, 37);
        check(peakDb(r.left, lead + 4800, lead + 9600) > -60.0 && r.sleptAtEnd && odd.left == r.left,
              "a resonant filter rings on after a click, ends, sleeps, and ends on the same sample at any block size");
    }

    // --- the master: its chain, mono below and the output clip
    {
        const Render ceiling = rendered({"Output=0", "L1 Output=Main+Aux", "Master Slot 1 Type=Clipper",
                                         "Master Slot 1 Drive=0", "Master Slot 1 Knee=0", "Master Slot 1 Ceiling=-12",
                                         "Quality=1x"},
                                        hit);
        check(ceiling.peak <= 0.2512f && ceiling.auxL[0] == unity.left,
              "a master clipper at -12 dB holds the main output there and leaves the lane outputs alone");

        // Held, so the crossover's group delay does not move a decaying tail.
        const Render hardLeft = rendered({"Output=0", "L1 Body Hold=1 s", "L1 Pan=L100"}, hit);
        const Render mono = rendered({"Output=0", "L1 Body Hold=1 s", "L1 Pan=L100", "Mono Below=200"}, hit);
        const size_t a = static_cast<size_t>(0.30 * rate), b = static_cast<size_t>(0.45 * rate);
        const double before = toneDb(hardLeft.left, rate, 48.0, a, b);
        check(silent(hardLeft.right) && std::fabs(toneDb(mono.left, rate, 48.0, a, b) - (before - 6.02)) < 0.3 &&
                  std::fabs(toneDb(mono.right, rate, 48.0, a, b) - (before - 6.02)) < 0.3,
              "Mono Below 200 Hz puts a hard-left 48 Hz body in the middle, 6 dB down on each side");

        for (const char* clip : {"Hard", "Soft"})
        {
            const Render hot = rendered({"Output=0", "L1 Level=+12", "Quality=1x", std::string("Output Clip=") + clip}, hit);
            check(hot.peak <= 1.0f && hot.peak > 0.9f, std::string("Output Clip ") + clip + " holds a +12 dB hit at full scale");
        }
    }

    // ================================================ time effects and the rest

    // A single-sample click, so what comes out is the chain's own response.
    const std::vector<std::string> impulse = {"Output=0", "L1 Source=Click", "L1 Click Type=Impulse",
                                              "L1 Click Filter=Off"};
    auto plus = [](std::vector<std::string> v, std::initializer_list<std::string> extra) {
        v.insert(v.end(), extra.begin(), extra.end());
        return v;
    };
    // The first sample at or after `from` whose magnitude passes `level`.
    auto onset = [](const std::vector<float>& x, size_t from, float level) -> long {
        for (size_t i = from; i < x.size(); ++i)
            if (std::fabs(x[i]) >= level)
                return static_cast<long>(i);
        return -1;
    };
    const Render click = rendered(impulse, hit);
    const long clickAt = onset(click.left, 0, 1e-6f);
    const float clickPeak = click.peak;

    // --- Delay: the hit stays, the echoes follow at the time, the next one
    //     down by the feedback; Sync follows the host's tempo
    {
        const Render r = rendered(plus(impulse, {"L1 Slot 1 Type=Delay", "L1 Slot 1 Time=100", "L1 Slot 1 Feedback=50",
                                                 "L1 Slot 1 Color=0"}),
                                  hit, 1.0);
        const size_t d = 4800, at = static_cast<size_t>(clickAt);
        bool dry = true;
        for (size_t i = 0; i < at + d - 10; ++i)
            dry &= r.left[i] == click.left[i];
        const double first = peakDb(r.left, at + d - 10, at + d + 10) - 20.0 * std::log10(clickPeak);
        const double second = peakDb(r.left, at + 2 * d - 10, at + 2 * d + 10) - 20.0 * std::log10(clickPeak);
        check(dry && std::fabs(first) < 0.01 && std::fabs(second + 6.02) < 0.2 &&
                  onset(r.left, at + 10, clickPeak * 0.5f) == static_cast<long>(at + d),
              "Delay 100 ms: the hit untouched, an echo at +4800 samples (" + std::to_string(first) +
                  " dB) and one at 50 % feedback (" + std::to_string(second) + " dB)");

        gTransportTempo = 140.0;
        const Render synced = rendered(plus(impulse, {"L1 Slot 1 Type=Delay", "L1 Slot 1 Sync=1/8",
                                                      "L1 Slot 1 Feedback=0", "L1 Slot 1 Color=0"}),
                                       hit, 1.0);
        gTransportTempo = 0.0;
        const long echo = onset(synced.left, at + 10, clickPeak * 0.5f);
        check(echo == static_cast<long>(at + std::lround(0.5 * 60.0 / 140.0 * rate)),
              "Delay synced to 1/8 at 140 BPM echoes after " + std::to_string(echo - static_cast<long>(at)) +
                  " samples");

        // A long gap between echoes is not the end of the tail.
        const std::vector<std::string> slow = plus(impulse, {"L1 Slot 1 Type=Delay", "L1 Slot 1 Time=600",
                                                             "L1 Slot 1 Feedback=40", "L1 Slot 1 Color=-50"});
        const Render a = rendered(slow, hit, 8.0), b = rendered(slow, hit, 8.0, 37);
        const size_t e3 = at + 3 * 28800;
        check(peakDb(a.left, e3 - 200, e3 + 200) > -40.0 && a.sleptAtEnd && a.left == b.left,
              "a 600 ms delay keeps ringing across its gaps, then sleeps; the same at a block size of 37");

        const Render pp = rendered(plus(impulse, {"L1 Slot 1 Type=Delay", "L1 Slot 1 Time=100",
                                                  "L1 Slot 1 Feedback=50", "L1 Slot 1 Color=0",
                                                  "L1 Slot 1 Ping-Pong=100"}),
                                   hit, 1.0);
        check(peakDb(pp.left, at + d - 10, at + d + 10) > -1.0 && peakDb(pp.right, at + d - 10, at + d + 10) < -100.0 &&
                  peakDb(pp.right, at + 2 * d - 10, at + 2 * d + 10) > -7.0 &&
                  peakDb(pp.left, at + 2 * d - 10, at + 2 * d + 10) < -100.0,
              "Ping-Pong 100 %: the first echo on the left, the second on the right");
    }

    // --- Reverb: decays at the time it is given, stereo by Width
    {
        auto level = [&](const Render& r, double t0, double t1) {
            const size_t a = static_cast<size_t>(clickAt) + static_cast<size_t>(t0 * rate);
            const size_t b = static_cast<size_t>(clickAt) + static_cast<size_t>(t1 * rate);
            double e = 0.0;
            for (size_t i = a; i < b; ++i)
                e += static_cast<double>(r.left[i]) * r.left[i] + static_cast<double>(r.right[i]) * r.right[i];
            return 10.0 * std::log10(e / static_cast<double>(b - a) + 1e-300);
        };
        const Render r = rendered(plus(impulse, {"L1 Slot 1 Type=Reverb", "L1 Slot 1 Decay=1000",
                                                 "L1 Slot 1 Damping=20000"}),
                                  hit, 2.0);
        // 60 dB per second: 18 dB between 0.1 s and 0.4 s.
        const double fall = level(r, 0.1, 0.15) - level(r, 0.4, 0.45);
        check(std::fabs(fall / 18.0 - 1.0) < 0.2, "Reverb at 1 s falls " + std::to_string(fall) + " dB from 0.1 s to 0.4 s");
        double side = 0.0;
        for (size_t i = 0; i < r.left.size(); ++i)
            side = std::max(side, static_cast<double>(std::fabs(r.left[i] - r.right[i])));
        const Render mono = rendered(plus(impulse, {"L1 Slot 1 Type=Reverb", "L1 Slot 1 Width=0"}), hit, 2.0);
        check(side > 0.01 && mono.left == mono.right && r.sleptAtEnd,
              "a centred click comes out of the reverb wide, and mono at Width 0; it sleeps after the tail");

        const Render pre = rendered(plus(impulse, {"L1 Slot 1 Type=Reverb", "L1 Slot 1 Pre-Delay=50"}), hit, 2.0);
        const long first = onset(pre.left, 0, 1e-6f);
        check(first >= clickAt + 2400, "Pre-Delay 50 ms: nothing for the first " +
                                             std::to_string(first - clickAt) + " samples");

        // The hit keeps about its level through a full-wet reverb.
        const Render kick = rendered({"Output=0", "L1 Slot 1 Type=Reverb"}, hit, 3.0);
        double eIn = 0.0, eOut = 0.0;
        for (size_t i = 0; i < kick.left.size(); ++i)
        {
            eIn += 2.0 * static_cast<double>(unity.left[std::min(i, unity.left.size() - 1)]) *
                   (i < unity.left.size() ? unity.left[i] : 0.0f);
            eOut += static_cast<double>(kick.left[i]) * kick.left[i] + static_cast<double>(kick.right[i]) * kick.right[i];
        }
        const double gain = 10.0 * std::log10(eOut / eIn);
        check(std::fabs(gain) < 6.0, "the default kick through the default reverb keeps its energy within 6 dB (" +
                                         std::to_string(gain) + " dB)");
    }

    // --- Smear spreads a hit without a tail; it is an allpass, so the
    //     energy stays
    {
        const Render r = rendered(plus(impulse, {"L1 Slot 1 Type=Smear", "L1 Slot 1 Time=10",
                                                 "L1 Slot 1 Stages=16"}),
                                  hit, 1.0);
        double eIn = 0.0, eOut = 0.0;
        for (size_t i = 0; i < r.left.size(); ++i)
        {
            eIn += static_cast<double>(click.left[i]) * click.left[i];
            eOut += static_cast<double>(r.left[i]) * r.left[i];
        }
        check(std::fabs(10.0 * std::log10(eOut / eIn)) < 0.05 && r.peak < 0.5f * clickPeak && r.sleptAtEnd,
              "Smear keeps the energy (" + std::to_string(10.0 * std::log10(eOut / eIn)) +
                  " dB), lowers the peak to " + std::to_string(r.peak / clickPeak) + " of it, and stops");
    }

    // --- Ring Mod and the shifts on a held 48 Hz body
    {
        const size_t a = static_cast<size_t>(0.30 * rate), b = static_cast<size_t>(0.45 * rate);
        auto body = [&](const char* mode) {
            return rendered({"Output=0", "L1 Body Hold=1 s", "L1 Slot 1 Type=Ring Mod",
                             std::string("L1 Slot 1 Mode=") + mode, "L1 Slot 1 Frequency=200"},
                            hit);
        };
        const Render ring = body("Ring"), up = body("Shift Up"), down = body("Shift Down");
        auto tone = [&](const Render& r, double f) { return toneDb(r.left, rate, f, a, b); };
        check(tone(ring, 152.0) > -10.0 && tone(ring, 248.0) > -10.0 && tone(ring, 48.0) < -40.0,
              "Ring Mod at 200 Hz turns 48 Hz into 152 and 248 Hz");
        // Shifted down by more than itself, 48 Hz folds over to 152 Hz.
        check(tone(up, 248.0) - tone(up, 152.0) > 30.0 && tone(down, 152.0) - tone(down, 248.0) > 30.0 &&
                  std::fabs(tone(up, 248.0)) < 1.0,
              "Shift Up moves 48 Hz to 248 Hz (" + std::to_string(tone(up, 248.0) - tone(up, 152.0)) +
                  " dB over the mirror), Shift Down to -152 Hz (" +
                  std::to_string(tone(down, 152.0) - tone(down, 248.0)) + " dB)");
    }

    // --- Stereo and Utility
    {
        // Pan comes after the chain, so a stereo source is what the slots see
        // as stereo: noise at full width.
        const std::vector<std::string> wide = {"Output=0", "L1 Source=Noise", "L1 Noise Width=100"};
        const Render noise = rendered(wide, hit);
        const Render narrow = rendered(plus(wide, {"L1 Slot 1 Type=Stereo", "L1 Slot 1 Width=0"}), hit);
        const Render haas = rendered({"Output=0", "L1 Slot 1 Type=Stereo", "L1 Slot 1 Haas=10"}, hit);
        // (+10 ms is a hair under 480 samples once it has been through the
        // parameter's normalised value.)
        float off480 = 0.0f;
        for (size_t i = 480; i < haas.right.size(); ++i)
            off480 = std::max(off480, std::fabs(haas.right[i] - unity.right[i - 480]));
        check(narrow.left == narrow.right && !silent(narrow.left) && off480 < 1e-4f && haas.left == unity.left,
              "Stereo: Width 0 is mono, Haas +10 ms delays the right side by 480 samples");

        const Render inv = rendered({"Output=0", "L1 Slot 1 Type=Utility", "L1 Slot 1 Polarity=Invert"}, hit);
        const Render off = rendered({"Output=0", "L1 Slot 1 Type=Utility", "L1 Slot 1 Gain=-inf"}, hit);
        const Render swapped = rendered(plus(wide, {"L1 Slot 1 Type=Utility", "L1 Slot 1 Channels=Swap"}), hit);
        bool negated = true;
        for (size_t i = 0; i < inv.left.size(); ++i)
            negated &= inv.left[i] == -unity.left[i];
        check(negated && silent(off.left) && swapped.left == noise.right && swapped.right == noise.left,
              "Utility: Invert negates exactly, -inf is silence, Swap swaps the sides");
    }

    // --- Warp: the echoes change pitch, play backwards, or come as taps
    {
        // A short 400 Hz tone; with no feedback the one echo is the tone
        // moved by Pitch, 150 ms later.
        const std::vector<std::string> tone = {"Output=0", "L1 Pitch Start=400", "L1 Pitch End=400",
                                               "L1 Body Hold=100", "L1 Body Decay=5"};
        const Render down = rendered(plus(tone, {"L1 Slot 1 Type=Warp", "L1 Slot 1 Time=150", "L1 Slot 1 Feedback=0",
                                                 "L1 Slot 1 Pitch=-12", "L1 Slot 1 Diffusion=0"}),
                                     hit);
        const size_t a = lead + static_cast<size_t>(0.19 * rate), b = lead + static_cast<size_t>(0.24 * rate);
        const double oct = toneDb(down.left, rate, 200.0, a, b) - toneDb(down.left, rate, 400.0, a, b);
        check(oct > 20.0, "Warp at -12 st: the echo sounds an octave down (" + std::to_string(oct) + " dB over 400 Hz)");

        // Reverse: a click becomes a swell that ends on the window's turn.
        const Render rev = rendered(plus(impulse, {"L1 Slot 1 Type=Warp", "L1 Slot 1 Time=100", "L1 Slot 1 Feedback=0",
                                                   "L1 Slot 1 Pitch=0", "L1 Slot 1 Mode=Reverse",
                                                   "L1 Slot 1 Diffusion=0"}),
                                    hit);
        const Render taps = rendered(plus(impulse, {"L1 Slot 1 Type=Warp", "L1 Slot 1 Time=200", "L1 Slot 1 Feedback=0",
                                                    "L1 Slot 1 Pitch=0", "L1 Slot 1 Mode=Taps",
                                                    "L1 Slot 1 Diffusion=0"}),
                                     hit);
        const size_t at = static_cast<size_t>(clickAt);
        check(!rev.sawNonFinite && peakDb(rev.left, at + 10, at + 9600) > -40.0 && rev.sleptAtEnd,
              "Warp Reverse echoes the click inside two windows and stops");
        check(onset(taps.left, at + 10, 1e-4f) == static_cast<long>(at + 2400) &&
                  onset(taps.right, at + 10, 1e-4f) == static_cast<long>(at + 4800),
              "Warp Taps: the first tap at a quarter of Time on the left, the second at half on the right");
    }

    // --- Bus lanes
    {
        // Lane 1 to its aux only; lane 2 plays it back to the main output.
        const std::vector<std::string> bus = {"Output=0", "L1 Output=Aux", "L2 On=On", "L2 Source=Bus",
                                              "L2 Level=0"};
        const Render post = rendered(bus, hit);
        check(post.left == unity.left && post.right == unity.right && post.auxL[0] == unity.left,
              "a bus lane on lane 1 plays lane 1 exactly");
        const Render muted = rendered(plus(bus, {"L1 Slot 1 Type=Utility", "L1 Slot 1 Gain=-inf"}), hit);
        const Render pre = rendered(plus(bus, {"L1 Slot 1 Type=Utility", "L1 Slot 1 Gain=-inf", "L2 Bus Tap=Pre Chain"}),
                                    hit);
        check(silent(muted.left) && pre.left == unity.left, "Post Chain hears lane 1's chain, Pre Chain hears before it");
        // The other way round: lane 1 reads lane 2, which runs first.
        const Render back = rendered({"Output=0", "L1 Source=Bus", "L1 Bus Lane 1=Off", "L1 Bus Lane 2=On", "L2 On=On",
                                      "L2 Source=Body", "L2 Level=0", "L2 Output=Aux"},
                                     hit);
        check(back.left == unity.left, "a bus lane reads a lane with a higher number in the same block");
        const Render loop = rendered({"Output=0", "L1 Source=Bus", "L1 Bus Lane 2=On", "L2 On=On", "L2 Source=Bus",
                                      "L3 On=On", "L3 Source=Body", "L3 Level=0"},
                                     hit);
        check(!loop.sawNonFinite && loop.left == unity.left && loop.sleptAtEnd,
              "two bus lanes feeding each other hear silence from each other, and sleep");
        // Switched off, a bus lane is silent: it has no voices to choke, so
        // it must stop taking its input.
        const Render busOff = rendered(plus(bus, {"L2 On=Off"}), hit);
        check(silent(busOff.left), "a bus lane switched off plays nothing");
        // Bus lanes summed: lanes 1 and 3 into lane 2.
        const Render sum = rendered({"Output=0", "L1 Output=Aux", "L3 On=On", "L3 Source=Body", "L3 Level=0",
                                     "L3 Output=Aux", "L2 On=On", "L2 Source=Bus", "L2 Bus Lane 3=On", "L2 Level=-6.02"},
                                    hit);
        float worst = 0.0f;
        for (size_t i = 0; i < sum.left.size(); ++i)
            worst = std::max(worst, std::fabs(sum.left[i] - unity.left[i]));
        check(worst < 1e-3f, "a bus lane sums the lanes switched on (two copies at -6 dB give one)");
    }

    // --- the transient guard
    {
        const Render g = rendered({"Output=0", "L1 Guard Delay=20", "L1 Guard Fade=10"}, hit);
        bool shut = true, open = true;
        for (size_t i = 0; i < lead + 960; ++i)
            shut &= g.left[i] == 0.0f;
        for (size_t i = lead + 1440; i < g.left.size(); ++i)
            open &= g.left[i] == unity.left[i];
        check(shut && open && peakDb(g.left, lead + 960, lead + 1440) > -60.0,
              "Guard 20 ms + 10 ms: silent for 960 samples after the hit, fading in, exact from 1440 on");

        // A noise lane ducked by the kick on lane 1.
        auto noise = [&](bool ducked) {
            std::vector<std::string> v = {"Output=0", "L1 Output=Aux", "L2 On=On", "L2 Source=Noise",
                                          "L2 Noise Decay=1 s", "L2 Level=0"};
            if (ducked)
                v.push_back("L2 Duck Source=Lane 1");
            return rendered(v, hit);
        };
        const Render plain = noise(false), ducked = noise(true);
        const double dip = peakDb(plain.left, lead + 100, lead + 1000) - peakDb(ducked.left, lead + 100, lead + 1000);
        const double late = peakDb(plain.left, lead + 33600, lead + 38400) - peakDb(ducked.left, lead + 33600, lead + 38400);
        check(dip > 12.0 && late < dip - 6.0,
              "Duck: the noise drops " + std::to_string(dip) + " dB under the kick and recovers as it fades (" +
                  std::to_string(late) + " dB at 0.7 s)");
    }

    // --- the rumble from the plan: a bus lane through drive, reverb, drive
    //     and a low pass, guarded, under a clean kick
    {
        const std::vector<std::string> rumble = {
            "L1 Slot 1 Type=Clipper",     "L2 On=On",                  "L2 Source=Bus",
            "L2 Slot 1 Type=Distortion",  "L2 Slot 1 Drive=70",        "L2 Slot 2 Type=Reverb",
            "L2 Slot 2 Decay=1500",       "L2 Slot 3 Type=Distortion", "L2 Slot 3 Model=Fuzz",
            "L2 Slot 4 Type=Filter",      "L2 Slot 4 Mode=LP 24",      "L2 Slot 4 Cutoff=120",
            "L2 Slot 5 Type=Gate",        "L2 Slot 5 Mode=Hit",        "L2 Slot 5 Hold=300",
            "L2 Guard Delay=30",          "L2 Guard Fade=40",          "L2 Duck Source=Lane 1",
            "L2 Level=-6",                "L3 On=On",                  "L3 Slot 1 Type=Delay",
            "L3 Slot 1 Sync=1/16",        "L3 Slot 2 Type=Warp",       "L3 Slot 2 Mode=Reverse",
            "L4 On=On",                   "L4 Slot 1 Type=Smear",      "L4 Slot 2 Type=Ring Mod",
            "L4 Slot 3 Type=Stereo",      "L4 Slot 3 Haas=-12",        "Master Slot 1 Type=Reverb",
            "Master Slot 1 Mix=20",       "Master Slot 2 Type=Delay",  "Master Slot 2 Mix=30",
            "L2 Output=Main+Aux"};
        EventList many;
        for (int h = 0; h < 4; ++h)
            many.add(noteOn(lead + static_cast<uint32_t>(h * 0.25 * rate), 36, 0.9));
        const Render a = rendered(rumble, many, 6.0, 512);
        const Render b = rendered(rumble, many, 6.0, 37);
        check(!a.sawNonFinite && a.left == b.left && a.right == b.right && a.auxL[1] == b.auxL[1] && a.sleptAtEnd &&
                  peakDb(a.auxL[1], lead, lead + 1400) < -100.0 && peakDb(a.auxL[1], lead + 4800, lead + 12000) > -40.0,
              "a rumble lane: silent under the click, there in the tail, bit-identical at a block size of 37, asleep");
    }

    // ========================================================== modulation

    {
        const size_t a = static_cast<size_t>(0.30 * rate), b = static_cast<size_t>(0.45 * rate);
        // A macro on the lane's level, full down: the lane is silent at 100 %
        // and as it was at 0 %.
        const std::vector<std::string> macro = {"Output=0", "Mod 1 Source=Macro 1", "Mod 1 Destination=L1 Level",
                                                "Mod 1 Amount=-100"};
        const Render up = rendered(plus(macro, {"Macro 1=100"}), hit);
        const Render down = rendered(plus(macro, {"Macro 1=0"}), hit);
        check(silent(up.left) && down.left == unity.left,
              "a macro routed to a lane's level at -100 %: silent at 100 %, untouched at 0 %");

        // Velocity onto Pitch End: +10 % of its range at full velocity is
        // 48 Hz x 100^0.1 = 76 Hz.
        const std::vector<std::string> vel = {"Output=0", "Mod 1 Source=Velocity", "Mod 1 Destination=L1 Pitch End",
                                              "Mod 1 Amount=10"};
        const Render full = rendered(vel, hit);
        EventList soft;
        soft.add(noteOn(lead, 36, 0.5));
        const Render half = rendered(vel, soft);
        const double ff = measureFrequency(full.left, rate, 0.30, 0.45);
        const double fh = measureFrequency(half.left, rate, 0.30, 0.45);
        check(std::fabs(ff / (48.0 * std::pow(100.0, 0.1)) - 1.0) < 0.015 &&
                  std::fabs(fh / (48.0 * std::pow(100.0, 0.05)) - 1.0) < 0.015,
              "Velocity to Pitch End: " + std::to_string(ff) + " Hz at full velocity, " + std::to_string(fh) +
                  " Hz at half");

        // Note: an octave over the root is +0.5; Random changes with every
        // hit.
        EventList octave;
        octave.add(noteOn(lead, 48, 1.0));
        const Render note = rendered({"Output=0", "Mod 1 Source=Note", "Mod 1 Destination=L1 Pitch End",
                                      "Mod 1 Amount=20"},
                                     octave);
        const double fn = measureFrequency(note.left, rate, 0.30, 0.45);
        check(std::fabs(fn / (48.0 * std::pow(100.0, 0.1)) - 1.0) < 0.015,
              "Note to Pitch End: C3 over a C2 root moves it by half the amount (" + std::to_string(fn) + " Hz)");

        EventList two;
        two.add(noteOn(lead, 36, 1.0));
        two.add(noteOn(lead + 48000, 36, 1.0));
        auto sameHits = [&](const Render& r) {
            for (size_t i = 0; i < 40000; ++i)
                if (r.left[lead + i] != r.left[lead + 48000 + i])
                    return false;
            return true;
        };
        const std::vector<std::string> rnd = {"Output=0", "Mod 1 Source=Random", "Mod 1 Destination=L1 Pitch End",
                                              "Mod 1 Amount=20"};
        check(!sameHits(rendered(rnd, two, 2.0)), "Random gives every hit its own value");

        // A retriggered LFO repeats with every hit; a free one does not (at a
        // rate that is not a whole number of cycles between the hits).
        const std::vector<std::string> lfo = {"Output=0", "LFO 1 Rate=7.3", "Mod 1 Source=LFO 1",
                                              "Mod 1 Destination=L1 Pitch End", "Mod 1 Amount=15"};
        const Render retrig = rendered(lfo, two, 2.0);
        const Render freeRun = rendered(plus(lfo, {"LFO 1 Retrigger=Off"}), two, 2.0);
        const Render odd = rendered(lfo, two, 2.0, 37);
        check(sameHits(retrig) && !sameHits(freeRun) && odd.left == retrig.left && retrig.left != full.left,
              "a retriggered LFO plays every hit alike, a free one does not; bit-identical at a block size of 37");

        // Synced to 1/8 at 120 BPM a square LFO on the level alternates every
        // 125 ms, counted from the start of the song.
        const Render sq = rendered({"Output=0", "L1 Body Hold=2 s", "LFO 1 Shape=Square", "LFO 1 Sync=1/8",
                                    "LFO 1 Retrigger=Off", "Mod 1 Source=LFO 1", "Mod 1 Destination=L1 Level",
                                    "Mod 1 Amount=-20"},
                                   hit, 2.0);
        bool alternates = true;
        for (int k = 2; k < 12; ++k)
        {
            const size_t mid = static_cast<size_t>((k + 0.5) * 0.125 * rate);
            const double lo = peakDb(sq.left, mid - 1200, mid + 1200);
            const size_t next = mid + static_cast<size_t>(0.125 * rate);
            const double hi = peakDb(sq.left, next - 1200, next + 1200);
            // Even eighths are the square's top half, which turns the level
            // down.
            alternates &= (k % 2 == 0 ? hi - lo : lo - hi) > 10.0;
        }
        check(alternates, "a synced square LFO switches the level every eighth note at 120 BPM");

        // An envelope: the default curve falls from 1 to 0 over its Time, so
        // the pitch starts raised and ends where it was.
        const std::vector<std::string> env = {"Output=0", "L1 Body Hold=1 s", "Env 1 Time=100", "Mod 1 Source=Env 1",
                                              "Mod 1 Destination=L1 Pitch End", "Mod 1 Amount=30"};
        const Render e = rendered(env, hit);
        const double late = measureFrequency(e.left, rate, 0.30, 0.45);
        check(std::fabs(late / 48.0 - 1.0) < 0.015 && measureFrequency(e.left, rate, lead / rate + 0.0, lead / rate + 0.05) >
                                                          measureFrequency(unity.left, rate, lead / rate, lead / rate + 0.05) * 1.3,
              "Env 1 raises Pitch End at the hit and lets go over its Time (" + std::to_string(late) + " Hz at 0.3 s)");
        const Render looped = rendered(plus(env, {"Env 1 Loop=On"}), hit);
        check(std::fabs(measureFrequency(looped.left, rate, 0.30, 0.45) / 48.0 - 1.0) > 0.05,
              "a looped envelope keeps moving after its Time");

        // A follower: lane 2's level ducked by lane 1 through the matrix.
        const std::vector<std::string> follow = {"Output=0",          "L1 Output=Aux",        "L2 On=On",
                                                 "L2 Source=Noise",   "L2 Noise Decay=1 s",   "L2 Level=0",
                                                 "Mod 1 Source=Follow 1", "Mod 1 Destination=L2 Level",
                                                 "Mod 1 Amount=-60"};
        const Render fa = rendered(follow, hit), fb = rendered(follow, hit, 1.0, 37);
        const Render noFollow = rendered({"Output=0", "L1 Output=Aux", "L2 On=On", "L2 Source=Noise",
                                          "L2 Noise Decay=1 s", "L2 Level=0"},
                                         hit);
        const double dip = peakDb(noFollow.left, lead + 2000, lead + 3000) - peakDb(fa.left, lead + 2000, lead + 3000);
        check(dip > 20.0 && fa.left == fb.left,
              "Follow 1 to lane 2's level ducks it by " + std::to_string(dip) + " dB; bit-identical at 37");

        // Per lane: in a kit, lane 1 on C2 keeps C2's velocity when D2 comes
        // with it softly, and two lanes each draw their own random value.
        {
            const std::vector<std::string> kit = {"Output=0", "L1 Note=C2", "L1 Output=Main+Aux", "L5 On=On",
                                                  "L5 Note=D2", "L5 Output=Aux", "Mod 1 Source=Velocity",
                                                  "Mod 1 Destination=L1 Pitch End", "Mod 1 Amount=10"};
            EventList both;
            both.add(noteOn(lead, 36, 1.0));
            both.add(noteOn(lead, 38, 0.2));
            const Render r = rendered(kit, both);
            const double f = measureFrequency(r.auxL[0], rate, 0.30, 0.45);
            check(std::fabs(f / (48.0 * std::pow(100.0, 0.1)) - 1.0) < 0.015,
                  "Velocity per lane: lane 1 on C2 plays at C2's velocity although a soft D2 came with it (" +
                      std::to_string(f) + " Hz)");
            const Render rnd = rendered({"Output=0", "L1 Output=Aux", "L5 On=On", "L5 Output=Aux",
                                         "Mod 1 Source=Random", "Mod 1 Destination=L1 Pitch End", "Mod 1 Amount=20",
                                         "Mod 2 Source=Random", "Mod 2 Destination=L5 Pitch End", "Mod 2 Amount=20"},
                                        hit);
            check(rnd.auxL[0] != rnd.auxL[4] && !silent(rnd.auxL[4]), "Random per lane: two lanes draw their own values");
        }

        // A hit into silence takes its modulated level at once: a velocity
        // route on a short click's level sounds the same whether the note
        // comes first thing or after the plugin has idled a while.
        {
            const std::vector<std::string> click = {"Output=0", "L1 On=Off", "L2 On=On", "L2 Level=-20",
                                                    "Mod 1 Source=Velocity", "Mod 1 Destination=L2 Level",
                                                    "Mod 1 Amount=20"};
            EventList first, later;
            first.add(noteOn(0, 36, 1.0));
            later.add(noteOn(1000, 36, 1.0));
            const Render a = rendered(click, first), b = rendered(click, later);
            bool same = true;
            for (size_t i = 0; i + 1000 < a.left.size(); ++i)
                same &= a.left[i] == b.left[i + 1000];
            check(same && !silent(a.left), "a velocity route on a click's level reaches the hit's first sample, "
                                           "first thing or after a pause alike");
        }

        // The routes and the envelope curves are saved and come back.
        p = configured(m, env);
        loadStateText(p, saveStateText(p) + "mod.env1=0,0,0;0.5,1,0;1,0,0\n");
        const std::string saved = saveStateText(p);
        const clap_plugin_t* q = createPlugin(m);
        const bool loaded = loadStateText(q, saved);
        const Render r1 = render(p, rate, 512, static_cast<uint32_t>(rate), hit);
        const Render r2 = render(q, rate, 512, static_cast<uint32_t>(rate), hit);
        check(loaded && saved.find("mod.route1.dest=L1 Pitch End\n") != std::string::npos &&
                  saved.find("mod.env1=0,0,0;0.5,1,0;1,0,0\n") != std::string::npos && r1.left == r2.left &&
                  r1.left != e.left,
              "routes and envelope curves are saved by name and play back bit-identically");
        p->destroy(p);
        q->destroy(q);
        (void)a;
        (void)b;
    }

    // --- the limiters: a slot on a lane, and Limit as the master's Output
    //     Clip; both hold a hot hit at their ceiling, and the limiter bends
    //     a held body less than a hard clip does
    {
        // (The lane's Level comes after its chain, so the limiter's own Gain
        // is what drives it.)
        const Render slotLimit = rendered({"Output=0", "Quality=1x", "L1 Slot 1 Type=Limiter", "L1 Slot 1 Gain=12",
                                           "L1 Slot 1 Ceiling=-6"},
                                          hit);
        const Render slotLimit4x = rendered({"Output=0", "Quality=4x", "L1 Slot 1 Type=Limiter", "L1 Slot 1 Gain=12",
                                             "L1 Slot 1 Ceiling=-6"},
                                            hit);
        check(slotLimit.peak <= 0.5012f && slotLimit.peak > 0.45f && slotLimit4x.peak <= 0.5012f,
              "a Limiter slot at -6 dB holds a lane turned up 12 dB there (" +
                  std::to_string(20.0 * std::log10(slotLimit.peak)) + " dB, " +
                  std::to_string(20.0 * std::log10(slotLimit4x.peak)) + " dB at 4x)");
        const Render outLimit = rendered({"Output=0", "Quality=4x", "L1 Level=+12", "Output Clip=Limit"}, hit);
        check(outLimit.peak <= 0.9661f, "Output Clip Limit holds -0.3 dBFS at 4x too");
        const size_t a = static_cast<size_t>(0.30 * rate), b = static_cast<size_t>(0.45 * rate);
        const std::vector<std::string> hot = {"Output=0", "Quality=1x", "L1 Level=+12", "L1 Body Hold=1 s"};
        const Render limited = rendered(plus(hot, {"Output Clip=Limit"}), hit);
        const Render clipped = rendered(plus(hot, {"Output Clip=Hard"}), hit);
        auto harm = [&](const Render& r) { return toneDb(r.left, rate, 144.0, a, b) - toneDb(r.left, rate, 48.0, a, b); };
        check(limited.peak <= 0.9661f && limited.peak > 0.9f && harm(limited) < harm(clipped) - 20.0,
              "Output Clip Limit holds a +12 dB body at -0.3 dBFS with far fewer harmonics than a clip (" +
                  std::to_string(harm(limited)) + " against " + std::to_string(harm(clipped)) + " dB)");
    }

    // ============================================================= presets

    // --- the factory presets, as a host finds and loads them: every one is
    //     described, loads, plays finite at -1 dBFS and goes to sleep
    {
        std::string error;
        const std::vector<FoundPreset> found = discoverPresets(m, &error);
        bool described = error.empty() && found.size() >= 100;
        for (const FoundPreset& f : found)
            described &= f.factory && !f.name.empty() && !f.description.empty() &&
                         std::find(f.features.begin(), f.features.end(), "kick") != f.features.end();
        check(described, std::to_string(found.size()) + " factory presets discovered, each with a name, a description "
                                                         "and its tags" + (error.empty() ? "" : " (" + error + ")"));
        int bad = 0;
        for (const FoundPreset& f : found)
        {
            const clap_plugin_t* q = createPlugin(m);
            const bool loaded = loadFactoryPreset(q, f.loadKey);
            const Render r = render(q, rate, 512, static_cast<uint32_t>(10.0 * rate), hit);
            // The state remembers which preset it came from.
            const bool named = saveStateText(q).find("preset=" + f.name + "\n") != std::string::npos;
            q->destroy(q);
            const double peak = 20.0 * std::log10(std::max(r.peak, 1e-9f));
            if (!loaded || !named || r.sawNonFinite || !r.sleptAtEnd || peak > -0.5 || peak < -1.6)
            {
                ++bad;
                std::printf("    %s: %s%s%speak %.1f dBFS%s\n", f.loadKey.c_str(), loaded ? "" : "not loaded, ",
                            named ? "" : "name not in the state, ", r.sawNonFinite ? "non-finite, " : "", peak,
                            r.sleptAtEnd ? "" : ", still sounding after 10 s");
            }
        }
        check(bad == 0, "every factory preset loads through preset-load, is named in the state, peaks at -1 dBFS and "
                        "ends within 10 s");
        // A preset switch while the old one still rings: the old sound fades
        // out over 5 ms as it was, then the new preset starts clean -- no
        // burst, and from there exactly the new preset alone, 5 ms late. (It
        // once fed a resonant filter fading out to an empty slot the empty
        // slot's values, +64 dBFS.)
        {
            const uint32_t at = 9728, fade = static_cast<uint32_t>(0.005 * rate);
            auto play = [&](const std::string& a, const std::string& b, uint32_t noteAt) {
                const clap_plugin_t* x = createPlugin(m);
                loadFactoryPreset(x, a);
                EventList ev;
                if (!b.empty())
                    ev.add(noteOn(0, 36, 1.0));
                ev.add(noteOn(noteAt, 36, 1.0));
                const Render r = render(x, rate, 512, static_cast<uint32_t>(1.5 * rate), ev,
                                        [&](const clap_plugin_t* y, uint32_t pos) {
                                            if (!b.empty() && pos == at)
                                                loadFactoryPreset(y, b);
                                        });
                x->destroy(x);
                return r;
            };
            bool clean = true;
            for (const auto& [a, b] : std::vector<std::pair<std::string, std::string>>{
                     {"techno-acid-floor", "acoustic-felt"},
                     {"rumble-rolling-thunder", "hiphop-cardboard"},
                     {"techno-dub-chamber", "exp-stutter"}})
            {
                const Render sw = play(a, b, at);
                const Render alone = play(b, "", at + fade);
                const Render first = play(a, "", 0);
                float before = 0.0f;
                for (size_t i = at; i < at + fade; ++i)
                    before = std::max(before, std::fabs(sw.left[i]));
                float old = 0.0f;
                for (size_t i = at - 480; i < at; ++i)
                    old = std::max(old, std::fabs(first.left[i]));
                bool same = true;
                for (size_t i = at + fade; i < sw.left.size(); ++i)
                    same &= sw.left[i] == alone.left[i];
                if (sw.sawNonFinite || before > old * 1.05f + 1e-6f || !same)
                {
                    clean = false;
                    std::printf("    %s -> %s: fade peak %g against %g before it%s\n", a.c_str(), b.c_str(), before,
                                old, same ? "" : ", not the new preset afterwards");
                }
            }
            check(clean, "a preset switch fades the old sound out as it was and starts the new one clean");
        }
        const clap_plugin_t* q = createPlugin(m);
        check(!loadFactoryPreset(q, "no-such-preset"), "an unknown load key is refused");
        q->destroy(q);
    }

    // --- everything at once: block size still changes nothing
    {
        const std::vector<std::string> all = {
            "L2 On=On",                 "L3 On=On",                    "L4 On=On",
            "L2 Delay=3",               "L3 Variation=40",             "L1 Slot 1 Type=Distortion",
            "L1 Slot 1 Model=Valve Stack", "L1 Slot 2 Type=Compressor", "L1 Slot 2 Band=Low",
            "L1 Slot 3 Type=Filter",    "L1 Slot 3 Env=4",             "L2 Slot 1 Type=Wavefolder",
            "L2 Slot 1 Band=Mid+High",  "L3 Slot 1 Type=Gate",         "L3 Slot 1 Mode=Hit",
            "L4 Slot 6 Type=Transient", "L4 Slot 6 Attack=80",         "Master Slot 1 Type=EQ",
            "Master Slot 1 Low=6",      "Master Slot 2 Type=Bitcrush", "Master Slot 2 Bits=6",
            "Master Slot 2 Mix=30",     "Mono Below=100",              "Output Clip=Soft",
            "Quality=4x",               "L1 Output=Main+Aux"};
        EventList many;
        for (int h = 0; h < 6; ++h)
            many.add(noteOn(lead + static_cast<uint32_t>(h * 0.071 * rate), static_cast<int16_t>(36 + h), 0.9));
        const Render a = rendered(all, many, 2.0, 512);
        const Render b = rendered(all, many, 2.0, 37);
        check(!a.sawNonFinite && a.left == b.left && a.right == b.right && a.auxL[0] == b.auxL[0] && a.sleptAtEnd,
              "slots in four lanes and the master: bit-identical with a block size of 37, asleep at the end");
    }

    std::printf("\nselftest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::string pluginPath = "./Substrike.clap", outPath = "kick.wav";
    double seconds = 1.5, bpm = 128.0, rate = 48000.0, velocity = 1.0;
    int hits = 1, key = 36, aux = 0, lead = 0;
    uint32_t block = 512;
    bool selfTest = false, listParams = false, listPresets = false, allPresets = false;
    std::string statePath, saveStatePath, presetKey, outDir = ".", thenPreset;
    uint32_t thenAt = 0;
    std::vector<std::string> specs;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--plugin")
            pluginPath = next();
        else if (a == "--out")
            outPath = next();
        else if (a == "--seconds")
            seconds = std::atof(next().c_str());
        else if (a == "--hits")
            hits = std::max(1, std::atoi(next().c_str()));
        else if (a == "--bpm")
            bpm = std::atof(next().c_str());
        else if (a == "--lead")
            lead = std::max(0, std::atoi(next().c_str()));
        else if (a == "--key")
            key = std::atoi(next().c_str());
        else if (a == "--velocity")
            velocity = std::atof(next().c_str());
        else if (a == "--rate")
            rate = std::atof(next().c_str());
        else if (a == "--block")
            block = static_cast<uint32_t>(std::max(1, std::atoi(next().c_str())));
        else if (a == "--aux")
            aux = std::clamp(std::atoi(next().c_str()), 0, kLanes);
        else if (a == "--param")
            specs.push_back(next());
        else if (a == "--list-params")
            listParams = true;
        else if (a == "--state")
            statePath = next();
        else if (a == "--save-state")
            saveStatePath = next();
        else if (a == "--preset")
            presetKey = next();
        else if (a == "--then-preset")
            thenPreset = next();
        else if (a == "--at")
            thenAt = static_cast<uint32_t>(std::max(0, std::atoi(next().c_str())));
        else if (a == "--list-presets")
            listPresets = true;
        else if (a == "--all-presets")
            allPresets = true;
        else if (a == "--outdir")
            outDir = next();
        else if (a == "--selftest")
            selfTest = true;
        else
        {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    Module m;
    m.dso = RD_DLOPEN(pluginPath.c_str());
    if (!m.dso)
    {
        std::fprintf(stderr, "cannot load %s: %s\n", pluginPath.c_str(), RD_DLERROR());
        return 1;
    }
    m.entry = static_cast<const clap_plugin_entry_t*>(RD_DLSYM(m.dso, "clap_entry"));
    if (!m.entry || !clap_version_is_compatible(m.entry->clap_version) || !m.entry->init(pluginPath.c_str()))
    {
        std::fprintf(stderr, "no usable clap_entry in %s\n", pluginPath.c_str());
        return 1;
    }
    m.factory = static_cast<const clap_plugin_factory_t*>(m.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));

    int rc = 0;
    // Before the parameters: a state or preset file, or a factory preset.
    auto prepare = [&](const clap_plugin_t* p) {
        if (!statePath.empty())
        {
            std::string text;
            if (!readTextFile(statePath, text) || !loadStateText(p, text))
            {
                std::fprintf(stderr, "cannot load %s\n", statePath.c_str());
                return false;
            }
        }
        if (!presetKey.empty() && !loadFactoryPreset(p, presetKey))
        {
            std::fprintf(stderr, "no factory preset %s\n", presetKey.c_str());
            return false;
        }
        return applyParams(p, specs);
    };
    if (selfTest)
        rc = runSelfTest(m);
    else if (listPresets)
    {
        std::string error;
        for (const FoundPreset& f : discoverPresets(m, &error))
        {
            std::string tags;
            for (const std::string& t : f.features)
                tags += (tags.empty() ? "" : ", ") + t;
            std::printf("%-28s %-26s %s\n", f.loadKey.c_str(), f.name.c_str(), tags.c_str());
        }
        if (!error.empty())
            std::fprintf(stderr, "%s\n", error.c_str());
    }
    else if (allPresets)
    {
        // Every factory preset, one hit each, to <outdir>/<load key>.wav.
        for (const FoundPreset& f : discoverPresets(m))
        {
            const clap_plugin_t* p = createPlugin(m);
            presetKey = f.loadKey;
            if (!p || !prepare(p))
            {
                rc = 1;
                if (p)
                    p->destroy(p);
                continue;
            }
            EventList notes;
            notes.add(noteOn(0, static_cast<int16_t>(key), velocity));
            const Render r = render(p, rate, block, static_cast<uint32_t>(seconds * rate), notes);
            p->destroy(p);
            const std::string path = outDir + "/" + f.loadKey + ".wav";
            if (!writeWav(path, r.left, r.right, static_cast<uint32_t>(rate)))
                rc = 1;
            std::printf("%-28s peak %+6.1f dBFS%s%s\n", f.loadKey.c_str(), 20.0 * std::log10(std::max(r.peak, 1e-9f)),
                        r.sleptAtEnd ? "" : "  (still sounding)", r.sawNonFinite ? "  !! NON-FINITE" : "");
        }
    }
    else if (listParams)
    {
        const clap_plugin_t* p = createPlugin(m);
        const clap_plugin_params_t* params = paramsOf(p);
        for (uint32_t i = 0; i < params->count(p); ++i)
        {
            clap_param_info_t info{};
            params->get_info(p, i, &info);
            char text[256];
            params->value_to_text(p, info.id, info.default_value, text, sizeof(text));
            std::printf("%6u  %-24s %-14s %s\n", info.id, info.name, text, info.module);
        }
        p->destroy(p);
    }
    else
    {
        const clap_plugin_t* p = createPlugin(m);
        if (!p || !prepare(p))
            rc = 1;
        else
        {
            if (!saveStatePath.empty())
            {
                std::FILE* f = std::fopen(saveStatePath.c_str(), "wb");
                const std::string text = saveStateText(p);
                if (!f || std::fwrite(text.data(), 1, text.size(), f) != text.size())
                    rc = 1;
                if (f)
                    std::fclose(f);
            }
            EventList notes;
            const double step = 60.0 / bpm;
            for (int h = 0; h < hits; ++h)
                notes.add(noteOn(static_cast<uint32_t>(lead + h * step * rate), static_cast<int16_t>(key), velocity));
            // --then-preset: switch to another factory preset at --at (rounded
            // to the block) and play it there, as the browser does.
            BetweenBlocks between;
            if (!thenPreset.empty())
            {
                const uint32_t at = thenAt / block * block;
                notes.add(noteOn(at, static_cast<int16_t>(key), velocity));
                notes.sort();
                between = [&, at](const clap_plugin_t* q, uint32_t pos) {
                    if (pos == at && !loadFactoryPreset(q, thenPreset))
                        std::fprintf(stderr, "no factory preset %s\n", thenPreset.c_str());
                };
            }
            const Render r = render(p, rate, block, static_cast<uint32_t>(seconds * rate), notes, between);
            const std::vector<float>& wl = aux ? r.auxL[aux - 1] : r.left;
            const std::vector<float>& wr = aux ? r.auxR[aux - 1] : r.right;
            if (!writeWav(outPath, wl, wr, static_cast<uint32_t>(rate)))
                rc = 1;
            float peak = r.peak;
            if (aux)
            {
                peak = 0.0f;
                for (size_t i = 0; i < wl.size(); ++i)
                    peak = std::max({peak, std::fabs(wl[i]), std::fabs(wr[i])});
            }
            std::printf("peak %.3f (%+.1f dBFS)%s -> %s\n", peak, 20.0 * std::log10(std::max(peak, 1e-9f)),
                        r.sawNonFinite ? "  !! NON-FINITE OUTPUT" : "", outPath.c_str());
            if (r.sawNonFinite)
                rc = 1;
        }
        if (p)
            p->destroy(p);
    }

    m.entry->deinit();
    RD_DLCLOSE(m.dso);
    return rc;
}
