#pragma once

// The SidMidi window: a strip of buttons, the C64's screen, and a status line.
// The Machine button swaps the screen for the machine page (video, MIDI
// interface, the SIDs and their mix), the CC button for the CC map page.
//
// A click into the screen gives the C64 the keyboard: from then on every key
// goes to the emulated machine (see Keymap.h for where they land), until the
// window loses focus or something outside the screen is clicked.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "FileDialog.h"
#include "Keymap.h"
#include "NativeWindow.h"
#include "Widgets.h"

namespace sidmidi {
class SidMidiPlugin;
}

namespace sidmidi::gui {

class ScreenView;
class MachinePage;
class CcPage;
class RomPanel;

class Editor : public RootWidget, public WindowListener
{
public:
    static constexpr float kBarH = 38.0f;
    static constexpr float kStatusH = 24.0f;
    static constexpr float kBaseW = 800.0f;
    static constexpr float kBaseH = kBarH + 8 + 544 + 8 + kStatusH;
    static constexpr float kMinW = 640.0f;
    static constexpr float kMinH = 420.0f;

    explicit Editor(SidMidiPlugin& plugin);
    ~Editor() override;

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

    std::function<bool(int, int)> requestResize;

    // Offscreen rendering (tests, screenshots).
    void renderTo(cairo_t* cr);

    // WindowListener
    void onPaint(cairo_t* cr, int physW, int physH) override;
    void onMouseDown(const MouseEvent& e) override;
    void onMouseUp(const MouseEvent& e) override { handleMouseUp(e); }
    void onMouseMove(const MouseEvent& e) override { handleMouseMove(e); }
    void onMouseWheel(const MouseEvent& e) override { handleWheel(e); }
    void onMouseLeave() override { handleMouseLeave(); }
    bool onKey(const KeyEvent& e) override;
    void onRawKey(int code, bool down) override;
    void onFocusLost() override;
    void onFilesDropped(const std::vector<std::string>& paths) override;

    void layout() override;
    void paint(cairo_t* cr) override;

    SidMidiPlugin& plugin() { return plugin_; }
    enum class Page { Screen, Machine, Cc };
    // What the Machine and CC buttons do (and the snapshot tool).
    void showPage(Page p);
    void notify(const std::string& text, bool error = false);
    bool c64HasKeyboard() const { return keyboardToC64_; }
    void giveKeyboardToC64(bool on);

private:
    void openLoadDialog();
    void openExportDialog();
    void showDiskMenu();
    void toggleMachinePage();
    void toggleCcPage();
    void showPage(Widget* which);
    void sendKeyState();
    void paintStatus(cairo_t* cr);

    SidMidiPlugin& plugin_;
    NativeWindow window_;
    double scale_ = 1.0;
    float logicalW_ = kBaseW, logicalH_ = kBaseH;

    Button* loadButton_ = nullptr;
    Button* resetButton_ = nullptr;
    Button* powerButton_ = nullptr;
    Button* warpButton_ = nullptr;
    Button* diskButton_ = nullptr;
    Choice* midiChoice_ = nullptr;
    Choice* joyChoice_ = nullptr;
    Choice* portChoice_ = nullptr;
    Button* machineButton_ = nullptr;
    Button* ccButton_ = nullptr;
    ScreenView* screen_ = nullptr;
    MachinePage* page_ = nullptr;
    CcPage* ccPage_ = nullptr;
    RomPanel* romPanel_ = nullptr;
    double romScanAt_ = 0.0;

    Keymap keymap_;
    std::array<uint8_t, 8> sentMatrix_{};
    uint8_t sentJoy_ = 0;
    int sentJoyPort_ = 1;
    bool keyboardToC64_ = false;

    std::unique_ptr<FileDialog> dialog_;
    bool dialogIsExport_ = false;
    std::string notice_;
    bool noticeError_ = false;
    double noticeUntil_ = 0.0;
    std::string media_;
    double mediaPolledAt_ = 0.0;
    uint32_t lastMidiIn_ = 0, lastMidiOut_ = 0;
    double midiInLit_ = 0.0, midiOutLit_ = 0.0;
    bool driveLit_ = false, warpLit_ = false;
    double lastLedRepaint_ = 0.0;

    cairo_surface_t* staticLayer_ = nullptr;
    int staticW_ = 0, staticH_ = 0;
};

} // namespace sidmidi::gui
