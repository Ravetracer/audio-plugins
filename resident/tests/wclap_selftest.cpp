// Self-test of the WebAssembly build (the WCLAP): drives the plugin through
// clap_entry as a WCLAP host does, and plays the page's part of clap.webview
// with the messages of plugin/WebProtocol.h. Built as a WASI command from the
// same objects as module.wasm and run by tools/run-wclap-selftest.mjs, which
// hands in a ROM folder and, optionally, a program:
//
//   node tools/run-wclap-selftest.mjs build-wasm/resident-wclap-selftest.wasm _dev_/roms [program.prg]
//
// The ROMs are read from that folder at run time and uploaded the way the
// editor uploads them; none is compiled in.
//
// Checks: the factory, the descriptor and the webview API; the machine waits
// without ROMs; the ROMs go in through the page protocol, are recognised and
// the machine boots; READY. reaches the page in the streamed screen; keys
// typed through the page reach the C64 (PRINT 6*7 answers 42); a state round
// trip into a fresh instance brings the ROMs and the machine back and plays
// on bit for bit; a state without ROMs (as 1.1.0 wrote it) still loads and
// waits for them; a program uploaded through the page starts and plays a
// MIDI note. With --bench, the time per 128-frame block on both SID engines
// and the screen traffic.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <clap/clap.h>
#include <clap/ext/draft/webview.h>

#include "emu/C64.h"
#include "emu/Vic.h"
#include "engine/Resources.h"
#include "plugin/MachineSettings.h"
#include "plugin/WebProtocol.h"

extern "C" const clap_plugin_entry_t clap_entry;

using namespace resident;

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

// --------------------------------------------------------------- host side

// What the plugin sends to the page, for the instance being driven.
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

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "resident-wclap-selftest", "Ravetracer", "", "0",
                           hostGetExtension,  hostNoop, hostNoop,                  hostNoop};

