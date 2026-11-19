// Self-test for the WebAssembly build: drives the plugin through clap_entry
// exactly as a WCLAP host would (minus the function-table plumbing), built as
// a WASI command from the same objects as Substrike.wclap.wasm and run under a
// WASI runtime: `node ../shared/tools/run-wasi.mjs build-wasm/substrike-wclap-selftest.wasm`.
//
// Checks: the factory and descriptor, the webview-only GUI, a hit that sounds
// and stays finite, the page protocol (polls, a gesture, a curve edit, a
// preset, undo, audition), a factory preset through clap.preset-load, a state
// round trip with values and curves; and it measures what a 128-frame block
// costs, per factory preset, while a hit sounds.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include "plugin/HitRender.h"
#include "plugin/Params.h"
#include "plugin/WebProtocol.h"
#include "state/Presets.h"

extern "C" const clap_plugin_entry_t clap_entry;

using namespace substrike;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

// --------------------------------------------------------------- host side

// The page's side of clap.webview: what the plugin sends, kept for the checks.
std::vector<std::vector<uint8_t>> sentToPage;
bool hostWebviewSend(const clap_host_t*, const void* data, uint32_t size)
{
    const auto* b = static_cast<const uint8_t*>(data);
    sentToPage.emplace_back(b, b + size);
    return true;
}
const clap_host_webview_t kHostWebview = {hostWebviewSend};
int dirtyMarks = 0;
void hostMarkDirty(const clap_host_t*) { ++dirtyMarks; }
const clap_host_state_t kHostState = {hostMarkDirty};

const void* hostGetExtension(const clap_host_t*, const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_WEBVIEW))
        return &kHostWebview;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &kHostState;
    return nullptr;
}
void hostNoop(const clap_host_t*) {}

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "substrike-wclap-selftest", "Ravetracer", "", "0",
                           hostGetExtension,  hostNoop, hostNoop,                   hostNoop};

// Notes and parameter values for the next block, all at its start.
struct EventList
{
    std::vector<clap_event_note_t> notes;
    std::vector<clap_event_param_value_t> params;

    static uint32_t size(const clap_input_events_t* l)
    {
        const auto* e = static_cast<const EventList*>(l->ctx);
        return static_cast<uint32_t>(e->params.size() + e->notes.size());
    }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i)
    {
        const auto* e = static_cast<const EventList*>(l->ctx);
        if (i < e->params.size())
            return &e->params[i].header;
        return &e->notes[i - e->params.size()].header;
    }
    void noteOn(int key)
    {
        clap_event_note_t n{};
        n.header = {sizeof(n), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
        n.note_id = -1;
        n.port_index = 0;
        n.channel = 0;
        n.key = static_cast<int16_t>(key);
        n.velocity = 1.0;
        notes.push_back(n);
    }
};

bool pushOut(const clap_output_events_t*, const clap_event_header_t*) { return true; }

struct Buffer
{
    std::vector<uint8_t> data;
    size_t readPos = 0;

    static int64_t write(const clap_ostream_t* s, const void* buf, uint64_t n)
    {
        auto* b = static_cast<Buffer*>(s->ctx);
        const auto* p = static_cast<const uint8_t*>(buf);
        b->data.insert(b->data.end(), p, p + n);
        return static_cast<int64_t>(n);
    }
    static int64_t read(const clap_istream_t* s, void* buf, uint64_t n)
    {
        auto* b = static_cast<Buffer*>(s->ctx);
        const size_t k = std::min<size_t>(n, b->data.size() - b->readPos);
        std::memcpy(buf, b->data.data() + b->readPos, k);
        b->readPos += k;
        return static_cast<int64_t>(k);
    }
};

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 128;

// Every port connected, as a host with all nine stereo outputs would.
struct Run
{
    const clap_plugin_t* plugin;
    std::vector<std::vector<float>> channels;
    std::vector<float*> pointers;
    std::vector<clap_audio_buffer_t> outs;
    EventList events;
    int64_t steady = 0;
    double lastMicros = 0.0;

