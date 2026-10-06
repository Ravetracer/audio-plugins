// Minimal CLAP host for Substrike: loads the built .clap the way a DAW does,
// plays one or more hits, and writes the result as a 32-bit float WAV. It also
// carries the self-test, which goes through the same CLAP interface, so it
// checks exactly what gets installed.
//
//   substrike-render --plugin build/Substrike.clap --out kick.wav
//   substrike-render --param "L1 Pitch Start=1.2 kHz" --param "L1 Decay=1.5 s"
//   substrike-render --hits 4 --bpm 140 --seconds 2 --key 38
//   substrike-render --list-params
//   substrike-render --selftest

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
bool resolveParams(const clap_plugin_t* p, const std::vector<std::string>& specs, EventList& out)
{
    for (const std::string& spec : specs)
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
    }
    return true;
}

// Flushes parameter events into an inactive plugin.
void flushParams(const clap_plugin_t* p, const EventList& events)
{
    const clap_input_events_t in = events.input();
    paramsOf(p)->flush(p, &in, &kOutEvents);
}

struct Render
{
    std::vector<float> left, right;
    bool sawNonFinite = false;
    bool sleptAtEnd = false;
    float peak = 0.0f;
};

// Plays `events` (note events with absolute sample times) over `frames`
// samples in blocks of `block`.
Render render(const clap_plugin_t* p, double rate, uint32_t block, uint32_t frames, const EventList& events)
{
    Render r;
    r.left.assign(frames, 0.0f);
    r.right.assign(frames, 0.0f);
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
        EventList blockEvents;
        while (next < events.events.size() && events.at(static_cast<uint32_t>(next))->time < pos + n)
        {
            std::vector<uint8_t> copy = events.events[next++];
            reinterpret_cast<clap_event_header_t*>(copy.data())->time -= pos;
            blockEvents.events.push_back(std::move(copy));
        }
        float* chans[2] = {r.left.data() + pos, r.right.data() + pos};
        clap_audio_buffer_t out{};
        out.data32 = chans;
        out.channel_count = 2;
        clap_process_t proc{};
        proc.steady_time = pos;
        proc.frames_count = n;
        proc.audio_outputs = &out;
        proc.audio_outputs_count = 1;
        const clap_input_events_t in = blockEvents.input();
        proc.in_events = &in;
        proc.out_events = &kOutEvents;
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
    }
    return r;
}

// ------------------------------------------------------------------- WAV

bool writeWav(const std::string& path, const Render& r, uint32_t rate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
    {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    const uint32_t frames = static_cast<uint32_t>(r.left.size());
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
        std::fwrite(&r.left[i], 4, 1, f);
        std::fwrite(&r.right[i], 4, 1, f);
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

float maxStep(const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = std::max<size_t>(from, 1); i < std::min(to, x.size()); ++i)
        m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
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

    std::printf("\nselftest: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::string pluginPath = "./Substrike.clap", outPath = "kick.wav";
    double seconds = 1.5, bpm = 128.0, rate = 48000.0, velocity = 1.0;
    int hits = 1, key = 36;
    uint32_t block = 512;
    bool selfTest = false, listParams = false;
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
        else if (a == "--key")
            key = std::atoi(next().c_str());
        else if (a == "--velocity")
            velocity = std::atof(next().c_str());
        else if (a == "--rate")
            rate = std::atof(next().c_str());
        else if (a == "--block")
            block = static_cast<uint32_t>(std::max(1, std::atoi(next().c_str())));
        else if (a == "--param")
            specs.push_back(next());
        else if (a == "--list-params")
            listParams = true;
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
    if (selfTest)
        rc = runSelfTest(m);
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
        EventList setup;
        if (!p || !resolveParams(p, specs, setup))
            rc = 1;
        else
        {
            flushParams(p, setup);
            EventList notes;
            const double step = 60.0 / bpm;
            for (int h = 0; h < hits; ++h)
                notes.add(noteOn(static_cast<uint32_t>(h * step * rate), static_cast<int16_t>(key), velocity));
            const Render r = render(p, rate, block, static_cast<uint32_t>(seconds * rate), notes);
            if (!writeWav(outPath, r, static_cast<uint32_t>(rate)))
                rc = 1;
            std::printf("peak %.3f (%+.1f dBFS)%s -> %s\n", r.peak, 20.0 * std::log10(std::max(r.peak, 1e-9f)),
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
