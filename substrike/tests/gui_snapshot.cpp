// Renders the editor offscreen to a PNG, without a host or a display, and
// reports what a full repaint costs.
//
//   substrike-gui-snapshot out.png [--scale 1.5] [--size 1344x864]
//       [--state file.substrike] [--lane 1..8|master] [--slot 1..6] [--tab pitch|amp]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "gui/Editor.h"
#include "state/StateIO.h"

using namespace substrike;

namespace {

class FakeController : public Controller
{
public:
    FakeController() : values(defaultValues()) {}
    double paramValue(int index) const override { return values[static_cast<size_t>(index)]; }
    void beginEdit(int) override {}
    void performEdit(int index, double v) override { values[static_cast<size_t>(index)] = v; }
    void endEdit(int) override {}
    const dsp::Curve& curve(int index) const override { return curves[static_cast<size_t>(index)]; }
    void setCurve(int index, const dsp::Curve& c) override { curves[static_cast<size_t>(index)] = c; }
    void audition() override {}
    double sampleRate() const override { return 48000.0; }

    std::vector<double> values;
    std::array<dsp::Curve, dsp::kNumCurves> curves{};
};

bool writeImage(cairo_surface_t* s, const char* path)
{
#ifdef CAIRO_HAS_PNG_FUNCTIONS
    return cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS;
#else
    // The Windows Cairo is built without libpng: a binary PPM instead.
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    cairo_surface_flush(s);
    const int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    const unsigned char* data = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            uint32_t px;
            std::memcpy(&px, data + y * stride + x * 4, 4);
            const unsigned char rgb[3] = {static_cast<unsigned char>(px >> 16), static_cast<unsigned char>(px >> 8),
                                          static_cast<unsigned char>(px)};
            std::fwrite(rgb, 1, 3, f);
        }
    std::fclose(f);
    return true;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    const char* out = "snapshot.png";
    double scale = 1.0;
    float width = 0, height = 0;
    std::string statePath, lane = "1", tab = "pitch";
    int slot = 1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--scale")
            scale = std::atof(next().c_str());
        else if (a == "--size")
            std::sscanf(next().c_str(), "%fx%f", &width, &height);
        else if (a == "--state")
            statePath = next();
        else if (a == "--lane")
            lane = next();
        else if (a == "--slot")
            slot = std::atoi(next().c_str());
        else if (a == "--tab")
            tab = next();
        else
            out = argv[i];
    }

    FakeController c;
    if (!statePath.empty())
    {
        std::ifstream in(statePath);
        std::stringstream ss;
        ss << in.rdbuf();
        StateDocument d;
        if (!parseState(ss.str(), d))
        {
            std::fprintf(stderr, "not a Substrike state: %s\n", statePath.c_str());
            return 1;
        }
        c.values = d.values;
        c.curves = d.curves;
    }

    gui::Editor ed(c);
    ed.setScale(scale);
    if (width > 0 && height > 0)
        ed.setPhysicalSize(static_cast<int>(width * scale), static_cast<int>(height * scale));
    ed.selectLane(lane == "master" ? gui::Editor::kMaster : std::atoi(lane.c_str()) - 1);
    ed.selectSlot(slot - 1);
    ed.selectCurveTab(tab == "amp" ? 1 : 0);
    ed.settlePreview();

    int w, h;
    ed.physicalSize(w, h);
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(s);
    cairo_scale(cr, scale, scale);
    ed.renderTo(cr);

    // A full repaint (everything stale) and a frame with nothing changed.
    using clock = std::chrono::steady_clock;
    constexpr int kFrames = 20;
    auto t0 = clock::now();
    for (int i = 0; i < kFrames; ++i)
    {
        ed.markDirty();
        ed.renderTo(cr);
    }
    auto t1 = clock::now();
    for (int i = 0; i < kFrames; ++i)
        ed.renderTo(cr);
    auto t2 = clock::now();
    cairo_destroy(cr);
    const double full = std::chrono::duration<double, std::milli>(t1 - t0).count() / kFrames;
    const double idle = std::chrono::duration<double, std::milli>(t2 - t1).count() / kFrames;
    std::printf("%dx%d: full repaint %.2f ms, unchanged frame %.2f ms\n", w, h, full, idle);
    const bool ok = writeImage(s, out);
    cairo_surface_destroy(s);
    return ok ? 0 : 1;
}
