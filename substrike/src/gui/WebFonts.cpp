// Fonts of the WebAssembly editor: Inter, embedded in the WCLAP bundle
// (web/fonts/, SIL Open Font License) and copied by the page into the editor's
// file system before the editor starts. FreeType renders them, as it does on
// Linux, so the text looks as it does in the native window.
#if defined(__wasm__)

#include <cstdio>
#include <vector>

#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "Graphics.h"

namespace substrike::gui {

namespace {

// Medium and Bold: what "Inter" and "Inter Bold" resolve to on the Linux
// desktop the editor was laid out on.
constexpr const char* kFontFiles[2] = {"/fonts/Inter-Medium.ttf", "/fonts/Inter-Bold.ttf"};

struct LoadedFont
{
    std::vector<unsigned char> data; // FreeType reads from it for the face's lifetime
    FT_Face face = nullptr;
    cairo_font_face_t* cairoFace = nullptr;
};

FT_Library library()
{
    static FT_Library lib = [] {
        FT_Library l = nullptr;
        FT_Init_FreeType(&l);
        return l;
    }();
    return lib;
}

cairo_font_face_t* load(const char* path)
{
    static LoadedFont fonts[2];
    LoadedFont& f = fonts[path == kFontFiles[0] ? 0 : 1];
    if (f.cairoFace)
        return f.cairoFace;
    if (FILE* in = std::fopen(path, "rb"))
    {
        std::vector<unsigned char> buf(65536); // not on wasm's small stack
        size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
            f.data.insert(f.data.end(), buf.data(), buf.data() + n);
        std::fclose(in);
    }
    if (!f.data.empty() && library() &&
        FT_New_Memory_Face(library(), f.data.data(), static_cast<FT_Long>(f.data.size()), 0, &f.face) == 0)
        f.cairoFace = cairo_ft_font_face_create_for_ft_face(f.face, 0);
    if (!f.cairoFace) // the bundle is incomplete: Cairo's built-in font, but text
        f.cairoFace = cairo_toy_font_face_create("sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                                 path == kFontFiles[1] ? CAIRO_FONT_WEIGHT_BOLD
                                                                       : CAIRO_FONT_WEIGHT_NORMAL);
    return f.cairoFace;
}

} // namespace

cairo_font_face_t* webFontFace(bool bold) { return load(kFontFiles[bold ? 1 : 0]); }

// Grey antialiasing and slight hinting, a typical Linux desktop's settings.
void webFontOptions(cairo_t* cr)
{
    static cairo_font_options_t* options = [] {
        cairo_font_options_t* o = cairo_font_options_create();
        cairo_font_options_set_antialias(o, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_style(o, CAIRO_HINT_STYLE_SLIGHT);
        cairo_font_options_set_hint_metrics(o, CAIRO_HINT_METRICS_ON);
        return o;
    }();
    cairo_set_font_options(cr, options);
}

} // namespace substrike::gui

#endif
