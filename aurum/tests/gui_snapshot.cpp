// Renders the editor offscreen to a PNG (layout check without a host).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "dsp/ReverbEngine.h"
#include "gui/Editor.h"
#include "state/FfpImport.h"
#include "state/StateIO.h"
#include <fstream>
#include <sstream>

using namespace aurum;

class FakeController : public Controller
{
public:
    FakeController() : values(defaultValues()) {}
    double paramValue(int index) const override { return values[static_cast<size_t>(index)]; }
    void beginEdit(int) override {}
    void performEdit(int index, double v) override { values[static_cast<size_t>(index)] = v; }
    void endEdit(int) override {}
    double sampleRate() const override { return 48000.0; }
    double tempo() const override { return 120.0; }
    std::vector<double> values;
};

int main(int argc, char** argv)
{
    const char* out = argc > 1 ? argv[1] : "snapshot.png";
    const double scale = argc > 2 ? atof(argv[2]) : 1.0;
    FakeController c;
    const ParamTable& t = ParamTable::get();
    auto set = [&](uint32_t id, double v) { c.values[static_cast<size_t>(t.indexOf(id))] = v; };
    using namespace pid;
    set(decay(0, DUsed), 1); set(decay(0, DShape), 1); set(decay(0, DFreq), conv::freqToValue(180)); set(decay(0, DRate), conv::rateLog2ToValue(0.8)); set(decay(0, DQ), conv::qToValue(0.7));
    set(decay(1, DUsed), 1); set(decay(1, DShape), 2); set(decay(1, DFreq), conv::freqToValue(5000)); set(decay(1, DRate), conv::rateLog2ToValue(-1.2)); set(decay(1, DQ), conv::qToValue(0.7));
    set(post(0, PUsed), 1); set(post(0, PShape), 3); set(post(0, PFreq), conv::freqToValue(90)); set(post(0, PSlope), 3); set(post(0, PQ), conv::qToValue(0.7071));
    set(post(1, PUsed), 1); set(post(1, PShape), 0); set(post(1, PFreq), conv::freqToValue(2500)); set(post(1, PGain), conv::gainDbToValue(-4)); set(post(1, PQ), conv::qToValue(1.2));
    set(post(2, PUsed), 1); set(post(2, PShape), 2); set(post(2, PFreq), conv::freqToValue(9000)); set(post(2, PGain), conv::gainDbToValue(3)); set(post(2, PQ), conv::qToValue(0.7));

    // Optional: show an imported .ffp preset instead of the demo bands.
    if (const char* preset = std::getenv("AURUM_SNAPSHOT_PRESET"))
    {
        std::ifstream in(preset);
        std::stringstream ss;
        ss << in.rdbuf();
        StateDocument d;
        if (importFfpPreset(ss.str(), d))
            c.values = d.values;
    }
    gui::Editor ed(c);

    ed.setScale(scale);
    if (argc > 4) // optional logical size
        ed.setPhysicalSize(static_cast<int>(atof(argv[3]) * scale), static_cast<int>(atof(argv[4]) * scale));
    int w, h;
    ed.physicalSize(w, h);
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(s);
    cairo_scale(cr, scale, scale);
    ed.renderTo(cr);
    cairo_destroy(cr);
#ifdef CAIRO_HAS_PNG_FUNCTIONS
    cairo_surface_write_to_png(s, out);
#else
    // The Windows Cairo is built without libpng: write a binary PPM instead.
    if (FILE* f = fopen(out, "wb"))
    {
        cairo_surface_flush(s);
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        const unsigned char* data = cairo_image_surface_get_data(s);
        const int stride = cairo_image_surface_get_stride(s);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                uint32_t px;
                memcpy(&px, data + y * stride + x * 4, 4);
                const unsigned char rgb[3] = {static_cast<unsigned char>(px >> 16), static_cast<unsigned char>(px >> 8),
                                              static_cast<unsigned char>(px)};
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
    }
#endif
    // Frame cost of a full redraw (static layer invalidated each time).
    auto timeIt = [&](const char* what, auto fn) {
        const auto a = std::chrono::steady_clock::now();
        for (int i = 0; i < 60; ++i)
            fn();
        printf("  %-22s %.2f ms\n", what, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count() / 60);
    };
    timeIt("whole tree paint", [&] {
        ed.markDirty();
        cairo_t* c2 = cairo_create(s);
        cairo_scale(c2, scale, scale);
        ed.renderTo(c2);
        cairo_destroy(c2);
    });
    cairo_surface_destroy(s);
    printf("wrote %s (%dx%d)\n", out, w, h);
}
