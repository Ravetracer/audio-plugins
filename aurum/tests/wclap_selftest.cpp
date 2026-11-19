// Self-test for the WebAssembly build: drives the plugin through clap_entry
// exactly as a WCLAP host would (minus the function-table plumbing), built as
// a WASI command from the same objects as Aurum.wclap.wasm and run under a
// WASI runtime, e.g. `node ../shared/tools/run-wasi.mjs build-wasm/aurum-wclap-selftest.wasm`.
//
// Checks: the factory and descriptor, parameter info, a realtime (not
// offline) impulse response that rings and stays finite, a parameter change
// mid-tail, and a state save/load round trip.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include "plugin/WebProtocol.h"

extern "C" const clap_plugin_entry_t clap_entry;

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

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "aurum-wclap-selftest", "Ravetracer", "", "0",
                           hostGetExtension,  hostNoop, hostNoop,               hostNoop};

struct EventList
{
    std::vector<clap_event_param_value_t> params;

    static uint32_t size(const clap_input_events_t* l)
    {
        return static_cast<uint32_t>(static_cast<const EventList*>(l->ctx)->params.size());
    }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i)
    {
        return &static_cast<const EventList*>(l->ctx)->params[i].header;
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
constexpr uint32_t kBlock = 256;

struct Run
{
    const clap_plugin_t* plugin;
    std::vector<float> inL, inR, outL, outR;
    EventList events;
    int64_t steady = 0;

    explicit Run(const clap_plugin_t* p)
        : plugin(p), inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock)
    {
    }

    // One block; returns the block's peak and flags non-finite output.
    float block(bool impulse, bool& finite)
    {
        std::fill(inL.begin(), inL.end(), 0.0f);
        std::fill(inR.begin(), inR.end(), 0.0f);
        if (impulse)
            inL[0] = inR[0] = 0.5f;
        float* in[2] = {inL.data(), inR.data()};
        float* out[2] = {outL.data(), outR.data()};
        clap_audio_buffer_t ain{in, nullptr, 2, 0, 0};
        clap_audio_buffer_t aout{out, nullptr, 2, 0, 0};
        clap_input_events_t ie{&events, EventList::size, EventList::get};
        clap_output_events_t oe{nullptr, pushOut};
        clap_process_t pr{};
        pr.steady_time = steady;
        pr.frames_count = kBlock;
        pr.audio_inputs = &ain;
        pr.audio_outputs = &aout;
        pr.audio_inputs_count = 1;
        pr.audio_outputs_count = 1;
        pr.in_events = &ie;
        pr.out_events = &oe;
        plugin->process(plugin, &pr);
        events.params.clear();
        steady += kBlock;
        float peak = 0.0f;
        for (uint32_t i = 0; i < kBlock; ++i)
        {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]))
                finite = false;
            peak = std::max({peak, std::fabs(outL[i]), std::fabs(outR[i])});
        }
        return peak;
    }
};

