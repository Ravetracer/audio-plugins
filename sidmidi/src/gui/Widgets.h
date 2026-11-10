#pragma once

// The widgets SidMidi's window is built from. TextEditor, PopupMenu, Button,
// Label and the icons come from Substrike; Slider and Choice are SidMidi's
// own, bound to callbacks rather than a parameter table, because most of what
// they set is machine configuration, not automatable parameters.

#include <functional>
#include <string>
#include <vector>

#include "Widget.h"

namespace sidmidi::gui {

// Single-line text editor shown as an overlay.
class TextEditor : public Widget
{
public:
    TextEditor(std::string initial, std::function<void(const std::string&)> onCommit,
               std::function<void()> onTab = {});
    void paint(cairo_t* cr) override;
    bool keyDown(const KeyEvent& e) override;
    bool mouseDown(const MouseEvent&) override { return true; }

private:
    void close();
    std::string text_;
    bool selectAll_ = true;
    std::function<void(const std::string&)> onCommit_;
    std::function<void()> onTab_;
};

struct MenuItem
{
    std::string label;
    std::function<void()> action;
    bool checked = false;
    bool enabled = true;
    bool separator = false;
    std::vector<MenuItem> submenu;
};

// Popup menu overlay.
class PopupMenu : public Widget
{
public:
    PopupMenu(std::vector<MenuItem> items, float x, float y, float rootW, float rootH, PopupMenu* parentMenu = nullptr);
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    bool keyDown(const KeyEvent& e) override;
    void mouseLeave() override;

private:
    int itemAt(float y) const;
    void activate(int i);
    void closeChain();
    void openSubmenu(int i);
    std::vector<MenuItem> items_;
    std::vector<float> itemY_;
    int hover_ = -1;
    float rootW_, rootH_;
    PopupMenu* parentMenu_;
    PopupMenu* childMenu_ = nullptr;
    int childIndex_ = -1;
    double openedAt_ = 0.0;
};

void showMenu(RootWidget* root, std::vector<MenuItem> items, float x, float y);

// Push button with a text label or a custom painter.
class Button : public Widget
{
public:
    Button(std::string label, std::function<void()> onClick);
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    std::string tooltip() const override { return tooltip_; }

    void setLabel(std::string l)
    {
        label_ = std::move(l);
        repaint();
    }
    void setTooltip(std::string t) { tooltip_ = std::move(t); }
    void setEnabled(bool e)
    {
        enabled_ = e;
        repaint();
    }
    void setActive(bool a)
    {
        active_ = a;
        repaint();
    }
    bool isActive() const { return active_; }
    std::function<void(cairo_t*, const Rect&, const Color&)> icon;
    std::function<void(const MouseEvent&)> onRightClick;
    std::function<void()> onRelease;
    float fontSize = 11.0f;
    bool frame = true;

protected:
    std::string label_;
    std::string tooltip_;
    std::function<void()> onClick_;
    bool enabled_ = true;
    bool active_ = false;
    bool pressed_ = false;
};

class Label : public Widget
{
public:
    Label(std::string text, float size = 11.0f, Align align = Align::Center, Color color = theme::textDim)
        : text_(std::move(text)), size_(size), align_(align), color_(color)
    {
    }
    void paint(cairo_t* cr) override;
    void setText(std::string t)
    {
        text_ = std::move(t);
        repaint();
    }
    void setColor(Color c)
    {
        color_ = c;
        repaint();
    }

private:
    std::string text_;
    float size_;
    Align align_;
    Color color_;
};

// A horizontal slider over a value range, with its text inside. Drag, wheel,
// double click for the default, shift for fine steps.
class Slider : public Widget
{
public:
    Slider(std::string caption, double lo, double hi, double def, std::function<double()> get,
           std::function<void(double)> set, std::function<std::string(double)> format);
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    std::string tooltip() const override { return tooltip_; }
    void setTooltip(std::string t) { tooltip_ = std::move(t); }
    std::function<void()> onBegin, onEnd;
    // Disabled: drawn faint, ignores the mouse, keeps its value.
    void setEnabled(bool e)
    {
        enabled_ = e;
        repaint();
    }

private:
    std::string caption_, tooltip_;
    bool enabled_ = true;
    double lo_, hi_, def_;
    std::function<double()> get_;
    std::function<void(double)> set_;
    std::function<std::string(double)> format_;
    float lastX_ = 0;
    double dragValue_ = 0;
};

// A dropdown of fixed choices.
class Choice : public Widget
{
public:
    Choice(std::string caption, std::vector<std::string> items, std::function<int()> get, std::function<void(int)> set);
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    std::string tooltip() const override { return tooltip_; }
    void setTooltip(std::string t) { tooltip_ = std::move(t); }
    void setItems(std::vector<std::string> items)
    {
        items_ = std::move(items);
        repaint();
    }
    // Disabled: drawn faint, ignores the mouse, keeps its value.
    void setEnabled(bool e)
    {
        enabled_ = e;
        repaint();
    }

private:
    std::string caption_, tooltip_;
    bool enabled_ = true;
    std::vector<std::string> items_;
    std::function<int()> get_;
    std::function<void(int)> set_;
};

namespace icons {
void undo(cairo_t* cr, const Rect& r, const Color& c);
void redo(cairo_t* cr, const Rect& r, const Color& c);
void arrowLeft(cairo_t* cr, const Rect& r, const Color& c);
void arrowRight(cairo_t* cr, const Rect& r, const Color& c);
void power(cairo_t* cr, const Rect& r, const Color& c);
void snowflake(cairo_t* cr, const Rect& r, const Color& c);
void lock(cairo_t* cr, const Rect& r, const Color& c, bool closed);
void menu(cairo_t* cr, const Rect& r, const Color& c);
void close(cairo_t* cr, const Rect& r, const Color& c);
void sync(cairo_t* cr, const Rect& r, const Color& c);
void star(cairo_t* cr, const Rect& r, float radius, const Color& c, bool filled);
} // namespace icons

} // namespace sidmidi::gui
