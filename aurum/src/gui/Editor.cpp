#include "Editor.h"

#include <X11/keysym.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "PresetBrowser.h"
#include "aurum.h"
#include "dsp/RoomModel.h"
#include "plugin/PresetSession.h"
#include "state/Settings.h"

namespace aurum::gui {

// ------------------------------------------------------------ History

void History::reset(const std::vector<double>& state)
{
    states_ = {state};
    index_ = 0;
}

void History::push(const std::vector<double>& state)
{
    if (index_ >= 0 && states_[static_cast<size_t>(index_)] == state)
        return;
    states_.resize(static_cast<size_t>(index_ + 1));
    states_.push_back(state);
    if (states_.size() > 200)
        states_.erase(states_.begin());
    index_ = static_cast<int>(states_.size()) - 1;
}

// ------------------------------------------------------------ RoomSlider

// Horizontal Room control: the room models are laid out as a ruler of names;
// click a name to jump to it, drag the handle to move continuously.
class RoomSlider : public Widget
{
public:
    RoomSlider(ParamContext& ctx, int idx) : ctx_(ctx), param_(idx) {}

    void paint(cairo_t* cr) override
    {
        const Rect t = track();
        const double v = std::clamp(ctx_.value(param_), 0.0, 1.0);
        const std::string current = dsp::roomAt(v).name;
        // Names
        setFont(cr, 10);
        for (int i = 0; i < dsp::kNumRooms; ++i)
        {
            const Rect r = labelRect(cr, i);
            const char* name = dsp::roomAnchors()[static_cast<size_t>(i)].name;
            const Color c = current == name ? theme::goldBright : (i == hoverLabel_ ? theme::text : theme::textFaint);
            drawText(cr, name, r, Align::Center, c);
        }
        // Ticks
        cairo_set_line_width(cr, 1.0);
        for (int i = 0; i < dsp::kNumRooms; ++i)
        {
            const float x = std::round(xFor(dsp::roomAnchorPosition(i))) + 0.5f;
            setColor(cr, theme::textFaint);
            cairo_move_to(cr, x, t.bottom() + 3);
            cairo_line_to(cr, x, t.bottom() + 7);
            cairo_stroke(cr);
        }
        // Track and handle
        fillRounded(cr, t, t.h * 0.5f, theme::plot);
        strokeRounded(cr, t, t.h * 0.5f, theme::outline);
        const float hx = xFor(v);
        fillRounded(cr, {t.x, t.y, hx - t.x, t.h}, t.h * 0.5f, theme::gold.withAlpha(dragging_ ? 0.95f : 0.8f));
        const Rect h{hx - 6, t.cy() - 10, 12, 20};
        fillRounded(cr, h, 3, theme::panelLight);
        strokeRounded(cr, h, 3, isHovered() || dragging_ ? theme::goldBright : theme::gold, 1.4f);
        setColor(cr, theme::gold);
        cairo_rectangle(cr, std::round(hx) - 0.5, h.y + 5, 1.0, h.h - 10);
        cairo_fill(cr);
    }

    bool mouseDown(const MouseEvent& e) override
    {
        if (e.button != 1)
            return false;
        if (e.mods & ModCtrl)
        {
            ctx_.change(param_, ctx_.defaultValue(param_));
            return true;
        }
        if (e.clicks == 2)
        {
            openParamEditor(ctx_, param_, {xFor(ctx_.value(param_)) - 50, track().y - 4, 100, 20}, root());
            return true;
        }
        if (hoverLabel_ >= 0)
        {
            ctx_.change(param_, dsp::roomAnchorPosition(hoverLabel_));
            return true;
        }
        const Rect t = track();
        dragging_ = true;
        ctx_.begin(param_);
        // Clicking the track jumps there; grabbing the handle keeps its offset.
        if (std::fabs(e.x - xFor(ctx_.value(param_))) > 8)
            ctx_.set(param_, std::clamp((e.x - t.x) / t.w, 0.0f, 1.0f));
        lastX_ = e.x;
        dragValue_ = ctx_.value(param_);
        repaint();
        return true;
    }

    void mouseDrag(const MouseEvent& e) override
    {
        if (!dragging_)
            return;
        const float dx = e.x - lastX_;
        lastX_ = e.x;
        dragValue_ = std::clamp(dragValue_ + dx / track().w * ((e.mods & ModShift) ? 0.15 : 1.0), 0.0, 1.0);
        ctx_.set(param_, dragValue_);
        repaint();
    }

    void mouseUp(const MouseEvent&) override
    {
        if (dragging_)
        {
            dragging_ = false;
            ctx_.end(param_);
            repaint();
        }
    }

