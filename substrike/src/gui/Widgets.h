#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Widget.h"
#include "plugin/Controller.h"
#include "plugin/Params.h"

namespace substrike::gui {

// Parameter access for widgets. A slot's A-F are read through the
// definition their slot's type gives them: name, range, text and default.
class ParamContext
{
public:
    ParamContext(Controller& c) : ctl_(c), table_(ParamTable::get()) {}

    const ParamTable& table() const { return table_; }
    int index(uint32_t id) const { return table_.indexOf(id); }
    double value(int idx) const { return ctl_.paramValue(idx); }
    // The definition as it is now (see ParamTable::effective).
    const ParamDef& def(int idx) const
    {
        const int t = table_.def(idx).typeParam;
        return t < 0 ? table_.def(idx) : table_.effective(idx, value(t));
    }
    double defaultValue(int idx) const { return def(idx).def; }
    double plain(int idx) const { return ParamTable::toPlain(def(idx), value(idx)); }
    std::string text(int idx) const { return ParamTable::toText(def(idx), value(idx)); }
    std::string textFor(int idx, double v) const { return ParamTable::toText(def(idx), v); }
    const std::string& name(int idx) const { return def(idx).name; }
    double minValue(int idx) const { return table_.minValue(idx); }
    double maxValue(int idx) const { return table_.maxValue(idx); }

    void begin(int idx);
    // `force` sends the value even when it equals the current one (a slot
    // swap, where the type change in front of it resets the letters).
    void set(int idx, double v, bool force = false);
    void end(int idx);
    // One-shot change (begin + set + end).
    void change(int idx, double v);
    bool setFromText(int idx, const std::string& t);

    Controller& controller() { return ctl_; }

    // How far the matrix can move a parameter, in stored units relative to
    // its value: the sum of the routes on it, a bipolar source counted both
    // ways. False when no route is on it.
    bool modRange(int idx, double& lo, double& hi) const;

    // Called after a completed gesture.
    std::function<void()> onGestureEnd;
    // Called for every value change made through the GUI.
    std::function<void(int)> onChanged;

private:
    Controller& ctl_;
    const ParamTable& table_;
    int openGestures_ = 0;
};

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

// Opens a text editor overlay for a parameter, centred on `anchor`.
void openParamEditor(ParamContext& ctx, int paramIndex, const Rect& anchor, RootWidget* root,
                     std::function<void()> onTab = {});

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

enum class KnobSize { Small, Medium, Large };

class Knob : public Widget
{
public:
    Knob(ParamContext& ctx, int paramIndex, std::string label, KnobSize size, bool bipolar = false);
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    std::string tooltip() const override;
    int boundParam() const override { return param_; }

    int paramIndex() const { return param_; }
    // Override what is drawn inside the knob (e.g. the Space decay time).
    std::function<void(cairo_t*, const Rect&, float)> drawCentre;
    // Optional label override (e.g. Predelay offset when synced).
    std::function<std::string()> dynamicLabel;
    void setParamIndex(int p)
    {
        param_ = p;
        repaint();
    }

protected:
    Rect knobRect() const;
    ParamContext& ctx_;
    int param_;
    std::string label_;
    KnobSize size_;
    bool bipolar_;
    bool dragging_ = false;
    float lastY_ = 0;
    double dragValue_ = 0;
    double lastMoveTime_ = 0;
};

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
    // Custom drawing (icon); receives rect and current colour.
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

// Toggle bound to a boolean parameter. With a caption it is laid out like a
// knob: the button where the knob would be, the caption under it.
class ParamToggle : public Button
{
public:
    ParamToggle(ParamContext& ctx, int paramIndex, std::string label);
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    std::string tooltip() const override;
    int boundParam() const override { return param_; }
    std::string caption;

private:
    ParamContext& ctx_;
    int param_;
};

// Shows an enum parameter, or a continuous one that is shown as a choice;
// click opens a menu of its values. With a caption it is laid out like a knob.
class ParamSelector : public Widget
{
public:
    ParamSelector(ParamContext& ctx, int paramIndex, std::string prefix = {});
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    std::string tooltip() const override;
    int boundParam() const override { return param_; }
    float fontSize = 11.0f;
    std::string caption;

private:
    Rect box() const;
    ParamContext& ctx_;
    int param_;
    std::string prefix_;
};

// Compact numeric field: drag vertically, scroll, double-click to type,
// Ctrl-click to reset.
class ParamField : public Widget
{
public:
    ParamField(ParamContext& ctx, int paramIndex, std::string prefix = {});
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    std::string tooltip() const override;
    int boundParam() const override { return param_; }
    Color accent = theme::accent;

private:
    ParamContext& ctx_;
    int param_;
    std::string prefix_;
    bool dragging_ = false;
    float lastY_ = 0;
    double dragValue_ = 0;
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

private:
    std::string text_;
    float size_;
    Align align_;
    Color color_;
};

// Small icon painters.
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
// Five-pointed star of outer radius `radius`, filled or outlined. Drawn rather
// than set as a glyph: Cairo has no per-glyph font fallback, and the Windows UI
// font has no star.
void star(cairo_t* cr, const Rect& r, float radius, const Color& c, bool filled);
} // namespace icons

} // namespace substrike::gui
