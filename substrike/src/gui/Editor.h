#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "FileDialog.h"
#include "HitPreview.h"
#include "NativeWindow.h"
#include "Widgets.h"

namespace substrike::gui {

class ControlGrid;
class CurveEditor;
class LaneRow;
class HitStrip;
class ChainRow;
class Scope;

// The Substrike window: the eight lanes and the master as a rack on the left,
// and the selected one's controls, curve and effect chain on the right.
class Editor : public RootWidget, public WindowListener
{
public:
    static constexpr float kBaseW = 1120.0f;
    static constexpr float kBaseH = 720.0f;
    static constexpr float kMinW = 1000.0f;
    static constexpr float kMinH = 660.0f;
    // The rack's last row: the master rather than a lane.
    static constexpr int kMaster = dsp::kNumLanes;

    explicit Editor(Controller& controller);
    ~Editor() override;

    // Window management (called by the plugin's GUI extension).
    bool attach(uintptr_t parentWindow);
    void detach();
    void setScale(double s);
    double scale() const { return scale_; }
    void setPhysicalSize(int w, int h);
    void physicalSize(int& w, int& h) const;
    void show();
    void hide();
    int fd() const { return window_.fd(); }
    void onFd() { window_.processEvents(); }
    void onTimer();

    // Asks the host for a new size (physical pixels); set by the plugin.
    std::function<bool(int, int)> requestResize;

    // Offscreen rendering for tests and screenshots.
    void renderTo(cairo_t* cr);
    // Renders the preview now and waits for it (tests and screenshots).
    void settlePreview();

    // WindowListener
    void onPaint(cairo_t* cr, int physW, int physH) override;
    void onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override { handleMouseUp(e); }
    void onMouseMove(const MouseEvent& e) override { handleMouseMove(e); }
    void onMouseWheel(const MouseEvent& e) override { handleWheel(e); }
    void onMouseLeave() override { handleMouseLeave(); }
    bool onKey(const KeyEvent& e) override { return handleKey(e); }

    void layout() override;
    void paint(cairo_t* cr) override;
    bool keyDown(const KeyEvent& e) override;

    ParamContext& params() { return ctx_; }
    Controller& controller() { return controller_; }

    // What is selected: a lane (0-7) or kMaster, and per lane its slot.
    int selectedLane() const { return lane_; }
    void selectLane(int lane);
    int selectedSlot() const { return slot_[static_cast<size_t>(lane_)]; }
    void selectSlot(int slot);
    // Swaps two slots of the selected lane's (or the master's) chain,
    // parameters included.
    void swapSlots(int a, int b);
    // The id of a slot field in the selected chain.
    uint32_t slotId(int slot, uint32_t field) const;
    void selectCurveTab(int tab);

    const HitView* hitView() const { return preview_.view(); }
    void audition();
    // Renders the hit and writes it to a file, or into the export folder for
    // a drag; returns the path, empty on failure.
    std::string exportHit(const std::string& path = {});
    void dragHit();
    void notify(const std::string& message);

    // Positions of the lane rows and the slot chips, for tests.
    Rect laneRowBounds(int lane) const;

protected:
    void paintTooltip(cairo_t* cr) override;

private:
    struct Structure
    {
        int lane = -1, source = -1, slot = -1, slotType = -1, tab = -1;
        bool operator==(const Structure&) const = default;
    };
    Structure structure() const;
    void rebuild();
    void buildStrip();
    void buildSource();
    void buildSlot();
    void showMainMenu();
    void saveHitAs();
    void setLogicalSize(float w, float h);
    void checkPreview(double now);

    Controller& controller_;
    ParamContext ctx_;
    NativeWindow window_;
    double scale_ = 1.0;
    float logicalW_ = kBaseW, logicalH_ = kBaseH;

    int lane_ = 0;
    std::array<int, dsp::kNumLanes + 1> slot_{};
    int curveTab_ = 0;
    Structure built_;

    HitPreview preview_;
    std::vector<double> lastValues_;
    std::array<dsp::Curve, dsp::kNumCurves> lastCurves_{};
    bool previewStale_ = true;
    double lastChange_ = 0.0;

    cairo_surface_t* staticLayer_ = nullptr;
    int staticW_ = 0, staticH_ = 0;
    std::unique_ptr<FileDialog> dialog_;
    std::string notice_;
    double noticeUntil_ = 0.0;

    // Widgets (owned by the tree)
    std::array<LaneRow*, dsp::kNumLanes + 1> rows_{};
    HitStrip* hitStrip_ = nullptr;
    Button* playButton_ = nullptr;
    Button* exportButton_ = nullptr;
    Button* menuButton_ = nullptr;
    ControlGrid* strip_ = nullptr;
    ControlGrid* source_ = nullptr;
    CurveEditor* curve_ = nullptr;
    ControlGrid* macros_ = nullptr;
    Scope* scope_ = nullptr;
    ChainRow* chain_ = nullptr;
    ControlGrid* slotGrid_ = nullptr;
};

} // namespace substrike::gui
