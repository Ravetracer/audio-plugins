#include "Editor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "CurveEditor.h"
#include "PresetBrowser.h"
#include "dsp/Modulation.h"
#include "Keys.h"
#include "state/Settings.h"
#include "substrike.h"
#include "util/Path.h"

namespace substrike::gui {

namespace {

constexpr float kCellW = 60.0f;
constexpr float kCellH = 64.0f;

const char* const kSourceNames[] = {"Body", "Click", "Noise", "Resonator", "Bus"};

// "L1 Slot 2 Drive" -> "Drive", "L1 Pitch Start" -> "Pitch Start".
std::string shortName(const std::string& name)
{
    const size_t slot = name.find("Slot ");
    if (slot != std::string::npos)
    {
        const size_t sp = name.find(' ', slot + 5);
        if (sp != std::string::npos)
            return name.substr(sp + 1);
    }
    if (name.size() > 3 && name[0] == 'L' && name[2] == ' ')
        return name.substr(3);
    return name;
}

// A linear range centred on zero: the knob lights from the middle.
bool isBipolar(const ParamDef& d)
{
    return d.kind == Kind::Continuous && d.choices.empty() && d.scale == Scale::Linear && d.lo < 0.0 &&
           std::fabs(d.lo + d.hi) < 1e-9;
}

// The peaks of one channel of a preview, as a filled waveform over `r`,
// scaled so that `full` reaches the edges.
void drawPeaks(cairo_t* cr, const Rect& r, const HitView& v, int channel, float full, const Color& c)
{
    const auto& peaks = v.peaks[static_cast<size_t>(channel)];
    const int cols = std::max(1, static_cast<int>(r.w));
    const float half = r.h * 0.5f, mid = r.cy();
    const float gain = full > 1e-9f ? 1.0f / full : 0.0f;
    cairo_move_to(cr, r.x, mid);
    std::vector<float> heights(static_cast<size_t>(cols));
    for (int x = 0; x < cols; ++x)
    {
        const int b0 = x * HitView::kBins / cols;
        const int b1 = std::max(b0 + 1, (x + 1) * HitView::kBins / cols);
        float p = 0.0f;
        for (int b = b0; b < b1 && b < HitView::kBins; ++b)
            p = std::max(p, peaks[static_cast<size_t>(b)]);
        heights[static_cast<size_t>(x)] = std::min(1.0f, p * gain) * half;
        cairo_line_to(cr, r.x + x + 0.5f, mid - heights[static_cast<size_t>(x)]);
    }
    for (int x = cols - 1; x >= 0; --x)
        cairo_line_to(cr, r.x + x + 0.5f, mid + heights[static_cast<size_t>(x)]);
    cairo_close_path(cr);
    setColor(cr, c);
    cairo_fill(cr);
}

// The note a frequency is, with the cents off it: "G1 -32 ct".
std::string noteOf(double hz)
{
    if (hz <= 0.0)
        return {};
    const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    const int n = static_cast<int>(std::lround(midi));
    const int cents = static_cast<int>(std::lround((midi - n) * 100.0));
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%d %+d ct", names[((n % 12) + 12) % 12], n / 12 - 1, cents);
    return buf;
}

std::string dbText(float peak)
{
    if (peak < 1e-6f)
        return "-inf dB";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.1f dB", 20.0 * std::log10(peak));
    return buf;
}

} // namespace

// ------------------------------------------------------------ ControlGrid

// A panel's controls in a grid of knob-sized cells, left to right and
// wrapping. Selectors and toggles take a cell like a knob, or more.
class ControlGrid : public Widget
{
public:
    explicit ControlGrid(ParamContext& ctx) : ctx_(ctx) {}

    void clear()
    {
        clearChildren();
        items_.clear();
    }

    // The control that suits the parameter: a knob, a selector for a choice,
    // a toggle for a switch.
    Widget* control(uint32_t id, std::string label = {}, float span = 1.0f)
    {
        const int idx = ctx_.index(id);
        const ParamDef& d = ctx_.def(idx);
        if (label.empty())
            label = shortName(d.name);
        Widget* w = nullptr;
        if (d.kind == Kind::Bool)
        {
            auto* t = add<ParamToggle>(ctx_, idx, "On");
            t->caption = label;
            t->fontSize = 10.5f;
            w = t;
        }
        else if (d.kind == Kind::Enum || !d.choices.empty())
        {
            auto* s = add<ParamSelector>(ctx_, idx);
            s->caption = label;
            s->fontSize = 10.5f;
            w = s;
        }
        else
            w = add<Knob>(ctx_, idx, label, KnobSize::Small, isBipolar(d));
        items_.push_back({w, span});
        return w;
    }
    void gap(float span) { items_.push_back({nullptr, span}); }

    void layout() override
    {
        float x = bounds_.x + 8, y = bounds_.y;
        for (const Item& it : items_)
        {
            const float w = kCellW * it.span;
            if (x + w > bounds_.right() + 1 && x > bounds_.x + 8)
            {
                x = bounds_.x + 8;
                y += kCellH + 6;
            }
            if (it.widget)
                it.widget->setBounds({x + 2, y, w - 4, kCellH});
            x += w;
        }
    }

private:
    struct Item
    {
        Widget* widget;
        float span;
    };
    ParamContext& ctx_;
    std::vector<Item> items_;
};

// ------------------------------------------------------------ Scope

// One channel of the preview: the main output or a lane, over the whole hit.
class Scope : public Widget
{
public:
    explicit Scope(Editor& ed) : ed_(ed) {}
    int channel = 0;

    void paint(cairo_t* cr) override
    {
        fillRounded(cr, bounds_, 4, theme::plot);
        const Rect r = bounds_.reduced(6, 18);
        setColor(cr, theme::outline.withAlpha(0.6f));
        cairo_rectangle(cr, r.x, std::round(r.cy()), r.w, 1);
        cairo_fill(cr);
        const HitView* v = ed_.hitView();
        setFont(cr, 10.5f);
        if (!v)
        {
            drawText(cr, "rendering...", bounds_, Align::Center, theme::textFaint);
            return;
        }
        if (channel > 0 && v->max[static_cast<size_t>(channel)] < 1e-6f)
        {
            const bool on =
                ed_.params().value(ed_.params().index(pid::lane(channel - 1, pid::LEnabled))) > 0.5;
            drawText(cr, on ? "silent on the root note" : "this lane is off", bounds_, Align::Center,
                     theme::textFaint);
            return;
        }
        // Full scale at the edges for the output, the lane's own peak for a
        // lane, so a quiet layer still shows its shape.
        const float peak = v->max[static_cast<size_t>(channel)];
        const float full = channel == 0 ? std::max(1.0f, peak) : std::max(peak, 1e-6f);
        drawPeaks(cr, r, *v, channel, full, (channel == 0 ? theme::accent : theme::amp).withAlpha(0.85f));
        drawText(cr, "peak " + dbText(peak), {bounds_.x + 8, bounds_.y + 2, 120, 16}, Align::Left, theme::textDim);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f ms", v->seconds * 1000.0);
        drawText(cr, buf, {bounds_.right() - 128, bounds_.bottom() - 17, 120, 16}, Align::Right, theme::textFaint);
        drawText(cr, "0", {bounds_.x + 8, bounds_.bottom() - 17, 40, 16}, Align::Left, theme::textFaint);
    }

private:
    Editor& ed_;
};

// ------------------------------------------------------------ LaneRow

// One lane in the rack (or the master): number, on/off, source, level, and
// its share of the hit as a small waveform. Clicking a row selects it.
class LaneRow : public Widget
{
public:
    LaneRow(Editor& ed, int lane) : ed_(ed), lane_(lane)
    {
        ParamContext& ctx = ed.params();
        if (lane < Editor::kMaster)
        {
            power_ = add<ParamToggle>(ctx, ctx.index(pid::lane(lane, pid::LEnabled)), "");
            power_->icon = icons::power;
            source_ = add<ParamSelector>(ctx, ctx.index(pid::lane(lane, pid::LSource)));
            source_->fontSize = 10.5f;
            level_ = add<ParamField>(ctx, ctx.index(pid::lane(lane, pid::LLevel)));
        }
        else if (lane == Editor::kMaster)
            level_ = add<ParamField>(ctx, ctx.index(pid::Output));
    }

    void layout() override
    {
        const Rect b = bounds_;
        if (power_)
            power_->setBounds({b.x + 26, b.y + 5, 22, 20});
        if (source_)
            source_->setBounds({b.x + 52, b.y + 5, 84, 20});
        if (level_)
            level_->setBounds({b.right() - 74, b.y + 5, 68, 20});
    }