    bool mouseWheel(const MouseEvent& e) override
    {
        const double step = (e.mods & ModShift) ? 0.002 : 0.01;
        ctx_.change(param_, std::clamp(ctx_.value(param_) + e.wheel * step, 0.0, 1.0));
        return true;
    }

    void mouseMove(const MouseEvent& e) override
    {
        int hit = -1;
        if (e.y < track().y - 4)
        {
            float best = 1e9f;
            for (int i = 0; i < dsp::kNumRooms; ++i)
            {
                const float d = std::fabs(xFor(dsp::roomAnchorPosition(i)) - e.x);
                if (d < best)
                {
                    best = d;
                    hit = i;
                }
            }
        }
        if (hit != hoverLabel_)
        {
            hoverLabel_ = hit;
            repaint();
        }
    }

    void mouseLeave() override
    {
        hoverLabel_ = -1;
        repaint();
    }

    std::string tooltip() const override
    {
        if (hoverLabel_ >= 0)
            return std::string("Jump to ") + dsp::roomAnchors()[static_cast<size_t>(hoverLabel_)].name;
        return "Room: " + ctx_.text(param_);
    }
    int boundParam() const override { return param_; }

private:
    Rect track() const { return {bounds_.x + 28, bounds_.y + 24, bounds_.w - 56, 8}; }
    float xFor(double v) const
    {
        const Rect t = track();
        return t.x + static_cast<float>(v) * t.w;
    }
    Rect labelRect(cairo_t*, int i) const
    {
        const float x = xFor(dsp::roomAnchorPosition(i));
        const float w = track().w / (dsp::kNumRooms - 1);
        return {x - w * 0.5f, bounds_.y, w, 16};
    }

    ParamContext& ctx_;
    int param_;
    bool dragging_ = false;
    float lastX_ = 0;
    double dragValue_ = 0;
    int hoverLabel_ = -1;
};

// Resulting decay time (Room x Length) with the room name.
class RoomReadout : public Widget
{
public:
    RoomReadout(ParamContext& ctx, int spaceIdx, int lengthIdx) : ctx_(ctx), space_(spaceIdx), length_(lengthIdx) {}

