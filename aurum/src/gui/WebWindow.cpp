#include "WebWindow.h"

#include <algorithm>

namespace aurum::gui {

namespace {
double gSystemScale = 1.0;
}

double WebWindow::systemScale() { return gSystemScale; }
void WebWindow::setSystemScale(double s) { gSystemScale = s > 0.25 ? s : 1.0; }

WebWindow::~WebWindow() { destroy(); }

bool WebWindow::attach(uintptr_t, int physW, int physH)
{
    open_ = true;
    setSize(physW, physH);
    return true;
}

void WebWindow::destroy()
{
    if (back_)
        cairo_surface_destroy(back_);
    back_ = nullptr;
    backW_ = backH_ = 0;
    open_ = false;
}

void WebWindow::setSize(int physW, int physH)
{
    width_ = std::max(physW, 1);
    height_ = std::max(physH, 1);
}

void WebWindow::render()
{
    if (!open_)
        return;
    if (!back_ || backW_ != width_ || backH_ != height_)
    {
        if (back_)
            cairo_surface_destroy(back_);
        back_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width_, height_);
        backW_ = width_;
        backH_ = height_;
    }
    cairo_t* cr = cairo_create(back_);
    cairo_scale(cr, scale_, scale_);
    listener_->onPaint(cr, width_, height_);
    cairo_destroy(cr);
    cairo_surface_flush(back_);

    // Cairo's RGB24 is a native-endian 0xXXRRGGBB word, B G R X in memory;
    // a canvas wants R G B A.
    const int stride = cairo_image_surface_get_stride(back_);
    const uint8_t* src = cairo_image_surface_get_data(back_);
    rgba_.resize(static_cast<size_t>(width_) * height_ * 4);
    uint8_t* dst = rgba_.data();
    for (int y = 0; y < height_; ++y)
    {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(src + static_cast<size_t>(y) * stride);
        uint32_t* out = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * width_ * 4);
        for (int x = 0; x < width_; ++x)
        {
            const uint32_t p = row[x];
            out[x] = 0xff000000u | ((p & 0xffu) << 16) | (p & 0xff00u) | ((p >> 16) & 0xffu);
        }
    }
    frameW_ = width_;
    frameH_ = height_;
    ++serial_;
}

MouseEvent WebWindow::toMouse(float x, float y, unsigned mods) const
{
    MouseEvent e;
    e.x = static_cast<float>(x / scale_);
    e.y = static_cast<float>(y / scale_);
    e.mods = mods;
    return e;
}

void WebWindow::pointerDown(float x, float y, int button, unsigned mods, int clicks)
{
    MouseEvent e = toMouse(x, y, mods);
    e.button = button;
    e.clicks = clicks;
    buttons_ |= 1 << button;
    listener_->onMouseDown(e);
}

void WebWindow::pointerUp(float x, float y, int button, unsigned mods)
{
    MouseEvent e = toMouse(x, y, mods);
    e.button = button;
    buttons_ &= ~(1 << button);
    listener_->onMouseUp(e);
}

void WebWindow::pointerMove(float x, float y, unsigned mods)
{
    MouseEvent e = toMouse(x, y, mods);
    // The button held during a drag, as X11 reports it in the motion state.
    for (int b = 1; b <= 3; ++b)
        if (buttons_ & (1 << b))
        {
            e.button = b;
            break;
        }
    listener_->onMouseMove(e);
}

void WebWindow::wheel(float x, float y, float notches, unsigned mods)
{
    MouseEvent e = toMouse(x, y, mods);
    e.wheel = notches;
    listener_->onMouseWheel(e);
}

void WebWindow::leave() { listener_->onMouseLeave(); }

bool WebWindow::key(unsigned keysym, const char* text, unsigned mods)
{
    KeyEvent e;
    e.keysym = keysym;
    e.text = text ? text : "";
    e.mods = mods;
    return listener_->onKey(e);
}

} // namespace aurum::gui
