#include "Widgets.h"

#include <X11/keysym.h>

#include <algorithm>
#include <cmath>

namespace aurum::gui {

// ------------------------------------------------------------ ParamContext

void ParamContext::begin(int idx)
{
    ++openGestures_;
    ctl_.beginEdit(idx);
}

void ParamContext::set(int idx, double v)
{
    v = std::clamp(v, table_.minValue(idx), table_.maxValue(idx));
    if (v == ctl_.paramValue(idx))
        return;
    ctl_.performEdit(idx, v);
    if (onChanged)
        onChanged(idx);
}

void ParamContext::end(int idx)
{
    ctl_.endEdit(idx);
    if (--openGestures_ <= 0)
    {
        openGestures_ = 0;
        if (onGestureEnd)
            onGestureEnd();
    }
}

void ParamContext::change(int idx, double v)
{
    begin(idx);
    set(idx, v);
    end(idx);
}

bool ParamContext::setFromText(int idx, const std::string& t)
{
    if (auto v = table_.fromText(idx, t))
    {
        change(idx, *v);
        return true;
    }
    return false;
}

// ------------------------------------------------------------ TextEditor

TextEditor::TextEditor(std::string initial, std::function<void(const std::string&)> onCommit,
                       std::function<void()> onTab)
    : text_(std::move(initial)), onCommit_(std::move(onCommit)), onTab_(std::move(onTab))
{
}

void TextEditor::paint(cairo_t* cr)
{
    fillRounded(cr, bounds_, 4, theme::panelLight);
    strokeRounded(cr, bounds_, 4, theme::gold, 1.0f);
    setFont(cr, 12);
    const Rect inner = bounds_.reduced(8, 0);
    if (selectAll_ && !text_.empty())
    {
        const float w = textWidth(cr, text_);
        const float x = inner.cx() - w * 0.5f;
        setColor(cr, theme::gold.withAlpha(0.35f));
        cairo_rectangle(cr, x - 2, bounds_.y + 5, w + 4, bounds_.h - 10);
        cairo_fill(cr);
    }
    drawText(cr, text_, inner, Align::Center, theme::text);
    // Caret
    const float w = textWidth(cr, text_);
    const float cx = inner.cx() + w * 0.5f + 1;
    if (std::fmod(nowSeconds(), 1.0) < 0.6)
    {
        setColor(cr, theme::goldBright);
        cairo_rectangle(cr, cx, bounds_.y + 6, 1.2, bounds_.h - 12);
        cairo_fill(cr);
    }
    repaint(); // caret blink
}

void TextEditor::close()
{
    if (RootWidget* r = root())
        r->closeOverlay(this);
}

bool TextEditor::keyDown(const KeyEvent& e)
{
    switch (e.keysym)
    {
    case XK_Return:
    case XK_KP_Enter:
    {
        auto cb = onCommit_;
        const std::string t = text_;
        close();
        if (cb)
            cb(t);
        return true;
    }
    case XK_Escape: close(); return true;
    case XK_Tab:
    {
        auto commit = onCommit_;
        auto tab = onTab_;
        const std::string t = text_;
        close();
        if (commit)
            commit(t);
        if (tab)
            tab();
        return true;
    }
    case XK_BackSpace:
        if (selectAll_)
            text_.clear();
        else if (!text_.empty())
        {
            // Remove one UTF-8 code point.
            size_t i = text_.size() - 1;
            while (i > 0 && (static_cast<unsigned char>(text_[i]) & 0xC0) == 0x80)
                --i;
            text_.erase(i);
        }
        selectAll_ = false;
        repaint();
        return true;
    default:
        if (!e.text.empty() && static_cast<unsigned char>(e.text[0]) >= 0x20)
        {
            if (selectAll_)
                text_.clear();
            selectAll_ = false;
            text_ += e.text;
            repaint();
            return true;
        }
        return true; // swallow other keys while editing
    }
}

void openParamEditor(ParamContext& ctx, int paramIndex, const Rect& anchor, RootWidget* root,
                     std::function<void()> onTab)
{
    if (!root)
        return;
    auto ed = std::make_unique<TextEditor>(
        ctx.text(paramIndex), [&ctx, paramIndex](const std::string& t) { ctx.setFromText(paramIndex, t); },
        std::move(onTab));
    const float w = std::max(90.0f, anchor.w + 20.0f);
    Rect r{anchor.cx() - w * 0.5f, anchor.cy() - 12.0f, w, 24.0f};
    r.x = std::clamp(r.x, 2.0f, root->bounds().w - r.w - 2.0f);
    ed->setBounds(r);
    Widget* raw = ed.get();
    root->pushOverlay(std::move(ed));
    root->setKeyFocus(raw);
    if (root->grabKeyboard)
        root->grabKeyboard();
}

// ------------------------------------------------------------ PopupMenu

namespace {
constexpr float kItemH = 22.0f;
constexpr float kSepH = 9.0f;
constexpr float kMenuPad = 4.0f;
} // namespace

PopupMenu::PopupMenu(std::vector<MenuItem> items, float x, float y, float rootW, float rootH, PopupMenu* parentMenu)
    : items_(std::move(items)), rootW_(rootW), rootH_(rootH), parentMenu_(parentMenu)
{
    // Measure with a scratch surface.
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(s);
    setFont(cr, 12);
    float w = 120.0f, h = kMenuPad;
    for (const auto& it : items_)
    {
        itemY_.push_back(h);
        if (it.separator)
            h += kSepH;
        else
        {
            w = std::max(w, textWidth(cr, it.label) + 48.0f);
            h += kItemH;
        }
    }
    h += kMenuPad;
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    if (x + w > rootW_ - 2)
        x = std::max(2.0f, rootW_ - w - 2);
    if (y + h > rootH_ - 2)
        y = std::max(2.0f, rootH_ - h - 2);
    bounds_ = {x, y, w, h};
    openedAt_ = nowSeconds();
}

int PopupMenu::itemAt(float y) const
{
    for (size_t i = 0; i < items_.size(); ++i)
    {
        const float top = bounds_.y + itemY_[i];
        const float hgt = items_[i].separator ? kSepH : kItemH;
        if (y >= top && y < top + hgt)
            return items_[i].separator ? -1 : static_cast<int>(i);
    }
    return -1;
}

void PopupMenu::paint(cairo_t* cr)
{
    // Shadow
    fillRounded(cr, {bounds_.x + 2, bounds_.y + 3, bounds_.w, bounds_.h}, 6, Color(0, 0, 0, 0.35f));
    fillRounded(cr, bounds_, 6, theme::panelLight);
    strokeRounded(cr, bounds_, 6, theme::outline);
    setFont(cr, 12);
    for (size_t i = 0; i < items_.size(); ++i)
    {
        const MenuItem& it = items_[i];
        const float top = bounds_.y + itemY_[i];
        if (it.separator)
        {
            setColor(cr, theme::outline);
            cairo_rectangle(cr, bounds_.x + 8, top + kSepH * 0.5f, bounds_.w - 16, 1);
            cairo_fill(cr);
            continue;
        }
        const Rect row{bounds_.x + 3, top, bounds_.w - 6, kItemH};
        if (static_cast<int>(i) == hover_ && it.enabled)
            fillRounded(cr, row, 4, theme::gold.withAlpha(0.22f));
        const Color c = it.enabled ? theme::text : theme::textFaint;
        if (it.checked)
        {
            setColor(cr, theme::gold);
            cairo_arc(cr, row.x + 12, row.cy(), 3.2, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        drawText(cr, it.label, {row.x + 24, row.y, row.w - 40, row.h}, Align::Left, c);
        if (!it.submenu.empty())
        {
            setColor(cr, theme::textDim);
            cairo_move_to(cr, row.right() - 12, row.cy() - 4);
            cairo_line_to(cr, row.right() - 8, row.cy());
            cairo_line_to(cr, row.right() - 12, row.cy() + 4);
            cairo_set_line_width(cr, 1.3);
            cairo_stroke(cr);
        }
    }
}

void PopupMenu::openSubmenu(int i)
{
    RootWidget* r = root();
    if (!r)
        return;
    if (childMenu_)
    {
        r->closeOverlay(childMenu_);
        childMenu_ = nullptr;
        childIndex_ = -1;
    }
    if (i < 0 || items_[static_cast<size_t>(i)].submenu.empty())
        return;
    auto sub = std::make_unique<PopupMenu>(items_[static_cast<size_t>(i)].submenu, bounds_.right() - 4,
                                           bounds_.y + itemY_[static_cast<size_t>(i)] - kMenuPad, rootW_, rootH_, this);
    childMenu_ = sub.get();
    childIndex_ = i;
    r->pushOverlay(std::move(sub));
}

void PopupMenu::mouseMove(const MouseEvent& e)
{
    const int h = itemAt(e.y);
    if (h != hover_)
    {
        hover_ = h;
        if (h >= 0 && h != childIndex_)
            openSubmenu(h);
        repaint();
    }
}

void PopupMenu::mouseLeave()
{
    if (!childMenu_)
        hover_ = -1;
    repaint();
}

bool PopupMenu::mouseDown(const MouseEvent& e)
{
    const int i = itemAt(e.y);
    if (i >= 0 && !items_[static_cast<size_t>(i)].submenu.empty())
        openSubmenu(i);
    return true;
}

void PopupMenu::mouseUp(const MouseEvent& e)
{
    // Ignore the release of the click that opened the menu.
    if (nowSeconds() - openedAt_ < 0.25)
        return;
    const int i = itemAt(e.y);
    if (i >= 0 && bounds_.contains(e.x, e.y))
        activate(i);
}

void PopupMenu::activate(int i)
{
    const MenuItem& it = items_[static_cast<size_t>(i)];
    if (!it.enabled || !it.submenu.empty())
        return;
    auto action = it.action;
    closeChain();
    if (action)
        action();
}

void PopupMenu::closeChain()
{
    // Close this menu tree only (other overlays, e.g. a browser, stay open).
    PopupMenu* top = this;
    while (top->parentMenu_)
        top = top->parentMenu_;
    RootWidget* r = root();
    if (!r)
        return;
    std::vector<PopupMenu*> chain;
    for (PopupMenu* m = top; m; m = m->childMenu_)
        chain.push_back(m);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        r->closeOverlay(*it);
}

bool PopupMenu::keyDown(const KeyEvent& e)
{
    const int n = static_cast<int>(items_.size());
    auto step = [&](int dir) {
        int i = hover_;
        for (int k = 0; k < n; ++k)
        {
            i = (i + dir + n) % n;
            if (!items_[static_cast<size_t>(i)].separator && items_[static_cast<size_t>(i)].enabled)
                break;
        }
        hover_ = i;
        repaint();
    };
    switch (e.keysym)
    {
    case XK_Down: step(1); return true;
    case XK_Up: step(-1); return true;
    case XK_Return:
    case XK_KP_Enter:
        if (hover_ >= 0)
            activate(hover_);
        return true;
    case XK_Escape: closeChain(); return true;
    default: return true;
    }
}

void showMenu(RootWidget* root, std::vector<MenuItem> items, float x, float y)
{
    if (!root || items.empty())
        return;
    root->closeAllOverlays();
    root->pushOverlay(std::make_unique<PopupMenu>(std::move(items), x, y, root->bounds().w, root->bounds().h));
}

// ------------------------------------------------------------ Knob

Knob::Knob(ParamContext& ctx, int paramIndex, std::string label, KnobSize size, bool bipolar)
    : ctx_(ctx), param_(paramIndex), label_(std::move(label)), size_(size), bipolar_(bipolar)
{
}

Rect Knob::knobRect() const
{
    const float labelH = size_ == KnobSize::Large ? 0.0f : 30.0f;
    const float d = std::min(bounds_.w, bounds_.h - labelH);
    return {bounds_.cx() - d * 0.5f, bounds_.y, d, d};
}

void Knob::paint(cairo_t* cr)
{
    const Rect k = knobRect();
    const float cx = k.cx(), cy = k.cy();
    const float radius = k.w * 0.5f;
    const double v = std::clamp(ctx_.value(param_), 0.0, 1.0);
    const bool hot = isHovered() || dragging_;
    constexpr double a0 = M_PI * 0.75, a1 = M_PI * 2.25;
    const double av = a0 + (a1 - a0) * v;

    // Segmented ring: short radial ticks, lit between the origin and the value.
    const int segments = size_ == KnobSize::Small ? 15 : 21;
    const float rOut = radius - 0.5f, rIn = radius - (size_ == KnobSize::Small ? 4.5f : 6.0f);
    const double origin = bipolar_ ? 0.5 : 0.0;
    cairo_set_line_width(cr, size_ == KnobSize::Small ? 1.8 : 2.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < segments; ++i)
    {
        const double t = static_cast<double>(i) / (segments - 1);
        const bool lit = (t >= std::min(origin, v) - 1e-6 && t <= std::max(origin, v) + 1e-6) &&
                         std::fabs(v - origin) > 0.5 / (segments - 1);
        const double a = a0 + (a1 - a0) * t;
        setColor(cr, lit ? (hot ? theme::goldBright : theme::gold) : theme::outline);
        cairo_move_to(cr, cx + std::cos(a) * rIn, cy + std::sin(a) * rIn);
        cairo_line_to(cr, cx + std::cos(a) * rOut, cy + std::sin(a) * rOut);
        cairo_stroke(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);

    // Flat body with a dot indicator.
    const float bodyR = rIn - (size_ == KnobSize::Small ? 2.5f : 3.5f);
    setColor(cr, hot ? theme::panelLight.mix(Color(1, 1, 1, 1), 0.05f) : theme::panelLight);
    cairo_arc(cr, cx, cy, bodyR, 0, 2 * M_PI);
    cairo_fill(cr);
    setColor(cr, theme::outline);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, cx, cy, bodyR - 0.5, 0, 2 * M_PI);
    cairo_stroke(cr);
    const float dotR = bodyR * 0.68f;
    setColor(cr, hot ? theme::goldBright : theme::text);
    cairo_arc(cr, cx + std::cos(av) * dotR, cy + std::sin(av) * dotR, size_ == KnobSize::Small ? 2.0 : 2.6, 0, 2 * M_PI);
    cairo_fill(cr);

    if (drawCentre)
        drawCentre(cr, k, bodyR);

    if (size_ != KnobSize::Large)
    {
        const std::string label = dynamicLabel ? dynamicLabel() : label_;
        setFont(cr, 10.5f);
        drawText(cr, label, {bounds_.x - 12, k.bottom() + 2, bounds_.w + 24, 14}, Align::Center,
                 hot ? theme::text : theme::textDim);
        setFont(cr, 10.0f);
        drawText(cr, ctx_.text(param_), {bounds_.x - 12, k.bottom() + 15, bounds_.w + 24, 13}, Align::Center,
                 hot ? theme::goldBright : theme::textFaint);
    }
}

bool Knob::mouseDown(const MouseEvent& e)
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
        openParamEditor(ctx_, param_, knobRect(), root());
        return true;
    }
    dragging_ = true;
    lastY_ = e.y;
    dragValue_ = ctx_.value(param_);
    lastMoveTime_ = nowSeconds();
    ctx_.begin(param_);
    return true;
}

void Knob::mouseDrag(const MouseEvent& e)
{
    if (!dragging_)
        return;
    const float dy = lastY_ - e.y;
    lastY_ = e.y;
    const double t = nowSeconds();
    const double dt = std::max(t - lastMoveTime_, 1e-3);
    lastMoveTime_ = t;
    // Speed-sensitive: slow movements give finer control.
    const double speed = std::fabs(dy) / dt; // px per second
    double sens = 1.0 / 220.0 * std::clamp(0.35 + speed / 900.0, 0.35, 1.6);
    if (e.mods & ModShift)
        sens *= 0.12;
    dragValue_ = std::clamp(dragValue_ + dy * sens, 0.0, 1.0);
    ctx_.set(param_, dragValue_);
    repaint();
}

void Knob::mouseUp(const MouseEvent&)
{
    if (dragging_)
    {
        dragging_ = false;
        ctx_.end(param_);
    }
}

bool Knob::mouseWheel(const MouseEvent& e)
{
    const double step = (e.mods & ModShift) ? 0.004 : 0.025;
    ctx_.change(param_, std::clamp(ctx_.value(param_) + e.wheel * step, 0.0, 1.0));
    return true;
}

std::string Knob::tooltip() const { return ctx_.name(param_) + ": " + ctx_.text(param_); }

// ------------------------------------------------------------ Buttons

Button::Button(std::string label, std::function<void()> onClick) : label_(std::move(label)), onClick_(std::move(onClick))
{
}

void Button::paint(cairo_t* cr)
{
    const bool hot = isHovered() && enabled_;
    if (frame)
    {
        Color bg = active_ ? theme::gold.withAlpha(0.22f) : theme::panelLight;
        if (pressed_)
            bg = bg.mix(Color(1, 1, 1, 1), 0.08f);
        fillRounded(cr, bounds_, 4, bg);
        strokeRounded(cr, bounds_, 4, active_ ? theme::gold.withAlpha(0.7f) : (hot ? theme::textFaint : theme::outline));
    }
    const Color c = !enabled_ ? theme::textFaint : (active_ ? theme::goldBright : (hot ? theme::text : theme::textDim));
    if (icon)
        icon(cr, bounds_, c);
    if (!label_.empty())
    {
        setFont(cr, fontSize);
        drawText(cr, label_, bounds_, Align::Center, c);
    }
}

bool Button::mouseDown(const MouseEvent& e)
{
    if (!enabled_)
        return true;
    if (e.button == 3 && onRightClick)
    {
        onRightClick(e);
        return true;
    }
    if (e.button != 1)
        return false;
    pressed_ = true;
    if (onClick_)
        onClick_();
    repaint();
    return true;
}

void Button::mouseUp(const MouseEvent&)
{
    pressed_ = false;
    if (onRelease)
        onRelease();
    repaint();
}

ParamToggle::ParamToggle(ParamContext& ctx, int paramIndex, std::string label)
    : Button(std::move(label), nullptr), ctx_(ctx), param_(paramIndex)
{
    onClick_ = [this] { ctx_.change(param_, ctx_.value(param_) > 0.5 ? 0.0 : 1.0); };
}

void ParamToggle::paint(cairo_t* cr)
{
    active_ = ctx_.value(param_) > 0.5;
    Button::paint(cr);
}

std::string ParamToggle::tooltip() const { return ctx_.name(param_) + ": " + ctx_.text(param_); }

ParamField::ParamField(ParamContext& ctx, int paramIndex, std::string prefix)
    : ctx_(ctx), param_(paramIndex), prefix_(std::move(prefix))
{
}

void ParamField::paint(cairo_t* cr)
{
    const bool hot = isHovered() || dragging_;
    fillRounded(cr, bounds_, 4, theme::plot);
    strokeRounded(cr, bounds_, 4, hot ? accent.withAlpha(0.7f) : theme::outline);
    setFont(cr, 10.5f);
    drawText(cr, prefix_ + ctx_.text(param_), bounds_.reduced(4, 0), Align::Center, hot ? theme::text : accent);
}

bool ParamField::mouseDown(const MouseEvent& e)
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
        openParamEditor(ctx_, param_, bounds_, root());
        return true;
    }
    dragging_ = true;
    lastY_ = e.y;
    dragValue_ = ctx_.value(param_);
    ctx_.begin(param_);
    return true;
}

