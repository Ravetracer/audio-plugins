// The WCLAP's editor (ui.wasm): Substrike's own editor, its widgets, curve
// editors and preset browser, running in the plugin's page and drawn into a
// canvas (shared/web/wclap-ui.js). It talks to the DSP module (module.wasm,
// the plugin) through the host's webview messages, plugin/WebProtocol.h.
//
// The plugin is represented by RemoteController: it mirrors what the plugin
// reports (values, curves, the preset's name, the modulated values, the
// scope, whether there is something to undo) and sends every edit as a
// message. The undo history stays in the plugin, as in the native build, so
// undo behaves the same in both. The hit preview and the export are rendered
// here, from the mirrored state, with the plugin's own engine code. Settings
// and user presets live in the editor's file system, which the page keeps.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "gui/Editor.h"
#include "gui/WebWindow.h"
#include "plugin/Controller.h"
#include "plugin/Params.h"
#include "plugin/WebProtocol.h"
#include "state/Settings.h"

extern "C" {
// Posts bytes to the plugin (window.parent.postMessage in the page).
__attribute__((import_module("wclap"), import_name("send"))) void wclapJsSend(const void* data, int size);
}

namespace substrike::web {

namespace {

void send(const std::vector<uint8_t>& m) { wclapJsSend(m.data(), static_cast<int>(m.size())); }

std::vector<uint8_t> message(uint8_t type)
{
    std::vector<uint8_t> m(4, 0);
    m[0] = type;
    return m;
}

class RemoteController final : public Controller
{
public:
    RemoteController() : table_(ParamTable::get())
    {
        const size_t n = static_cast<size_t>(table_.count());
        values_.resize(n);
        for (size_t i = 0; i < n; ++i)
            values_[i] = table_.def(static_cast<int>(i)).def;
        valueEdit_.assign(n, 0);
        modulated_.assign(n, std::nan(""));
        std::random_device random;
        page_ = random() | 1u;
    }

    double paramValue(int index) const override { return values_[static_cast<size_t>(index)]; }
    void beginEdit(int index) override { sendIndex(PageBegin, index); }
    void performEdit(int index, double value) override
    {
        value = std::clamp(value, table_.minValue(index), table_.maxValue(index));
        values_[static_cast<size_t>(index)] = value;
        valueEdit_[static_cast<size_t>(index)] = ++edits_;
        auto m = message(PageSet);
        put(m, static_cast<uint32_t>(index));
        put(m, value);
        send(m);
    }
    void endEdit(int index) override { sendIndex(PageEnd, index); }

    const dsp::Curve& curve(int index) const override { return curves_[static_cast<size_t>(index)]; }
    void setCurve(int index, const dsp::Curve& c) override
    {
        if (curves_[static_cast<size_t>(index)] == c)
            return;
        curves_[static_cast<size_t>(index)] = c;
        curveEdit_[static_cast<size_t>(index)] = ++edits_;
        auto m = message(PageCurve);
        put(m, static_cast<uint32_t>(index));
        putCurve<uint32_t>(m, c);
        send(m);
    }

    void audition() override { send(message(PageAudition)); }
    double sampleRate() const override { return sampleRate_; }

    void loadDocument(const StateDocument& doc, const std::string& name) override
    {
        const size_t n = values_.size();
        for (size_t i = 0; i < n && i < doc.values.size(); ++i)
            values_[i] = doc.values[i];
        curves_ = doc.curves;
        preset_ = name;
        const uint32_t e = ++edits_;
        std::fill(valueEdit_.begin(), valueEdit_.end(), e);
        curveEdit_.fill(e);
        presetEdit_ = e;
        auto m = message(PageLoad);
        put(m, static_cast<uint32_t>(n));
        for (double v : values_)
            put(m, v);
        for (const dsp::Curve& c : curves_)
            putCurve<uint8_t>(m, c);
        putString(m, name);
        send(m);
    }
    StateDocument currentDocument() const override
    {
        StateDocument doc;
        doc.values = values_;
        doc.curves = curves_;
        doc.meta["preset"] = preset_;
        return doc;
    }
    std::string presetName() const override { return preset_; }

    double modulatedValue(int index) const override { return modulated_[static_cast<size_t>(index)]; }
    uint32_t modulationGeneration() const override { return modGeneration_; }

    int readScope(float* lo, float* hi, int max) override
    {
        int n = 0;
        while (n < max && !scope_.empty())
        {
            lo[n] = scope_.front().first;
            hi[n] = scope_.front().second;
            scope_.pop_front();
            ++n;
        }
        return n;
    }

    // The history is the plugin's; what it allows is as of its last answer.
    void checkpoint() override { send(message(PageCheckpoint)); }
    bool undo() override
    {
        send(message(PageUndo));
        return canUndo_;
    }
    bool redo() override
    {
        send(message(PageRedo));
        return canRedo_;
    }
    bool canUndo() const override { return canUndo_; }
    bool canRedo() const override { return canRedo_; }

