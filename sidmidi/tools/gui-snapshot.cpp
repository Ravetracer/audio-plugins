// sidmidi-gui-snapshot: the plugin's window rendered offscreen into a PNG,
// with no display and no window anywhere -- for the manual's screenshots.
//
//   sidmidi-gui-snapshot OUT.png [options]
//     --load FILE        load a .prg/.d64/.crt as the Load button does
//     --seconds S        run the machine this long first (offline, default 10)
//     --page P           screen (default), machine or cc
//     --scale S          GUI scale (default 1)
//     --size WxH         window size in logical pixels (default 800x622)
//     --sids N           number of SIDs (for the machine page)
//     --model I:NAME     SID I's model (6581, 6581R3, 6581R4, 6581R4AR, 8580R5)
//     --engine NAME      SID engine: resid-fp (default) or lite
//     --open FILE        after the run, open FILE as Load does (a .d64 shows its files)
//     --media-menu       after the run, open the Media menu
//
// The ROMs come from SidMidi's ROM folder or SIDMIDI_ROM_DIR, CC maps from
// SIDMIDI_CCMAP_DIR, like the plugin. Without ROMs the window shows where
// they go, which is a screenshot of its own.

#include <cairo/cairo.h>
#include <clap/clap.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gui/Editor.h"
#include "plugin/SidMidiPlugin.h"

namespace {

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostNoop(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "sidmidi-gui-snapshot", "Ravetracer", "", "1",
                           hostGetExtension, hostNoop, hostNoop, hostNoop};

uint32_t noEvents(const clap_input_events_t*) { return 0; }
const clap_event_header_t* noEvent(const clap_input_events_t*, uint32_t) { return nullptr; }
bool dropEvent(const clap_output_events_t*, const clap_event_header_t*) { return true; }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: sidmidi-gui-snapshot OUT.png [--load FILE] [--seconds S] [--page screen|machine|cc] "
                             "[--scale S] [--size WxH] [--sids N] [--model I:NAME]\n");
        return 2;
    }
    const std::string out = argv[1];
    std::string load, page = "screen", open;
    bool mediaMenu = false;
    double seconds = 10.0, scale = 1.0;
    int w = 800, h = 622, sids = 0;
    bool lite = false;
    std::vector<std::pair<int, std::string>> models;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--load")
            load = next();
        else if (a == "--seconds")
            seconds = std::atof(next().c_str());
        else if (a == "--page")
            page = next();
        else if (a == "--scale")
            scale = std::atof(next().c_str());
        else if (a == "--size")
            std::sscanf(next().c_str(), "%dx%d", &w, &h);
        else if (a == "--open")
            open = next();
        else if (a == "--media-menu")
            mediaMenu = true;
        else if (a == "--sids")
            sids = std::atoi(next().c_str());
        else if (a == "--engine")
            lite = next() == "lite";
        else if (a == "--model") {
            const std::string v = next();
            const auto colon = v.find(':');
            models.push_back({std::atoi(v.substr(0, colon).c_str()) - 1, v.substr(colon + 1)});
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    // Owned by its clap_plugin_t: destroy() deletes it.
    auto& plugin = *new sidmidi::SidMidiPlugin(&kHost);
    const clap_plugin_t* p = plugin.clapPlugin();
    p->init(p);
    if (auto* render = static_cast<const clap_plugin_render_t*>(p->get_extension(p, CLAP_EXT_RENDER)))
        render->set(p, CLAP_RENDER_OFFLINE); // fixed warp: the same picture every time
    p->activate(p, 48000.0, 1, 512);
    p->start_processing(p);

    if (sids > 0 || !models.empty() || lite) {
        sidmidi::MachineSettings s = plugin.settings();
        if (sids > 0)
            s.sids.count = sids;
        if (lite)
            s.sids.engine = sidmidi::emu::SidEngine::Lite;
        for (const auto& m : models)
            for (int v = 0; v < static_cast<int>(sidmidi::emu::SidVariant::Count); ++v)
                if (m.first >= 0 && m.first < 8 &&
                    m.second == sidmidi::emu::sidVariantName(static_cast<sidmidi::emu::SidVariant>(v))) {
                    auto& c = s.sids.chips[static_cast<size_t>(m.first)];
                    c.variant = static_cast<sidmidi::emu::SidVariant>(v);
                    c.tuning = sidmidi::emu::sidVariantTuning(c.variant);
                }
        plugin.applySettings(s);
    }
    if (!load.empty()) {
        std::string err;
        if (!plugin.loadFile(load, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
    }

    // The window, before the machine runs, so the screen is drawn.
    auto editorPtr = std::make_unique<sidmidi::gui::Editor>(plugin);
    auto& editor = *editorPtr;
    plugin.engine().videoWanted = true;

    std::vector<float> l(512), r(512), aux(512);
    float* main[2] = {l.data(), r.data()};
    clap_audio_buffer_t bufs[9]{};
    bufs[0].data32 = main;
    bufs[0].channel_count = 2;
    float* auxPtr[8];
    for (int i = 0; i < 8; ++i) {
        auxPtr[i] = aux.data();
        bufs[i + 1].data32 = &auxPtr[i];
        bufs[i + 1].channel_count = 1;
    }
    clap_input_events_t in{nullptr, noEvents, noEvent};
    clap_output_events_t outEv{nullptr, dropEvent};
    const int blocks = static_cast<int>(seconds * 48000.0 / 512.0);
    for (int b = 0; b < blocks; ++b) {
        clap_process_t pr{};
        pr.frames_count = 512;
        pr.steady_time = -1;
        pr.audio_outputs = bufs;
        pr.audio_outputs_count = 9;
        pr.in_events = &in;
        pr.out_events = &outEv;
        p->process(p, &pr);
    }

    editor.setScale(scale);
    editor.setBounds({0, 0, static_cast<float>(w), static_cast<float>(h)});
    if (page == "machine")
        editor.showPage(sidmidi::gui::Editor::Page::Machine);
    else if (page == "cc")
        editor.showPage(sidmidi::gui::Editor::Page::Cc);
    for (int i = 0; i < 3; ++i)
        editor.onTimer(); // pulls the frame, syncs the pages, polls the media line
    if (!open.empty())
        editor.openMedia(open);
    if (mediaMenu)
        editor.showDiskMenu();
    editor.onTimer();

    const int pw = static_cast<int>(w * scale + 0.5), ph = static_cast<int>(h * scale + 0.5);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw, ph);
    cairo_t* cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    editor.renderTo(cr);
    cairo_destroy(cr);
    const bool ok = cairo_surface_write_to_png(surface, out.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);

    editorPtr.reset();
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    std::printf("%s %dx%d\n", out.c_str(), pw, ph);
    return ok ? 0 : 1;
}