struct EventList
{
    std::vector<clap_event_midi_t> midi;
    static uint32_t size(const clap_input_events_t* l)
    {
        return static_cast<uint32_t>(static_cast<const EventList*>(l->ctx)->midi.size());
    }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i)
    {
        return &static_cast<const EventList*>(l->ctx)->midi[i].header;
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
constexpr int W = emu::Vic::kWidth, H = emu::Vic::kHeight;

const clap_plugin_factory_t* factory = nullptr;

// One plugin instance and the page that talks to it.
struct Instance
{
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_webview_t* webview = nullptr;
    const clap_plugin_state_t* state = nullptr;
    std::vector<float> outL = std::vector<float>(kBlock), outR = std::vector<float>(kBlock);
    EventList events;
    int64_t steady = 0;

    // The page's mirror.
    uint32_t serials[6] = {};
    std::vector<uint8_t> frame = std::vector<uint8_t>(static_cast<size_t>(W) * H, 0);
    uint8_t flags = 0, romsInUse = 0, romsInState = 0;
    std::string notice, settings, ccMap;
    bool noticeError = false;
    int screenUpdates = 0;
    long screenLines = 0;
    size_t received = 0;
    double processSeconds = 0;
    double processMax = 0;
    long blocks = 0;

    explicit Instance(bool offline)
    {
        plugin = factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
        if (!plugin || !plugin->init(plugin))
            return;
        webview = static_cast<const clap_plugin_webview_t*>(plugin->get_extension(plugin, CLAP_EXT_WEBVIEW));
        state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
        if (offline)
            if (auto* r = static_cast<const clap_plugin_render_t*>(plugin->get_extension(plugin, CLAP_EXT_RENDER)))
                r->set(plugin, CLAP_RENDER_OFFLINE);
        plugin->activate(plugin, kRate, 1, kBlock);
        plugin->start_processing(plugin);
    }
    ~Instance()
    {
        if (!plugin)
            return;
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }

    void send(const std::vector<uint8_t>& m)
    {
        sentToPage.clear();
        webview->receive(plugin, m.data(), static_cast<uint32_t>(m.size()));
        for (const auto& msg : sentToPage)
            handle(msg);
        sentToPage.clear();
    }

    void poll(bool screen)
    {
        std::vector<uint8_t> m(web::kPollSize, 0);
        m[0] = web::PagePoll;
        m[1] = screen ? 1 : 0;
        std::memcpy(m.data() + 4, serials, sizeof(serials));
        send(m);
    }

    void handle(const std::vector<uint8_t>& m)
    {
        received += m.size();
        if (m.size() < web::kStatusHeader || m[0] != web::PluginStatus)
            return;
        flags = m[1];
        romsInUse = m[2];
        romsInState = m[3];
        const uint16_t sections = web::get<uint16_t>(m.data(), 12);
        size_t pos = web::kStatusHeader;
        for (int s = 0; s < sections && pos + web::kSectionHeader <= m.size(); ++s) {
            const uint8_t id = m[pos];
            const uint32_t serial = web::get<uint32_t>(m.data(), pos + 4);
            const uint32_t size = web::get<uint32_t>(m.data(), pos + 8);
            const uint8_t* p = m.data() + pos + web::kSectionHeader;
            pos += web::kSectionHeader + size;
            if (pos > m.size())
                break;
            switch (id) {
            case web::SectionValues: serials[0] = serial; break;
            case web::SectionSettings:
                serials[1] = serial;
                settings.assign(reinterpret_cast<const char*>(p), size);
                break;
            case web::SectionCcMap:
                serials[2] = serial;
                ccMap.assign(reinterpret_cast<const char*>(p), size);
                break;
            case web::SectionMedia: serials[3] = serial; break;
            case web::SectionNotice:
                serials[4] = serial;
                noticeError = size > 0 && p[0];
                notice.assign(reinterpret_cast<const char*>(p + 1), size > 0 ? size - 1 : 0);
                break;
            case web::SectionScreen: {
                const uint32_t base = web::get<uint32_t>(p, 0);
                if (base != 0 && base != serials[5])
                    break; // a diff against a frame this page does not have
                const uint16_t lines = web::get<uint16_t>(p, 4);
                size_t q = 8;
                bool ok = true;
                for (int i = 0; i < lines && ok; ++i) {
                    const uint16_t y = web::get<uint16_t>(p, q);
                    const uint8_t mode = p[q + 2];
                    const uint16_t len = web::get<uint16_t>(p, q + 4);
                    ok = y < H && q + 6 + len <= size &&
                         web::decodeLine(p + q + 6, len, mode, &frame[static_cast<size_t>(y) * W], W);
                    q += 6u + len;
                }
                if (ok) {
                    serials[5] = serial;
                    ++screenUpdates;
                    screenLines += lines;
                }
                break;
            }
            default: break;
            }
        }
    }

    // One block of audio, timed. Returns the peak.
    float block()
    {
        float* out[2] = {outL.data(), outR.data()};
        clap_audio_buffer_t aout{out, nullptr, 2, 0, 0};
        clap_input_events_t ie{&events, EventList::size, EventList::get};
        clap_output_events_t oe{nullptr, pushOut};
        clap_process_t pr{};
        pr.steady_time = steady;
        pr.frames_count = kBlock;
        pr.audio_outputs = &aout;
        pr.audio_outputs_count = 1;
        pr.in_events = &ie;
        pr.out_events = &oe;
        const auto t0 = std::chrono::steady_clock::now();
        plugin->process(plugin, &pr);
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        processSeconds += dt;
        processMax = std::max(processMax, dt);
        ++blocks;
        events.midi.clear();
        steady += kBlock;
        float peak = 0.0f;
        for (uint32_t i = 0; i < kBlock; ++i)
            peak = std::max({peak, std::fabs(outL[i]), std::fabs(outR[i])});
        return peak;
    }

    // Runs `seconds`, polling like the page (about 30 times a second).
    float run(double seconds, bool screen = true)
    {
        const long n = static_cast<long>(seconds * kRate / kBlock);
        float peak = 0.0f;
        for (long i = 0; i < n; ++i) {
            peak = std::max(peak, block());
            if (i % 12 == 0)
                poll(screen);
        }
        return peak;
    }

    void midi(uint8_t a, uint8_t b, uint8_t c)
    {
        clap_event_midi_t e{};
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
        e.port_index = 0;
        e.data[0] = a;
        e.data[1] = b;
        e.data[2] = c;
        events.midi.push_back(e);
    }

    void upload(uint8_t kind, const std::string& name, const std::vector<uint8_t>& data)
    {
        for (uint32_t off = 0; off == 0 || off < data.size(); off += web::kUploadChunk) {
            const uint32_t n = std::min<uint32_t>(web::kUploadChunk, static_cast<uint32_t>(data.size()) - off);
            std::vector<uint8_t> m{web::PageUpload, kind, 0, 0};
            web::put<uint32_t>(m, static_cast<uint32_t>(data.size()));
            web::put<uint32_t>(m, off);
            web::putString(m, name);
            m.insert(m.end(), data.begin() + off, data.begin() + off + n);
            send(m);
        }
    }

    void key(uint8_t pos, bool down)
    {
        std::vector<uint8_t> m{web::PageInput, web::InputKey, pos, static_cast<uint8_t>(down ? 1 : 0), 0, 0, 0, 0};
        send(m);
    }

    // Types like a person: each key held for three frames, then let go.
    void type(const std::vector<uint8_t>& keys)
    {
        for (uint8_t k : keys) {
            const bool shifted = k & 0x80;
            if (shifted)
                key(emu::keys::LShift, true);
            key(k & 0x7f, true);
            run(0.06);
            key(k & 0x7f, false);
            if (shifted)
                key(emu::keys::LShift, false);
            run(0.06);
        }
    }

    void settingsMessage(const std::string& text)
    {
        std::vector<uint8_t> m{web::PageSettings, 0, 0, 0};
        m.insert(m.end(), text.begin(), text.end());
        send(m);
    }

    std::vector<uint8_t> saveState()
    {
        Buffer b;
        clap_ostream_t os{&b, Buffer::write};
        return state->save(plugin, &os) ? b.data : std::vector<uint8_t>{};
    }
    bool loadState(const std::vector<uint8_t>& data)
    {
        Buffer b;
        b.data = data;
        clap_istream_t is{&b, Buffer::read};
        return state->load(plugin, &is);
    }
};

// Where `text` (screen codes) stands on the streamed screen: each glyph from
// the character ROM, one colour for its set bits and another for the rest.
bool screenShows(const std::vector<uint8_t>& frame, const std::vector<uint8_t>& chargen, const std::vector<uint8_t>& text)
{
    if (chargen.size() != 4096)
        return false;
    const int tw = 8 * static_cast<int>(text.size());
    for (int y0 = 0; y0 + 8 <= H; ++y0)
        for (int x0 = 0; x0 + tw <= W; ++x0) {
            int fg = -1, bg = -1;
            bool ok = true;
            for (size_t c = 0; c < text.size() && ok; ++c)
                for (int r = 0; r < 8 && ok; ++r) {
                    const uint8_t bits = chargen[text[c] * 8u + static_cast<unsigned>(r)];
                    for (int b = 0; b < 8 && ok; ++b) {
                        const int px = frame[static_cast<size_t>(y0 + r) * W + static_cast<size_t>(x0) + c * 8 + b];
                        int& want = (bits & (0x80 >> b)) ? fg : bg;
                        if (want < 0)
                            want = px;
                        ok = want == px;
                    }
                }
            if (ok && fg != bg && fg >= 0 && bg >= 0)
                return true;
        }
    return false;
}

std::vector<uint8_t> screenCodes(const char* s)
{
    std::vector<uint8_t> out;
    for (; *s; ++s) {
        const char c = *s;
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<uint8_t>(c - 'A' + 1) : static_cast<uint8_t>(c));
    }
    return out;
}

