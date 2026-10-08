#include "CurveEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "plugin/Params.h"

namespace substrike::gui {

namespace {
constexpr float kPointR = 4.5f;
constexpr float kGrab = 8.0f;
constexpr const char* kTabs[2] = {"Pitch", "Amp"};

std::string hzText(double hz)
{
    char buf[32];
    if (hz >= 999.5)
        std::snprintf(buf, sizeof(buf), "%.2g kHz", hz / 1000.0);
    else
        std::snprintf(buf, sizeof(buf), "%.0f Hz", hz);
    return buf;
}

std::string msText(double ms)
{
    char buf[32];
    if (ms >= 999.5)
        std::snprintf(buf, sizeof(buf), "%.2f s", ms / 1000.0);
    else if (ms >= 9.95)
        std::snprintf(buf, sizeof(buf), "%.0f ms", ms);
    else
        std::snprintf(buf, sizeof(buf), "%.1f ms", ms);
    return buf;
}
} // namespace

void CurveEditor::setLane(int lane)
{
    if (lane != lane_)
    {
        lane_ = lane;
        hover_ = drag_ = {};
        repaint();
    }
}

void CurveEditor::setEnv(int env)
{
    if (env != env_)
    {
        env_ = env;
        hover_ = drag_ = {};
        repaint();
    }
}

void CurveEditor::setTab(int which)
{
    if (which != tab_)
    {
        tab_ = which;
        hover_ = drag_ = {};
        repaint();
    }
}

Rect CurveEditor::plot() const { return {bounds_.x + 52, bounds_.y + 28, bounds_.w - 62, bounds_.h - 50}; }

Rect CurveEditor::tabRect(int i) const { return {bounds_.right() - 118 + i * 56.0f, bounds_.y, 54, 20}; }

int CurveEditor::curveIndex() const
{
    if (env_ >= 0)
        return dsp::modEnvCurveIndex(env_);
    return dsp::curveIndex(lane_, tab_ == 0 ? dsp::PitchCurve : dsp::AmpCurve);
}

const dsp::Curve& CurveEditor::curve() const { return ctx_.controller().curve(curveIndex()); }

void CurveEditor::commit(const dsp::Curve& c)
{
    ctx_.controller().setCurve(curveIndex(), c);
    repaint();
}

double CurveEditor::param(uint32_t field) const
{
    return ctx_.plain(ctx_.index(pid::lane(lane_, static_cast<pid::LaneField>(field))));
}

double CurveEditor::bend() const
{
    if (env_ >= 0)
        return 0.0;
    return param(tab_ == 0 ? pid::BodySweepCurve : pid::BodyDecayCurve) / 100.0;
}

float CurveEditor::sx(double x) const
{
    const Rect p = plot();
    return p.x + static_cast<float>(x) * p.w;
}

float CurveEditor::sy(double y) const
{
    const Rect p = plot();
    return p.bottom() - static_cast<float>(y) * p.h;
}

double CurveEditor::xAt(float px) const
{
    const Rect p = plot();
    return std::clamp((px - p.x) / p.w, 0.0f, 1.0f);
}

double CurveEditor::yAt(float py) const
{
    const Rect p = plot();
    return std::clamp((p.bottom() - py) / p.h, 0.0f, 1.0f);
}

void CurveEditor::handlePos(int i, float& x, float& y) const
{
    const dsp::Curve& c = curve();
    const double mid = 0.5 * (c.point(i).x + c.point(i + 1).x);
    x = sx(mid);
    y = sy(c.eval(mid, bend()));
}

std::string CurveEditor::xLabel(double x) const
{
    if (env_ >= 0)
        return msText(x * ctx_.plain(ctx_.index(pid::modEnv(env_, pid::EnvTime))));
    return msText(x * param(tab_ == 0 ? pid::BodySweep : pid::BodyDecay));
}

std::string CurveEditor::yLabel(double y) const
{
    if (tab_ == 1 || env_ >= 0)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f %%", y * 100.0);
        return buf;
    }
    // The pitch falls from Start (y = 1) to End (y = 0) exponentially, which
    // is what the engine does with the curve's value.
    const double start = param(pid::BodyPitchStart), end = param(pid::BodyPitchEnd);
    return hzText(end * std::pow(start / end, y));
}

