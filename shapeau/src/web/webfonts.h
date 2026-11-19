#pragma once

// Force-included (-include) into every file of the WCLAP's editor, ui.wasm.
//
// The window draws its text through Cairo's toy font API, which on the desktop
// asks fontconfig for "sans-serif". The WebAssembly Cairo has no fontconfig,
// and its toy API would fall back to a stroked built-in font. So the editor's
// calls to cairo_select_font_face() -- in the shared toolkit and in the window
// -- are routed to selectFontFace(), which sets Noto Sans, the face the
// desktop resolves "sans-serif" to, loaded from the bundle's fonts/ (OFL).

#if defined(__cplusplus) && defined(__wasm__)

#include <cairo/cairo.h>

namespace shapeau::web {
void selectFontFace(cairo_t *cr, const char *family, cairo_font_slant_t slant,
                    cairo_font_weight_t weight);
}

#define cairo_select_font_face(cr, family, slant, weight)                                         \
   ::shapeau::web::selectFontFace(cr, family, slant, weight)

#endif