void ParamField::mouseDrag(const MouseEvent& e)
{
    if (!dragging_)
        return;
    const float dy = lastY_ - e.y;
    lastY_ = e.y;
    dragValue_ = std::clamp(dragValue_ + dy * ((e.mods & ModShift) ? 0.0006 : 0.004), 0.0, 1.0);
    ctx_.set(param_, dragValue_);
    repaint();
}

void ParamField::mouseUp(const MouseEvent&)
{
    if (dragging_)
    {
        dragging_ = false;
        ctx_.end(param_);
    }
}

bool ParamField::mouseWheel(const MouseEvent& e)
{
    const double step = (e.mods & ModShift) ? 0.002 : 0.012;
    ctx_.change(param_, std::clamp(ctx_.value(param_) + e.wheel * step, 0.0, 1.0));
    return true;
}

std::string ParamField::tooltip() const { return ctx_.name(param_) + ": " + ctx_.text(param_); }

ParamSelector::ParamSelector(ParamContext& ctx, int paramIndex, std::string prefix)
    : ctx_(ctx), param_(paramIndex), prefix_(std::move(prefix))
{
}

void ParamSelector::paint(cairo_t* cr)
{
    const bool hot = isHovered();
    fillRounded(cr, bounds_, 4, hot ? theme::panelLight.mix(Color(1, 1, 1, 1), 0.04f) : theme::panelLight);
    strokeRounded(cr, bounds_, 4, hot ? theme::textFaint : theme::outline);
    setFont(cr, fontSize);
    drawText(cr, prefix_ + ctx_.text(param_), bounds_.reduced(6, 0), Align::Center, hot ? theme::text : theme::textDim);
}