double dB(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

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
    if (webview)
    {
        char uri[64] = {};
        check(webview->get_uri(plugin, uri, sizeof(uri)) > 0 && !std::strcmp(uri, "/index.html"), "page /index.html");
    }

    const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    check(params && state, "params and state extensions");
    if (!params || !state)
        return 1;
    const uint32_t nParams = params->count(plugin);
    check(nParams > 10, "parameter count");
    std::printf("      %u parameters\n", nParams);
    clap_id mixId = CLAP_INVALID_ID, lengthId = CLAP_INVALID_ID;
    for (uint32_t i = 0; i < nParams; ++i)
    {
        clap_param_info_t info{};
        params->get_info(plugin, i, &info);
        if (!std::strcmp(info.name, "Mix"))
            mixId = info.id;
        if (!std::strcmp(info.name, "Length"))
            lengthId = info.id;
    }
    check(mixId != CLAP_INVALID_ID && lengthId != CLAP_INVALID_ID, "found Mix and Length");

    check(plugin->activate(plugin, kRate, 32, kBlock), "activate 48 kHz");
    check(plugin->start_processing(plugin), "start_processing");

    Run run(plugin);
    bool finite = true;
    float early = 0.0f, late = 0.0f;
    const auto t0 = std::chrono::steady_clock::now();
    const int blocks = static_cast<int>(3.0 * kRate / kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        // Halfway, move Length the way automation would.
        if (b == blocks / 2 && lengthId != CLAP_INVALID_ID)
        {
            clap_param_info_t info{};
            for (uint32_t i = 0; i < nParams; ++i)
                if (params->get_info(plugin, i, &info) && info.id == lengthId)
                    break;
            clap_event_param_value_t ev{};
            ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            ev.param_id = lengthId;
            ev.note_id = ev.port_index = ev.channel = ev.key = -1;
            ev.value = info.min_value + 0.8 * (info.max_value - info.min_value);
            run.events.params.push_back(ev);
        }
        const float peak = run.block(b == 4, finite);
        const double t = b * kBlock / kRate;
        if (t > 0.05 && t < 0.5)
            early = std::max(early, peak);
        if (t > 1.0 && t < 1.5)
            late = std::max(late, peak);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(finite, "output finite");
    std::printf("      peak 0.05-0.5 s: %.1f dB, 1.0-1.5 s: %.1f dB\n", dB(early), dB(late));
    check(early > 1e-3f, "the impulse rings");
    check(late > 1e-6f && late < early, "the tail decays");
    std::printf("      %.2f s of audio in %.2f s (%.1fx realtime)\n", blocks * kBlock / kRate, secs,
                blocks * kBlock / kRate / secs);

    // The page's messages: a first poll gets every value, a second none, an
    // edit shows up in the next answer, a gesture end marks the state dirty.
    if (webview)
    {
        using namespace aurum::web;
        auto poll = [&](uint32_t serial) {
            uint8_t m[8] = {PagePoll};
            std::memcpy(m + 4, &serial, 4);
            sentToPage.clear();
            webview->receive(plugin, m, sizeof(m));
            return sentToPage.empty() ? std::vector<uint8_t>{} : sentToPage.back();
        };
        auto first = poll(0);
        uint32_t serial = 0;
        if (first.size() >= kStatusHeader)
            std::memcpy(&serial, first.data() + 4, 4);
        check(first.size() > kStatusHeader + 8u * nParams && first[0] == PluginStatus && first[1] == 1,
              "page: first poll carries the values");
        auto second = poll(serial);
        check(second.size() == kStatusHeader && second[1] == 0, "page: nothing new, 24 bytes");

        int mixIndex = -1;
        for (uint32_t i = 0; i < nParams; ++i)
        {
            clap_param_info_t info{};
            params->get_info(plugin, i, &info);
            if (info.id == mixId)
                mixIndex = static_cast<int>(i);
        }
        const int marks = dirtyMarks;
        uint8_t begin[8] = {PageBegin}, set[16] = {PageSet}, end[8] = {PageEnd};
        const uint32_t idx = static_cast<uint32_t>(mixIndex);
        const double mix = 0.123;
        std::memcpy(begin + 4, &idx, 4);
        std::memcpy(set + 4, &idx, 4);
        std::memcpy(set + 8, &mix, 8);
        std::memcpy(end + 4, &idx, 4);
        webview->receive(plugin, begin, 8);
        webview->receive(plugin, set, 16);
        webview->receive(plugin, end, 8);
        double now = 0;
        params->get_value(plugin, mixId, &now);
        check(std::fabs(now - mix) < 1e-9, "page: an edit reaches the parameter");
        check(dirtyMarks == marks + 1, "page: a gesture is one mark_dirty");
        auto third = poll(serial);
        double seen = -1;
        if (third.size() > kStatusHeader + 8u * nParams)
            std::memcpy(&seen, third.data() + kStatusHeader + 8u * mixIndex, 8);
        check(third.size() > kStatusHeader && third[1] == 1 && seen == mix, "page: the next answer has the edit");

        // A preset from the page: all values at once, then its name.
        std::vector<uint8_t> apply(8 + 8u * nParams);
        apply[0] = PageApply;
        std::memcpy(apply.data() + 4, &nParams, 4);
        for (uint32_t i = 0; i < nParams; ++i)
        {
            clap_param_info_t info{};
            params->get_info(plugin, i, &info);
            const double v = info.id == mixId ? 0.456 : info.default_value;
            std::memcpy(apply.data() + 8 + 8u * i, &v, 8);
        }
        webview->receive(plugin, apply.data(), static_cast<uint32_t>(apply.size()));
        std::vector<uint8_t> preset(4 + 2 + 6 + 2 + 0, 0);
        preset[0] = PagePreset;
        putString(preset.data() + 4, "Tested");
        putString(preset.data() + 12, "");
        webview->receive(plugin, preset.data(), static_cast<uint32_t>(preset.size()));
        params->get_value(plugin, mixId, &now);
        check(std::fabs(now - 0.456) < 1e-9, "page: a preset's values arrive");
        Buffer named;
        clap_ostream_t nos{&named, Buffer::write};
        state->save(plugin, &nos);
        const std::string text(named.data.begin(), named.data.end());
        check(text.find("Tested") != std::string::npos, "page: the preset's name is in the state");
    }

    // State round trip.
    Buffer saved;
    clap_ostream_t os{&saved, Buffer::write};
    check(state->save(plugin, &os) && !saved.data.empty(), "state save");
    double mixBefore = 0, mixAfter = 0;
    params->get_value(plugin, mixId, &mixBefore);
    clap_istream_t is{&saved, Buffer::read};
    check(state->load(plugin, &is), "state load");
    params->get_value(plugin, mixId, &mixAfter);
    check(mixBefore == mixAfter, "Mix survives the round trip");

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    clap_entry.deinit();

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
