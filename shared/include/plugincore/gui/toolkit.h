#pragma once

// The drawing toolkit every plugin window here is built from.
//
// Both platforms draw through Cairo -- on Linux against X11, on Windows against
// Cairo's win32 backend -- so everything here is platform independent except
// the X error handler, which is guarded.
//
// A plugin keeps its own palette and its own panel layout; what lives here is
// the machinery they are drawn with.

#include <algorithm>
#include <cstddef>
#include <cmath>

#include <cairo/cairo.h>

// MinGW's <cmath> hides M_PI unless _USE_MATH_DEFINES is set before it, which
// is not something a header can rely on having happened. The arcs below need
// it, so define it if it is missing.
#ifndef M_PI
#   define M_PI 3.14159265358979323846
#endif

#if !defined(_WIN32)
#   include <X11/Xlib.h>
#endif

#include "plugincore/params.h"

namespace plugincore {

// ------------------------------------------------------------------- colour

struct Rgb {
   double r, g, b;
};


inline void setColor(cairo_t *cr, const Rgb &c, double a = 1.0) {
   cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}


inline void roundedRect(cairo_t *cr, double x, double y, double w, double h, double r) {
   cairo_new_sub_path(cr);
   cairo_arc(cr, x + w - r, y + r, r, -0.5 * M_PI, 0.0);
   cairo_arc(cr, x + w - r, y + h - r, r, 0.0, 0.5 * M_PI);
   cairo_arc(cr, x + r, y + h - r, r, 0.5 * M_PI, M_PI);
   cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
   cairo_close_path(cr);
}


// Button numbers follow X11's, which is what the shared code was written
// against; the Win32 side translates into them.
constexpr unsigned kButtonLeft = 1;
constexpr unsigned kButtonRight = 3;
constexpr unsigned kWheelUp = 4;
constexpr unsigned kWheelDown = 5;

enum class Align { Left, Center, Right };

#if !defined(_WIN32)
// Asking an embedded window for the keyboard focus can fail for reasons that
// are none of the plugin's business. A failure must not reach Xlib's default
// handler, which exits the host.
inline int ignoreXError(Display *, XErrorEvent *) { return 0; }
#endif

// ------------------------------------------------------------------ drawing

// Drawn rather than typed: a glyph like U+25C0 is not in every sans font, and
// a missing-glyph box in the preset bar would look like a bug.
inline void drawTriangle(cairo_t *cr, double cx, double cy, double size, int dir) {
   const double h = size * 0.5;
   cairo_new_path(cr);
   if (dir < 0) {
      cairo_move_to(cr, cx + h * 0.7, cy - h);
      cairo_line_to(cr, cx + h * 0.7, cy + h);
      cairo_line_to(cr, cx - h * 0.7, cy);
   } else if (dir > 0) {
      cairo_move_to(cr, cx - h * 0.7, cy - h);
      cairo_line_to(cr, cx - h * 0.7, cy + h);
      cairo_line_to(cr, cx + h * 0.7, cy);
   } else {
      cairo_move_to(cr, cx - h, cy - h * 0.7);
      cairo_line_to(cr, cx + h, cy - h * 0.7);
      cairo_line_to(cr, cx, cy + h * 0.7);
   }
   cairo_close_path(cr);
   cairo_fill(cr);
}


inline void drawText(cairo_t *cr, double x, double baseline, const char *text, double size, bool bold,
              Align align) {
   cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                          bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
   cairo_set_font_size(cr, size);
   cairo_text_extents_t ext;
   cairo_text_extents(cr, text, &ext);
   double tx = x;
   if (align == Align::Center)
      tx = x - (ext.width * 0.5 + ext.x_bearing);
   else if (align == Align::Right)
      tx = x - (ext.width + ext.x_bearing);
   cairo_move_to(cr, tx, baseline);
   cairo_show_text(cr, text);
}

inline double textWidth(cairo_t *cr, const char *text, double size, bool bold) {
   cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                          bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
   cairo_set_font_size(cr, size);
   cairo_text_extents_t ext;
   cairo_text_extents(cr, text, &ext);
   return ext.width;
}


// Upper-cases into a small buffer for the panel and parameter labels, which are
// drawn in caps regardless of how they are written in the table.
inline void upperCase(const char *in, char *out, size_t outSize) {
   size_t i = 0;
   for (; in[i] && i + 1 < outSize; ++i)
      out[i] = static_cast<char>(in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i]);
   out[i] = 0;
}


// --------------------------------------------------------------- parameters

// The parameter's position on its knob, 0..1.
inline double normalised(const ParamDesc &d, double raw) {
   const double span = d.max - d.min;
   if (span <= 0.0)
      return 0.0;
   return std::min(1.0, std::max(0.0, (raw - d.min) / span));
}

inline bool isChip(const ParamDesc &d) { return d.kind == ParamKind::Enum; }

inline int cellSpan(uint32_t) { return 1; }
inline bool isStepped(const ParamDesc &d) {
   return d.kind == ParamKind::Enum || d.kind == ParamKind::Stepped;
}
inline bool isBipolar(const ParamDesc &d) { return d.min < -0.0001; }


// -------------------------------------------------------------- rectangles

struct Rect {
   double x = 0, y = 0, w = 0, h = 0;
   bool contains(double px, double py) const {
      return px >= x && px < x + w && py >= y && py < y + h;
   }
};


} // namespace plugincore