bool ParamSelector::mouseDown(const MouseEvent& e)
{
    if (e.button != 1 && e.button != 3)
        return false;
    const ParamDef& d = ctx_.table().def(param_);
    std::vector<MenuItem> items;
    const int cur = static_cast<int>(std::lround(ctx_.value(param_)));
    for (int i = 0; i < d.steps; ++i)
    {
        MenuItem it;
        it.label = d.labels.empty() ? std::to_string(i) : d.labels[static_cast<size_t>(i)];
        it.checked = i == cur;
        it.action = [this, i] { ctx_.change(param_, i); };
        items.push_back(std::move(it));
    }
    showMenu(root(), std::move(items), bounds_.x, bounds_.bottom() + 2);
    return true;
}

bool ParamSelector::mouseWheel(const MouseEvent& e)
{
    const ParamDef& d = ctx_.table().def(param_);
    const int cur = static_cast<int>(std::lround(ctx_.value(param_)));
    const int next = std::clamp(cur - static_cast<int>(e.wheel), 0, d.steps - 1);
    if (next != cur)
        ctx_.change(param_, next);
    return true;
}

std::string ParamSelector::tooltip() const { return ctx_.name(param_) + ": " + ctx_.text(param_); }

void Label::paint(cairo_t* cr)
{
    setFont(cr, size_);
    drawText(cr, text_, bounds_, align_, color_);
}