    void paint(cairo_t* cr) override
    {
        const dsp::Room room = dsp::roomAt(ctx_.value(space_));
        const double rate = conv::decayRate(ctx_.value(length_));
        const double t = room.t60 * rate;
        char buf[48];
        std::snprintf(buf, sizeof(buf), t < 10.0 ? "%.2f s" : "%.1f s", t);
        setFont(cr, 26, true);
        drawText(cr, buf, {bounds_.x, bounds_.y, bounds_.w, 32}, Align::Right, theme::goldBright);
        std::string detail = room.name;
        if (std::fabs(rate - 1.0) > 0.005)
        {
            std::snprintf(buf, sizeof(buf), "  \xc2\xb7  %.2f s x %.0f%%", room.t60, rate * 100.0);
            detail += buf;
        }
        setFont(cr, 11);
        drawText(cr, detail, {bounds_.x, bounds_.y + 34, bounds_.w, 16}, Align::Right, theme::textDim);
    }

private:
    ParamContext& ctx_;
    int space_, length_;
};

// ------------------------------------------------------------ FreezeButton

// Click toggles freeze; click and hold freezes only while held.
class FreezeButton : public Button
{
public:
    FreezeButton(ParamContext& ctx, int idx) : Button("Freeze", nullptr), ctx_(ctx), idx_(idx)
    {
        icon = [](cairo_t* cr, const Rect& r, const Color& c) { icons::snowflake(cr, {r.x + 6, r.y, 16, r.h}, c); };
        setTooltip("Freeze (click and hold for momentary)");
    }
    void paint(cairo_t* cr) override
    {
        active_ = ctx_.value(idx_) > 0.5;
        // Frame and icon from Button, label placed right of the icon.
        const std::string label = std::move(label_);
        label_.clear();
        Button::paint(cr);
        label_ = label;
        setFont(cr, fontSize);
        const bool hot = isHovered();
        drawText(cr, label_, {bounds_.x + 25, bounds_.y, bounds_.w - 29, bounds_.h}, Align::Left,
                 active_ ? theme::goldBright : (hot ? theme::text : theme::textDim));
    }
    bool mouseDown(const MouseEvent& e) override
    {
        if (e.button != 1)
            return false;
        wasOn_ = ctx_.value(idx_) > 0.5;
        pressTime_ = nowSeconds();
        ctx_.change(idx_, wasOn_ ? 0.0 : 1.0);
        pressed_ = true;
        return true;
    }
    void mouseUp(const MouseEvent&) override
    {
        pressed_ = false;
        if (!wasOn_ && nowSeconds() - pressTime_ > 0.45)
            ctx_.change(idx_, 0.0);
        repaint();
    }
    int boundParam() const override { return idx_; }

private:
    ParamContext& ctx_;
    int idx_;
    bool wasOn_ = false;
    double pressTime_ = 0.0;
};

// ------------------------------------------------------------ Panels

// Generic overlay panel holding child widgets.
class OverlayPanel : public Widget
{
public:
    void paint(cairo_t* cr) override
    {
        fillRounded(cr, {bounds_.x + 2, bounds_.y + 3, bounds_.w, bounds_.h}, 8, Color(0, 0, 0, 0.4f));
        fillRounded(cr, bounds_, 8, theme::panelLight);
        strokeRounded(cr, bounds_, 8, theme::outline);
        if (!title.empty())
        {
            setFont(cr, 11, true);
            drawText(cr, title, {bounds_.x, bounds_.y + 4, bounds_.w, 18}, Align::Center, theme::textDim);
        }
    }
    bool mouseDown(const MouseEvent&) override { return true; }
    bool keyDown(const KeyEvent& e) override
    {
        if (e.keysym == XK_Escape)
        {
            if (RootWidget* r = root())
                r->closeOverlay(this);
            return true;
        }
        return false;
    }
    std::string title;
};

// ------------------------------------------------------------ Editor

Editor::Editor(Controller& controller) : controller_(controller), ctx_(controller), window_(this)
{
    Settings& s = Settings::get();
    logicalW_ = static_cast<float>(s.getDouble("gui_width", kBaseW));
    logicalH_ = static_cast<float>(s.getDouble("gui_height", kBaseH));
    logicalW_ = std::max(logicalW_, kMinW);
    logicalH_ = std::max(logicalH_, kMinH);
    scale_ = X11Window::systemScale() * s.getDouble("gui_scaling", 1.0);

    buildTopBar();
    buildControls();

    ctx_.onGestureEnd = [this] {
        if (!applyingHistory_)
            history_.push(snapshot());
        updateButtons();
    };
    grabKeyboard = [this] { window_.grabKeyboard(); };
    resetHistory();
    abSlot_[0] = snapshot();
    setBounds({0, 0, logicalW_, logicalH_});
    lastTick_ = nowSeconds();
}

Editor::~Editor()
{
    detach();
    if (staticLayer_)
        cairo_surface_destroy(staticLayer_);
}

std::vector<double> Editor::snapshot() const
{
    std::vector<double> v(static_cast<size_t>(ctx_.table().count()));
    for (int i = 0; i < ctx_.table().count(); ++i)
        v[static_cast<size_t>(i)] = ctx_.value(i);
    return v;
}

void Editor::applySnapshot(const std::vector<double>& values)
{
    applyingHistory_ = true;
    std::vector<int> changed;
    for (int i = 0; i < ctx_.table().count() && i < static_cast<int>(values.size()); ++i)
        if (ctx_.value(i) != values[static_cast<size_t>(i)])
            changed.push_back(i);
    for (int i : changed)
        ctx_.begin(i);
    for (int i : changed)
        ctx_.set(i, values[static_cast<size_t>(i)]);
    for (int i : changed)
        ctx_.end(i);
    applyingHistory_ = false;
    repaint();
}

void Editor::resetHistory() { history_.reset(snapshot()); }

void Editor::pushHistory()
{
    history_.push(snapshot());
    updateButtons();
    markDirty();
}

void Editor::buildTopBar()
{
    menuButton_ = add<Button>("", [this] { showMainMenu(); });
    menuButton_->icon = icons::menu;
    menuButton_->setTooltip("Options");

    prevPreset_ = add<Button>("", [this] {
        if (session_)
            session_->step(-1);
    });
    prevPreset_->icon = icons::arrowLeft;
    prevPreset_->setTooltip("Previous preset");
    nextPreset_ = add<Button>("", [this] {
        if (session_)
            session_->step(1);
    });
    nextPreset_->icon = icons::arrowRight;
    nextPreset_->setTooltip("Next preset");
    presetButton_ = add<Button>("Init", [this] { openPresetBrowser(); });
    presetButton_->fontSize = 12;
    presetButton_->onRightClick = [this](const MouseEvent& e) {
        if (!session_)
            return;
        PresetSession* s = session_;
        const std::string path = s->path();
        std::string rel;
        for (const auto& p : PresetManager::get().presets())
            if (p.path == path)
                rel = p.relPath;
        std::vector<MenuItem> items;
        items.push_back({"Favorite", [rel] { PresetManager::get().setFavorite(rel, !PresetManager::get().isFavorite(rel)); },
                         !rel.empty() && PresetManager::get().isFavorite(rel), !rel.empty()});
        items.push_back({"Save", [this, s] {
                             if (s->saveCurrent())
                                 notify("Preset saved");
                         }, false, !path.empty()});
        items.push_back({"Save As...", [this] {
                             openPresetBrowser();
                         }});
        showMenu(this, std::move(items), e.x, e.y);
    };

    undoButton_ = add<Button>("", [this] { undo(); });
    undoButton_->icon = icons::undo;
    undoButton_->setTooltip("Undo");
    redoButton_ = add<Button>("", [this] { redo(); });
    redoButton_->icon = icons::redo;
    redoButton_->setTooltip("Redo");
    abButton_ = add<Button>("A", [this] { toggleAB(); });
    abButton_->setTooltip("Switch between state A and B");
    copyButton_ = add<Button>("Copy", [this] { copyAB(); });
    copyButton_->setTooltip("Copy the active state to the other slot");

    midiButton_ = add<Button>("MIDI", [this] {
        controller_.setMidiLearn(!controller_.midiLearnActive());
        updateButtons();
        markDirty();
    });
    midiButton_->fontSize = 10.5f;
    midiButton_->setTooltip("MIDI Learn (right-click for options)");
    midiButton_->onRightClick = [this](const MouseEvent& e) { showMidiMenu(e.x, e.y); };
    ioButton_ = add<Button>("I/O", [this] { openIoPanel(); });
    ioButton_->fontSize = 10.5f;
    ioButton_->setTooltip("Input and output level / pan");
    bypass_ = add<ParamToggle>(ctx_, ctx_.index(pid::Bypass), "");
    bypass_->icon = icons::power;
    updateButtons();
}

void Editor::buildControls()
{
    using namespace pid;
    auto idx = [this](uint32_t id) { return ctx_.index(id); };
    room_ = add<RoomSlider>(ctx_, idx(Space));
    roomReadout_ = add<RoomReadout>(ctx_, idx(Space), idx(DecayRate));
    decayRate_ = add<Knob>(ctx_, idx(DecayRate), "Length", KnobSize::Medium, true);
    predelay_ = add<Knob>(ctx_, idx(Predelay), "Pre-Delay", KnobSize::Medium);
    predelay_->dynamicLabel = [this] {
        return ctx_.value(ctx_.index(pid::PredelaySync)) > 0.5 ? std::string("Pre-Delay Offset") : std::string("Pre-Delay");
    };
    predelaySync_ = add<ParamSelector>(ctx_, idx(PredelaySync), "Sync ");
    predelaySync_->fontSize = 10;
    style_ = add<ParamSelector>(ctx_, idx(Style), "Algorithm: ");

    character_ = add<Knob>(ctx_, idx(Character), "Motion", KnobSize::Medium);
    brightness_ = add<Knob>(ctx_, idx(Brightness), "Air", KnobSize::Medium, true);
    distance_ = add<Knob>(ctx_, idx(Distance), "Depth", KnobSize::Medium);
    thickness_ = add<Knob>(ctx_, idx(Thickness), "Density", KnobSize::Medium, true);

    width_ = add<Knob>(ctx_, idx(Width), "Width", KnobSize::Medium);
    ducking_ = add<Knob>(ctx_, idx(Ducking), "Ducking", KnobSize::Medium);
    gateOn_ = add<ParamToggle>(ctx_, idx(GateEnabled), "Gate");
    gateOn_->fontSize = 10.5f;
    gateSync_ = add<ParamSelector>(ctx_, idx(GateSync), "Sync ");
    gateSync_->fontSize = 9.5f;
    gateHold_ = add<Knob>(ctx_, idx(GateHold), "Hold", KnobSize::Small);
    gateHold_->dynamicLabel = [this] {
        return ctx_.value(ctx_.index(pid::GateSync)) > 0.5 ? std::string("Offset") : std::string("Hold");
    };
    freeze_ = add<FreezeButton>(ctx_, idx(Freeze));
    mix_ = add<Knob>(ctx_, idx(Mix), "Mix", KnobSize::Medium);
    lockMix_ = add<Button>("", [this] {
        Settings& s = Settings::get();
        const bool on = s.getDouble("lock_mix", 0) < 0.5;
        s.set("lock_mix", on ? 1.0 : 0.0);
        s.save();
        lockMix_->setActive(on);
    });
    lockMix_->icon = [this](cairo_t* cr, const Rect& r, const Color& c) { icons::lock(cr, r, c, lockMix_->isActive()); };
    lockMix_->setTooltip("Lock Mix: keep the mix when loading presets");
    lockMix_->setActive(Settings::get().getDouble("lock_mix", 0) > 0.5);

    decayEq_ = add<EqPanel>(ctx_, false);
    toneEq_ = add<EqPanel>(ctx_, true);
}

namespace {
// Section frames (shared by layout and paint).
struct Sections
{
    Rect room, character, output, decay, tone;
};

Sections sectionsFor(float W, float H)
{
    Sections s;
    const float half = (W - 24 - 8) * 0.5f;
    s.room = {12, 44, W - 24, 156};
    s.character = {12, s.room.bottom() + 8, half, 122};
    s.output = {s.character.right() + 8, s.character.y, half, 122};
    const float eqY = s.character.bottom() + 8;
    s.decay = {12, eqY, half, H - eqY - 12};
    s.tone = {s.decay.right() + 8, eqY, half, H - eqY - 12};
    return s;
}
} // namespace

void Editor::layout()
{
    const float W = bounds_.w, H = bounds_.h;
    // Top bar: logo and presets on the left, history / A-B and window controls on the right.
    prevPreset_->setBounds({128, 8, 26, 26});
    presetButton_->setBounds({158, 8, 250, 26});
    nextPreset_->setBounds({412, 8, 26, 26});
    float x = W - 12;
    auto right = [&](Widget* w, float width, float gapAfter = 6) {
        x -= width;
        w->setBounds({x, 8, width, 26});
        x -= gapAfter;
    };
    right(menuButton_, 30);
    right(bypass_, 30);
    right(ioButton_, 40);
    right(midiButton_, 46, 18);
    right(copyButton_, 50);
    right(abButton_, 30, 14);
    right(redoButton_, 30);
    right(undoButton_, 30);

    const Sections S = sectionsFor(W, H);
    auto knob = [](Widget* w, float cx, float y, float d) { w->setBounds({cx - d * 0.5f, y, d, d + 30}); };

    // Room: slider across the top, time controls and readout below.
    const Rect R = S.room;
    room_->setBounds({R.x + 6, R.y + 30, R.w - 12, 44});
    const float ry = R.y + 82;
    knob(decayRate_, R.x + 48, ry, 42);
    knob(predelay_, R.x + 128, ry, 42);
    predelaySync_->setBounds({R.x + 172, ry + 11, 64, 20});
    style_->setBounds({R.x + 262, ry + 10, 176, 22});
    roomReadout_->setBounds({R.right() - 300, ry + 2, 284, 52});

    // Character: four evenly spaced knobs.
    const Rect C = S.character;
    auto col = [](const Rect& r, int i, int n) { return r.x + 10 + (r.w - 20) * (i + 0.5f) / n; };
    knob(character_, col(C, 0, 4), C.y + 30, 46);
    knob(brightness_, col(C, 1, 4), C.y + 30, 46);
    knob(distance_, col(C, 2, 4), C.y + 30, 46);
    knob(thickness_, col(C, 3, 4), C.y + 30, 46);

    // Output: width, ducking, gate group (wider column), freeze, mix.
    const Rect O = S.output;
    const float weights[5] = {1.0f, 1.0f, 1.35f, 1.0f, 1.0f};
    auto ocol = [&](int i) {
        float sum = 0, before = 0;
        for (int k = 0; k < 5; ++k)
        {
            sum += weights[k];
            if (k < i)
                before += weights[k];
        }
        return O.x + 10 + (O.w - 20) * (before + weights[i] * 0.5f) / sum;
    };
    knob(width_, ocol(0), O.y + 30, 46);
    knob(ducking_, ocol(1), O.y + 30, 46);
    const float gx = ocol(2);
    gateOn_->setBounds({gx - 30, O.y + 28, 60, 20});
    knob(gateHold_, gx - 22, O.y + 56, 30);
    gateSync_->setBounds({gx + 2, O.y + 62, 54, 18});
    const float fx = ocol(3);
    freeze_->setBounds({fx - 34, O.y + 50, 68, 24});
    const float mx = ocol(4);
    knob(mix_, mx, O.y + 30, 46);
    lockMix_->setBounds({O.right() - 34, O.y + 6, 24, 20});

    decayEq_->setBounds(S.decay);
    toneEq_->setBounds(S.tone);
}

void Editor::paint(cairo_t* cr)
{
    setColor(cr, theme::background);
    cairo_paint(cr);
    // Logo
    setFont(cr, 17, true);
    drawText(cr, "Aurum", {16, 8, 70, 26}, Align::Left, theme::gold);
    const float logoW = textWidth(cr, "Aurum");
    setFont(cr, 10);
    drawText(cr, "reverb", {16 + logoW + 5, 10, 50, 26}, Align::Left, theme::textFaint);

    const Sections S = sectionsFor(bounds_.w, bounds_.h);
    auto section = [&](const Rect& r, const char* title) {
        fillRounded(cr, r, 8, theme::panel);
        setFont(cr, 11, true);
        drawText(cr, title, {r.x + 12, r.y + 6, 200, 18}, Align::Left, theme::gold);
    };
    section(S.room, "ROOM");
    section(S.character, "CHARACTER");
    section(S.output, "OUTPUT");
    // Divider between the room ruler and the time controls.
    setColor(cr, theme::outline);
    cairo_rectangle(cr, S.room.x + 12, S.room.y + 80, S.room.w - 24, 1);
    cairo_fill(cr);
}

void Editor::paintTooltip(cairo_t* cr)
{
    if (controller_.midiLearnActive())
    {
        paintMidiLearn(cr);
        return;
    }
    if (!notice_.empty())
    {
        setFont(cr, 12);
        const float tw = textWidth(cr, notice_) + 28;
        const Rect r{bounds_.w * 0.5f - tw * 0.5f, 52, tw, 28};
        fillRounded(cr, r, 6, theme::panelLight);
        strokeRounded(cr, r, 6, theme::gold.withAlpha(0.6f));
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
    float x = std::clamp(b.cx() - tw * 0.5f, 4.0f, bounds_.w - tw - 4);
    float y = b.y - 26;
    if (y < 2)
        y = b.bottom() + 4;
    const Rect r{x, y, tw, 22};
    fillRounded(cr, {r.x + 1, r.y + 2, r.w, r.h}, 5, Color(0, 0, 0, 0.4f));
    fillRounded(cr, r, 5, theme::panelLight);
    strokeRounded(cr, r, 5, theme::gold.withAlpha(0.45f));
    drawText(cr, tip, r, Align::Center, theme::text);
}

void Editor::updateButtons()
{
    undoButton_->setEnabled(history_.canUndo());
    redoButton_->setEnabled(history_.canRedo());
    abButton_->setLabel(abActive_ == 0 ? "A" : "B");
    const std::vector<double> cur = snapshot();
    const std::vector<double>& other = abSlot_[1 - abActive_];
    copyButton_->setEnabled(other.empty() || other != cur);
    midiButton_->setActive(controller_.midiLearnActive());
}

void Editor::undo()
{
    if (!history_.canUndo())
        return;
    applySnapshot(history_.undo());
    updateButtons();
}

void Editor::redo()
{
    if (!history_.canRedo())
        return;
    applySnapshot(history_.redo());
    updateButtons();
}

void Editor::toggleAB()
{
    abSlot_[abActive_] = snapshot();
    abActive_ = 1 - abActive_;
    if (abSlot_[abActive_].empty())
        abSlot_[abActive_] = abSlot_[1 - abActive_];
    applySnapshot(abSlot_[abActive_]);
    history_.push(snapshot());
    updateButtons();
}

void Editor::copyAB()
{
    abSlot_[1 - abActive_] = snapshot();
    updateButtons();
}

void Editor::openIoPanel()
{
    using namespace pid;
    auto panel = std::make_unique<OverlayPanel>();
    panel->title = "Input / Output";
    const Rect b = ioButton_->bounds();
    const float w = 250, h = 112;
    panel->setBounds({std::min(b.right() - w, bounds_.w - w - 8), b.bottom() + 6, w, h});
    const Rect pr = panel->bounds();
    const int ids[4] = {ctx_.index(InputLevel), ctx_.index(InputPan), ctx_.index(OutputLevel), ctx_.index(OutputPan)};
    const char* names[4] = {"In Level", "In Pan", "Out Level", "Out Pan"};
    for (int i = 0; i < 4; ++i)
    {
        Knob* k = panel->add<Knob>(ctx_, ids[i], names[i], KnobSize::Small, true);
        k->setBounds({pr.x + 14 + i * 58, pr.y + 28, 42, 60});
    }
    pushOverlay(std::move(panel));
}

MenuItem Editor::sizeMenu()
{
    struct Preset
    {
        const char* name;
        float w, h;
    };
    const Preset sizes[] = {{"Medium", kBaseW, kBaseH}, {"Large", 1200, 792}, {"Extra Large", 1440, 950}};
    MenuItem m{"Window Size"};
    for (const auto& s : sizes)
        m.submenu.push_back({s.name, [this, s] { setLogicalSize(s.w, s.h); },
                             std::fabs(logicalW_ - s.w) < 1 && std::fabs(logicalH_ - s.h) < 1});
    return m;
}

MenuItem Editor::scalingMenu()
{
    MenuItem scaling{"Scaling"};
    const double cur = Settings::get().getDouble("gui_scaling", 1.0);
    for (double f : {0.75, 0.85, 1.0, 1.25, 1.5, 2.0})
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f%%", f * 100);
        scaling.submenu.push_back({buf, [this, f] {
                                       Settings::get().set("gui_scaling", f);
                                       Settings::get().save();
                                       setScale(X11Window::systemScale() * f);
                                       if (requestResize)
                                           requestResize(static_cast<int>(logicalW_ * scale_),
                                                         static_cast<int>(logicalH_ * scale_));
                                   }, std::fabs(cur - f) < 1e-6});
    }
    return scaling;
}

void Editor::showMidiMenu(float x, float y)
{
    std::vector<MenuItem> items;
    const bool enabled = controller_.midiEnabled();
    items.push_back({"Enable MIDI", [this, enabled] { controller_.setMidiEnabled(!enabled); }, enabled});
    MenuItem clear{"Clear"};
    for (const auto& [cc, idx] : controller_.midiMappings())
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CC %d: %s", cc, ctx_.name(idx).c_str());
        const int c = cc;
        clear.submenu.push_back({buf, [this, c] { controller_.clearMidiMapping(c); }});
    }
    if (!clear.submenu.empty())
        clear.submenu.push_back({"", nullptr, false, true, true});
    clear.submenu.push_back({"Clear All", [this] { controller_.clearAllMidiMappings(); }});
    items.push_back(clear);
    items.push_back({"Revert", [this] { controller_.revertMidiMappings(); }});
    items.push_back({"Save", [this] { controller_.saveMidiMappings(); }});
    showMenu(this, std::move(items), x, y + 10);
}