void uploadRoms(Instance& inst, const emu::Roms& roms)
{
    inst.upload(web::UploadRom, "basic-901226-01.bin", roms.basic);
    inst.upload(web::UploadRom, "chargen-901225-01.bin", roms.chargen);
    if (roms.drive1541.size() == 16384)
        inst.upload(web::UploadRom, "dos1541.bin", roms.drive1541);
    inst.upload(web::UploadRom, "kernal-901227-03.bin", roms.kernal);
}

// The time per 128-frame block, with the machine running a program (or
// BASIC) and a note held, after the start.
void bench(const emu::Roms& roms, const std::vector<uint8_t>& program, const std::string& programName, bool lite,
           int sids, bool screen = true)
{
    Instance inst(false);
    uploadRoms(inst, roms);
    MachineSettings s;
    s.fromText(inst.settings);
    s.sids.engine = lite ? emu::SidEngine::Lite : emu::SidEngine::ReSidFp;
    s.sids.count = sids;
    inst.settingsMessage(s.toText());
    if (!program.empty())
        inst.upload(web::UploadLoad, programName, program);
    inst.run(8.0, screen);
    inst.midi(0x90, 60, 100);
    inst.run(0.5, screen);
    inst.processSeconds = 0;
    inst.processMax = 0;
    inst.blocks = 0;
    inst.screenUpdates = 0;
    inst.screenLines = 0;
    const size_t received0 = inst.received;
    inst.run(10.0, screen);
    std::printf("      bench %-9s %-8s screen %-3s: %6.1f us per %u-frame block (max %6.1f us), %4.1f %% of realtime, "
                "page traffic %.0f bytes/s\n",
                program.empty() ? "BASIC" : programName.substr(0, 9).c_str(), lite ? "Light" : "Accurate",
                screen ? "on" : "off", 1e6 * inst.processSeconds / inst.blocks, kBlock, 1e6 * inst.processMax,
                100.0 * inst.processSeconds / (inst.blocks * kBlock / kRate), (inst.received - received0) / 10.0);
    if (screen && inst.screenUpdates)
        std::printf("      %.1f screen updates/s, %.1f lines each\n", inst.screenUpdates / 10.0,
                    static_cast<double>(inst.screenLines) / inst.screenUpdates);
}

} // namespace

