#pragma once

// Fonts for the WebAssembly editor (ui.wasm), force-included into the window's
// sources by cmake/wclap.cmake.
//
// The window and the shared toolkit ask Cairo for "sans-serif" through its toy
// font API. On the desktop that goes to fontconfig; in a page there is no font
// configuration, and Cairo would fall back to its built-in stroke font. So the
// toy call is routed here, to the fonts the bundle carries (web/fonts/, put at
// /fonts/ in the editor's file system by the page) and opened through
// FreeType, which is what renders them on Linux as well. Noto Sans is what
// "sans-serif" resolves to on the desktop the window was laid out on.

#include <cairo/cairo.h>

namespace rumpelkiste {
void webSelectFontFace(cairo_t *cr, const char *family, cairo_font_slant_t slant,
                       cairo_font_weight_t weight);
}

#define cairo_select_font_face(cr, family, slant, weight)                                         \
   ::rumpelkiste::webSelectFontFace(cr, family, slant, weight)