CurveEditor::Target CurveEditor::hitAt(float x, float y) const
{
    for (int i = 0; i < 2 && env_ < 0; ++i)
        if (tabRect(i).contains(x, y))
            return {Hit::Tab, i};
    const dsp::Curve& c = curve();
    Target best;
    float bestD = kGrab;
    for (int i = 0; i < c.count(); ++i)
    {
        const float d = std::hypot(sx(c.point(i).x) - x, sy(c.point(i).y) - y);
        if (d < bestD)
        {
            bestD = d;
            best = {Hit::Point, i};
        }
    }
    if (best.what != Hit::None)
        return best;
    bestD = kGrab;
    for (int i = 0; i + 1 < c.count(); ++i)
    {
        float hx, hy;
        handlePos(i, hx, hy);
        const float d = std::hypot(hx - x, hy - y);
        if (d < bestD)
        {
            bestD = d;
            best = {Hit::Handle, i};
        }
    }
    return best;
}

void CurveEditor::paint(cairo_t* cr)
{
    const Rect p = plot();
    const dsp::Curve& c = curve();
    const bool violet = tab_ == 0 || env_ >= 0;
    const Color line = violet ? theme::accent : theme::amp;
    const Color bright = violet ? theme::accentBright : theme::amp.mix(Color(1, 1, 1, 1), 0.35f);

    // Tabs
    setFont(cr, 10.5f, true);
    for (int i = 0; i < 2 && env_ < 0; ++i)
    {
        const Rect t = tabRect(i);
        const bool on = i == tab_;
        const Color tc = i == 0 ? theme::accent : theme::amp;
        fillRounded(cr, t, 4, on ? tc.withAlpha(0.22f) : theme::panelLight);
        strokeRounded(cr, t, 4, on ? tc.withAlpha(0.8f) : theme::outline);
        drawText(cr, kTabs[i], t, Align::Center, on ? theme::text : theme::textDim);
    }

    // Plot and grid
    fillRounded(cr, p.reduced(-1), 4, theme::plot);
    cairo_set_line_width(cr, 1.0);
    setFont(cr, 9.5f);
    for (int i = 0; i <= 4; ++i)
    {
        const double f = i / 4.0;
        setColor(cr, theme::outline.withAlpha(i == 0 || i == 4 ? 0.9f : 0.45f));
        const float gx = std::round(sx(f)) + 0.5f, gy = std::round(sy(f)) + 0.5f;
        cairo_move_to(cr, gx, p.y);
        cairo_line_to(cr, gx, p.bottom());
        cairo_move_to(cr, p.x, gy);
        cairo_line_to(cr, p.right(), gy);
        cairo_stroke(cr);
        // The end labels sit inside the plot's width, the others centred.
        const float lx = i == 0 ? gx : (i == 4 ? gx - 60 : gx - 30);
        drawText(cr, xLabel(f), {lx, p.bottom() + 3, 60, 14},
                 i == 0 ? Align::Left : (i == 4 ? Align::Right : Align::Center), theme::textFaint);
        if (i == 0 || i == 2 || i == 4)
            drawText(cr, yLabel(f), {bounds_.x, gy - 7, 46, 14}, Align::Right, theme::textFaint);
    }

    // The curve as it plays: the drawn points plus the bend macro.
    const double b = bend();
    cairo_save(cr);
    cairo_rectangle(cr, p.x - 1, p.y - 2, p.w + 2, p.h + 4);
    cairo_clip(cr);
    const int steps = std::max(16, static_cast<int>(p.w / 2));
    cairo_move_to(cr, sx(0), sy(c.eval(0, b)));
    for (int i = 1; i <= steps; ++i)
    {
        const double x = static_cast<double>(i) / steps;
        cairo_line_to(cr, sx(x), sy(c.eval(x, b)));
    }
    cairo_line_to(cr, sx(1), p.bottom());
    cairo_line_to(cr, sx(0), p.bottom());
    cairo_close_path(cr);
    setColor(cr, line.withAlpha(0.10f));
    cairo_fill_preserve(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, sx(0), sy(c.eval(0, b)));
    for (int i = 1; i <= steps; ++i)
    {
        const double x = static_cast<double>(i) / steps;
        cairo_line_to(cr, sx(x), sy(c.eval(x, b)));
    }
    setColor(cr, line);
    cairo_set_line_width(cr, 1.8);
    cairo_stroke(cr);
    cairo_restore(cr);

    // Segment handles: small diamonds, brighter on hover.
    for (int i = 0; i + 1 < c.count(); ++i)
    {
        float hx, hy;
        handlePos(i, hx, hy);
        const bool hot = (hover_.what == Hit::Handle && hover_.index == i) ||
                         (drag_.what == Hit::Handle && drag_.index == i);
        const float r = hot ? 4.5f : 3.0f;
        cairo_move_to(cr, hx, hy - r);
        cairo_line_to(cr, hx + r, hy);
        cairo_line_to(cr, hx, hy + r);
        cairo_line_to(cr, hx - r, hy);
        cairo_close_path(cr);
        setColor(cr, hot ? bright : line.withAlpha(0.55f));
        cairo_fill(cr);
    }

    // Points
    for (int i = 0; i < c.count(); ++i)
    {
        const bool hot = (hover_.what == Hit::Point && hover_.index == i) ||
                         (drag_.what == Hit::Point && drag_.index == i);
        const float x = sx(c.point(i).x), y = sy(c.point(i).y);
        setColor(cr, theme::plot);
        cairo_arc(cr, x, y, kPointR + 1.5, 0, 2 * M_PI);
        cairo_fill(cr);
        setColor(cr, hot ? bright : line);
        cairo_arc(cr, x, y, hot ? kPointR + 1.0 : kPointR, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    // What the Pitch Link means for this curve.
    if (tab_ == 0 && env_ < 0)
    {
        const int link = static_cast<int>(std::lround(param(pid::LPitchLink))) - 1;
        if (link >= 0 && link != lane_)
        {
            setFont(cr, 10.5f);
            drawText(cr, "Pitch Link: this lane follows Lane " + std::to_string(link + 1) + "'s curve",
                     {p.x + 8, p.y + 4, p.w - 16, 16}, Align::Left, theme::textDim);
        }
    }

    // Readout of the point under the pointer.
    const Target& t = drag_.what == Hit::Point ? drag_ : hover_;
    if (t.what == Hit::Point && t.index < c.count())
    {
        const dsp::CurvePoint& pt = c.point(t.index);
        setFont(cr, 10.5f);
        drawText(cr, xLabel(pt.x) + "  \xc2\xb7  " + yLabel(pt.y), {bounds_.x, bounds_.y, 260, 20}, Align::Left,
                 bright);
    }
    else
    {
        setFont(cr, 10.5f);
        const std::string caption = env_ >= 0 ? "Env " + std::to_string(env_ + 1) + " over its Time"
                                    : tab_ == 0 ? "Pitch over Sweep Time"
                                                : "Level over Body Decay";
        drawText(cr, caption, {bounds_.x, bounds_.y, 260, 20}, Align::Left, theme::textFaint);
    }
}

void CurveEditor::insertPoint(double x, double y)
{
    const dsp::Curve& c = curve();
    if (c.count() >= dsp::Curve::kMaxPoints)
        return;
    dsp::CurvePoint pts[dsp::Curve::kMaxPoints];
    int n = 0;
    bool placed = false;
    for (int i = 0; i < c.count(); ++i)
    {
        const dsp::CurvePoint& p = c.point(i);
        if (!placed && i > 0 && x < p.x && x > c.point(i - 1).x)
        {
            // The new point splits a segment: both halves keep its bend.
            pts[n++] = {x, y, c.point(i - 1).k};
            placed = true;
        }
        pts[n++] = p;
    }
    if (!placed)
        return;
    dsp::Curve next;
    next.set(pts, n);
    commit(next);
}

void CurveEditor::removePoint(int index)
{
    const dsp::Curve& c = curve();
    if (index <= 0 || index >= c.count() - 1)
        return;
    dsp::CurvePoint pts[dsp::Curve::kMaxPoints];
    int n = 0;
    for (int i = 0; i < c.count(); ++i)
        if (i != index)
            pts[n++] = c.point(i);
    dsp::Curve next;
    next.set(pts, n);
    commit(next);
}

void CurveEditor::setCurvature(int index, double k)
{
    const dsp::Curve& c = curve();
    dsp::CurvePoint pts[dsp::Curve::kMaxPoints];
    for (int i = 0; i < c.count(); ++i)
        pts[i] = c.point(i);
    pts[index].k = std::clamp(k, -1.0, 1.0);
    dsp::Curve next;
    next.set(pts, c.count());
    commit(next);
}

void CurveEditor::showMenu(const MouseEvent& e, const Target& t)
{
    std::vector<MenuItem> items;
    const dsp::Curve& c = curve();
    if (t.what == Hit::Point)
    {
        const int i = t.index;
        items.push_back({"Remove Point", [this, i] { removePoint(i); }, false, i > 0 && i < c.count() - 1});
        if (i + 1 < c.count())
            items.push_back({"Straighten Segment", [this, i] { setCurvature(i, 0.0); }});
    }
    else if (t.what == Hit::Handle)
        items.push_back({"Straighten Segment", [this, i = t.index] { setCurvature(i, 0.0); }});
    else
    {
        const double x = xAt(e.x), y = yAt(e.y);
        items.push_back({"Add Point", [this, x, y] { insertPoint(x, y); }, false,
                         plot().contains(e.x, e.y) && c.count() < dsp::Curve::kMaxPoints});
    }
    items.push_back({"", nullptr, false, true, true});
    items.push_back({"Straighten All", [this] {
                         const dsp::Curve& cur = curve();
                         dsp::CurvePoint pts[dsp::Curve::kMaxPoints];
                         for (int i = 0; i < cur.count(); ++i)
                             pts[i] = {cur.point(i).x, cur.point(i).y, 0.0};
                         dsp::Curve next;
                         next.set(pts, cur.count());
                         commit(next);
                     }});
    items.push_back({"Reset Curve", [this] { commit(dsp::Curve()); }});
    gui::showMenu(root(), std::move(items), e.x, e.y);
}

bool CurveEditor::mouseDown(const MouseEvent& e)
{
    const Target t = hitAt(e.x, e.y);
    if (e.button == 3)
    {
        showMenu(e, t);
        return true;
    }
    if (e.button != 1)
        return false;
    if (t.what == Hit::Tab)
    {
        if (onTab)
            onTab(t.index);
        return true;
    }
    if (e.clicks == 2)
    {
        if (t.what == Hit::Point)
            removePoint(t.index);
        else if (t.what == Hit::Handle)
            setCurvature(t.index, 0.0);
        else if (plot().contains(e.x, e.y))
            insertPoint(xAt(e.x), yAt(e.y));
        return true;
    }
    if (t.what == Hit::Point || t.what == Hit::Handle)
    {
        drag_ = t;
        lastX_ = e.x;
        lastY_ = e.y;
        if (t.what == Hit::Point)
        {
            dragX_ = curve().point(t.index).x;
            dragY_ = curve().point(t.index).y;
        }
        repaint();
        return true;
    }
    return plot().contains(e.x, e.y);
}

void CurveEditor::mouseDrag(const MouseEvent& e)
{
    if (drag_.what == Hit::None)
        return;
    const dsp::Curve& c = curve();
    if (drag_.index >= c.count())
    {
        drag_ = {};
        return;
    }
    const Rect p = plot();
    const float fine = (e.mods & ModShift) ? 0.15f : 1.0f;
    const float dx = (e.x - lastX_) * fine, dy = (e.y - lastY_) * fine;
    lastX_ = e.x;
    lastY_ = e.y;
    dsp::CurvePoint pts[dsp::Curve::kMaxPoints];
    for (int i = 0; i < c.count(); ++i)
        pts[i] = c.point(i);
    const int i = drag_.index;
    if (drag_.what == Hit::Point)
    {
        dragX_ += dx / p.w;
        dragY_ -= dy / p.h;
        // The ends stay at the ends; a point in between stays between its
        // neighbours.
        if (i > 0 && i < c.count() - 1)
            pts[i].x = std::clamp(dragX_, pts[i - 1].x + 1e-4, pts[i + 1].x - 1e-4);
        pts[i].y = std::clamp(dragY_, 0.0, 1.0);
    }
    else
    {
        // Pulling a handle towards where the segment heads bends it to get
        // there sooner: for a falling segment that is down, for a rising one
        // up.
        const bool rising = pts[i + 1].y > pts[i].y;
        pts[i].k = std::clamp(pts[i].k + (rising ? -dy : dy) / 120.0, -1.0, 1.0);
    }
    dsp::Curve next;
    next.set(pts, c.count());
    commit(next);
}

void CurveEditor::mouseUp(const MouseEvent&)
{
    drag_ = {};
    repaint();
}

void CurveEditor::mouseMove(const MouseEvent& e)
{
    const Target t = hitAt(e.x, e.y);
    if (t.what != hover_.what || t.index != hover_.index)
    {
        hover_ = t;
        repaint();
    }
}

void CurveEditor::mouseLeave()
{
    hover_ = {};
    repaint();
}

std::string CurveEditor::tooltip() const
{
    if (drag_.what != Hit::None)
        return {};
    switch (hover_.what)
    {
    case Hit::Point: return "Drag to move, double-click to remove";
    case Hit::Handle: return "Drag to bend, double-click to straighten";
    default: return {};
    }
}

} // namespace substrike::gui