void Editor::paintMidiLearn(cairo_t* cr)
{
    setColor(cr, Color(0, 0, 0, 0.55f));
    cairo_paint(cr);
    const int target = controller_.learnTarget();
    std::function<void(Widget*)> visit = [&](Widget* w) {
        if (!w->isVisible())
            return;
        const int p = w->boundParam();
        if (p >= 0)
        {
            const Rect b = w->bounds();
            strokeRounded(cr, b.reduced(-3), 5, p == target ? theme::red : theme::gold.withAlpha(0.7f), p == target ? 2.0f : 1.0f);
            const int cc = controller_.ccForParam(p);
            const std::string label = cc >= 0 ? "CC " + std::to_string(cc) : "-";
            setFont(cr, 10);
            const float tw = textWidth(cr, label) + 12;
            const Rect bubble{b.right() - tw + 4, b.y - 10, tw, 16};
            fillRounded(cr, bubble, 8, cc >= 0 ? theme::gold : theme::panelLight);
            drawText(cr, label, bubble, Align::Center, cc >= 0 ? theme::plot : theme::textDim);
        }
        for (const auto& c : w->children())
            visit(c.get());
    };
    visit(this);
    // Keep the MIDI button readable.
    midiButton_->paintAll(cr);
    setFont(cr, 12);
    drawText(cr, "MIDI Learn: click a control, then move a knob on your MIDI controller. Click MIDI Learn to finish.",
             {0, bounds_.h - 64, bounds_.w, 20}, Align::Center, theme::text);
}

