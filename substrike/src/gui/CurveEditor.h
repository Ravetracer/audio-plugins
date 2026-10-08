#pragma once

#include <functional>

#include "Widgets.h"
#include "dsp/Curve.h"

namespace substrike::gui {

// Draws and edits one lane's Body pitch or amplitude curve, or a modulation
// envelope's curve (setEnv). The curve is
// shown scaled by its macros -- Pitch Start/End over Sweep Time for pitch,
// the level over Body Decay for amplitude -- and bent by the Sweep or Decay
// Curve macro, so what is drawn is what plays.
//
//   drag a point          move it (Shift: fine)
//   drag a segment handle bend that segment
//   double-click          add a point / remove one / straighten a segment
//   right-click           point and curve menu
class CurveEditor : public Widget
{
public:
    explicit CurveEditor(ParamContext& ctx) : ctx_(ctx) {}

    void setLane(int lane);
    int lane() const { return lane_; }
    void setTab(int which);
    int tab() const { return tab_; }
    // Edits modulation envelope `env` (0-3) instead, over its Time; -1 goes
    // back to the lane's curves.
    void setEnv(int env);
    // A tab was clicked; the editor rebuilds the macro knobs for it.
    std::function<void(int)> onTab;

    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseLeave() override;
    std::string tooltip() const override;

private:
    enum class Hit { None, Point, Handle, Tab };
    struct Target
    {
        Hit what = Hit::None;
        int index = -1;
    };

    Rect plot() const;
    Rect tabRect(int i) const;
    int curveIndex() const;
    const dsp::Curve& curve() const;
    void commit(const dsp::Curve& c);
    // The macros: the bend added to every segment, and the axes.
    double bend() const;
    double param(uint32_t field) const;
    float sx(double x) const;
    float sy(double y) const;
    double xAt(float px) const;
    double yAt(float py) const;
    // The midpoint handle of the segment from point i.
    void handlePos(int i, float& x, float& y) const;
    Target hitAt(float x, float y) const;
    std::string xLabel(double x) const;
    std::string yLabel(double y) const;
    void insertPoint(double x, double y);
    void removePoint(int i);
    void setCurvature(int i, double k);
    void showMenu(const MouseEvent& e, const Target& t);

    ParamContext& ctx_;
    int lane_ = 0;
    int tab_ = 0;
    int env_ = -1;
    Target hover_;
    Target drag_;
    float lastX_ = 0, lastY_ = 0;
    double dragX_ = 0, dragY_ = 0;
};

} // namespace substrike::gui
