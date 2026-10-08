#pragma once

#include <string>
#include <vector>

#include "Widgets.h"
#include "state/Presets.h"

namespace substrike::gui {

class Editor;

// The top bar's preset field: the current preset's name, the previous and
// next preset, and the browser on a click.
class PresetBar : public Widget
{
public:
    explicit PresetBar(Editor& ed) : ed_(ed) {}
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseLeave() override;
    std::string tooltip() const override;

private:
    Rect left() const { return {bounds_.x, bounds_.y, 26, bounds_.h}; }
    Rect right() const { return {bounds_.right() - 26, bounds_.y, 26, bounds_.h}; }
    Editor& ed_;
    int hover_ = 0; // -1 left, 1 right, 2 the name
};

// The preset browser, over the right side of the window: the categories, the
// presets of the one selected, and what the hovered or loaded preset is.
// Clicking a preset loads it and plays it.
class PresetBrowser : public Widget
{
public:
    explicit PresetBrowser(Editor& ed) : ed_(ed) {}

    // Reads the user's folder again and selects the loaded preset's
    // category.
    void refresh();
    // The presets in browsing order: the factory's, then the user's.
    const std::vector<PresetInfo>& all() const { return all_; }
    // Moves the selection by `step` in the list shown and loads it.
    void step(int step);

    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseLeave() override;
    bool mouseWheel(const MouseEvent& e) override;

private:
    static constexpr float kRowH = 22.0f;
    Rect categoryArea() const;
    Rect listArea() const;
    Rect infoArea() const;
    Rect button(int i) const; // 0 save, 1 close
    std::vector<int> shown() const;
    int rowAt(float y) const;

    Editor& ed_;
    std::vector<PresetInfo> all_;
    std::vector<std::string> categories_; // "All", the factory's, "User"
    int category_ = 0;
    int scroll_ = 0;
    int hover_ = -1;       // index into all_
    int hoverCategory_ = -1;
    int hoverButton_ = -1;
};

} // namespace substrike::gui