void Editor::onMouseDown(const MouseEvent& e)
{
    if (controller_.midiLearnActive() && !hasOverlay())
    {
        if (midiButton_->bounds().contains(e.x, e.y))
        {
            handleMouseDown(e);
            return;
        }
        for (Widget* w = hitTest(e.x, e.y); w; w = w->parent())
            if (w->boundParam() >= 0)
            {
                controller_.setLearnTarget(w->boundParam());
                break;
            }
        markDirty();
        return;
    }
    handleMouseDown(e);
}

void Editor::showMainMenu()
{
    std::vector<MenuItem> items;
    items.push_back(sizeMenu());
    items.push_back(scalingMenu());
    items.push_back({"", nullptr, false, true, true});
    items.push_back({std::string(kPluginName) + " " + kPluginVersion, nullptr, false, false});
    showMenu(this, std::move(items), menuButton_->bounds().right() - 200, menuButton_->bounds().bottom() + 4);
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

bool Editor::keyDown(const KeyEvent& e)
{
    if (decayEq_->keyDown(e) || toneEq_->keyDown(e))
        return true;
    if ((e.mods & ModCtrl) && (e.keysym == XK_z || e.keysym == XK_Z))
    {
        if (e.mods & ModShift)
            redo();
        else
            undo();
        return true;
    }
    if (e.keysym == XK_Escape)
    {
        closeAllOverlays();
        return true;
    }
    return false;
}

void Editor::onFilesDropped(const std::string& uris)
{
    // First file:// entry of a text/uri-list.
    size_t start = 0;
    while (start < uris.size())
    {
        size_t end = uris.find_first_of("\r\n", start);
        if (end == std::string::npos)
            end = uris.size();
        std::string line = uris.substr(start, end - start);
        start = end + 1;
        if (line.rfind("file://", 0) != 0)
            continue;
        std::string path;
        for (size_t i = 7; i < line.size(); ++i)
        {
            if (line[i] == '%' && i + 2 < line.size())
            {
                path += static_cast<char>(std::stoi(line.substr(i + 1, 2), nullptr, 16));
                i += 2;
            }
            else
                path += line[i];
        }
        if (onImportIr)
            onImportIr(path);
        return;
    }
}

// ------------------------------------------------------------ window

bool Editor::attach(unsigned long parentWindow)
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

void Editor::onTimer()
{
    window_.processEvents();
    const double t = nowSeconds();
    const double dt = std::clamp(t - lastTick_, 0.0, 0.2);
    lastTick_ = t;
    (void)dt;
    // Synced predelay / gate show their offset knob instead of the time.
    {
        using namespace pid;
        const bool pdSync = ctx_.value(ctx_.index(PredelaySync)) > 0.5;
        const int pdIdx = ctx_.index(pdSync ? PredelayOffset : Predelay);
        if (predelay_->paramIndex() != pdIdx)
            predelay_->setParamIndex(pdIdx);
        const bool gSync = ctx_.value(ctx_.index(GateSync)) > 0.5;
        const int gIdx = ctx_.index(gSync ? GateOffset : GateHold);
        if (gateHold_->paramIndex() != gIdx)
            gateHold_->setParamIndex(gIdx);
    }
    decayEq_->sync();
    toneEq_->sync();
    // Repaint when parameters change from outside (automation, host).
    std::vector<double> now = snapshot();
    if (now != lastSeen_)
    {
        lastSeen_ = std::move(now);
        updateButtons();
        markDirty();
    }
    // File dialog running in the background.
    if (dialog_)
    {
        std::string result;
        if (dialog_->poll(result))
        {
            auto done = std::move(dialogDone_);
            dialog_.reset();
            if (done && !result.empty())
                done(result);
        }
    }
    if (!notice_.empty() && t > noticeUntil_)
    {
        notice_.clear();
        markDirty();
    }
    // Preset name (dimmed marker when modified).
    if (session_)
    {
        const std::string label = session_->name() + (session_->modified() ? " *" : "");
        if (label != presetLabel_)
        {
            presetLabel_ = label;
            presetButton_->setLabel(label);
        }
    }
    if (takeDirty())
        window_.render();
}

void Editor::onPaint(cairo_t* cr, int, int) { renderTo(cr); }

void Editor::runFileDialog(FileDialog::Mode mode, const std::string& title, const std::string& filter,
                           std::function<void(const std::string&)> done)
{
    if (dialog_ && dialog_->running())
        return;
    dialog_ = std::make_unique<FileDialog>();
    std::string start = Settings::get().getString("last_dir", "");
    if (!dialog_->start(mode, title, filter, start))
    {
        dialog_.reset();
        notify("No file dialog available (install zenity or kdialog)");
        return;
    }
    dialogDone_ = [done = std::move(done)](const std::string& path) {
        Settings::get().set("last_dir", std::filesystem::path(path).parent_path().string());
        Settings::get().save();
        done(path);
    };
}

void Editor::openPresetBrowser()
{
    if (!session_)
        return;
    closeAllOverlays();
    auto b = std::make_unique<PresetBrowser>(*this, *session_);
    Widget* raw = b.get();
    pushOverlay(std::move(b));
    setKeyFocus(raw);
    window_.grabKeyboard();
}

void Editor::notify(const std::string& message)
{
    notice_ = message;
    noticeUntil_ = nowSeconds() + 3.0;
    markDirty();
}

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
    paintOverlays(cr);
    paintTooltip(cr);
}

} // namespace aurum::gui
