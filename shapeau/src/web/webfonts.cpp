// The fonts of the WCLAP's editor; see webfonts.h. The page copies the bundle's
// fonts into the editor's file system before it starts, and FreeType renders
// them, as it does on Linux, so the text looks as it does in the native window.

#include "web/webfonts.h"

#include <cstdio>
#include <vector>

#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace shapeau::web {

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
       FT_New_Memory_Face(library(), f.data.data(), static_cast<FT_Long>(f.data.size()), 0,
                          &f.face) == 0)
      f.cairoFace = cairo_ft_font_face_create_for_ft_face(f.face, 0);
   if (!f.cairoFace) // the bundle is incomplete: Cairo's built-in font, but text
      f.cairoFace = cairo_toy_font_face_create(
         "sans-serif", CAIRO_FONT_SLANT_NORMAL,
         bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
   return f.cairoFace;
}

// Grey antialiasing and slight hinting, a typical Linux desktop's settings.
cairo_font_options_t *options() {
   static cairo_font_options_t *o = [] {
      cairo_font_options_t *created = cairo_font_options_create();
      cairo_font_options_set_antialias(created, CAIRO_ANTIALIAS_GRAY);
      cairo_font_options_set_hint_style(created, CAIRO_HINT_STYLE_SLIGHT);
      cairo_font_options_set_hint_metrics(created, CAIRO_HINT_METRICS_ON);
      return created;
   }();
   return o;
}

} // namespace

void selectFontFace(cairo_t *cr, const char *, cairo_font_slant_t, cairo_font_weight_t weight) {
   cairo_set_font_face(cr, face(weight == CAIRO_FONT_WEIGHT_BOLD));
   cairo_set_font_options(cr, options());
}

} // namespace shapeau::web