    void poll()
    {
        auto m = message(PagePoll);
        put(m, page_);
        put(m, serial_);
        put(m, edits_);
        send(m);
    }

    // The plugin's answer to a poll (this page's or another's). An item this
    // page has edited since the answer was made keeps the page's value: the
    // edit is still on its way and the next answers bring it back.
    void status(const uint8_t* b, size_t size)
    {
        if (size < kStatusHeader || b[0] != PluginStatus)
            return;
        uint32_t page, serial, echoed;
        uint16_t nValues, nCurves, nModulated, nBins;
        std::memcpy(&page, b + 4, 4);
        if (page != page_)
            return;
        const uint8_t flags = b[1];
        std::memcpy(&serial, b + 8, 4);
        std::memcpy(&echoed, b + 12, 4);
        double rate;
        std::memcpy(&rate, b + 16, 8);
        std::memcpy(&nValues, b + 24, 2);
        std::memcpy(&nCurves, b + 26, 2);
        std::memcpy(&nModulated, b + 28, 2);
        std::memcpy(&nBins, b + 30, 2);
        if (rate > 0)
            sampleRate_ = rate;
        canUndo_ = (flags & StatusCanUndo) != 0;
        canRedo_ = (flags & StatusCanRedo) != 0;

        size_t pos = kStatusHeader;
        for (uint16_t k = 0; k < nValues; ++k)
        {
            uint16_t i;
            double v;
            if (!get(b, size, pos, i) || !get(b, size, pos, v))
                return;
            if (i < values_.size() && valueEdit_[i] <= echoed)
                values_[i] = v;
        }
        for (uint16_t k = 0; k < nCurves; ++k)
        {
            uint8_t i;
            dsp::Curve c;
            if (!get(b, size, pos, i) || !getCurve<uint8_t>(b, size, pos, c))
                return;
            if (i < dsp::kNumCurves && curveEdit_[i] <= echoed)
                curves_[i] = c;
        }
        if (flags & StatusPreset)
        {
            std::string name;
            if (!getString(b, size, pos, name))
                return;
            if (presetEdit_ <= echoed)
                preset_ = name;
        }
        if (flags & StatusModulation)
        {
            std::fill(modulated_.begin(), modulated_.end(), std::nan(""));
            for (uint16_t k = 0; k < nModulated; ++k)
            {
                uint16_t i;
                double v;
                if (!get(b, size, pos, i) || !get(b, size, pos, v))
                    return;
                if (i < modulated_.size())
                    modulated_[i] = v;
            }
            ++modGeneration_;
        }
        for (uint16_t k = 0; k < nBins; ++k)
        {
            int16_t lo, hi;
            if (!get(b, size, pos, lo) || !get(b, size, pos, hi))
                return;
            scope_.emplace_back(lo / kScopeScale, hi / kScopeScale);
        }
        // About two seconds at 48 kHz, as much as the scope shows.
        while (scope_.size() > 2048)
            scope_.pop_front();
        serial_ = serial;
    }

private:
    void sendIndex(uint8_t type, int index)
    {
        auto m = message(type);
        put(m, static_cast<uint32_t>(index));
        send(m);
    }

    const ParamTable& table_;
    uint32_t page_ = 1;
    uint32_t serial_ = 0; // 0: the plugin answers with everything
    uint32_t edits_ = 0;
    std::vector<double> values_;
    std::vector<uint32_t> valueEdit_; // the edit that last set each value here
    std::array<dsp::Curve, dsp::kNumCurves> curves_{};
    std::array<uint32_t, dsp::kNumCurves> curveEdit_{};
    std::string preset_ = "Init";
    uint32_t presetEdit_ = 0;
    std::vector<double> modulated_;
    uint32_t modGeneration_ = 0;
    std::deque<std::pair<float, float>> scope_;
    double sampleRate_ = 48000.0;
    bool canUndo_ = false, canRedo_ = false;
};

struct Ui
{
    RemoteController controller;
    std::unique_ptr<gui::Editor> editor;
    int cssW = 0, cssH = 0;
    double dpr = 1.0;
    bool relayout = false;
    double lastPoll = 0;
};

Ui* ui = nullptr;
std::vector<uint8_t> scratch;

// The editor fills the page. Its zoom is the user's Scaling, made smaller when
// the page is too small for the editor's minimum size at that scaling.
void layout()
{
    Ui& u = *ui;
    const double scaling = Settings::get().getDouble("gui_scaling", 1.0);
    const double zoom = std::max(0.5, std::min({scaling, u.cssW / static_cast<double>(gui::Editor::kMinW),
                                                u.cssH / static_cast<double>(gui::Editor::kMinH)}));
    gui::WebWindow::setSystemScale(u.dpr);
    u.editor->setScale(u.dpr * zoom);
    u.editor->setPhysicalSize(static_cast<int>(std::lround(u.cssW * u.dpr)),
                              static_cast<int>(std::lround(u.cssH * u.dpr)));
}

} // namespace

} // namespace substrike::web

using namespace substrike;
using substrike::web::ui;