    explicit Run(const clap_plugin_t* p, uint32_t ports) : plugin(p)
    {
        channels.assign(2 * ports, std::vector<float>(kBlock));
        for (auto& c : channels)
            pointers.push_back(c.data());
        outs.resize(ports);
        for (uint32_t i = 0; i < ports; ++i)
            outs[i] = {pointers.data() + 2 * i, nullptr, 2, 0, 0};
    }

    // One block; returns the main output's peak and flags non-finite output.
    float block(bool& finite)
    {
        clap_input_events_t ie{&events, EventList::size, EventList::get};
        clap_output_events_t oe{nullptr, pushOut};
        clap_process_t pr{};
        pr.steady_time = steady;
        pr.frames_count = kBlock;
        pr.audio_outputs = outs.data();
        pr.audio_outputs_count = static_cast<uint32_t>(outs.size());
        pr.in_events = &ie;
        pr.out_events = &oe;
        const auto t0 = std::chrono::steady_clock::now();
        plugin->process(plugin, &pr);
        lastMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        events.params.clear();
        events.notes.clear();
        steady += kBlock;
        float peak = 0.0f;
        for (uint32_t i = 0; i < kBlock; ++i)
        {
            for (auto& c : channels)
                if (!std::isfinite(c[i]))
                    finite = false;
            peak = std::max({peak, std::fabs(channels[0][i]), std::fabs(channels[1][i])});
        }
        return peak;
    }
};

double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// A parsed PluginStatus, as the editor reads it.
struct Status
{
    bool valid = false;
    uint8_t flags = 0;
    uint32_t page = 0, serial = 0, edits = 0;
    std::vector<std::pair<int, double>> values;
    std::vector<std::pair<int, dsp::Curve>> curves;
    std::string preset;
    int modulated = 0, bins = 0;
};

Status parse(const std::vector<uint8_t>& m)
{
    using namespace web;
    Status s;
    if (m.size() < kStatusHeader || m[0] != PluginStatus)
        return s;
    const uint8_t* b = m.data();
    s.flags = b[1];
    std::memcpy(&s.page, b + 4, 4);
    std::memcpy(&s.serial, b + 8, 4);
    std::memcpy(&s.edits, b + 12, 4);
    uint16_t nv, nc, nm, nb;
    std::memcpy(&nv, b + 24, 2);
    std::memcpy(&nc, b + 26, 2);
    std::memcpy(&nm, b + 28, 2);
    std::memcpy(&nb, b + 30, 2);
    size_t pos = kStatusHeader;
    for (int k = 0; k < nv; ++k)
    {
        uint16_t i;
        double v;
        if (!get(b, m.size(), pos, i) || !get(b, m.size(), pos, v))
            return s;
        s.values.emplace_back(i, v);
    }
    for (int k = 0; k < nc; ++k)
    {
        uint8_t i;
        dsp::Curve c;
        if (!get(b, m.size(), pos, i) || !getCurve<uint8_t>(b, m.size(), pos, c))
            return s;
        s.curves.emplace_back(i, c);
    }
    if ((s.flags & StatusPreset) && !getString(b, m.size(), pos, s.preset))
        return s;
    s.modulated = nm;
    s.bins = nb;
    pos += 10u * nm + 4u * nb;
    s.valid = pos == m.size();
    return s;
}

} // namespace