    void paint(cairo_t* cr) override
    {
        const bool selected = ed_.selectedLane() == lane_;
        const bool on = lane_ == Editor::kMaster ||
                        ed_.params().value(ed_.params().index(pid::lane(lane_, pid::LEnabled))) > 0.5;
        fillRounded(cr, bounds_, 5, selected ? theme::panelLight : theme::panel);
        if (selected)
        {
            strokeRounded(cr, bounds_, 5, theme::accent.withAlpha(0.75f));
            fillRounded(cr, {bounds_.x, bounds_.y + 6, 3, bounds_.h - 12}, 1.5f, theme::accent);
        }
        else if (isHovered())
            strokeRounded(cr, bounds_, 5, theme::outline);
        setFont(cr, 12.0f, true);
        if (lane_ == Editor::kMod)
        {
            drawText(cr, "MODULATION", {bounds_.x + 12, bounds_.y + 5, 120, 20}, Align::Left,
                     selected ? theme::accentBright : theme::textDim);
            ParamContext& ctx = ed_.params();
            int routes = 0;
            for (int r = 0; r < pid::kNumRoutes; ++r)
                routes += std::lround(ctx.value(ctx.index(pid::route(r, pid::RSource)))) > 0 &&
                          std::lround(ctx.value(ctx.index(pid::route(r, pid::RDest)))) > 0;
            setFont(cr, 10.5f);
            drawText(cr, routes == 0 ? "no routes" : std::to_string(routes) + (routes == 1 ? " route" : " routes"),
                     {bounds_.right() - 110, bounds_.y + 5, 104, 20}, Align::Right,
                     routes ? theme::amp : theme::textFaint);
            return;
        }
        if (lane_ < Editor::kMaster)
            drawText(cr, std::to_string(lane_ + 1), {bounds_.x + 8, bounds_.y + 5, 16, 20}, Align::Center,
                     selected ? theme::accentBright : theme::textDim);
        else
            drawText(cr, "MASTER", {bounds_.x + 12, bounds_.y + 5, 100, 20}, Align::Left,
                     selected ? theme::accentBright : theme::textDim);

        const Rect wave{bounds_.x + 26, bounds_.y + 29, bounds_.w - 32, bounds_.h - 33};
        if (wave.h < 6)
            return;
        const HitView* v = ed_.hitView();
        const int channel = lane_ == Editor::kMaster ? 0 : lane_ + 1;
        if (v && on && v->max[static_cast<size_t>(channel)] > 1e-6f)
        {
            const float peak = v->max[static_cast<size_t>(channel)];
            drawPeaks(cr, wave, *v, channel, lane_ == Editor::kMaster ? std::max(1.0f, peak) : peak,
                      (selected ? theme::accentBright : theme::accent).withAlpha(on ? 0.75f : 0.3f));
        }
        else
        {
            setColor(cr, theme::outline.withAlpha(0.6f));
            cairo_rectangle(cr, wave.x, std::round(wave.cy()), wave.w, 1);
            cairo_fill(cr);
        }
    }

    bool mouseDown(const MouseEvent& e) override { return e.button == 1; }
    std::string tooltip() const override
    {
        if (lane_ == Editor::kMod)
            return "LFOs, envelopes, macros and the modulation matrix";
        return lane_ == Editor::kMaster ? "Master chain and output" : "Lane " + std::to_string(lane_ + 1);
    }

private:
    Editor& ed_;
    int lane_;
    ParamToggle* power_ = nullptr;
    ParamSelector* source_ = nullptr;
    ParamField* level_ = nullptr;
};

// ------------------------------------------------------------ HitStrip

// The whole hit as a waveform in the top bar. Click plays it through the
// plugin; drag it out to drop the rendered WAV into the DAW.
class HitStrip : public Widget
{
public:
    explicit HitStrip(Editor& ed) : ed_(ed) {}

    void paint(cairo_t* cr) override
    {
        const bool hot = isHovered() || pressed_;
        fillRounded(cr, bounds_, 4, theme::plot);
        strokeRounded(cr, bounds_, 4, hot ? theme::accent.withAlpha(0.7f) : theme::outline);
        if (const HitView* v = ed_.hitView())
            drawPeaks(cr, bounds_.reduced(4, 3), *v, 0, std::max(1.0f, v->max[0]),
                      (hot ? theme::accentBright : theme::accent).withAlpha(0.85f));
        if (hot)
        {
            setFont(cr, 10.0f);
            drawText(cr, "drag to export", bounds_.reduced(8, 0), Align::Right, theme::text);
        }
    }

    bool mouseDown(const MouseEvent& e) override
    {
        if (e.button != 1)
            return false;
        pressed_ = true;
        dragged_ = false;
        downX_ = e.x;
        downY_ = e.y;
        return true;
    }

    void mouseDrag(const MouseEvent& e) override
    {
        if (!pressed_ || dragged_)
            return;
        if (std::hypot(e.x - downX_, e.y - downY_) > 6.0f)
        {
            dragged_ = true;
            ed_.dragHit();
        }
    }

    void mouseUp(const MouseEvent&) override
    {
        if (pressed_ && !dragged_)
            ed_.audition();
        pressed_ = false;
        repaint();
    }

    std::string tooltip() const override { return "Click to play the hit, drag it into your DAW as a WAV"; }

private:
    Editor& ed_;
    bool pressed_ = false, dragged_ = false;
    float downX_ = 0, downY_ = 0;
};

// ------------------------------------------------------------ ChainRow

// The six slots of the selected chain, in signal order, and for a lane its
// transient guard at the end. Click selects one, drag swaps two slots, the
// dot is the slot's on/off, right-click picks a type.
class ChainRow : public Widget
{
public:
    explicit ChainRow(Editor& ed) : ed_(ed) {}

    // The guard's chip is narrower than a slot's.
    static constexpr float kGuardShare = 0.62f;
    bool hasGuard() const { return ed_.selectedLane() < Editor::kMaster; }
    float chipW() const
    {
        const float gap = 14.0f;
        const float n = dsp::kNumSlots + (hasGuard() ? kGuardShare : 0.0f);
        const int gaps = dsp::kNumSlots - 1 + (hasGuard() ? 1 : 0);
        return (bounds_.w - gap * gaps) / n;
    }
    Rect chip(int i) const
    {
        const float gap = 14.0f, w = chipW();
        return {bounds_.x + i * (w + gap), bounds_.y, w, bounds_.h};
    }
    Rect guardChip() const
    {
        const Rect last = chip(dsp::kNumSlots - 1);
        return {last.right() + 14.0f, bounds_.y, chipW() * kGuardShare, bounds_.h};
    }
    Rect led(int i) const
    {
        const Rect c = chip(i);
        return {c.right() - 20, c.y + 4, 16, 16};
    }

    void paint(cairo_t* cr) override
    {
        ParamContext& ctx = ed_.params();
        for (int i = 0; i < dsp::kNumSlots; ++i)
        {
            const Rect c = chip(i);
            const int type = static_cast<int>(std::lround(ctx.value(ctx.index(ed_.slotId(i, pid::SType)))));
            const bool bypass = ctx.value(ctx.index(ed_.slotId(i, pid::SBypass))) > 0.5;
            const int band = static_cast<int>(std::lround(ctx.value(ctx.index(ed_.slotId(i, pid::SBand)))));
            const bool selected = !ed_.guardSelected() && ed_.selectedSlot() == i;
            const bool target = dragFrom_ >= 0 && dragTo_ == i && dragTo_ != dragFrom_;
            fillRounded(cr, c, 5, selected ? theme::accent.withAlpha(0.16f) : theme::panelLight);
            strokeRounded(cr, c, 5,
                          target ? theme::accentBright
                                 : (selected ? theme::accent.withAlpha(0.8f)
                                             : (hover_ == i ? theme::textFaint : theme::outline)),
                          target ? 2.0f : 1.0f);
            setFont(cr, 9.5f, true);
            drawText(cr, std::to_string(i + 1), {c.x + 7, c.y + 3, 14, 16}, Align::Left, theme::textFaint);
            const ParamDef& td = ctx.def(ctx.index(ed_.slotId(i, pid::SType)));
            setFont(cr, 11.5f, type != 0);
            const Color nameColor = type == 0 ? theme::textFaint : (bypass ? theme::textDim : theme::text);
            drawText(cr, type == 0 ? "Empty" : td.labels[static_cast<size_t>(type)], {c.x, c.y + 4, c.w, c.h - 8},
                     Align::Center, nameColor);
            if (type != 0)
            {
                if (band != 0)
                {
                    static const char* bands[] = {"FULL", "LOW", "MID", "HIGH", "LOW+MID", "MID+HIGH"};
                    setFont(cr, 8.5f, true);
                    drawText(cr, bands[std::clamp(band, 0, 5)], {c.x + 7, c.bottom() - 15, c.w - 14, 12},
                             Align::Left, theme::amp);
                }
                const Rect l = led(i);
                setColor(cr, bypass ? theme::outline : theme::accent);
                cairo_arc(cr, l.cx(), l.cy(), 4.0, 0, 2 * M_PI);
                cairo_fill(cr);
            }
            // The signal runs left to right.
            if (i + 1 < dsp::kNumSlots || hasGuard())
            {
                const float ax = c.right() + 4, ay = c.cy();
                setColor(cr, theme::textFaint);
                cairo_move_to(cr, ax, ay - 3.5);
                cairo_line_to(cr, ax + 6, ay);
                cairo_line_to(cr, ax, ay + 3.5);
                cairo_close_path(cr);
                cairo_fill(cr);
            }
        }
        if (hasGuard())
            paintGuard(cr);
    }