// ------------------------------------------------------------ Icons

namespace icons {

static void stroke(cairo_t* cr, const Color& c, float w = 1.6f)
{
    setColor(cr, c);
    cairo_set_line_width(cr, w);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

void undo(cairo_t* cr, const Rect& r, const Color& c)
{
    const float cx = r.cx(), cy = r.cy() + 1, s = std::min(r.w, r.h) * 0.28f;
    cairo_arc(cr, cx, cy, s, M_PI * 1.0, M_PI * 2.2);
    stroke(cr, c);
    cairo_move_to(cr, cx - s - 3, cy - 3);
    cairo_line_to(cr, cx - s, cy + 1);
    cairo_line_to(cr, cx - s + 3.5, cy - 2.5);
    stroke(cr, c);
}

void redo(cairo_t* cr, const Rect& r, const Color& c)
{
    const float cx = r.cx(), cy = r.cy() + 1, s = std::min(r.w, r.h) * 0.28f;
    cairo_arc_negative(cr, cx, cy, s, 0.0, -M_PI * 1.2);
    stroke(cr, c);
    cairo_move_to(cr, cx + s + 3, cy - 3);
    cairo_line_to(cr, cx + s, cy + 1);
    cairo_line_to(cr, cx + s - 3.5, cy - 2.5);
    stroke(cr, c);
}

void arrowLeft(cairo_t* cr, const Rect& r, const Color& c)
{
    cairo_move_to(cr, r.cx() + 2, r.cy() - 4);
    cairo_line_to(cr, r.cx() - 2, r.cy());
    cairo_line_to(cr, r.cx() + 2, r.cy() + 4);
    stroke(cr, c);
}

void arrowRight(cairo_t* cr, const Rect& r, const Color& c)
{
    cairo_move_to(cr, r.cx() - 2, r.cy() - 4);
    cairo_line_to(cr, r.cx() + 2, r.cy());
    cairo_line_to(cr, r.cx() - 2, r.cy() + 4);
    stroke(cr, c);
}

void power(cairo_t* cr, const Rect& r, const Color& c)
{
    const float s = std::min(r.w, r.h) * 0.27f;
    cairo_arc(cr, r.cx(), r.cy() + 1, s, -M_PI * 0.3, M_PI * 1.3);
    stroke(cr, c);
    cairo_move_to(cr, r.cx(), r.cy() - s - 1);
    cairo_line_to(cr, r.cx(), r.cy());
    stroke(cr, c);
}

void snowflake(cairo_t* cr, const Rect& r, const Color& c)
{
    const float s = std::min(r.w, r.h) * 0.3f;
    for (int i = 0; i < 3; ++i)
    {
        const double a = i * M_PI / 3.0 + M_PI / 2;
        cairo_move_to(cr, r.cx() - std::cos(a) * s, r.cy() - std::sin(a) * s);
        cairo_line_to(cr, r.cx() + std::cos(a) * s, r.cy() + std::sin(a) * s);
    }
    stroke(cr, c, 1.4f);
}

void lock(cairo_t* cr, const Rect& r, const Color& c, bool closed)
{
    const float w = 8, h = 6;
    const Rect body{r.cx() - w / 2, r.cy() - 1, w, h};
    fillRounded(cr, body, 1.2f, c);
    cairo_arc(cr, r.cx() + (closed ? 0 : 3), r.cy() - 1, 2.6, M_PI, 2 * M_PI);
    stroke(cr, c, 1.3f);
}

void menu(cairo_t* cr, const Rect& r, const Color& c)
{
    for (int i = -1; i <= 1; ++i)
    {
        cairo_move_to(cr, r.cx() - 5, r.cy() + i * 4);
        cairo_line_to(cr, r.cx() + 5, r.cy() + i * 4);
    }
    stroke(cr, c, 1.4f);
}

void close(cairo_t* cr, const Rect& r, const Color& c)
{
    const float s = 3.5f;
    cairo_move_to(cr, r.cx() - s, r.cy() - s);
    cairo_line_to(cr, r.cx() + s, r.cy() + s);
    cairo_move_to(cr, r.cx() + s, r.cy() - s);
    cairo_line_to(cr, r.cx() - s, r.cy() + s);
    stroke(cr, c, 1.4f);
}

void sync(cairo_t* cr, const Rect& r, const Color& c)
{
    // Note symbol
    cairo_arc(cr, r.cx() - 1.5, r.cy() + 3, 2.2, 0, 2 * M_PI);
    setColor(cr, c);
    cairo_fill(cr);
    cairo_move_to(cr, r.cx() + 0.7, r.cy() + 3);
    cairo_line_to(cr, r.cx() + 0.7, r.cy() - 5);
    cairo_line_to(cr, r.cx() + 3.5, r.cy() - 3);
    stroke(cr, c, 1.2f);
}

} // namespace icons

} // namespace aurum::gui
