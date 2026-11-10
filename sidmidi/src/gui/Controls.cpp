#include <algorithm>
#include <cmath>

#include "Widgets.h"

namespace sidmidi::gui {

// ------------------------------------------------------------ Slider

Slider::Slider(std::string caption, double lo, double hi, double def, std::function<double()> get,
               std::function<void(double)> set, std::function<std::string(double)> format)
    : caption_(std::move(caption)), lo_(lo), hi_(hi), def_(def), get_(std::move(get)), set_(std::move(set)),
      format_(std::move(format))
{
}

void Slider::paint(cairo_t* cr)
{
    const Rect r = bounds_;
    fillRounded(cr, r, 4, theme::plot);
    const double v = get_ ? get_() : lo_;
    const float t = static_cast<float>(std::clamp((v - lo_) / (hi_ - lo_), 0.0, 1.0));
    const bool bipolar = lo_ < 0.0 && hi_ > 0.0 && std::fabs(lo_ + hi_) < 1e-9; // e.g. pan
    Rect fill = r.reduced(1.5f);
    if (bipolar) {
        const float mid = fill.x + fill.w * static_cast<float>(-lo_ / (hi_ - lo_));
        const float x = fill.x + fill.w * t;
        fill = {std::min(mid, x), fill.y, std::max(1.0f, std::fabs(x - mid)), fill.h};
    } else {
        fill.w *= t;
    }
    const Color bar = !enabled_ ? theme::textFaint.withAlpha(0.35f)
                                : (isHovered() ? theme::accentBright : theme::accent).withAlpha(0.55f);
    fillRounded(cr, fill, 3, bar);
    strokeRounded(cr, r, 4, theme::outline);
    setFont(cr, 10.5f);
    const Color tc = enabled_ ? theme::text : theme::textFaint;
    drawText(cr, caption_, r.reduced(7, 0), Align::Left, tc);
    drawText(cr, format_ ? format_(v) : std::to_string(v), r.reduced(7, 0), Align::Right, tc);
}

bool Slider::mouseDown(const MouseEvent& e)
{
    if (!enabled_)
        return false;
    if (e.clicks == 2) {
        if (onBegin)
            onBegin();
        set_(def_);
        if (onEnd)
            onEnd();
        repaint();
        return true;
    }
    lastX_ = e.x;
    dragValue_ = get_ ? get_() : lo_;
    if (onBegin)
        onBegin();
    return true;
}

void Slider::mouseDrag(const MouseEvent& e)
{
    if (!enabled_)
        return;
    const double span = hi_ - lo_;
    const double perPx = span / std::max(40.0f, bounds_.w) * ((e.mods & ModShift) ? 0.1 : 1.0);
    dragValue_ = std::clamp(dragValue_ + (e.x - lastX_) * perPx, lo_, hi_);
    lastX_ = e.x;
    set_(dragValue_);
    repaint();
}

void Slider::mouseUp(const MouseEvent&)
{
    if (!enabled_)
        return;
    if (onEnd)
        onEnd();
}

bool Slider::mouseWheel(const MouseEvent& e)
{
    if (!enabled_)
        return false;
    const double step = (hi_ - lo_) * ((e.mods & ModShift) ? 0.005 : 0.02);
    if (onBegin)
        onBegin();
    set_(std::clamp((get_ ? get_() : lo_) + step * e.wheel, lo_, hi_));
    if (onEnd)
        onEnd();
    repaint();
    return true;
}

// ------------------------------------------------------------ Choice

Choice::Choice(std::string caption, std::vector<std::string> items, std::function<int()> get, std::function<void(int)> set)
    : caption_(std::move(caption)), items_(std::move(items)), get_(std::move(get)), set_(std::move(set))
{
}

void Choice::paint(cairo_t* cr)
{
    const Rect r = bounds_;
    fillRounded(cr, r, 4, isHovered() && enabled_ ? theme::panelLight : theme::panel);
    strokeRounded(cr, r, 4, theme::outline);
    const int i = get_ ? get_() : 0;
    const std::string value = (i >= 0 && i < static_cast<int>(items_.size())) ? items_[static_cast<size_t>(i)] : "-";
    setFont(cr, 10.5f);
    Rect inner = r.reduced(7, 0);
    if (!caption_.empty()) {
        drawText(cr, caption_, inner, Align::Left, theme::textDim);
        inner.x += textWidth(cr, caption_) + 6;
        inner.w = r.right() - 16 - inner.x;
    } else {
        inner.w -= 10;
    }
    drawText(cr, value, inner, caption_.empty() ? Align::Left : Align::Right, enabled_ ? theme::text : theme::textFaint);
    // The little down arrow.
    const float ax = r.right() - 10, ay = r.cy();
    cairo_move_to(cr, ax - 3.5, ay - 1.5);
    cairo_line_to(cr, ax + 3.5, ay - 1.5);
    cairo_line_to(cr, ax, ay + 2.5);
    cairo_close_path(cr);
    setColor(cr, enabled_ ? theme::textDim : theme::textFaint);
    cairo_fill(cr);
}

bool Choice::mouseDown(const MouseEvent& e)
{
    if (!enabled_)
        return false;
    std::vector<MenuItem> items;
    const int cur = get_ ? get_() : -1;
    for (size_t k = 0; k < items_.size(); ++k) {
        MenuItem mi;
        mi.label = items_[k];
        mi.checked = static_cast<int>(k) == cur;
        mi.action = [this, k] {
            set_(static_cast<int>(k));
            repaint();
        };
        items.push_back(std::move(mi));
    }
    showMenu(root(), std::move(items), bounds_.x, bounds_.bottom());
    return true;
}

bool Choice::mouseWheel(const MouseEvent& e)
{
    if (!enabled_)
        return false;
    const int n = static_cast<int>(items_.size());
    if (!n)
        return false;
    const int cur = get_ ? get_() : 0;
    const int next = std::clamp(cur - static_cast<int>(e.wheel), 0, n - 1);
    if (next != cur) {
        set_(next);
        repaint();
    }
    return true;
}

} // namespace sidmidi::gui