    void paintGuard(cairo_t* cr)
    {
        ParamContext& ctx = ed_.params();
        const int l = ed_.selectedLane();
        const Rect c = guardChip();
        const bool selected = ed_.guardSelected();
        auto value = [&](pid::LaneField f) { return ctx.value(ctx.index(pid::lane(l, f))); };
        const bool active = value(pid::LGuardDelay) > 0.0 || value(pid::LGuardFade) > 0.0 ||
                            std::lround(value(pid::LDuckSource)) > 0;
        fillRounded(cr, c, 5, selected ? theme::amp.withAlpha(0.14f) : theme::panel);
        strokeRounded(cr, c, 5,
                      selected ? theme::amp.withAlpha(0.8f) : (hover_ == kGuard ? theme::textFaint : theme::outline));
        setFont(cr, 11.0f, active);
        drawText(cr, "Guard", {c.x, c.y + 4, c.w, c.h - 8}, Align::Center, active ? theme::text : theme::textFaint);
        if (active)
        {
            setColor(cr, theme::amp);
            cairo_arc(cr, c.right() - 12, c.y + 12, 4.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    static constexpr int kGuard = dsp::kNumSlots;

    int chipAt(float x, float y) const
    {
        for (int i = 0; i < dsp::kNumSlots; ++i)
            if (chip(i).contains(x, y))
                return i;
        if (hasGuard() && guardChip().contains(x, y))
            return kGuard;
        return -1;
    }

    bool mouseDown(const MouseEvent& e) override
    {
        const int i = chipAt(e.x, e.y);
        if (i < 0)
            return false;
        if (i == kGuard)
        {
            if (e.button != 1)
                return false;
            ed_.selectGuard();
            return true;
        }
        ParamContext& ctx = ed_.params();
        const int typeIdx = ctx.index(ed_.slotId(i, pid::SType));
        if (e.button == 3 || (e.button == 1 && e.clicks == 2))
        {
            ed_.selectSlot(i);
            const ParamDef& d = ctx.def(typeIdx);
            std::vector<MenuItem> items;
            const int cur = static_cast<int>(std::lround(ctx.value(typeIdx)));
            for (size_t t = 0; t < d.labels.size(); ++t)
                items.push_back({t == 0 ? std::string("Empty") : d.labels[t],
                                 [&ctx, typeIdx, t] { ctx.change(typeIdx, static_cast<double>(t)); },
                                 static_cast<int>(t) == cur});
            showMenu(root(), std::move(items), e.x, e.y);
            return true;
        }
        if (e.button != 1)
            return false;
        if (led(i).contains(e.x, e.y) && std::lround(ctx.value(typeIdx)) != 0)
        {
            const int b = ctx.index(ed_.slotId(i, pid::SBypass));
            ctx.change(b, ctx.value(b) > 0.5 ? 0.0 : 1.0);
            return true;
        }
        ed_.selectSlot(i);
        dragFrom_ = i;
        dragTo_ = i;
        return true;
    }

    void mouseDrag(const MouseEvent& e) override
    {
        if (dragFrom_ < 0)
            return;
        int to = chipAt(e.x, std::clamp(e.y, bounds_.y + 1, bounds_.bottom() - 1));
        if (to == kGuard)
            to = -1;
        if (to != dragTo_)
        {
            dragTo_ = to;
            repaint();
        }
    }

    void mouseUp(const MouseEvent&) override
    {
        if (dragFrom_ >= 0 && dragTo_ >= 0 && dragTo_ != dragFrom_)
            ed_.swapSlots(dragFrom_, dragTo_);
        dragFrom_ = dragTo_ = -1;
        repaint();
    }

    void mouseMove(const MouseEvent& e) override
    {
        const int h = chipAt(e.x, e.y);
        if (h != hover_)
        {
            hover_ = h;
            repaint();
        }
    }
    void mouseLeave() override
    {
        hover_ = -1;
        repaint();
    }

    std::string tooltip() const override
    {
        if (dragFrom_ >= 0)
            return dragTo_ >= 0 && dragTo_ != dragFrom_
                       ? "Swap slot " + std::to_string(dragFrom_ + 1) + " with slot " + std::to_string(dragTo_ + 1)
                       : std::string();
        if (hover_ == kGuard)
            return "The transient guard: a fade-in window after each hit, and a duck";
        return hover_ >= 0 ? "Drag onto another slot to swap them, right-click for a type" : std::string();
    }

private:
    Editor& ed_;
    int hover_ = -1;
    int dragFrom_ = -1, dragTo_ = -1;
};

// ------------------------------------------------------------ LiveScope

// The plugin's main output as it plays, scrolling: the last two seconds or
// so, newest at the right. When the plugin sleeps nothing arrives, and the
// picture runs on with silence.
class LiveScope : public Widget
{
public:
    static constexpr int kBins = 1536; // ~2 s of 64-sample bins at 48 kHz

    explicit LiveScope(Editor& ed) : ed_(ed) {}

    // Takes what the plugin produced since the last call; true if the
    // picture changed.
    bool update(double now)
    {
        float lo[512], hi[512];
        int got = 0, n;
        while ((n = ed_.controller().readScope(lo, hi, 512)) > 0)
        {
            for (int i = 0; i < n; ++i)
                push(lo[i], hi[i]);
            got += n;
        }
        const double rate = ed_.controller().sampleRate() / Controller::kScopeBin;
        if (got > 0)
        {
            lastData_ = now;
            pending_ = 0.0;
        }
        else if (now - lastData_ > 0.1 && quiet_ < kBins)
        {
            // Asleep: let silence scroll in at the rate the bins would come.
            pending_ += (now - lastTick_) * rate;
            while (pending_ >= 1.0 && quiet_ < kBins)
            {
                push(0.0f, 0.0f);
                pending_ -= 1.0;
            }
            got = 1;
        }
        lastTick_ = now;
        return got > 0;
    }

    // The background is part of the window's cached layer; the waveform is
    // drawn over it every frame (paintLive), so a moving scope does not
    // repaint the whole window.
    void paint(cairo_t* cr) override
    {
        fillRounded(cr, bounds_, 4, theme::plot);
        const Rect r = bounds_.reduced(4, 4);
        setColor(cr, theme::outline.withAlpha(0.6f));
        cairo_rectangle(cr, r.x, std::round(r.cy()), r.w, 1);
        cairo_fill(cr);
    }

    void paintLive(cairo_t* cr)
    {
        if (!isVisible())
            return;
        const Rect r = bounds_.reduced(4, 4);
        const int cols = std::max(1, static_cast<int>(r.w));
        const float half = r.h * 0.5f;
        cairo_move_to(cr, r.x, r.cy());
        std::vector<float> top(static_cast<size_t>(cols)), bottom(static_cast<size_t>(cols));
        for (int x = 0; x < cols; ++x)
        {
            const int b0 = x * kBins / cols, b1 = std::max(b0 + 1, (x + 1) * kBins / cols);
            float a = 0.0f, b = 0.0f;
            for (int k = b0; k < b1; ++k)
            {
                const size_t i = (head_ + static_cast<size_t>(k)) % kBins;
                a = std::min(a, lo_[i]);
                b = std::max(b, hi_[i]);
            }
            top[static_cast<size_t>(x)] = r.cy() - std::clamp(b, -1.0f, 1.0f) * half;
            bottom[static_cast<size_t>(x)] = r.cy() - std::clamp(a, -1.0f, 1.0f) * half;
            cairo_line_to(cr, r.x + x + 0.5f, top[static_cast<size_t>(x)]);
        }
        for (int x = cols - 1; x >= 0; --x)
            cairo_line_to(cr, r.x + x + 0.5f, bottom[static_cast<size_t>(x)]);
        cairo_close_path(cr);
        setColor(cr, theme::amp.withAlpha(0.8f));
        cairo_fill(cr);
    }

    std::string tooltip() const override { return "The output as it plays"; }

private:
    void push(float lo, float hi)
    {
        lo_[head_] = lo;
        hi_[head_] = hi;
        head_ = (head_ + 1) % kBins;
        quiet_ = lo == 0.0f && hi == 0.0f ? quiet_ + 1 : 0;
    }

    Editor& ed_;
    std::array<float, kBins> lo_{}, hi_{};
    size_t head_ = 0;
    int quiet_ = kBins;
    double lastData_ = 0.0, lastTick_ = 0.0, pending_ = 0.0;
};

// ------------------------------------------------------------ LfoView

// Two cycles of the selected LFO's shape, from its start phase.
class LfoView : public Widget
{
public:
    explicit LfoView(Editor& ed) : ed_(ed) {}
    int lfo = 0;

    void paint(cairo_t* cr) override
    {
        fillRounded(cr, bounds_, 4, theme::plot);
        const Rect r = bounds_.reduced(10, 22);
        ParamContext& ctx = ed_.params();
        setColor(cr, theme::outline.withAlpha(0.6f));
        cairo_rectangle(cr, r.x, std::round(r.cy()), r.w, 1);
        cairo_fill(cr);
        const auto shape = static_cast<dsp::LfoShape>(std::lround(ctx.plain(ctx.index(pid::lfo(lfo, pid::LfoShape)))));
        const double phase = ctx.plain(ctx.index(pid::lfo(lfo, pid::LfoPhase))) / 360.0;
        dsp::Lfo l(0x51ull + static_cast<uint64_t>(lfo));
        const int cols = std::max(2, static_cast<int>(r.w));
        for (int x = 0; x <= cols; ++x)
        {
            l.position = phase + 2.0 * x / cols;
            const float y = r.cy() - static_cast<float>(l.value(shape)) * r.h * 0.5f;
            if (x == 0)
                cairo_move_to(cr, r.x, y);
            else
                cairo_line_to(cr, r.x + x, y);
        }
        setColor(cr, theme::accent);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
        setFont(cr, 10.5f);
        const int sync = static_cast<int>(std::lround(ctx.plain(ctx.index(pid::lfo(lfo, pid::LfoSync)))));
        const std::string speed = sync > 0 ? ctx.text(ctx.index(pid::lfo(lfo, pid::LfoSync))) + " per cycle"
                                           : ctx.text(ctx.index(pid::lfo(lfo, pid::LfoRate)));
        const bool retrig = ctx.value(ctx.index(pid::lfo(lfo, pid::LfoRetrigger))) > 0.5;
        drawText(cr, "two cycles  \xc2\xb7  " + speed + (retrig ? "  \xc2\xb7  restarts with every note" : "  \xc2\xb7  free running"),
                 {bounds_.x + 8, bounds_.y + 2, bounds_.w - 16, 16}, Align::Left, theme::textDim);
    }

private:
    Editor& ed_;
};

// ------------------------------------------------------------ DestButton

// A route's destination: its name as it is now (a slot's letter by its
// type's name for it); click for the list of destinations, right-click or
// Ctrl-click clears it.
class DestButton : public Widget
{
public:
    DestButton(Editor& ed, int route) : ed_(ed), route_(route) {}

    int paramIndex() const { return ed_.params().index(pid::route(route_, pid::RDest)); }

    std::string text() const
    {
        ParamContext& ctx = ed_.params();
        const int v = static_cast<int>(std::lround(ctx.value(paramIndex())));
        if (v <= 0)
            return "No destination";
        const int target = ctx.table().destinations()[static_cast<size_t>(v - 1)];
        // A letter its slot's type has no use for is shown by its slot only.
        const ParamDef& d = ctx.def(target);
        return d.name;
    }

    void paint(cairo_t* cr) override
    {
        const bool off = std::lround(ed_.params().value(paramIndex())) <= 0;
        fillRounded(cr, bounds_, 4, isHovered() ? theme::panelLight.mix(Color(1, 1, 1, 1), 0.04f) : theme::panelLight);
        strokeRounded(cr, bounds_, 4, theme::outline);
        setFont(cr, 10.5f);
        drawText(cr, text(), bounds_.reduced(8, 0), Align::Left, off ? theme::textFaint : theme::text);
    }

    bool mouseDown(const MouseEvent& e) override
    {
        ParamContext& ctx = ed_.params();
        const int idx = paramIndex();
        if (e.button == 3 || (e.button == 1 && (e.mods & ModCtrl)))
        {
            ctx.change(idx, 0.0);
            return true;
        }
        if (e.button != 1)
            return false;
        showMenu(root(), ed_.destinationMenu([&ctx, idx](int v) { ctx.change(idx, static_cast<double>(v)); }), e.x,
                 e.y);
        return true;
    }

    int boundParam() const override { return paramIndex(); }
    std::string tooltip() const override { return "Click for a destination, right-click to clear"; }

private:
    Editor& ed_;
    int route_;
};

// ------------------------------------------------------------ MatrixPanel

// Sixteen routes of the matrix, in two columns: source, destination, amount
// and curve. The page (1-16, 17-32) is the editor's.
class MatrixPanel : public Widget
{
public:
    static constexpr int kPerPage = 16;

    explicit MatrixPanel(Editor& ed) : ed_(ed) {}

    void build()
    {
        clearChildren();
        rows_.clear();
        ParamContext& ctx = ed_.params();
        for (int i = 0; i < kPerPage; ++i)
        {
            const int r = ed_.matrixPage() * kPerPage + i;
            Row row;
            row.route = r;
            row.source = add<ParamSelector>(ctx, ctx.index(pid::route(r, pid::RSource)));
            row.source->fontSize = 10.5f;
            row.dest = add<DestButton>(ed_, r);
            row.amount = add<ParamField>(ctx, ctx.index(pid::route(r, pid::RAmount)));
            row.amount->accent = theme::amp;
            row.curve = add<ParamField>(ctx, ctx.index(pid::route(r, pid::RCurve)));
            rows_.push_back(row);
        }
        layout();
    }

    // The panel covers the whole section: the page tabs share the title
    // line, the column heads and the rows come under it.
    Rect pageTab(int i) const { return {bounds_.right() - 124 + i * 62.0f, bounds_.y + 2, 58, 20}; }
    static constexpr float kHeads = 28.0f, kRows = 44.0f;

    void layout() override
    {
        const float colW = (bounds_.w - 16) / 2, rowH = std::min(26.0f, (bounds_.h - kRows - 4) / 8);
        for (size_t i = 0; i < rows_.size(); ++i)
        {
            const int col = static_cast<int>(i) / 8, line = static_cast<int>(i) % 8;
            const float x = bounds_.x + col * (colW + 16), y = bounds_.y + kRows + line * rowH;
            const float w = colW - 26;
            Row& r = rows_[i];
            r.source->setBounds({x + 26, y + 2, w * 0.24f, rowH - 4});
            r.dest->setBounds({x + 30 + w * 0.24f, y + 2, w * 0.44f, rowH - 4});
            r.amount->setBounds({x + 34 + w * 0.68f, y + 2, w * 0.16f - 4, rowH - 4});
            r.curve->setBounds({x + 34 + w * 0.84f, y + 2, w * 0.16f - 4, rowH - 4});
        }
    }

    void paint(cairo_t* cr) override
    {
        setFont(cr, 9.5f, true);
        const float colW = (bounds_.w - 16) / 2;
        for (int col = 0; col < 2; ++col)
        {
            const float x = bounds_.x + col * (colW + 16), w = colW - 26;
            const float y = bounds_.y + kHeads;
            drawText(cr, "SOURCE", {x + 26, y, w * 0.24f, 14}, Align::Left, theme::textFaint);
            drawText(cr, "DESTINATION", {x + 30 + w * 0.24f, y, w * 0.44f, 14}, Align::Left, theme::textFaint);
            drawText(cr, "AMOUNT", {x + 34 + w * 0.68f, y, w * 0.16f, 14}, Align::Left, theme::textFaint);
            drawText(cr, "CURVE", {x + 34 + w * 0.84f, y, w * 0.16f, 14}, Align::Left, theme::textFaint);
        }
        setFont(cr, 10.0f, true);
        for (const Row& r : rows_)
        {
            const Rect b = r.source->bounds();
            drawText(cr, std::to_string(r.route + 1), {b.x - 26, b.y, 22, b.h}, Align::Right, theme::textFaint);
        }
        for (int i = 0; i < 2; ++i)
        {
            const Rect t = pageTab(i);
            const bool on = ed_.matrixPage() == i;
            fillRounded(cr, t, 4, on ? theme::accent.withAlpha(0.2f) : theme::panelLight);
            strokeRounded(cr, t, 4, on ? theme::accent.withAlpha(0.8f) : theme::outline);
            setFont(cr, 10.0f, on);
            drawText(cr, i == 0 ? "1-16" : "17-32", t, Align::Center, on ? theme::text : theme::textDim);
        }
    }

    bool mouseDown(const MouseEvent& e) override
    {
        for (int i = 0; i < 2; ++i)
            if (pageTab(i).contains(e.x, e.y))
            {
                ed_.selectMatrixPage(i);
                return true;
            }
        return false;
    }

private:
    struct Row
    {
        int route = 0;
        ParamSelector* source = nullptr;
        DestButton* dest = nullptr;
        ParamField* amount = nullptr;
        ParamField* curve = nullptr;
    };
    Editor& ed_;
    std::vector<Row> rows_;
};

// ------------------------------------------------------------ Editor

namespace {
struct Sections
{
    Rect rack, strip, source, chain;
};

Sections sectionsFor(float W, float H)
{
    Sections s;
    s.rack = {12, 46, 236, H - 58};
    const float x = s.rack.right() + 8, w = W - x - 12;
    s.strip = {x, 46, w, 100};
    s.source = {x, s.strip.bottom() + 8, w, 262};
    s.chain = {x, s.source.bottom() + 8, w, H - s.source.bottom() - 20};
    return s;
}
} // namespace

Editor::Editor(Controller& controller) : controller_(controller), ctx_(controller), window_(this)
{
    Settings& s = Settings::get();
    logicalW_ = std::max(static_cast<float>(s.getDouble("gui_width", kBaseW)), kMinW);
    logicalH_ = std::max(static_cast<float>(s.getDouble("gui_height", kBaseH)), kMinH);
    scale_ = NativeWindow::systemScale() * s.getDouble("gui_scaling", 1.0);

    for (int i = 0; i <= kMod; ++i)
        rows_[static_cast<size_t>(i)] = add<LaneRow>(*this, i);
    hitStrip_ = add<HitStrip>(*this);
    liveScope_ = add<LiveScope>(*this);
    undoButton_ = add<Button>("", [this] { undo(); });
    undoButton_->icon = icons::undo;
    undoButton_->setTooltip("Undo (Ctrl+Z)");
    redoButton_ = add<Button>("", [this] { redo(); });
    redoButton_->icon = icons::redo;
    redoButton_->setTooltip("Redo (Ctrl+Shift+Z)");
    // Every finished edit is a step to undo.
    ctx_.onGestureEnd = [this] { controller_.checkpoint(); };
    playButton_ = add<Button>("Play", [this] { audition(); });
    playButton_->setTooltip("Play the hit through the plugin");
    exportButton_ = add<Button>("Export", [this] { saveHitAs(); });
    exportButton_->setTooltip("Save the hit as a WAV file");
    menuButton_ = add<Button>("", [this] { showMainMenu(); });
    menuButton_->icon = icons::menu;
    menuButton_->setTooltip("Options");

    strip_ = add<ControlGrid>(ctx_);
    source_ = add<ControlGrid>(ctx_);
    curve_ = add<CurveEditor>(ctx_);
    curve_->onTab = [this](int t) { selectCurveTab(t); };
    macros_ = add<ControlGrid>(ctx_);
    scope_ = add<Scope>(*this);
    chain_ = add<ChainRow>(*this);
    slotGrid_ = add<ControlGrid>(ctx_);
    for (int i = 0; i < static_cast<int>(modTabs_.size()); ++i)
    {
        const bool lfo = i < pid::kNumLfos;
        modTabs_[static_cast<size_t>(i)] =
            add<Button>((lfo ? "LFO " : "Env ") + std::to_string((lfo ? i : i - pid::kNumLfos) + 1),
                        [this, i] { selectModTab(i); });
        modTabs_[static_cast<size_t>(i)]->fontSize = 10.5f;
    }
    lfoView_ = add<LfoView>(*this);
    matrix_ = add<MatrixPanel>(*this);
    presetBar_ = add<PresetBar>(*this);
    browser_ = add<PresetBrowser>(*this);
    browser_->setVisible(false);

    grabKeyboard = [this] { window_.grabKeyboard(); };
    setBounds({0, 0, logicalW_, logicalH_});
    rebuild();
}

Editor::~Editor()
{
    detach();
    if (staticLayer_)
        cairo_surface_destroy(staticLayer_);
}

uint32_t Editor::slotId(int slot, uint32_t field) const
{
    return lane_ == kMaster ? pid::masterSlot(slot, field) : pid::slot(lane_, slot, field);
}

Editor::Structure Editor::structure() const
{
    Structure s;
    s.lane = lane_;
    s.slot = selectedSlot();
    s.guard = guardSelected();
    s.tab = curveTab_;
    if (lane_ == kMod)
    {
        s.modTab = modTab_;
        s.page = matrixPage_;
        return s;
    }
    if (lane_ < kMaster)
        s.source = static_cast<int>(std::lround(ctx_.value(ctx_.index(pid::lane(lane_, pid::LSource)))));
    s.slotType = static_cast<int>(std::lround(ctx_.value(ctx_.index(slotId(s.slot, pid::SType)))));
    return s;
}

void Editor::rebuild()
{
    // A menu still open may belong to a control about to go.
    closeAllOverlays();
    buildStrip();
    buildSource();
    buildSlot();
    built_ = structure();
    layout();
    markDirty();
}

void Editor::buildStrip()
{
    using namespace pid;
    strip_->clear();
    if (lane_ == kMod)
    {
        for (int m = 0; m < pid::kNumMacros; ++m)
            strip_->control(macro(m), "Macro " + std::to_string(m + 1));
        return;
    }
    if (lane_ == kMaster)
    {
        strip_->control(Output, "Output");
        strip_->control(Tune, "Tune");
        strip_->control(RootNote, "Root Note", 1.2f);
        strip_->control(Quality, "Quality");
        strip_->gap(0.4f);
        strip_->control(MonoBelow, "Mono Below", 1.2f);
        strip_->control(OutputClip, "Output Clip", 1.2f);
        return;
    }
    const int l = lane_;
    strip_->control(lane(l, LSource), "Source", 1.5f);
    strip_->gap(0.3f);
    strip_->control(lane(l, LLevel), "Level");
    strip_->control(lane(l, LPan), "Pan");
    strip_->control(lane(l, LVelocity), "Velocity");
    strip_->control(lane(l, LDelay), "Delay");
    strip_->control(lane(l, LTranspose), "Transpose");
    strip_->control(lane(l, LVariation), "Variation");
    strip_->control(lane(l, LInvert), "Invert");
    strip_->gap(0.3f);
    strip_->control(lane(l, LNote), "Note", 1.1f);
    strip_->control(lane(l, LOutput), "Output", 1.4f);
    strip_->control(lane(l, LPitchLink), "Pitch Link", 1.2f);
}

void Editor::buildSource()
{
    using namespace pid;
    source_->clear();
    macros_->clear();
    const bool master = lane_ == kMaster, mod = lane_ == kMod;
    const int src = master || mod ? -1 : static_cast<int>(std::lround(ctx_.value(ctx_.index(lane(lane_, LSource)))));
    const bool body = src == static_cast<int>(dsp::Source::Body);
    const bool env = mod && modTab_ >= pid::kNumLfos;
    curve_->setVisible(body || env);
    curve_->setEnv(env ? modTab_ - pid::kNumLfos : -1);
    macros_->setVisible(body);
    scope_->setVisible(!body && !mod);
    scope_->channel = master ? 0 : lane_ + 1;
    lfoView_->setVisible(mod && !env);
    for (size_t i = 0; i < modTabs_.size(); ++i)
    {
        modTabs_[i]->setVisible(mod);
        modTabs_[i]->setActive(static_cast<int>(i) == modTab_);
    }
    if (master)
        return;
    if (mod)
    {
        if (env)
        {
            const int k = modTab_ - pid::kNumLfos;
            source_->control(modEnv(k, EnvTime), "Time");
            source_->control(modEnv(k, EnvLoop), "Loop");
        }
        else
        {
            const int k = modTab_;
            lfoView_->lfo = k;
            source_->control(lfo(k, LfoShape), "Shape", 1.3f);
            source_->control(lfo(k, LfoRate), "Rate");
            source_->control(lfo(k, LfoSync), "Sync", 1.2f);
            source_->control(lfo(k, LfoPhase), "Phase");
            source_->control(lfo(k, LfoRetrigger), "Retrigger", 1.2f);
        }
        return;
    }
    const int l = lane_;
    auto c = [&](LaneField f, const char* label, float span = 1.0f) { source_->control(lane(l, f), label, span); };
    switch (static_cast<dsp::Source>(src))
    {
    case dsp::Source::Body:
        c(BodyWave, "Wave", 1.3f);
        c(BodyPhase, "Phase");
        c(BodyShape, "Shape");
        c(BodyTilt, "Tilt");
        c(BodyEven, "Even");
        c(BodyStretch, "Stretch");
        c(BodyFmAmount, "FM");
        c(BodyFmRatio, "FM Ratio");
        c(BodyFmDecay, "FM Decay");
        c(BodyFeedback, "Feedback");
        c(BodyDrift, "Drift");
        curve_->setLane(l);
        curve_->setTab(curveTab_);
        if (curveTab_ == 0)
        {
            macros_->control(lane(l, BodyPitchStart), "Start");
            macros_->control(lane(l, BodyPitchEnd), "End");
            macros_->control(lane(l, BodySweep), "Sweep");
            macros_->control(lane(l, BodySweepCurve), "Bend");
            macros_->control(lane(l, BodyKeyTrack), "Key Track");
        }
        else
        {
            macros_->control(lane(l, BodyAttack), "Attack");
            macros_->control(lane(l, BodyHold), "Hold");
            macros_->control(lane(l, BodyDecay), "Decay");
            macros_->control(lane(l, BodyDecayCurve), "Bend");
        }
        break;
    case dsp::Source::Click:
        c(ClickType, "Type", 1.2f);
        c(ClickDecay, "Decay");
        c(ClickPitch, "Pitch");
        c(ClickSweep, "Sweep");
        c(ClickFilter, "Filter", 1.4f);
        c(ClickCutoff, "Cutoff");
        c(ClickReso, "Reso");
        break;
    case dsp::Source::Noise:
        c(NoiseColor, "Color", 1.2f);
        c(NoiseDensity, "Density");
        c(NoiseWidth, "Width");
        c(NoiseFilter, "Filter", 1.4f);
        c(NoiseCutoff, "Cutoff");
        c(NoiseReso, "Reso");
        c(NoiseFilterEnv, "Filter Env");
        c(NoiseEnvDecay, "Env Decay");
        c(NoiseAttack, "Attack");
        c(NoiseHold, "Hold");
        c(NoiseDecay, "Decay");
        c(NoiseCurve, "Curve");
        break;
    case dsp::Source::Resonator:
        c(ResExciter, "Exciter", 1.2f);
        c(ResModel, "Model", 1.3f);
        c(ResModes, "Modes");
        c(ResTune, "Tune");
        c(ResKeyTrack, "Key Track");
        c(ResDecay, "Decay");
        c(ResDamping, "Damping");
        c(ResBrightness, "Brightness");
        c(ResHardness, "Hardness");
        c(ResDrop, "Drop");
        c(ResDropTime, "Drop Time");
        break;
    case dsp::Source::Bus:
        for (int b = 0; b < dsp::kNumLanes; ++b)
            if (b != l)
                source_->control(lane(l, BusLane) + static_cast<uint32_t>(b), "Lane " + std::to_string(b + 1));
        c(BusTap, "Tap", 1.6f);
        break;
    }
}

void Editor::buildSlot()
{
    using namespace pid;
    slotGrid_->clear();
    chain_->setVisible(lane_ != kMod);
    slotGrid_->setVisible(lane_ != kMod);
    matrix_->setVisible(lane_ == kMod);
    if (lane_ == kMod)
    {
        matrix_->build();
        return;
    }
    if (guardSelected())
    {
        const int l = lane_;
        slotGrid_->control(lane(l, LGuardDelay), "Delay");
        slotGrid_->control(lane(l, LGuardFade), "Fade");
        slotGrid_->gap(0.4f);
        slotGrid_->control(lane(l, LDuckSource), "Duck", 1.3f);
        slotGrid_->control(lane(l, LDuckDepth), "Depth");
        slotGrid_->control(lane(l, LDuckRelease), "Release");
        return;
    }
    const int s = selectedSlot();
    slotGrid_->control(slotId(s, SType), "Type", 1.5f);
    const int type = static_cast<int>(std::lround(ctx_.value(ctx_.index(slotId(s, SType)))));
    if (type != 0)
    {
        slotGrid_->control(slotId(s, SBand), "Band", 1.4f);
        slotGrid_->control(slotId(s, SMix), "Mix");
        slotGrid_->control(slotId(s, SBypass), "Bypass");
        slotGrid_->gap(0.4f);
        for (int k = 0; k < dsp::kSlotValues; ++k)
        {
            const uint32_t id = slotId(s, SValueA + static_cast<uint32_t>(k));
            const int idx = ctx_.index(id);
            // A letter the type has no use for keeps its generic key.
            if (ctx_.def(idx).key == ctx_.table().def(idx).key)
                continue;
            slotGrid_->control(id, {}, ctx_.def(idx).choices.empty() ? 1.0f : 1.6f);
        }
    }
    // The band split the slots share.
    slotGrid_->gap(0.4f);
    if (lane_ == kMaster)
    {
        slotGrid_->control(MasterXoverLow, "Xover Lo");
        slotGrid_->control(MasterXoverHigh, "Xover Hi");
    }
    else
    {
        slotGrid_->control(lane(lane_, LXoverLow), "Xover Lo");
        slotGrid_->control(lane(lane_, LXoverHigh), "Xover Hi");
    }
}

void Editor::selectLane(int lane)
{
    lane = std::clamp(lane, 0, kMod);
    if (lane != lane_)
    {
        lane_ = lane;
        markDirty();
    }
}

void Editor::selectSlot(int slot)
{
    slot = std::clamp(slot, 0, dsp::kNumSlots - 1);
    if (slot != selectedSlot() || guardSelected())
    {
        slot_[static_cast<size_t>(lane_)] = slot;
        guard_[static_cast<size_t>(lane_)] = false;
        markDirty();
    }
}

void Editor::selectGuard()
{
    if (lane_ < kMaster && !guardSelected())
    {
        guard_[static_cast<size_t>(lane_)] = true;
        markDirty();
    }
}

void Editor::selectModTab(int tab)
{
    tab = std::clamp(tab, 0, pid::kNumLfos + pid::kNumModEnvs - 1);
    if (tab != modTab_)
    {
        modTab_ = tab;
        markDirty();
    }
}

void Editor::selectMatrixPage(int page)
{
    page = std::clamp(page, 0, pid::kNumRoutes / MatrixPanel::kPerPage - 1);
    if (page != matrixPage_)
    {
        matrixPage_ = page;
        markDirty();
    }
}

bool Editor::addRoute(ModSource source, int index)
{
    const int dest = ctx_.table().destinationOf(index);
    if (dest <= 0)
        return false;
    for (int r = 0; r < pid::kNumRoutes; ++r)
    {
        const int s = ctx_.index(pid::route(r, pid::RSource)), d = ctx_.index(pid::route(r, pid::RDest));
        if (std::lround(ctx_.value(s)) > 0 || std::lround(ctx_.value(d)) > 0)
            continue;
        const int a = ctx_.index(pid::route(r, pid::RAmount)), c = ctx_.index(pid::route(r, pid::RCurve));
        ctx_.change(s, static_cast<double>(source));
        ctx_.change(d, static_cast<double>(dest));
        ctx_.change(a, ctx_.table().fromPlain(a, 25.0));
        ctx_.change(c, ctx_.defaultValue(c));
        return true;
    }
    return false;
}

std::vector<MenuItem> Editor::destinationMenu(std::function<void(int)> pick)
{
    const ParamTable& t = ctx_.table();
    const std::vector<int>& dests = t.destinations();
    // "L1 Slot 2 Cutoff" -> "Cutoff", "L1 Pitch Start" -> "Pitch Start".
    auto item = [&](int index) {
        MenuItem it;
        it.label = shortName(ctx_.def(index).name);
        const int v = t.destinationOf(index);
        it.action = [pick, v] { pick(v); };
        return it;
    };
    auto group = [&](const std::string& module, const std::string& label) {
        MenuItem m;
        m.label = label;
        for (int index : dests)
        {
            if (t.def(index).module != module)
                continue;
            // A letter its slot's type has no use for is left out.
            if (t.def(index).typeParam >= 0 && ctx_.def(index).key == t.def(index).key)
                continue;
            m.submenu.push_back(item(index));
        }
        return m;
    };
    auto slots = [&](std::vector<MenuItem>& out, const std::string& prefix, auto typeId) {
        for (int s = 0; s < dsp::kNumSlots; ++s)
        {
            const int type = static_cast<int>(std::lround(ctx_.value(ctx_.index(typeId(s)))));
            if (type == 0)
                continue;
            const std::string n = std::to_string(s + 1);
            out.push_back(group(prefix + "/Slot " + n,
                                "Slot " + n + ": " + ctx_.def(ctx_.index(typeId(s))).labels[static_cast<size_t>(type)]));
        }
    };

    std::vector<MenuItem> items;
    items.push_back({"None", [pick] { pick(0); }});
    items.push_back({"", {}, false, true, true});
    for (int l = 0; l < dsp::kNumLanes; ++l)
    {
        const std::string lane = "Lane " + std::to_string(l + 1);
        MenuItem top;
        top.label = lane;
        top.submenu.push_back(group(lane, "Lane"));
        const int src = static_cast<int>(std::lround(ctx_.value(ctx_.index(pid::lane(l, pid::LSource)))));
        if (src != static_cast<int>(dsp::Source::Bus))
            top.submenu.push_back(group(lane + "/" + kSourceNames[std::clamp(src, 0, 3)], kSourceNames[std::clamp(src, 0, 3)]));
        slots(top.submenu, lane, [l](int s) { return pid::slot(l, s, pid::SType); });
        items.push_back(std::move(top));
    }
    MenuItem master;
    master.label = "Master";
    master.submenu.push_back(group("Master", "Output"));
    slots(master.submenu, "Master", [](int s) { return pid::masterSlot(s, pid::SType); });
    items.push_back(std::move(master));
    return items;
}

void Editor::showModMenu(int index, float x, float y)
{
    const int dest = ctx_.table().destinationOf(index);
    std::vector<MenuItem> items;
    items.push_back({"Modulate " + shortName(ctx_.def(index).name), {}, false, false});
    const ParamDef& sources = ctx_.table().def(ctx_.index(pid::route(0, pid::RSource)));
    MenuItem add;
    add.label = "Add";
    for (int s = 1; s < static_cast<int>(ModSource::Count); ++s)
        add.submenu.push_back({sources.labels[static_cast<size_t>(s)], [this, s, index] {
                                   if (!addRoute(static_cast<ModSource>(s), index))
                                       notify("All 32 routes are in use");
                               }});
    items.push_back(std::move(add));
    // The routes already on it, each with its amount and a way to remove it.
    for (int r = 0; r < pid::kNumRoutes; ++r)
    {
        const int si = ctx_.index(pid::route(r, pid::RSource)), di = ctx_.index(pid::route(r, pid::RDest));
        const int src = static_cast<int>(std::lround(ctx_.value(si)));
        if (src <= 0 || std::lround(ctx_.value(di)) != dest)
            continue;
        const int ai = ctx_.index(pid::route(r, pid::RAmount));
        MenuItem it;
        it.label = "Mod " + std::to_string(r + 1) + ": " + ctx_.text(si) + ", " + ctx_.text(ai);
        it.submenu.push_back({"Show in the matrix", [this, r] {
                                  selectLane(kMod);
                                  selectMatrixPage(r / MatrixPanel::kPerPage);
                              }});
        it.submenu.push_back({"Remove", [this, si, di, ai] {
                                  ctx_.change(si, 0.0);
                                  ctx_.change(di, 0.0);
                                  ctx_.change(ai, ctx_.defaultValue(ai));
                              }});
        if (items.size() == 2)
            items.push_back({"", {}, false, true, true});
        items.push_back(std::move(it));
    }
    showMenu(this, std::move(items), x, y);
}

void Editor::selectCurveTab(int tab)
{
    if (tab != curveTab_)
    {
        curveTab_ = tab;
        markDirty();
    }
}

void Editor::swapSlots(int a, int b)
{
    if (a == b)
        return;
    using namespace pid;
    std::vector<int> fa, fb;
    std::vector<double> va, vb;
    for (uint32_t f = 0; f < SValueA + dsp::kSlotValues; ++f)
    {
        fa.push_back(ctx_.index(slotId(a, f)));
        fb.push_back(ctx_.index(slotId(b, f)));
        va.push_back(ctx_.value(fa.back()));
        vb.push_back(ctx_.value(fb.back()));
    }
    // One gesture. The types go first: a type change resets the letters,
    // and the letters are set after it.
    for (size_t i = 0; i < fa.size(); ++i)
    {
        ctx_.begin(fa[i]);
        ctx_.begin(fb[i]);
    }
    for (size_t i = 0; i < fa.size(); ++i)
    {
        ctx_.set(fa[i], vb[i], true);
        ctx_.set(fb[i], va[i], true);
    }
    for (size_t i = 0; i < fa.size(); ++i)
    {
        ctx_.end(fa[i]);
        ctx_.end(fb[i]);
    }
    selectSlot(b);
}

void Editor::layout()
{
    const float W = bounds_.w, H = bounds_.h;
    const Sections S = sectionsFor(W, H);

    // Top bar: the hit in the middle, play, export and the menu at the right.
    menuButton_->setBounds({W - 42, 8, 30, 28});
    exportButton_->setBounds({W - 112, 8, 64, 28});
    playButton_->setBounds({W - 170, 8, 52, 28});
    presetBar_->setBounds({S.strip.x, 8, 250, 28});
    undoButton_->setBounds({S.strip.x + 256, 8, 28, 28});
    redoButton_->setBounds({S.strip.x + 288, 8, 28, 28});
    hitStrip_->setBounds({S.strip.x + 324, 8, W - 170 - 8 - S.strip.x - 324, 28});
    browser_->setBounds({S.strip.x, 46, W - S.strip.x - 12, H - 58});

    // Rack: eight lanes and the master, sharing the height.
    const Rect R = S.rack;
    const float top = R.y + 28, gap = 5;
    // The live scope at the foot of the rack, the rows above it.
    constexpr float kScopeH = 56.0f;
    liveScope_->setBounds({R.x + 8, R.bottom() - 8 - kScopeH, R.w - 16, kScopeH});
    const float rowH = (R.bottom() - 8 - kScopeH - 22 - top - gap * kMod) / (kMod + 1);
    for (int i = 0; i <= kMod; ++i)
        rows_[static_cast<size_t>(i)]->setBounds({R.x + 8, top + i * (rowH + gap), R.w - 16, rowH});

    strip_->setBounds({S.strip.x, S.strip.y + 28, S.strip.w, S.strip.h - 30});

    const Rect src = S.source;
    const bool body = curve_->isVisible() && lane_ != kMod;
    const float left = lane_ == kMaster ? 0.0f : (body ? 6.3f * kCellW + 20 : 4.6f * kCellW + 20);
    source_->setBounds({src.x, src.y + (lane_ == kMod ? 62 : 30), left, src.h - (lane_ == kMod ? 66 : 34)});
    for (size_t i = 0; i < modTabs_.size(); ++i)
        modTabs_[i]->setBounds({src.x + 10 + i * 56.0f, src.y + 28, 52, 22});
    const Rect right{src.x + left + 4, src.y + 10, src.w - left - 14, src.h - 18};
    curve_->setBounds({right.x, right.y, right.w, right.h - kCellH - 10});
    macros_->setBounds({right.x + 40, right.bottom() - kCellH, right.w - 40, kCellH});
    scope_->setBounds(lane_ == kMaster ? Rect{src.x + 10, src.y + 30, src.w - 20, src.h - 40}
                                       : Rect{right.x, right.y + 20, right.w, right.h - 20});

    if (lane_ == kMod)
    {
        // The modulators' editors start under their tabs.
        const Rect mr{src.x + left + 4, src.y + 56, src.w - left - 14, src.h - 64};
        curve_->setBounds(mr);
        lfoView_->setBounds(mr);
    }
    const Rect C = S.chain;
    matrix_->setBounds({C.x + 8, C.y + 4, C.w - 16, C.h - 8});
    chain_->setBounds({C.x + 10, C.y + 30, C.w - 20, 46});
    slotGrid_->setBounds({C.x, C.y + 90, C.w, C.h - 92});
}

Rect Editor::laneRowBounds(int lane) const { return rows_[static_cast<size_t>(std::clamp(lane, 0, kMod))]->bounds(); }

// Where a Body lane's pitch comes to rest on the root note: its Pitch End,
// moved by its Transpose and the master's Tune. Empty for other sources.
std::string Editor::landing(int lane) const
{
    using namespace pid;
    if (std::lround(ctx_.value(ctx_.index(pid::lane(lane, LSource)))) != static_cast<int>(dsp::Source::Body))
        return {};
    const int link = static_cast<int>(std::lround(ctx_.value(ctx_.index(pid::lane(lane, LPitchLink))))) - 1;
    if (link >= 0 && link != lane)
        return {};
    const double end = ctx_.plain(ctx_.index(pid::lane(lane, BodyPitchEnd)));
    const double semis = ctx_.plain(ctx_.index(pid::lane(lane, LTranspose))) + ctx_.plain(ctx_.index(Tune));
    const double hz = end * std::exp2(semis / 12.0);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ends on %s  (%.1f Hz)", noteOf(hz).c_str(), hz);
    return buf;
}

void Editor::paint(cairo_t* cr)
{
    setColor(cr, theme::background);
    cairo_paint(cr);
    setFont(cr, 18, true);
    drawText(cr, "Substrike", {16, 8, 120, 28}, Align::Left, theme::accent);
    const float logoW = textWidth(cr, "Substrike");
    setFont(cr, 10);
    setFont(cr, 10, true);
    drawText(cr, kPluginVersion, {16 + logoW + 6, 10, 40, 28}, Align::Left, theme::textDim);
    const float versionW = textWidth(cr, kPluginVersion);
    setFont(cr, 10);
    drawText(cr, "kick designer", {16 + logoW + 6 + versionW + 7, 10, 90, 28}, Align::Left, theme::textFaint);

    const Sections S = sectionsFor(bounds_.w, bounds_.h);
    auto section = [&](const Rect& r, const std::string& title) {
        fillRounded(cr, r, 8, theme::panel);
        setFont(cr, 11, true);
        drawText(cr, title, {r.x + 12, r.y + 6, r.w - 24, 18}, Align::Left, theme::accent);
    };
    section(S.rack, "LANES");
    setFont(cr, 10.5f, true);
    drawText(cr, "OUTPUT", {S.rack.x + 12, liveScope_->bounds().y - 19, 100, 16}, Align::Left, theme::textDim);
    if (lane_ == kMod)
    {
        section(S.strip, "MACROS");
        section(S.source, "MODULATORS");
        section(S.chain, "MATRIX");
        setFont(cr, 10.5f);
        drawText(cr, "Right-click any knob to modulate it", {S.chain.x + 120, S.chain.y + 6, 300, 18}, Align::Left,
                 theme::textFaint);
        return;
    }
    if (lane_ == kMaster)
    {
        section(S.strip, "MASTER");
        for (int l = 0; l < dsp::kNumLanes; ++l)
        {
            if (ctx_.value(ctx_.index(pid::lane(l, pid::LEnabled))) < 0.5)
                continue;
            const std::string where = landing(l);
            if (where.empty())
                continue;
            setFont(cr, 10.5f);
            drawText(cr, "Lane " + std::to_string(l + 1) + " " + where,
                     {S.strip.x + 12, S.strip.y + 6, S.strip.w - 24, 18}, Align::Right, theme::amp);
            break;
        }
        section(S.source, "HIT");
        section(S.chain, "MASTER CHAIN");
    }
    else
    {
        const std::string lane = "LANE " + std::to_string(lane_ + 1);
        section(S.strip, lane);
        const int src = static_cast<int>(std::lround(ctx_.value(ctx_.index(pid::lane(lane_, pid::LSource)))));
        std::string name = kSourceNames[std::clamp(src, 0, 4)];
        for (auto& ch : name)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        section(S.source, name);
        section(S.chain, lane + " CHAIN");
        if (ctx_.value(ctx_.index(pid::lane(lane_, pid::LEnabled))) < 0.5)
        {
            setFont(cr, 10.5f);
            drawText(cr, "this lane is off", {S.strip.x + 12, S.strip.y + 6, S.strip.w - 24, 18}, Align::Right,
                     theme::textFaint);
        }
        else if (const std::string where = landing(lane_); !where.empty())
        {
            setFont(cr, 10.5f);
            drawText(cr, where, {S.strip.x + 12, S.strip.y + 6, S.strip.w - 24, 18}, Align::Right, theme::amp);
        }
    }
    // Under the chain: which slot the controls below belong to.
    setFont(cr, 10.5f, true);
    drawText(cr, guardSelected() ? std::string("GUARD") : "SLOT " + std::to_string(selectedSlot() + 1),
             {S.chain.x + 12, S.chain.y + 78, 120, 14}, Align::Left, theme::textDim);
    setColor(cr, theme::outline);
    cairo_rectangle(cr, S.chain.x + 60, S.chain.y + 85, S.chain.w - 72, 1);
    cairo_fill(cr);
}

void Editor::paintTooltip(cairo_t* cr)
{
    if (!notice_.empty())
    {
        setFont(cr, 12);
        const float tw = textWidth(cr, notice_) + 28;
        const Rect r{bounds_.w * 0.5f - tw * 0.5f, 52, tw, 28};
        fillRounded(cr, r, 6, theme::panelLight);
        strokeRounded(cr, r, 6, theme::accent.withAlpha(0.6f));
        drawText(cr, notice_, r, Align::Center, theme::text);
    }
    Widget* w = captured() ? captured() : hovered();
    if (!w || hasOverlay())
        return;
    const std::string tip = w->tooltip();
    if (tip.empty())
        return;
    setFont(cr, 11);
    const float tw = textWidth(cr, tip) + 16;
    const Rect b = w->bounds();
    const float x = std::clamp(b.cx() - tw * 0.5f, 4.0f, bounds_.w - tw - 4);
    float y = b.y - 26;
    if (y < 2)
        y = b.bottom() + 4;
    const Rect r{x, y, tw, 22};
    fillRounded(cr, {r.x + 1, r.y + 2, r.w, r.h}, 5, Color(0, 0, 0, 0.4f));
    fillRounded(cr, r, 5, theme::panelLight);
    strokeRounded(cr, r, 5, theme::accent.withAlpha(0.45f));
    drawText(cr, tip, r, Align::Center, theme::text);
}

void Editor::onMouseDown(const MouseEvent& e)
{
    // A click anywhere in a rack row selects it, its controls included.
    if (!hasOverlay())
        for (int i = 0; i <= kMod; ++i)
            if (rows_[static_cast<size_t>(i)]->bounds().contains(e.x, e.y))
                selectLane(i);
    // Right-click on a knob: its modulation.
    if (e.button == 3 && !hasOverlay())
        if (auto* k = dynamic_cast<Knob*>(hitTest(e.x, e.y)))
            if (ctx_.table().destinationOf(k->paramIndex()) > 0)
            {
                showModMenu(k->paramIndex(), e.x, e.y);
                return;
            }
    handleMouseDown(e);
}

bool Editor::keyDown(const KeyEvent& e)
{
    if (e.mods & ModCtrl)
    {
        const unsigned k = e.keysym;
        if (k == 'z' || k == 'Z')
        {
            (e.mods & ModShift) ? redo() : undo();
            return true;
        }
        if (k == 'y' || k == 'Y')
        {
            redo();
            return true;
        }
    }
    if (browser_->isVisible() && !hasOverlay())
    {
        if (e.keysym == key::Escape)
        {
            togglePresetBrowser();
            return true;
        }
        if (e.keysym == key::Up || e.keysym == key::Down)
        {
            browser_->step(e.keysym == key::Up ? -1 : 1);
            return true;
        }
    }
    if (e.keysym == key::Escape)
    {
        closeAllOverlays();
        return true;
    }
    return false;
}

void Editor::audition() { controller_.audition(); }

void Editor::undo()
{
    if (!controller_.undo())
        notify("Nothing to undo");
    markDirty();
}

void Editor::redo()
{
    if (!controller_.redo())
        notify("Nothing to redo");
    markDirty();
}

bool Editor::presetBrowserOpen() const { return browser_->isVisible(); }

void Editor::togglePresetBrowser()
{
    const bool open = !browser_->isVisible();
    if (open)
        browser_->refresh();
    browser_->setVisible(open);
    markDirty();
}

void Editor::loadPreset(const PresetInfo& p)
{
    StateDocument doc;
    bool ok = false;
    if (p.factory)
    {
        const char* text = factoryPresetText(p.key);
        ok = text && parseState(text, doc);
    }
    else
        ok = readPresetFile(p.key, doc);
    if (!ok)
    {
        notify("Cannot read " + p.name);
        return;
    }
    controller_.loadDocument(doc, p.name);
    if (Settings::get().getDouble("preset_audition", 1.0) > 0.5)
        controller_.audition();
    markDirty();
}

void Editor::stepPreset(int step)
{
    if (!browser_->isVisible())
        browser_->refresh();
    browser_->step(step);
}

void Editor::savePresetAs()
{
    const Rect anchor = presetBar_->bounds();
    auto ed = std::make_unique<TextEditor>(controller_.presetName(), [this](const std::string& typed) {
        std::string name = typed;
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name.empty())
            return;
        StateDocument doc = controller_.currentDocument();
        const std::string path = writeUserPreset(name, doc);
        if (path.empty())
        {
            notify("Could not save " + name);
            return;
        }
        controller_.loadDocument(doc, name);
        browser_->refresh();
        notify("Saved " + name);
    });
    ed->setBounds({anchor.x, anchor.y, anchor.w, anchor.h});
    Widget* raw = ed.get();
    pushOverlay(std::move(ed));
    setKeyFocus(raw);
    if (grabKeyboard)
        grabKeyboard();
}

std::string Editor::exportHit(const std::string& requested)
{
    std::vector<double> values(static_cast<size_t>(ctx_.table().count()));
    for (int i = 0; i < ctx_.table().count(); ++i)
        values[static_cast<size_t>(i)] = ctx_.value(i);
    std::array<dsp::Curve, dsp::kNumCurves> curves;
    for (int i = 0; i < dsp::kNumCurves; ++i)
        curves[static_cast<size_t>(i)] = controller_.curve(i);
    const HitRender hit = renderHit(values, curves, controller_.sampleRate(), HitPreview::kMaxSeconds);

    std::string path = requested;
    if (path.empty())
    {
        // A drag leaves the file where the DAW can keep referring to it.
        const std::filesystem::path dir = toPath(Settings::dataDir()) / "Exports";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        std::filesystem::path file = dir / (std::string("Substrike-") + stamp + ".wav");
        for (int n = 2; std::filesystem::exists(file, ec) && n < 100; ++n)
            file = dir / (std::string("Substrike-") + stamp + "-" + std::to_string(n) + ".wav");
        path = fromPath(file);
    }
    if (!writeHitWav(path, hit, Settings::get().getDouble("export_normalise", 0.0) > 0.5))
    {
        notify("Could not write " + path);
        return {};
    }
    return path;
}

void Editor::dragHit()
{
    const std::string path = exportHit();
    if (path.empty())
        return;
    if (!window_.startFileDrag(path))
        notify("Saved " + fromPath(toPath(path).filename()) + " in the export folder");
}

void Editor::saveHitAs()
{
    if (dialog_ && dialog_->running())
        return;
    dialog_ = std::make_unique<FileDialog>();
    const std::string start = Settings::get().getString("export_dir", "");
    if (!dialog_->start(FileDialog::Mode::SaveFile, "Export Hit", "WAV files | *.wav", start))
    {
        dialog_.reset();
        // No dialog to ask with: the export folder it is.
        const std::string path = exportHit();
        if (!path.empty())
            notify("Saved " + path);
    }
}

void Editor::showMainMenu()
{
    std::vector<MenuItem> items;
    struct Size
    {
        const char* name;
        float w, h;
    };
    const Size sizes[] = {{"Medium", kBaseW, kBaseH}, {"Large", 1344, 864}, {"Extra Large", 1568, 1008}};
    MenuItem size{"Window Size"};
    for (const Size& s : sizes)
        size.submenu.push_back({s.name, [this, s] { setLogicalSize(s.w, s.h); },
                                std::fabs(logicalW_ - s.w) < 1 && std::fabs(logicalH_ - s.h) < 1});
    items.push_back(size);
    MenuItem scaling{"Scaling"};
    const double cur = Settings::get().getDouble("gui_scaling", 1.0);
    for (double f : {0.75, 0.85, 1.0, 1.25, 1.5, 2.0})
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f%%", f * 100);
        scaling.submenu.push_back({buf, [this, f] {
                                       Settings::get().set("gui_scaling", f);
                                       Settings::get().save();
                                       setScale(NativeWindow::systemScale() * f);
                                       if (requestResize)
                                           requestResize(static_cast<int>(logicalW_ * scale_),
                                                         static_cast<int>(logicalH_ * scale_));
                                   },
                                   std::fabs(cur - f) < 1e-6});
    }
    items.push_back(scaling);
    items.push_back({"", nullptr, false, true, true});
    const bool normalise = Settings::get().getDouble("export_normalise", 0.0) > 0.5;
    items.push_back({"Normalise Exported Hits", [normalise] {
                         Settings::get().set("export_normalise", normalise ? 0.0 : 1.0);
                         Settings::get().save();
                     },
                     normalise});
    items.push_back({"Export Hit...", [this] { saveHitAs(); }});
    items.push_back({"", nullptr, false, true, true});
    const bool play = Settings::get().getDouble("preset_audition", 1.0) > 0.5;
    items.push_back({"Play Presets When Loaded", [play] {
                         Settings::get().set("preset_audition", play ? 0.0 : 1.0);
                         Settings::get().save();
                     },
                     play});
    items.push_back({"Save Preset As...", [this] { savePresetAs(); }});
    items.push_back({"", nullptr, false, true, true});
    items.push_back({std::string(kPluginName) + " " + kPluginVersion, nullptr, false, false});
    showMenu(this, std::move(items), menuButton_->bounds().right() - 220, menuButton_->bounds().bottom() + 4);
}

void Editor::setLogicalSize(float w, float h)
{
    Settings::get().set("gui_width", static_cast<double>(w));
    Settings::get().set("gui_height", static_cast<double>(h));
    Settings::get().save();
    const int pw = static_cast<int>(std::lround(w * scale_)), ph = static_cast<int>(std::lround(h * scale_));
    if (requestResize && requestResize(pw, ph))
        setPhysicalSize(pw, ph);
}

void Editor::notify(const std::string& message)
{
    notice_ = message;
    noticeUntil_ = nowSeconds() + 3.0;
    markDirty();
}

// ------------------------------------------------------------ window

bool Editor::attach(uintptr_t parentWindow)
{
    int w, h;
    physicalSize(w, h);
    window_.setScale(scale_);
    return window_.attach(parentWindow, w, h);
}

void Editor::detach() { window_.destroy(); }

void Editor::setScale(double s)
{
    scale_ = std::clamp(s, 0.5, 4.0);
    window_.setScale(scale_);
    markDirty();
}

void Editor::physicalSize(int& w, int& h) const
{
    w = static_cast<int>(std::lround(logicalW_ * scale_));
    h = static_cast<int>(std::lround(logicalH_ * scale_));
}

void Editor::setPhysicalSize(int w, int h)
{
    logicalW_ = std::max(kMinW, static_cast<float>(w / scale_));
    logicalH_ = std::max(kMinH, static_cast<float>(h / scale_));
    setBounds({0, 0, logicalW_, logicalH_});
    window_.setSize(w, h);
    markDirty();
}

void Editor::show() { window_.show(); }
void Editor::hide() { window_.hide(); }

void Editor::checkPreview(double now)
{
    // Re-render the hit once the parameters or curves have held still for a
    // moment, so a knob drag renders a handful of times rather than every
    // frame.
    std::vector<double> values(static_cast<size_t>(ctx_.table().count()));
    for (int i = 0; i < ctx_.table().count(); ++i)
        values[static_cast<size_t>(i)] = ctx_.value(i);
    std::array<dsp::Curve, dsp::kNumCurves> curves;
    for (int i = 0; i < dsp::kNumCurves; ++i)
        curves[static_cast<size_t>(i)] = controller_.curve(i);
    if (values != lastValues_ || curves != lastCurves_)
    {
        lastValues_ = std::move(values);
        lastCurves_ = curves;
        previewStale_ = true;
        lastChange_ = now;
        markDirty();
    }
    if (previewStale_ && now - lastChange_ > 0.04)
    {
        previewStale_ = false;
        preview_.request(lastValues_, lastCurves_, controller_.sampleRate());
    }
    if (preview_.poll())
        markDirty();
}

void Editor::settlePreview()
{
    checkPreview(nowSeconds());
    previewStale_ = false;
    preview_.request(lastValues_, lastCurves_, controller_.sampleRate());
    for (int i = 0; i < 2000 && !preview_.poll(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (structure() != built_)
        rebuild();
    markDirty();
}

void Editor::onTimer()
{
    window_.processEvents();
    const double t = nowSeconds();
    // A new selection, source or slot type changes which controls exist.
    if (structure() != built_)
        rebuild();
    checkPreview(t);
    // The scope moves without repainting the window's cached layer.
    if (liveScope_->update(t))
        markAnimDirty();
    undoButton_->setEnabled(controller_.canUndo());
    redoButton_->setEnabled(controller_.canRedo());
    // The matrix moved something a knob shows.
    if (const uint32_t g = controller_.modulationGeneration(); g != modGeneration_)
    {
        modGeneration_ = g;
        markDirty();
    }
    if (dialog_)
    {
        std::string result;
        if (dialog_->poll(result))
        {
            dialog_.reset();
            if (!result.empty())
            {
                if (toPath(result).extension() != ".wav")
                    result += ".wav";
                Settings::get().set("export_dir", fromPath(toPath(result).parent_path()));
                Settings::get().save();
                if (!exportHit(result).empty())
                    notify("Saved " + fromPath(toPath(result).filename()));
            }
        }
    }
    if (!notice_.empty() && t > noticeUntil_)
    {
        notice_.clear();
        markDirty();
    }
    if (takeDirty())
        window_.render();
}

void Editor::onPaint(cairo_t* cr, int, int) { renderTo(cr); }

void Editor::renderTo(cairo_t* cr)
{
    double sx = 1, sy = 1;
    cairo_user_to_device_distance(cr, &sx, &sy);
    const int w = static_cast<int>(std::ceil(bounds_.w * sx)), h = static_cast<int>(std::ceil(bounds_.h * sx));
    if (!staticLayer_ || staticW_ != w || staticH_ != h)
    {
        if (staticLayer_)
            cairo_surface_destroy(staticLayer_);
        staticLayer_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        staticW_ = w;
        staticH_ = h;
        staticStale_ = true;
    }
    if (staticStale_)
    {
        cairo_t* s = cairo_create(staticLayer_);
        cairo_scale(s, sx, sx);
        paintAll(s);
        cairo_destroy(s);
        staticStale_ = false;
    }
    cairo_save(cr);
    cairo_scale(cr, 1.0 / sx, 1.0 / sx);
    cairo_set_source_surface(cr, staticLayer_, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    liveScope_->paintLive(cr);
    paintOverlays(cr);
    paintTooltip(cr);
}

} // namespace substrike::gui