extern "C" {

// A buffer the page writes into before a call (messages, key text).
__attribute__((export_name("wclap_ui_buffer"))) uint8_t* wclapUiBuffer(int size)
{
    web::scratch.resize(static_cast<size_t>(std::max(size, 1)));
    return web::scratch.data();
}

__attribute__((export_name("wclap_ui_start"))) void wclapUiStart(int cssW, int cssH, double dpr)
{
    if (ui)
        return;
    ui = new web::Ui;
    ui->cssW = cssW;
    ui->cssH = cssH;
    ui->dpr = dpr;
    gui::WebWindow::setSystemScale(dpr);

    ui->editor = std::make_unique<gui::Editor>(ui->controller);
    // The size and scaling menus ask the host for a window size (CSS pixels);
    // the page relays out with whatever size the host then gives it.
    ui->editor->requestResize = [](int pw, int ph) {
        auto m = web::message(web::PageResize);
        web::put(m, static_cast<uint32_t>(std::lround(pw / ui->dpr)));
        web::put(m, static_cast<uint32_t>(std::lround(ph / ui->dpr)));
        web::send(m);
        ui->relayout = true;
        return false;
    };

    // The user's window size from the settings, as on the desktop.
    int pw, ph;
    ui->editor->physicalSize(pw, ph);
    const int wantW = static_cast<int>(std::lround(pw / dpr)), wantH = static_cast<int>(std::lround(ph / dpr));
    ui->editor->attach(0);
    web::layout();
    if (std::abs(wantW - cssW) > 2 || std::abs(wantH - cssH) > 2)
        ui->editor->requestResize(pw, ph);
    ui->controller.poll();
}

__attribute__((export_name("wclap_ui_resize"))) void wclapUiResize(int cssW, int cssH, double dpr)
{
    if (!ui)
        return;
    ui->cssW = cssW;
    ui->cssH = cssH;
    ui->dpr = dpr;
    web::layout();
}

// Once per animation frame. Returns the serial of the newest rendered frame.
__attribute__((export_name("wclap_ui_tick"))) uint32_t wclapUiTick()
{
    if (!ui)
        return 0;
    web::Ui& u = *ui;
    const double now = gui::nowSeconds();
    if (now - u.lastPoll > 0.033)
    {
        u.lastPoll = now;
        u.controller.poll();
    }
    if (u.relayout)
    {
        u.relayout = false;
        web::layout();
    }
    u.editor->onTimer();
    return u.editor->window().frameSerial();
}

__attribute__((export_name("wclap_ui_frame"))) const uint8_t* wclapUiFrame()
{
    return ui ? ui->editor->window().frame() : nullptr;
}
__attribute__((export_name("wclap_ui_frame_width"))) int wclapUiFrameWidth()
{
    return ui ? ui->editor->window().frameWidth() : 0;
}
__attribute__((export_name("wclap_ui_frame_height"))) int wclapUiFrameHeight()
{
    return ui ? ui->editor->window().frameHeight() : 0;
}

// Pointer input in canvas pixels; buttons 1 left, 2 middle, 3 right.
__attribute__((export_name("wclap_ui_pointer_down"))) void wclapUiPointerDown(float x, float y, int button,
                                                                             unsigned mods, int clicks)
{
    if (ui)
        ui->editor->window().pointerDown(x, y, button, mods, clicks);
}
__attribute__((export_name("wclap_ui_pointer_up"))) void wclapUiPointerUp(float x, float y, int button, unsigned mods)
{
    if (ui)
        ui->editor->window().pointerUp(x, y, button, mods);
}
__attribute__((export_name("wclap_ui_pointer_move"))) void wclapUiPointerMove(float x, float y, unsigned mods)
{
    if (ui)
        ui->editor->window().pointerMove(x, y, mods);
}
__attribute__((export_name("wclap_ui_wheel"))) void wclapUiWheel(float x, float y, float notches, unsigned mods)
{
    if (ui)
        ui->editor->window().wheel(x, y, notches, mods);
}
__attribute__((export_name("wclap_ui_leave"))) void wclapUiLeave()
{
    if (ui)
        ui->editor->window().leave();
}
// The key's text (UTF-8) is in the buffer, `textLen` bytes. Returns 1 when used.
__attribute__((export_name("wclap_ui_key"))) int wclapUiKey(unsigned keysym, int textLen, unsigned mods)
{
    if (!ui)
        return 0;
    std::string text(reinterpret_cast<const char*>(web::scratch.data()),
                     static_cast<size_t>(std::clamp<int>(textLen, 0, static_cast<int>(web::scratch.size()))));
    return ui->editor->window().key(keysym, text.c_str(), mods) ? 1 : 0;
}

// A message from the plugin, `size` bytes in the buffer.
__attribute__((export_name("wclap_ui_message"))) void wclapUiMessage(int size)
{
    if (!ui || size <= 0 || static_cast<size_t>(size) > web::scratch.size())
        return;
    ui->controller.status(web::scratch.data(), static_cast<size_t>(size));
}

} // extern "C"
