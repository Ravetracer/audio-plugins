#include "Widget.h"

#include <chrono>

namespace substrike::gui {

double nowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Widget::paintAll(cairo_t* cr)
{
    if (!visible_)
        return;
    paint(cr);
    for (auto& c : children_)
        c->paintAll(cr);
}

void Widget::clearChildren()
{
    if (RootWidget* r = root())
        for (auto& c : children_)
            r->forget(c.get());
    children_.clear();
    repaint();
}

Widget* Widget::hitTest(float x, float y)
{
    if (!visible_ || !bounds_.contains(x, y))
        return nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
        if (Widget* w = (*it)->hitTest(x, y))
            return w;
    return this;
}

RootWidget* Widget::root()
{
    Widget* w = this;
    while (w->parent_)
        w = w->parent_;
    return dynamic_cast<RootWidget*>(w);
}

bool Widget::isHovered() const
{
    Widget* self = const_cast<Widget*>(this);
    RootWidget* r = self->root();
    return r && r->hovered() == this;
}

void Widget::repaint()
{
    if (RootWidget* r = root())
        r->markDirty();
}

namespace {
bool isWithin(const Widget* w, const Widget* ancestor)
{
    for (; w; w = w->parent())
        if (w == ancestor)
            return true;
    return false;
}
} // namespace

Widget* RootWidget::findTarget(float x, float y)
{
    for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it)
        if (Widget* w = (*it)->hitTest(x, y))
            return w;
    if (!overlays_.empty())
        return nullptr; // modal: outside of overlays
    return hitTest(x, y);
}

void RootWidget::handleMouseDown(const MouseEvent& ev)
{
    MouseEvent e = ev;
    const double t = nowSeconds();
    if (t - lastClickTime_ < 0.35 && std::fabs(e.x - lastClickX_) < 4 && std::fabs(e.y - lastClickY_) < 4 &&
        e.button == 1)
    {
        e.clicks = 2;
        lastClickTime_ = 0.0;
    }
    else
    {
        lastClickTime_ = t;
    }
    lastClickX_ = e.x;
    lastClickY_ = e.y;

    Widget* target = findTarget(e.x, e.y);
    if (!target && !overlays_.empty())
    {
        closeAllOverlays();
        markDirty();
        return;
    }
    if (keyFocus_ && target != keyFocus_ && !isWithin(target, keyFocus_))
        keyFocus_ = nullptr;
    for (Widget* w = target; w; w = w->parent())
        if (w->mouseDown(e))
        {
            captured_ = w;
            break;
        }
    markDirty();
}

void RootWidget::handleMouseUp(const MouseEvent& e)
{
    if (captured_)
    {
        Widget* c = captured_;
        captured_ = nullptr;
        c->mouseUp(e);
    }
    markDirty();
    graveyard_.clear();
}

void RootWidget::handleMouseMove(const MouseEvent& e)
{
    mouseX_ = e.x;
    mouseY_ = e.y;
    if (captured_)
    {
        captured_->mouseDrag(e);
        return;
    }
    Widget* h = findTarget(e.x, e.y);
    if (h != hovered_)
    {
        if (hovered_)
            hovered_->mouseLeave();
        hovered_ = h;
        if (hovered_)
            hovered_->mouseEnter();
        markDirty();
    }
    if (hovered_)
        hovered_->mouseMove(e);
}

void RootWidget::handleWheel(const MouseEvent& e)
{
    for (Widget* w = findTarget(e.x, e.y); w; w = w->parent())
        if (w->mouseWheel(e))
            break;
    markDirty();
}

void RootWidget::handleMouseLeave()
{
    if (hovered_ && !captured_)
    {
        hovered_->mouseLeave();
        hovered_ = nullptr;
        markDirty();
    }
}

bool RootWidget::handleKey(const KeyEvent& e)
{
    bool used = false;
    if (keyFocus_)
        used = keyFocus_->keyDown(e);
    if (!used && !overlays_.empty())
        used = overlays_.back()->keyDown(e);
    if (!used)
        used = keyDown(e);
    markDirty();
    graveyard_.clear();
    return used;
}

void RootWidget::pushOverlay(std::unique_ptr<Widget> w)
{
    w->parent_ = this;
    overlays_.push_back(std::move(w));
    markDirty();
}

void RootWidget::forget(Widget* w)
{
    if (isWithin(hovered_, w))
        hovered_ = nullptr;
    if (isWithin(captured_, w))
        captured_ = nullptr;
    if (isWithin(keyFocus_, w))
        keyFocus_ = nullptr;
}

void RootWidget::closeOverlay(Widget* w)
{
    for (auto it = overlays_.begin(); it != overlays_.end(); ++it)
        if (it->get() == w)
        {
            forget(w);
            // Defer destruction: we may be inside one of its handlers.
            graveyard_.push_back(std::move(*it));
            overlays_.erase(it);
            markDirty();
            return;
        }
}

void RootWidget::closeAllOverlays()
{
    while (!overlays_.empty())
        closeOverlay(overlays_.back().get());
}

void RootWidget::paintRoot(cairo_t* cr)
{
    paintAll(cr);
    paintOverlays(cr);
    paintTooltip(cr);
}

void RootWidget::paintOverlays(cairo_t* cr)
{
    for (auto& o : overlays_)
        o->paintAll(cr);
}

void RootWidget::paintTooltip(cairo_t*) {}

} // namespace substrike::gui
