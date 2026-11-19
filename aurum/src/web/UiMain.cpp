// The WCLAP's editor (ui.wasm): Aurum's own editor, widgets, preset browser
// and preset session, running in the plugin's page and drawn into a canvas
// (shared/web/wclap-ui.js). It talks to the DSP module (module.wasm, the plugin)
// through the host's webview messages, plugin/WebProtocol.h.
//
// The plugin is represented by RemoteController: it mirrors the parameter
// values the plugin reports and sends every edit as a message. Presets,
// settings and the undo history live here, in the editor's own file system
// (the page persists it), exactly as the native editor keeps them on disk.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "aurum.h"
#include "gui/Editor.h"
#include "gui/WebWindow.h"
#include "plugin/AurumPlugin.h"
#include "plugin/Params.h"
#include "plugin/PresetSession.h"
#include "plugin/WebProtocol.h"
#include "state/AudioFile.h"
#include "state/IrImport.h"
#include "state/Settings.h"
#include "util/Path.h"

extern "C" {
// Posts bytes to the plugin (window.parent.postMessage in the page).
__attribute__((import_module("wclap"), import_name("send"))) void wclapJsSend(const void* data, int size);
}

namespace aurum::web {

namespace {

void send(const std::vector<uint8_t>& m) { wclapJsSend(m.data(), static_cast<int>(m.size())); }

std::vector<uint8_t> message(uint8_t type, size_t size)
{
    std::vector<uint8_t> m(size, 0);
    m[0] = type;
    return m;
}

void put32(std::vector<uint8_t>& m, size_t at, uint32_t v) { std::memcpy(m.data() + at, &v, 4); }

class RemoteController final : public Controller
{
public:
    RemoteController() : table_(ParamTable::get())
    {
        values_.resize(static_cast<size_t>(table_.count()));
        for (int i = 0; i < table_.count(); ++i)
            values_[static_cast<size_t>(i)] = table_.def(i).def;
    }

    double paramValue(int index) const override { return values_[static_cast<size_t>(index)]; }
    void beginEdit(int index) override { sendIndex(PageBegin, index); }
    void performEdit(int index, double value) override
    {
        value = std::clamp(value, table_.minValue(index), table_.maxValue(index));
        values_[static_cast<size_t>(index)] = value;
        auto m = message(PageSet, 16);
        put32(m, 4, static_cast<uint32_t>(index));
        std::memcpy(m.data() + 8, &value, 8);
        send(m);
    }
    void endEdit(int index) override { sendIndex(PageEnd, index); }
    double sampleRate() const override { return sampleRate_; }
    double tempo() const override { return tempo_; }

    // A whole new sound: all values at once, the plugin cuts the tail.
    void applyAll(const std::vector<double>& values)
    {
        const size_t n = std::min(values.size(), values_.size());
        for (size_t i = 0; i < n; ++i)
            values_[i] = std::clamp(values[i], table_.minValue(static_cast<int>(i)), table_.maxValue(static_cast<int>(i)));
        auto m = message(PageApply, 8 + 8 * values_.size());
        put32(m, 4, static_cast<uint32_t>(values_.size()));
        std::memcpy(m.data() + 8, values_.data(), 8 * values_.size());
        send(m);
    }

    std::vector<double> values() const { return values_; }

    // The plugin's answer to a poll. Returns true when it carried values.
    bool status(const uint8_t* b, size_t size, std::string& name, std::string& path)
    {
        if (size < kStatusHeader || b[0] != PluginStatus)
            return false;
        uint16_t count;
        std::memcpy(&count, b + 2, 2);
        std::memcpy(&serial_, b + 4, 4);
        std::memcpy(&sampleRate_, b + 8, 8);
        std::memcpy(&tempo_, b + 16, 8);
        if (!b[1] || count != values_.size() || size < kStatusHeader + 8u * count)
            return false;
        std::memcpy(values_.data(), b + kStatusHeader, 8u * count);
        size_t pos = kStatusHeader + 8u * count;
        getString(b, size, pos, name);
        getString(b, size, pos, path);
        return true;
    }

    void poll()
    {
        auto m = message(PagePoll, 8);
        put32(m, 4, serial_);
        send(m);
    }

private:
    void sendIndex(uint8_t type, int index)
    {
        auto m = message(type, 8);
        put32(m, 4, static_cast<uint32_t>(index));
        send(m);
    }

