#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "FileDialog.h"
#include "EqPanel.h"
#include "Widgets.h"
#include "NativeWindow.h"

namespace aurum {
class PresetSession;
}

namespace aurum::gui {

class RoomSlider;
class RoomReadout;
class FreezeButton;

// Undo history and A/B slots of full parameter snapshots.
class History
{
public:
    void reset(const std::vector<double>& state);
    void push(const std::vector<double>& state);
    bool canUndo() const { return index_ > 0; }
    bool canRedo() const { return index_ + 1 < static_cast<int>(states_.size()); }
    const std::vector<double>& undo() { return states_[static_cast<size_t>(--index_)]; }
    const std::vector<double>& redo() { return states_[static_cast<size_t>(++index_)]; }
    const std::vector<double>& current() const { return states_[static_cast<size_t>(index_)]; }

private:
    std::vector<std::vector<double>> states_;
    int index_ = -1;
};

class Editor : public RootWidget, public WindowListener
{
public:
    static constexpr float kBaseW = 1000.0f;
    static constexpr float kBaseH = 660.0f;
    static constexpr float kMinW = 860.0f;
    static constexpr float kMinH = 580.0f;

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

    // Offscreen rendering for tests/screenshots.
    void renderTo(cairo_t* cr);

    // WindowListener
    void onPaint(cairo_t* cr, int physW, int physH) override;
    void onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override { handleMouseUp(e); }
    void onMouseMove(const MouseEvent& e) override { handleMouseMove(e); }
    void onMouseWheel(const MouseEvent& e) override { handleWheel(e); }
    void onMouseLeave() override { handleMouseLeave(); }
    bool onKey(const KeyEvent& e) override { return handleKey(e); }
    void onFilesDropped(const std::vector<std::string>& paths) override;

    void layout() override;
    void paint(cairo_t* cr) override;
    bool keyDown(const KeyEvent& e) override;

    ParamContext& params() { return ctx_; }

    // Hooks for features living outside the editor (presets, IR import, MIDI).
    void setPresetSession(PresetSession* s) { session_ = s; }
    std::function<void(const std::string& path)> onImportIr;
    void runFileDialog(FileDialog::Mode mode, const std::string& title, const std::string& filter,
                       std::function<void(const std::string&)> done);
    void openPresetBrowser();
    // Shows a short message near the top of the window (e.g. IR import result).
    void notify(const std::string& message);

    std::vector<double> snapshot() const;
    void applySnapshot(const std::vector<double>& values);
    void resetHistory();
    void pushHistory();

protected:
    void paintTooltip(cairo_t* cr) override;

private:
    void buildTopBar();
    void buildControls();
    void openIoPanel();
    MenuItem sizeMenu();
    MenuItem scalingMenu();
    void showMainMenu();
    void showMidiMenu(float x, float y);
    void paintMidiLearn(cairo_t* cr);
    void undo();
    void redo();
    void toggleAB();
    void copyAB();
    void updateButtons();
    void setLogicalSize(float w, float h);

    Controller& controller_;
    ParamContext ctx_;
    NativeWindow window_;
    double scale_ = 1.0;
    float logicalW_ = kBaseW, logicalH_ = kBaseH;

    History history_;
    bool applyingHistory_ = false;
    std::vector<double> abSlot_[2];
    int abActive_ = 0;
    std::vector<double> lastSeen_;
    std::string presetLabel_;
    PresetSession* session_ = nullptr;
    cairo_surface_t* staticLayer_ = nullptr;
    int staticW_ = 0, staticH_ = 0;
    std::unique_ptr<FileDialog> dialog_;
    std::function<void(const std::string&)> dialogDone_;
    std::string notice_;
    double noticeUntil_ = 0.0;
    double lastTick_ = 0.0;

    // Widgets (owned by the tree)
    Button* presetButton_ = nullptr;
    Button* undoButton_ = nullptr;
    Button* redoButton_ = nullptr;
    Button* abButton_ = nullptr;
    Button* copyButton_ = nullptr;
    Button* menuButton_ = nullptr;
    Button* prevPreset_ = nullptr;
    Button* nextPreset_ = nullptr;

    RoomSlider* room_ = nullptr;
    RoomReadout* roomReadout_ = nullptr;
    Knob* decayRate_ = nullptr;
    Knob* predelay_ = nullptr;
    Knob* character_ = nullptr;
    Knob* brightness_ = nullptr;
    Knob* distance_ = nullptr;
    Knob* thickness_ = nullptr;
    Knob* width_ = nullptr;
    Knob* mix_ = nullptr;
    Knob* ducking_ = nullptr;
    Knob* gateHold_ = nullptr;
    ParamSelector* style_ = nullptr;
    ParamSelector* predelaySync_ = nullptr;
    ParamSelector* gateSync_ = nullptr;
    ParamToggle* gateOn_ = nullptr;
    FreezeButton* freeze_ = nullptr;
    Button* lockMix_ = nullptr;

    EqPanel* decayEq_ = nullptr;
    EqPanel* toneEq_ = nullptr;

    Button* midiButton_ = nullptr;
    Button* ioButton_ = nullptr;
    ParamToggle* bypass_ = nullptr;
};

} // namespace aurum::gui
