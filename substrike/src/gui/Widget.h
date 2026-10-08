#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Graphics.h"

namespace substrike::gui {

enum Modifier : unsigned { ModShift = 1, ModCtrl = 2, ModAlt = 4 };

struct MouseEvent
{
    float x = 0, y = 0;  // logical coordinates (window space)
    int button = 0;      // 1 left, 2 middle, 3 right
    unsigned mods = 0;
    float wheel = 0;     // +1 up, -1 down (per notch)
    int clicks = 1;      // 2 on double click
};

struct KeyEvent
{
    unsigned keysym = 0; // X11 keysym
    std::string text;    // UTF-8 text for printable keys
    unsigned mods = 0;
};

class RootWidget;

class Widget
{
public:
    virtual ~Widget() = default;

    void setBounds(const Rect& r)
    {
        bounds_ = r;
        layout();
    }
    const Rect& bounds() const { return bounds_; }
    bool isVisible() const { return visible_; }
    void setVisible(bool v)
    {
        if (v != visible_)
        {
            visible_ = v;
            repaint();
        }
    }

    template <typename T, typename... Args> T* add(Args&&... args)
    {
        auto w = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = w.get();
        raw->parent_ = this;
        children_.push_back(std::move(w));
        return raw;
    }
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    // Removes every child. Not from inside a child's own event handler: the
    // editor rebuilds panels from its timer.
    void clearChildren();
    Widget* parent() const { return parent_; }

    virtual void layout() {}
    virtual void paint(cairo_t* cr) {}
    void paintAll(cairo_t* cr);

    // Event handlers return true when consumed.
    virtual bool mouseDown(const MouseEvent&) { return false; }
    virtual void mouseDrag(const MouseEvent&) {}
    virtual void mouseUp(const MouseEvent&) {}
    virtual void mouseMove(const MouseEvent&) {}
    virtual bool mouseWheel(const MouseEvent&) { return false; }
    virtual void mouseEnter() {}
    virtual void mouseLeave() {}
    virtual bool keyDown(const KeyEvent&) { return false; }
    // Text shown in the hover bubble; empty for none.
    virtual std::string tooltip() const { return {}; }
    // Parameter controlled by this widget (for MIDI learn), -1 if none.
    virtual int boundParam() const { return -1; }

    Widget* hitTest(float x, float y);
    bool isHovered() const;
    void repaint();
    RootWidget* root();

protected:
    friend class RootWidget;
    Rect bounds_;
    bool visible_ = true;
    Widget* parent_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
};

// Top-level widget: owns mouse capture, hover tracking, keyboard focus and
// overlays (menus, text entry), and the dirty flag.
class RootWidget : public Widget
{
public:
    void handleMouseDown(const MouseEvent& e);
    void handleMouseUp(const MouseEvent& e);
    void handleMouseMove(const MouseEvent& e);
    void handleWheel(const MouseEvent& e);
    void handleMouseLeave();
    bool handleKey(const KeyEvent& e);

    // markDirty: something in the widget tree changed (static layers are
    // re-rendered). markAnimDirty: only animated content changed.
    void markDirty()
    {
        dirty_ = true;
        staticStale_ = true;
    }
    void markAnimDirty() { dirty_ = true; }
    bool takeDirty()
    {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }

    Widget* hovered() const { return hovered_; }
    Widget* captured() const { return captured_; }

    // Overlays are drawn above everything and receive events first.
    void pushOverlay(std::unique_ptr<Widget> w);
    void closeOverlay(Widget* w);
    void closeAllOverlays();
    bool hasOverlay() const { return !overlays_.empty(); }
    void setKeyFocus(Widget* w) { keyFocus_ = w; }
    Widget* keyFocus() const { return keyFocus_; }

    void paintRoot(cairo_t* cr);
    void paintOverlays(cairo_t* cr);
    // Called when a widget is about to be destroyed elsewhere.
    void forget(Widget* w);

    // Requests keyboard focus from the window system (set by the window).
    std::function<void()> grabKeyboard;

protected:
    virtual void paintTooltip(cairo_t* cr);

    bool staticStale_ = true;

private:
    Widget* findTarget(float x, float y);

    std::vector<std::unique_ptr<Widget>> overlays_;
    std::vector<std::unique_ptr<Widget>> graveyard_;
    Widget* hovered_ = nullptr;
    Widget* captured_ = nullptr;
    Widget* keyFocus_ = nullptr;
    bool dirty_ = true;
    double lastClickTime_ = 0.0;
    float lastClickX_ = 0, lastClickY_ = 0;
    float mouseX_ = 0, mouseY_ = 0;
};

double nowSeconds();

} // namespace substrike::gui