    const ParamTable& table_;
    std::vector<double> values_;
    uint32_t serial_ = 0; // 0: the plugin answers with everything
    double sampleRate_ = 48000.0;
    double tempo_ = 120.0;
};

struct Ui
{
    RemoteController controller;
    std::unique_ptr<PresetSession> session;
    std::unique_ptr<gui::Editor> editor;
    int cssW = 0, cssH = 0;
    double dpr = 1.0;
    bool relayout = false;
    double lastPoll = 0;
    // The preset the plugin's state names, and the one last told to it.
    std::string pluginName, pluginPath, sentName, sentPath;
    bool heardPlugin = false;
};

Ui* ui = nullptr;
std::vector<uint8_t> scratch;

// The editor fills the page. Its zoom is the user's Scaling, made smaller when
// the page is too small for the editor's minimum size at that scaling.
void layout()
{
    Ui& u = *ui;
    const double scaling = Settings::get().getDouble("gui_scaling", 1.0);
    const double zoom = std::max(0.25, std::min({scaling, u.cssW / static_cast<double>(gui::Editor::kMinW),
                                                 u.cssH / static_cast<double>(gui::Editor::kMinH)}));
    gui::WebWindow::setSystemScale(u.dpr);
    u.editor->setScale(u.dpr * zoom);
    u.editor->setPhysicalSize(static_cast<int>(std::lround(u.cssW * u.dpr)),
                              static_cast<int>(std::lround(u.cssH * u.dpr)));
}

void importIr(const std::string& path)
{
    Ui& u = *ui;
    AudioData audio;
    std::string err;
    StateDocument doc;
    IrReport report;
    if (!readAudioFile(path, audio, &err) ||
        !importImpulseResponse(audio, u.controller.values(), doc, &report, &err))
    {
        u.editor->notify("IR import failed: " + err);
        return;
    }
    const std::string name = fromPath(toPath(path).stem());
    u.session->applyDocument(doc, "IR: " + name, "");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Imported %s (T60 %.2f s)", name.c_str(), report.t60Mid);
    u.editor->notify(buf);
}

} // namespace

} // namespace aurum::web

using namespace aurum;
using aurum::web::ui;

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

    ui->session = std::make_unique<PresetSession>([](const std::vector<double>& v) { ui->controller.applyAll(v); },
                                                  [] { return ui->controller.values(); });
    ui->editor = std::make_unique<gui::Editor>(ui->controller);
    ui->editor->setPresetSession(ui->session.get());
    ui->editor->onImportIr = [](const std::string& path) { web::importIr(path); };
    ui->session->onLoaded = [] { ui->editor->pushHistory(); };
    // The size and scaling menus ask the host for a window size (CSS pixels);
    // the page relays out with whatever size the host then gives it.
    ui->editor->requestResize = [](int pw, int ph) {
        auto m = web::message(web::PageResize, 12);
        web::put32(m, 4, static_cast<uint32_t>(std::lround(pw / ui->dpr)));
        web::put32(m, 8, static_cast<uint32_t>(std::lround(ph / ui->dpr)));
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
    // Tell the plugin which preset is loaded, for its state.
    if (u.heardPlugin && (u.session->name() != u.sentName || u.session->path() != u.sentPath))
    {
        u.sentName = u.session->name();
        u.sentPath = u.session->path();
        auto m = web::message(web::PagePreset, 8 + u.sentName.size() + u.sentPath.size());
        size_t pos = 4;
        pos += web::putString(m.data() + pos, u.sentName);
        web::putString(m.data() + pos, u.sentPath);
        web::send(m);
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
    web::Ui& u = *ui;
    std::string name, path;
    if (!u.controller.status(web::scratch.data(), static_cast<size_t>(size), name, path))
        return;
    // A preset the plugin's state names (a project was opened): the session
    // follows, so the browser shows it and Save writes to it.
    const bool first = !u.heardPlugin;
    u.heardPlugin = true;
    if (first || name != u.pluginName || path != u.pluginPath)
    {
        u.pluginName = name;
        u.pluginPath = path;
        if (!name.empty() && (name != u.session->name() || path != u.session->path()))
            u.session->restore(name, path, u.controller.values());
        u.sentName = u.session->name();
        u.sentPath = u.session->path();
        if (first)
            u.editor->resetHistory();
    }
}

} // extern "C"