int main(int argc, char** argv)
{
    std::string romDir = "/roms", programPath;
    bool doBench = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--bench")
            doBench = true;
        else if (a == "--roms" && i + 1 < argc)
            romDir = argv[++i];
        else if (a == "--program" && i + 1 < argc)
            programPath = argv[++i];
    }

    check(clap_entry.init(""), "clap_entry.init");
    factory = static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(factory && factory->get_plugin_count(factory) == 1, "plugin factory with one plugin");
    if (!factory)
        return 1;
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    std::printf("      %s %s (%s)\n", desc->name, desc->version, desc->id);
    check(!std::strcmp(desc->id, "de.ravetracer.sidmidi"), "the plugin id is unchanged");

    emu::Roms roms;
    std::string report;
    romsFromFolder(romDir, roms, report);
    check(roms.complete(), "the test's ROMs are readable from " + romDir);
    if (!roms.complete())
        return 1;

    const auto t0 = std::chrono::steady_clock::now();
    auto a = std::make_unique<Instance>(true);
    std::printf("      create + init + activate: %.0f ms\n",
                1e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    check(a->plugin != nullptr, "create + init");
    if (!a->plugin)
        return 1;
    const auto* gui = static_cast<const clap_plugin_gui_t*>(a->plugin->get_extension(a->plugin, CLAP_EXT_GUI));
    check(gui && gui->is_api_supported(a->plugin, CLAP_WINDOW_API_WEBVIEW, false) &&
              !gui->is_api_supported(a->plugin, CLAP_WINDOW_API_X11, false),
          "gui: the webview API only");
    check(a->webview != nullptr, "clap.webview offered");
    if (!a->webview)
        return 1;
    char uri[64] = {};
    check(a->webview->get_uri(a->plugin, uri, sizeof(uri)) > 0 && !std::strcmp(uri, "/index.html"), "page /index.html");

    // Without ROMs: silent, waiting, and the page hears that.
    a->run(0.5);
    check(!(a->flags & web::StatusHasRoms) && a->romsInUse == 0, "without ROMs the machine waits");
    check(!a->settings.empty() && a->serials[2] != 0, "the first answer carries the settings and the CC map");

    // Something that is no ROM is refused, with a notice.
    a->upload(web::UploadRom, "notes.txt", std::vector<uint8_t>(8192, 0x55));
    a->poll(true);
    check(a->noticeError && a->romsInState == 0, "a file that is no ROM is refused: " + a->notice);

    // The ROMs through the page protocol, one by one; the machine starts with the last.
    a->upload(web::UploadRom, "basic-901226-01.bin", roms.basic);
    a->upload(web::UploadRom, "chargen-901225-01.bin", roms.chargen);
    a->poll(true);
    check(!(a->flags & web::StatusHasRoms) && a->romsInState == (2 | 4), "two of three: still waiting (" + a->notice + ")");
    a->upload(web::UploadRom, "kernal-901227-03.bin", roms.kernal);
    a->poll(true);
    check((a->flags & web::StatusHasRoms) && a->romsInUse == 7, "KERNAL, BASIC and character ROM: the machine boots (" +
                                                                    a->notice + ")");
    a->upload(web::UploadRom, "dos1541ii-251968-03.bin", roms.drive1541);
    a->poll(true);
    check((a->flags & web::StatusHasDriveRom) && a->romsInState == 15, "the 1541 DOS is taken too");
    check(dirtyMarks > 0, "uploads mark the state dirty");

    // READY. on the streamed screen.
    a->screenUpdates = 0;
    a->run(4.0);
    check(a->screenUpdates > 0, "the screen streams to the page (" + std::to_string(a->screenUpdates) + " updates)");
    check(screenShows(a->frame, roms.chargen, screenCodes("READY.")), "READY. reaches the page");

    // Keys through the page: PRINT 6*7 and RETURN.
    using namespace emu::keys;
    a->type({P, R, I, N, T, Space, K6, Asterisk, K7, Return});
    a->run(0.5);
    check(screenShows(a->frame, roms.chargen, screenCodes(" 42")), "typed through the page, PRINT 6*7 answers 42");

    // The screen's traffic, BASIC with its cursor blinking.
    {
        const size_t r0 = a->received;
        a->run(5.0);
        std::printf("      page traffic at the READY prompt: %.0f bytes/s\n", (a->received - r0) / 5.0);
        // A page that has no frame yet gets it whole.
        const uint32_t had = a->serials[5];
        a->serials[5] = 0;
        const size_t r1 = a->received;
        a->poll(true);
        std::printf("      a whole screen for a page that has none: %zu bytes\n", a->received - r1);
        check(a->serials[5] == had || a->serials[5] > had, "a page without a frame gets the whole screen");
    }

    // State: into a fresh instance, which needs no upload and plays on exactly.
    const std::vector<uint8_t> st = a->saveState();
    check(st.size() > 8192 * 2 + 4096, "state saved (" + std::to_string(st.size()) + " bytes, with the ROMs)");
    {
        Instance b(true);
        check(b.loadState(st), "state loads into a fresh instance");
        b.poll(true);
        check((b.flags & web::StatusHasRoms) && b.romsInState == 15, "the ROMs come back with the state");
        // Both run on from the same point: the same audio, the same screen.
        a->midi(0x90, 60, 100);
        b.midi(0x90, 60, 100);
        bool same = true;
        for (int i = 0; i < 400; ++i) {
            a->block();
            b.block();
            same = same && std::memcmp(a->outL.data(), b.outL.data(), kBlock * sizeof(float)) == 0;
        }
        check(same, "the restored machine plays on bit for bit");
        b.run(0.5);
        a->run(0.5);
        check(screenShows(b.frame, roms.chargen, screenCodes(" 42")), "the restored screen shows the answer");
    }

    // A state as 1.1.0 wrote it (version 1, no ROMs): it loads and waits.
    {
        std::vector<uint8_t> old = st;
        // The four ROM blobs are the last ones: [u32 size][bytes] each.
        size_t end = old.size();
        for (size_t n : {roms.drive1541.size(), roms.chargen.size(), roms.basic.size(), roms.kernal.size()})
            end -= 4 + n;
        old.resize(end);
        old[8] = 1;
        Instance c(true);
        check(c.loadState(old), "a state without ROMs (1.1.0) loads");
        c.poll(true);
        check(!(c.flags & web::StatusHasRoms), "and waits for the ROMs");
        uploadRoms(c, roms);
        c.run(1.0);
        check((c.flags & web::StatusHasRoms) && screenShows(c.frame, roms.chargen, screenCodes(" 42")),
              "the ROMs uploaded, its machine runs on where it was saved");
    }

    // A program through the page, and a MIDI note that plays.
    std::vector<uint8_t> program;
    if (!programPath.empty() && readFile(programPath, program) && !program.empty()) {
        Instance p(false);
        uploadRoms(p, roms);
        const std::string name = programPath.substr(programPath.find_last_of('/') + 1);
        p.upload(web::UploadLoad, name, program);
        p.poll(true);
        check(!p.noticeError, "program uploaded: " + p.notice);
        p.run(6.0);
        float quiet = p.run(0.5);
        p.midi(0x90, 60, 110);
        const float peak = p.run(1.0);
        p.midi(0x80, 60, 0);
        p.run(0.2);
        std::printf("      %s: peak %.4f before the note, %.4f with it\n", name.c_str(), quiet, peak);
        check(peak > 0.01f && peak > quiet * 2, "a MIDI note plays the program");
        if (doBench) {
            for (bool screen : {true, false}) {
                bench(roms, program, name, false, 1, screen);
                bench(roms, program, name, true, 1, screen);
            }
        }
    } else if (!programPath.empty()) {
        check(false, "the program " + programPath + " is readable");
    }
    if (doBench) {
        // BASIC alone: the machine without a SID at work (a SID nobody wrote
        // to is not clocked).
        bench(roms, {}, "", false, 1, true);
        bench(roms, {}, "", true, 1, true);
        bench(roms, {}, "", false, 1, false);
    }

    a.reset();
    clap_entry.deinit();
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
