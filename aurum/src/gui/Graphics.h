#pragma once

#include <cairo/cairo.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace aurum::gui {

struct Color
{
    float r = 0, g = 0, b = 0, a = 1;
    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    static constexpr Color hex(unsigned v, float a = 1.0f)
    {
        return {((v >> 16) & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f, (v & 0xff) / 255.0f, a};
    }
    Color withAlpha(float na) const { return {r, g, b, na}; }
    Color mix(const Color& o, float t) const
    {
        return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t};
    }
};

struct Rect
{
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    Rect reduced(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect reduced(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
};

// Palette: neutral graphite surfaces with a muted brass accent.
namespace theme {
inline constexpr Color background = Color::hex(0x161616);
inline constexpr Color panel = Color::hex(0x202021);
inline constexpr Color panelLight = Color::hex(0x2b2b2d);
inline constexpr Color outline = Color::hex(0x3a3a3d);
inline constexpr Color text = Color::hex(0xe4e0d8);
inline constexpr Color textDim = Color::hex(0x9a968e);
inline constexpr Color textFaint = Color::hex(0x66635e);
inline constexpr Color gold = Color::hex(0xc9a66b);       // brass accent
inline constexpr Color goldBright = Color::hex(0xe3c48e);
inline constexpr Color decay = Color::hex(0x8fb89a);      // Decay Contour (sage)
inline constexpr Color postEq = Color::hex(0xc9a66b);     // Tone EQ (brass)
inline constexpr Color plot = Color::hex(0x121212);       // graph background
inline constexpr Color red = Color::hex(0xd9614f);
inline constexpr Color green = Color::hex(0x7fb37c);
inline const char* const fontFamily = "Inter";
} // namespace theme

enum class Align { Left, Center, Right };

inline void setColor(cairo_t* cr, const Color& c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

inline void roundedRect(cairo_t* cr, const Rect& r, float radius)
{
    const float rad = std::min(radius, std::min(r.w, r.h) * 0.5f);
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.w - rad, r.y + rad, rad, -M_PI / 2, 0);
    cairo_arc(cr, r.x + r.w - rad, r.y + r.h - rad, rad, 0, M_PI / 2);
    cairo_arc(cr, r.x + rad, r.y + r.h - rad, rad, M_PI / 2, M_PI);
    cairo_arc(cr, r.x + rad, r.y + rad, rad, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

inline void fillRounded(cairo_t* cr, const Rect& r, float radius, const Color& c)
{
    roundedRect(cr, r, radius);
    setColor(cr, c);
    cairo_fill(cr);
}

inline void strokeRounded(cairo_t* cr, const Rect& r, float radius, const Color& c, float width = 1.0f)
{
    roundedRect(cr, r.reduced(width * 0.5f), radius);
    setColor(cr, c);
    cairo_set_line_width(cr, width);
    cairo_stroke(cr);
}

inline void setFont(cairo_t* cr, float size, bool bold = false)
{
    // Font faces are resolved once; selecting by name each time is slow.
    static cairo_font_face_t* faces[2] = {
        cairo_toy_font_face_create(theme::fontFamily, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL),
        cairo_toy_font_face_create(theme::fontFamily, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD)};
    cairo_set_font_face(cr, faces[bold ? 1 : 0]);
    cairo_set_font_size(cr, size);
}

inline float textWidth(cairo_t* cr, const std::string& s)
{
    cairo_text_extents_t e;
    cairo_text_extents(cr, s.c_str(), &e);
    return static_cast<float>(e.x_advance);
}

// Draws text vertically centred in `r`.
inline void drawText(cairo_t* cr, const std::string& s, const Rect& r, Align align, const Color& c)
{
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    const float w = textWidth(cr, s);
    float x = r.x;
    if (align == Align::Center)
        x = r.cx() - w * 0.5f;
    else if (align == Align::Right)
        x = r.right() - w;
    const float y = r.cy() + static_cast<float>(fe.ascent - fe.descent) * 0.5f;
    setColor(cr, c);
    cairo_move_to(cr, std::round(x), std::round(y));
    cairo_show_text(cr, s.c_str());
    cairo_new_path(cr); // show_text leaves a current point behind
}

} // namespace aurum::gui
