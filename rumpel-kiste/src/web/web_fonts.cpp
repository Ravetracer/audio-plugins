// See web_fonts.h.

#include "web/web_fonts.h"

#include <cstdio>
#include <vector>

#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace rumpelkiste {

namespace {

constexpr const char *kFontFiles[2] = {"/fonts/NotoSans-Regular.ttf", "/fonts/NotoSans-Bold.ttf"};

struct LoadedFont {
   std::vector<unsigned char> data; // FreeType reads from it for the face's lifetime
   FT_Face face = nullptr;
   cairo_font_face_t *cairoFace = nullptr;
};

FT_Library library() {
   static FT_Library lib = [] {
      FT_Library l = nullptr;
      FT_Init_FreeType(&l);
      return l;
   }();
   return lib;
}

// One face per weight for the whole editor module, which holds one editor.
cairo_font_face_t *face(bool bold) {
   static LoadedFont fonts[2];
   LoadedFont &f = fonts[bold ? 1 : 0];
   if (f.cairoFace)
      return f.cairoFace;
   if (FILE *in = std::fopen(kFontFiles[bold ? 1 : 0], "rb")) {
      std::vector<unsigned char> buf(65536); // not on wasm's small stack
      size_t n;
      while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
         f.data.insert(f.data.end(), buf.data(), buf.data() + n);
      std::fclose(in);
   }
   if (!f.data.empty() && library() &&
       FT_New_Memory_Face(library(), f.data.data(), static_cast<FT_Long>(f.data.size()), 0, &f.face) == 0)
      f.cairoFace = cairo_ft_font_face_create_for_ft_face(f.face, 0);
   if (!f.cairoFace) // an incomplete bundle: Cairo's own font, but still text
      f.cairoFace = cairo_toy_font_face_create("sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                               bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
   return f.cairoFace;
}

// What fontconfig hands the window on the desktop it was laid out on:
// subpixel antialiasing in RGB order, slight hinting, hinted metrics.
cairo_font_options_t *options() {
   static cairo_font_options_t *o = [] {
      cairo_font_options_t *opt = cairo_font_options_create();
      cairo_font_options_set_antialias(opt, CAIRO_ANTIALIAS_SUBPIXEL);
      cairo_font_options_set_subpixel_order(opt, CAIRO_SUBPIXEL_ORDER_RGB);
      cairo_font_options_set_hint_style(opt, CAIRO_HINT_STYLE_SLIGHT);
      cairo_font_options_set_hint_metrics(opt, CAIRO_HINT_METRICS_ON);
      return opt;
   }();
   return o;
}

} // namespace

void webSelectFontFace(cairo_t *cr, const char *, cairo_font_slant_t, cairo_font_weight_t weight) {
   cairo_set_font_face(cr, face(weight == CAIRO_FONT_WEIGHT_BOLD));
   cairo_set_font_options(cr, options());
}

} // namespace rumpelkiste