int main()
{
    check(clap_entry.init(""), "clap_entry.init");
    const auto* factory = static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(factory && factory->get_plugin_count(factory) == 1, "plugin factory with one plugin");
    if (!factory)
        return 1;
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    std::printf("      %s %s (%s)\n", desc->name, desc->version, desc->id);

    const clap_plugin_t* plugin = factory->create_plugin(factory, &kHost, desc->id);
    check(plugin && plugin->init(plugin), "create + init");
    if (!plugin)
        return 1;
    const auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    const auto* webview = static_cast<const clap_plugin_webview_t*>(plugin->get_extension(plugin, CLAP_EXT_WEBVIEW));
    check(gui && gui->is_api_supported(plugin, CLAP_WINDOW_API_WEBVIEW, false) &&
              !gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false),
          "gui: the webview API only");
    check(webview != nullptr, "clap.webview offered");
    check(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT) == nullptr &&
              plugin->get_extension(plugin, CLAP_EXT_POSIX_FD_SUPPORT) == nullptr,
          "no timer or fd support (nothing to drive)");
    if (webview)
    {
        char uri[64] = {};
        check(webview->get_uri(plugin, uri, sizeof(uri)) > 0 && !std::strcmp(uri, "/index.html"), "page /index.html");
    }

    const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    const auto* presetLoad =
        static_cast<const clap_plugin_preset_load_t*>(plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
    check(params && state && ports && presetLoad, "params, state, audio-ports and preset-load extensions");
    if (!params || !state || !ports || !presetLoad)
        return 1;
    const uint32_t nParams = params->count(plugin);
    const ParamTable& table = ParamTable::get();
    check(nParams == static_cast<uint32_t>(table.count()) && nParams < 65536, "parameter count");
    const uint32_t nPorts = ports->count(plugin, false);
    std::printf("      %u parameters, %u output ports\n", nParams, nPorts);

    const int outputIndex = table.indexOf(pid::Output);
    check(outputIndex >= 0, "found Output");

    check(plugin->activate(plugin, kRate, 1, kBlock), "activate 48 kHz, blocks up to 128");
    check(plugin->start_processing(plugin), "start_processing");

    // ------------------------------------------------------------ a hit
    Run run(plugin, nPorts);
    bool finite = true;
    {
        float idle = run.block(finite);
        run.events.noteOn(36);
        float peak = 0.0f;
        for (int b = 0; b < static_cast<int>(0.5 * kRate / kBlock); ++b)
            peak = std::max(peak, run.block(finite));
        check(idle == 0.0f, "silent before the note");
        std::printf("      Init hit peak %.1f dB\n", dB(peak));
        check(peak > 1e-3f, "a note plays a hit");
        check(finite, "output finite");
    }

    // ------------------------------------------------------------ page
    using namespace web;
    const uint32_t page = 0x5eed;
    uint32_t edits = 0;
    auto poll = [&](uint32_t serial) {
        std::vector<uint8_t> m(4, 0);
        m[0] = PagePoll;
        put(m, page);
        put(m, serial);
        put(m, edits);
        sentToPage.clear();
        webview->receive(plugin, m.data(), static_cast<uint32_t>(m.size()));
        return sentToPage.empty() ? Status{} : parse(sentToPage.back());
    };
    auto sendPage = [&](const std::vector<uint8_t>& m) {
        return webview->receive(plugin, m.data(), static_cast<uint32_t>(m.size()));
    };
    auto indexMsg = [&](uint8_t type, int index) {
        std::vector<uint8_t> m(4, 0);
        m[0] = type;
        put(m, static_cast<uint32_t>(index));
        return m;
    };
    auto plain = [&](uint8_t type) { return std::vector<uint8_t>{type, 0, 0, 0}; };
    auto value = [&](int index) {
        double v = 0;
        params->get_value(plugin, table.def(index).id, &v);
        return v;
    };

    Status first = poll(0);
    check(first.valid && first.page == page && first.values.size() == nParams &&
              first.curves.size() == static_cast<size_t>(dsp::kNumCurves) && (first.flags & StatusPreset) &&
              first.preset == "Init",
          "page: the first poll carries every value, every curve and the preset");
    std::printf("      first answer %zu bytes, %d scope bins\n", sentToPage.back().size(), first.bins);
    Status second = poll(first.serial);
    check(second.valid && second.values.empty() && second.curves.empty() && !(second.flags & StatusPreset),
          "page: nothing new, nothing sent");
    std::printf("      quiet answer %zu bytes\n", sentToPage.back().size());

    // A gesture.
    {
        const int marks = dirtyMarks;
        const double v = table.minValue(outputIndex) + 0.25 * (table.maxValue(outputIndex) - table.minValue(outputIndex));
        auto set = indexMsg(PageSet, outputIndex);
        put(set, v);
        sendPage(indexMsg(PageBegin, outputIndex));
        sendPage(set);
        sendPage(indexMsg(PageEnd, outputIndex));
        sendPage(plain(PageCheckpoint));
        edits += 1;
        check(value(outputIndex) == v, "page: an edit reaches the parameter");
        check(dirtyMarks == marks + 1, "page: a gesture is one mark_dirty");
        Status s = poll(second.serial);
        check(s.valid && s.values.size() == 1 && s.values[0].first == outputIndex && s.values[0].second == v &&
                  s.edits == edits && (s.flags & StatusCanUndo),
              "page: the next answer has the edit, the echo and something to undo");
        second = s;
    }

    // A curve edit.
    const int pitchCurve = dsp::curveIndex(0, 0);
    dsp::Curve drawn;
    {
        const dsp::CurvePoint pts[3] = {{0.0, 1.0, 0.4}, {0.3, 0.45, -0.2}, {1.0, 0.0, 0.0}};
        drawn.set(pts, 3);
        std::vector<uint8_t> m = indexMsg(PageCurve, pitchCurve);
        putCurve<uint32_t>(m, drawn);
        const int marks = dirtyMarks;
        check(sendPage(m), "page: a curve edit is taken");
        sendPage(plain(PageCheckpoint));
        edits += 1;
        check(dirtyMarks == marks + 1, "page: a curve edit marks the state dirty");
        Status s = poll(second.serial);
        check(s.valid && s.curves.size() == 1 && s.curves[0].first == pitchCurve && s.curves[0].second == drawn &&
                  s.values.empty(),
              "page: the next answer has the curve, and only it");
        Buffer saved;
        clap_ostream_t os{&saved, Buffer::write};
        state->save(plugin, &os);
        const std::string text(saved.data.begin(), saved.data.end());
        check(text.find("l1.pitch=" + curveToText(drawn)) != std::string::npos, "page: the curve is in the state");
        second = s;
    }

    // Undo and redo, kept by the plugin.
    {
        sendPage(plain(PageUndo));
        Status s = poll(second.serial);
        check(s.valid && s.curves.size() == 1 && s.curves[0].second != drawn && (s.flags & StatusCanRedo),
              "page: undo takes the curve back");
        sendPage(plain(PageRedo));
        s = poll(s.serial);
        check(s.valid && s.curves.size() == 1 && s.curves[0].second == drawn, "page: redo brings it again");
        second = s;
    }

    // A preset from the page: every value, every curve and its name (not
    // "Init", which the plugin already has).
    const PresetInfo& preset = factoryPresets()[factoryPresets().size() / 2];
    {
        StateDocument doc;
        check(parseState(factoryPresetText(preset.key), doc), "a factory preset parses");
        std::vector<uint8_t> m(4, 0);
        m[0] = PageLoad;
        put(m, nParams);
        for (double v : doc.values)
            put(m, v);
        for (const dsp::Curve& c : doc.curves)
            putCurve<uint8_t>(m, c);
        putString(m, preset.name);
        check(sendPage(m), "page: a preset is taken");
        edits += 1;
        bool same = true;
        for (int i = 0; i < table.count(); ++i)
            same = same && value(i) == doc.values[static_cast<size_t>(i)];
        check(same, "page: the preset's values arrive");
        Status s = poll(second.serial);
        check(s.valid && (s.flags & StatusPreset) && s.preset == preset.name, "page: the preset's name comes back");
        // Audition plays it.
        sendPage(plain(PageAudition));
        float peak = 0.0f;
        for (int b = 0; b < static_cast<int>(0.3 * kRate / kBlock); ++b)
            peak = std::max(peak, run.block(finite));
        check(peak > 1e-3f && finite, "page: audition plays the preset");
        s = poll(s.serial);
        check(s.valid && s.bins > 0, "page: the scope sends what it saw");
        std::printf("      %d scope bins after 0.3 s\n", s.bins);
        second = s;
    }
    check(!sendPage({0x7f, 0, 0, 0}) && !sendPage({PageSet, 0, 0, 0}), "page: unknown or short messages refused");
    // What a poll costs the host's audio thread when nothing changed.
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i)
            poll(second.serial);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        std::printf("      a quiet poll: %.1f us\n", us / 1000);
    }

    // A factory preset through the host's preset-load, as openDAW's browser may.
    {
        const PresetInfo& other = factoryPresets().back();
        check(presetLoad->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, other.key.c_str()),
              "preset-load: a factory preset");
        Status s = poll(second.serial);
        check(s.valid && s.preset == other.name && !s.values.empty(), "preset-load: the page hears of it");
        second = s;
    }

    // State round trip: values and curves.
    {
        sendPage([&] {
            std::vector<uint8_t> m = indexMsg(PageCurve, pitchCurve);
            putCurve<uint32_t>(m, drawn);
            return m;
        }());
        Buffer saved;
        clap_ostream_t os{&saved, Buffer::write};
        check(state->save(plugin, &os) && !saved.data.empty(), "state save");
        const double outBefore = value(outputIndex);
        auto set = indexMsg(PageSet, outputIndex);
        put(set, table.maxValue(outputIndex));
        sendPage(set);
        const dsp::Curve flat;
        sendPage([&] {
            std::vector<uint8_t> m = indexMsg(PageCurve, pitchCurve);
            putCurve<uint32_t>(m, flat);
            return m;
        }());
        clap_istream_t is{&saved, Buffer::read};
        check(state->load(plugin, &is), "state load");
        Status s = poll(0);
        bool curveBack = false;
        for (auto& [i, c] : s.curves)
            if (i == pitchCurve)
                curveBack = c == drawn;
        check(value(outputIndex) == outBefore && curveBack, "values and curves survive the round trip");
        check(!(s.flags & StatusCanUndo), "a loaded state starts a new history");
    }

    // ------------------------------------------------------------ cost
    // Each factory preset: one hit and the second after it, block by block,
    // timed around process() only.
    {
        double total = 0.0, worst = 0.0, worstAvg = 0.0;
        long blocks = 0;
        std::string worstName;
        std::vector<double> perBlock;
        for (const PresetInfo& p : factoryPresets())
        {
            presetLoad->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, p.key.c_str());
            // The fade of the fresh start.
            for (int b = 0; b < 8; ++b)
                run.block(finite);
            run.events.noteOn(36);
            double sum = 0.0;
            const int n = static_cast<int>(1.0 * kRate / kBlock);
            for (int b = 0; b < n; ++b)
            {
                run.block(finite);
                sum += run.lastMicros;
                perBlock.push_back(run.lastMicros);
                worst = std::max(worst, run.lastMicros);
            }
            total += sum;
            blocks += n;
            if (sum / n > worstAvg)
            {
                worstAvg = sum / n;
                worstName = p.name;
            }
        }
        std::sort(perBlock.begin(), perBlock.end());
        const double budget = 1e6 * kBlock / kRate;
        std::printf("      %zu presets, 1 s after the note each: %.1f us per 128-frame block on average "
                    "(%.1f %% of %.0f us), median %.1f, 99th percentile %.1f, max %.1f\n",
                    factoryPresets().size(), total / blocks, 100.0 * total / blocks / budget, budget,
                    perBlock[perBlock.size() / 2], perBlock[perBlock.size() * 99 / 100], worst);
        std::printf("      heaviest preset: %s, %.1f us per block\n", worstName.c_str(), worstAvg);
        check(finite, "every preset's output finite");
        // Silence: the plugin sleeps.
        for (int b = 0; b < static_cast<int>(10.0 * kRate / kBlock); ++b)
            run.block(finite);
        double idle = 0.0;
        for (int b = 0; b < 100; ++b)
        {
            run.block(finite);
            idle += run.lastMicros;
        }
        std::printf("      idle: %.2f us per block\n", idle / 100);
    }

    // What the editor's hit preview costs (it renders in the page, a slice per
    // frame): the whole render of each factory preset's hit.
    {
        double total = 0.0, worst = 0.0;
        std::string worstName;
        for (const PresetInfo& p : factoryPresets())
        {
            StateDocument doc;
            parseState(factoryPresetText(p.key), doc);
            const auto t0 = std::chrono::steady_clock::now();
            const HitRender hit = renderHit(doc.values, doc.curves, kRate, 8.0);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            total += ms;
            if (ms > worst)
            {
                worst = ms;
                worstName = p.name + " (" + std::to_string(hit.left.size() / 48) + " ms long)";
            }
        }
        std::printf("      hit preview render: %.1f ms on average, %.1f ms at most (%s)\n",
                    total / static_cast<double>(factoryPresets().size()), worst, worstName.c_str());
    }

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    clap_entry.deinit();

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
