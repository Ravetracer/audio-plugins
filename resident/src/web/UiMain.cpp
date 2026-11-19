// The WCLAP's editor (ui.wasm): ReSIDent's own editor -- the C64 screen, the
// keyboard, the media, the machine and CC pages, and the ROM page -- running
// in the plugin's page and drawn into a canvas (shared/web/wclap-ui.js). The
// machine runs in the plugin (module.wasm, in the host's audio worklet); the
// editor reaches it only through the webview messages, plugin/WebProtocol.h.
//
// RemoteLink stands in for the plugin: it mirrors what the plugin reports
// (settings, parameter values, the CC map, the media, the screen) and turns
// every action into a message. Files the user picks or drops are in the
// editor's file system (/upload); their bytes go to the plugin in chunks.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "emu/Vic.h"
#include "engine/Resources.h"
#include "gui/Editor.h"
#include "gui/WebWindow.h"
#include "plugin/Params.h"
#include "plugin/WebProtocol.h"

extern "C" {
// Posts bytes to the plugin (window.parent.postMessage in the page).
__attribute__((import_module("wclap"), import_name("send"))) void wclapJsSend(const void* data, int size);
// Hands the user a file.
__attribute__((import_module("wclap"), import_name("download"))) void wclapJsDownload(const char* name, int nameLen,
                                                                                       const void* data, int size);
}

namespace resident::web {

namespace {

void send(const std::vector<uint8_t>& m) { wclapJsSend(m.data(), static_cast<int>(m.size())); }

std::vector<uint8_t> message(uint8_t type, uint8_t sub = 0)
{
    return {type, sub, 0, 0};
}

std::string fileNameOf(const std::string& path) { return path.substr(path.find_last_of("/\\") + 1); }

class RemoteLink final : public gui::PluginLink
{
public:
    RemoteLink() : params_(parameterTable())
    {
        values_.resize(params_.size());
        for (size_t i = 0; i < params_.size(); ++i) {
            values_[i] = params_[i].def;
            index_[params_[i].id] = i;
        }
        frame_.assign(static_cast<size_t>(emu::Vic::kWidth) * emu::Vic::kHeight, 0);
    }

    bool remote() const override { return true; }
    bool takeNotice(std::string& text, bool& error) override
    {
        if (notices_.empty())
            return false;
        text = notices_.front().first;
        error = notices_.front().second;
        notices_.erase(notices_.begin());
        return true;
    }

    // ------------------------------------------------------------ screen, indicators
    uint64_t frameSerial() override { return frameSerial_; }
    uint64_t copyFrame(uint8_t* dst) override
    {
        std::memcpy(dst, frame_.data(), frame_.size());
        return frameSerial_;
    }
    void setScreenWanted(bool on) override { screenWanted_ = on; }
    uint32_t midiInCount() override { return midiIn_; }
    uint32_t midiOutCount() override { return midiOut_; }
    bool driveLed() override { return flags_ & StatusDriveLed; }
    bool warping() override { return flags_ & StatusWarping; }
    void setWarp(bool on) override { command(on ? CommandWarpOn : CommandWarpOff); }

    // ------------------------------------------------------------ the machine page
    const MachineSettings& settings() override { return settings_; }
    void applySettings(const MachineSettings& s) override
    {
        settings_ = s;
        auto m = message(PageSettings);
        const std::string text = s.toText();
        m.insert(m.end(), text.begin(), text.end());
        send(m);
    }
    double paramValue(uint32_t id) override
    {
        const auto it = index_.find(id);
        return it == index_.end() ? 0.0 : values_[it->second];
    }
    void beginParam(uint32_t id) override { param(0, id, 0.0); }
    void setParam(uint32_t id, double value) override
    {
        const auto it = index_.find(id);
        if (it == index_.end())
            return;
        value = std::clamp(value, params_[it->second].lo, params_[it->second].hi);
        values_[it->second] = value;
        param(1, id, value);
    }
    void endParam(uint32_t id) override { param(2, id, 0.0); }

    // ------------------------------------------------------------ CC maps
    const CcMap& ccMap() override { return ccMap_; }
    void setCcMap(const CcMap& map) override
    {
        ccMap_ = map;
        ccText_ = map.toText();
        ++ccSerial_;
        auto m = message(PageCcMap);
        m.insert(m.end(), ccText_.begin(), ccText_.end());
        send(m);
    }
    void setCcSlot(int slot, const CcSlot& s) override
    {
        if (slot < 0 || slot >= kCcSlots)
            return;
        CcMap map = ccMap_;
        map.slots[slot] = s;
        setCcMap(map);
    }
    uint32_t ccMapSerial() override { return ccSerial_; }

    // ------------------------------------------------------------ ROMs
    bool hasRoms() override { return flags_ & StatusHasRoms; }
    bool hasDriveRom() override { return flags_ & StatusHasDriveRom; }
    bool rescanRoms() override { return hasRoms(); }
    std::string romReport() override { return romReport_; }
    unsigned romsPresent() override { return romsInState_; }
    void uploadRom(const std::string& name, const std::vector<uint8_t>& data) override { upload(UploadRom, name, data); }

    // ------------------------------------------------------------ input
    void key(uint8_t matrixPos, bool down) override { input(InputKey, matrixPos, down ? 1 : 0); }
    void joystick(int port, uint8_t bits) override { input(InputJoystick, static_cast<uint8_t>(port), bits); }
    void restoreKey() override { input(InputRestore, 0, 0); }
    void mouseMove(int port, int dx, int dy) override
    {
        input(InputMouse, static_cast<uint8_t>(port), 0, static_cast<int16_t>(dx), static_cast<int16_t>(dy));
    }
    void paddles(int port, uint8_t a, uint8_t b) override { input(InputPaddles, static_cast<uint8_t>(port), 0, a, b); }
    void deviceButtons(int port, uint8_t bits) override { input(InputButtons, static_cast<uint8_t>(port), bits); }

    // ------------------------------------------------------------ buttons
    void resetButton() override { command(CommandReset); }
    void powerCycle() override { command(CommandPower); }
    void freezeButton() override { command(CommandFreeze); }
    bool cartridgeFreezable() override { return flags_ & StatusFreezable; }
    void ejectDisk() override { command(CommandEjectDisk); }
    void ejectCartridge() override { command(CommandEjectCartridge); }

    // ------------------------------------------------------------ media
    bool loadFile(const std::string& path, std::string& error) override
    {
        std::vector<uint8_t> data;
        if (!readFile(path, data)) {
            error = "cannot read " + fileNameOf(path);
            return false;
        }
        // A ROM image loaded or dropped as a program goes to the project's ROMs.
        upload(identifyRom(data) != RomKind::None ? UploadRom : UploadLoad, fileNameOf(path), data);
        return true;
    }
    std::string loadNotice() override { return {}; }
    bool attachDiskFile(const std::string& path, std::string& error) override
    {
        std::vector<uint8_t> data;
        if (!readFile(path, data)) {
            error = "cannot read " + fileNameOf(path);
            return false;
        }
        upload(UploadDisk, fileNameOf(path), data);
        return true;
    }
    bool diskPresent() override { return flags_ & StatusDisk; }
    uint32_t diskSerial() override { return diskInserts_; }
    std::string diskTitle() override { return diskTitle_; }
    std::vector<DiskEntry> diskDirectory() override { return files_; }
    bool startFromDisk(const std::string& petsciiName, std::string&) override
    {
        command(CommandStartFromDisk, petsciiName);
        return true;
    }
    bool exportDisk(const std::string& path, std::string&) override
    {
        command(CommandExportDisk, fileNameOf(path));
        return true;
    }
    std::string mediaSummary() override { return summary_; }

    // ------------------------------------------------------------ the page's side
    void poll()
    {
        std::vector<uint8_t> m(kPollSize, 0);
        m[0] = PagePoll;
        m[1] = screenWanted_ ? 1 : 0;
        std::memcpy(m.data() + 4, serials_, sizeof(serials_));
        send(m);
    }

    // A message from the plugin. Returns true when something the pages draw
    // (beyond the screen) changed.
    bool receive(const uint8_t* b, size_t size)
    {
        if (size >= 4 && b[0] == PluginDiskImage) {
            size_t pos = 4;
            std::string name;
            if (getString(b, size, pos, name))
                wclapJsDownload(name.data(), static_cast<int>(name.size()), b + pos, static_cast<int>(size - pos));
            return false;
        }
        if (size < kStatusHeader || b[0] != PluginStatus)
            return false;
        bool changed = b[1] != flags_ || b[3] != romsInState_;
        flags_ = b[1];
        romsInState_ = b[3];
        midiIn_ = get<uint32_t>(b, 4);
        midiOut_ = get<uint32_t>(b, 8);
        const uint16_t sections = get<uint16_t>(b, 12);
        size_t pos = kStatusHeader;
        for (int s = 0; s < sections && pos + kSectionHeader <= size; ++s) {
            const uint8_t id = b[pos];
            const uint32_t serial = get<uint32_t>(b, pos + 4);
            const uint32_t n = get<uint32_t>(b, pos + 8);
            const uint8_t* p = b + pos + kSectionHeader;
            pos += kSectionHeader + n;
            if (pos > size)
                break;
            switch (id) {
            case SectionValues: {
                const size_t count = std::min<size_t>(n / 8, values_.size());
                std::memcpy(values_.data(), p, count * 8);
                serials_[0] = serial;
                changed = true;
                break;
            }
            case SectionSettings: {
                MachineSettings s;
                s.fromText(std::string(reinterpret_cast<const char*>(p), n));
                settings_ = s;
                serials_[1] = serial;
                changed = true;
                break;
            }
            case SectionCcMap: {
                std::string text(reinterpret_cast<const char*>(p), n);
                if (text != ccText_) {
                    ccMap_ = CcMap::fromText(text);
                    ccText_ = std::move(text);
                    ++ccSerial_;
                }
                serials_[2] = serial;
                changed = true;
                break;
            }
            case SectionMedia:
                media(p, n);
                serials_[3] = serial;
                changed = true;
                break;
            case SectionNotice:
                // The notice the plugin had before this page opened is old news.
                if (heard_ && n > 0)
                    notices_.push_back({std::string(reinterpret_cast<const char*>(p + 1), n - 1), p[0] != 0});
                serials_[4] = serial;
                break;
            case SectionScreen:
                screen(p, n, serial);
                break;
            default: break;
            }
        }
        heard_ = true;
        return changed;
    }

private:
    void param(uint8_t what, uint32_t id, double value)
    {
        auto m = message(PageParam, what);
        put<uint32_t>(m, id);
        put<double>(m, value);
        send(m);
    }
    void input(uint8_t kind, uint8_t a, uint8_t b, int16_t c = 0, int16_t d = 0)
    {
        std::vector<uint8_t> m{PageInput, kind, a, b};
        put<int16_t>(m, c);
        put<int16_t>(m, d);
        send(m);
    }
    void command(uint8_t c, const std::string& arg = {})
    {
        auto m = message(PageCommand, c);
        putString(m, arg);
        send(m);
    }
    void upload(uint8_t kind, const std::string& name, const std::vector<uint8_t>& data)
    {
        const uint32_t total = static_cast<uint32_t>(data.size());
        for (uint32_t off = 0; off == 0 || off < total; off += kUploadChunk) {
            const uint32_t n = std::min(kUploadChunk, total - off);
            auto m = message(PageUpload, kind);
            put<uint32_t>(m, total);
            put<uint32_t>(m, off);
            putString(m, name);
            m.insert(m.end(), data.begin() + off, data.begin() + off + n);
            send(m);
            if (total == 0)
                break;
        }
    }
    void media(const uint8_t* p, size_t n)
    {
        if (n < 4)
            return;
        diskInserts_ = get<uint32_t>(p, 0);
        size_t pos = 4;
        getString(p, n, pos, summary_);
        getString(p, n, pos, diskTitle_);
        getString(p, n, pos, romReport_);
        files_.clear();
        if (pos + 2 > n)
            return;
        const uint16_t count = get<uint16_t>(p, pos);
        pos += 2;
        for (int i = 0; i < count; ++i) {
            DiskEntry e;
            if (!getString(p, n, pos, e.petscii) || !getString(p, n, pos, e.display) || !getString(p, n, pos, e.type) ||
                pos + 2 > n)
                break;
            e.blocks = get<uint16_t>(p, pos);
            pos += 2;
            files_.push_back(std::move(e));
        }
    }
    void screen(const uint8_t* p, size_t n, uint32_t serial)
    {
        constexpr int W = emu::Vic::kWidth, H = emu::Vic::kHeight;
        if (n < 8)
            return;
        const uint32_t base = get<uint32_t>(p, 0);
        if (base != 0 && base != serials_[5])
            return; // lines against a frame this page does not have: the next poll asks again
        const uint16_t lines = get<uint16_t>(p, 4);
        size_t q = 8;
        for (int i = 0; i < lines; ++i) {
            if (q + 6 > n)
                return;
            const uint16_t y = get<uint16_t>(p, q);
            const uint8_t mode = p[q + 2];
            const uint16_t len = get<uint16_t>(p, q + 4);
            if (y >= H || q + 6 + len > n || !decodeLine(p + q + 6, len, mode, &frame_[static_cast<size_t>(y) * W], W)) {
                serials_[5] = 0; // something is off: ask for a whole screen
                return;
            }
            q += 6u + len;
        }
        serials_[5] = serial;
        ++frameSerial_;
    }

    const std::vector<ParamInfo> params_;
    std::map<uint32_t, size_t> index_;
    std::vector<double> values_;
    MachineSettings settings_;
    CcMap ccMap_;
    std::string ccText_;
    uint32_t ccSerial_ = 1;
    uint8_t flags_ = 0, romsInState_ = 0;
    uint32_t midiIn_ = 0, midiOut_ = 0;
    std::string summary_, diskTitle_, romReport_;
    std::vector<DiskEntry> files_;
    uint32_t diskInserts_ = 0;
    std::vector<std::pair<std::string, bool>> notices_;
    bool heard_ = false;
    bool screenWanted_ = false;
    std::vector<uint8_t> frame_;
    uint64_t frameSerial_ = 0;
    uint32_t serials_[6] = {}; // values, settings, CC map, media, notice, screen
};

// The page's keys arrive as X11 keysyms; the editor's keyboard mapping wants
// the desktop's evdev codes as well. Named keys have theirs; printable ones
// take the code of their key on a US layout, so Symbolic mapping (by the
// character) works for every layout, and Positional for a US one. A
// character without a US key gets a code of its own past the real ones.
int evdevCode(unsigned keysym)
{
    switch (keysym) {
    case 0xff1b: return 1;    // Escape
    case 0xff08: return 14;   // BackSpace
    case 0xff09: return 15;   // Tab
    case 0xff0d: return 28;   // Return
    case 0xffe3: return 29;   // Control
    case 0xffe1: return 42;   // Shift
    case 0xffe9: return 100;  // Alt: fire for the cursor-key joystick, nothing else
    case 0xffe5: return 58;   // Caps Lock
    case 0xff50: return 102;  // Home
    case 0xff52: return 103;  // Up
    case 0xff55: return 104;  // Page Up (RESTORE)
    case 0xff51: return 105;  // Left
    case 0xff53: return 106;  // Right
    case 0xff57: return 107;  // End
    case 0xff54: return 108;  // Down
    case 0xff56: return 109;  // Page Down
    case 0xff63: return 110;  // Insert
    case 0xffff: return 111;  // Delete
    default: break;
    }
    if (keysym >= 0xffbe && keysym <= 0xffc7) // F1-F10
        return 59 + static_cast<int>(keysym - 0xffbe);
    if (keysym >= 'A' && keysym <= 'Z')
        keysym += 'a' - 'A';
    static const char* const rows[] = {"1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "zxcvbnm,./"};
    static const char* const shifted[] = {"!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"~", "ZXCVBNM<>?"};
    static const int first[] = {2, 16, 30, 44};
    for (int r = 0; r < 4; ++r)
        for (int i = 0; rows[r][i]; ++i)
            if (keysym == static_cast<unsigned char>(rows[r][i]) || keysym == static_cast<unsigned char>(shifted[r][i]))
                return first[r] + i;
    if (keysym == '\\' || keysym == '|')
        return 43;
    if (keysym == ' ')
        return 57;
    return 0x10000 + static_cast<int>(keysym);
}

bool isModifierCode(int code) { return code == 29 || code == 42 || code == 54 || code == 58 || code == 100; }

struct Ui
{
    RemoteLink link;
    std::unique_ptr<gui::Editor> editor;
    int cssW = 0, cssH = 0;
    double dpr = 1.0;
    double lastPoll = 0;
    std::set<int> held; // keys down, by the code they went down with
};

Ui* ui = nullptr;
std::vector<uint8_t> scratch;

// The editor fills the page, zoomed to fit its natural size.
void layout()
{
    Ui& u = *ui;
    const double zoom = std::clamp(std::min(u.cssW / static_cast<double>(gui::Editor::kBaseW),
                                            u.cssH / static_cast<double>(gui::Editor::kBaseH)),
                                   0.5, 4.0);
    gui::WebWindow::setSystemScale(u.dpr);
    u.editor->setScale(u.dpr * zoom);
    u.editor->setPhysicalSize(static_cast<int>(std::lround(u.cssW * u.dpr)),
                              static_cast<int>(std::lround(u.cssH * u.dpr)));
}

} // namespace

} // namespace resident::web

using namespace resident;
using resident::web::ui;

extern "C" {

// A buffer the page writes into before a call (messages, key text, paths).
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
    ui->editor = std::make_unique<gui::Editor>(ui->link);
    ui->editor->attach(0);
    web::layout();
    ui->link.poll();
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
    if (now - u.lastPoll > 0.033) {
        u.lastPoll = now;
        u.link.poll();
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

// A key goes down: to the C64 when it has the keyboard (as an evdev code with
// its keysym, as the desktop windows report it), else to the editor's
// widgets. The key's text (UTF-8) is in the buffer, `textLen` bytes. Returns
// 1 when used.
__attribute__((export_name("wclap_ui_key"))) int wclapUiKey(unsigned keysym, int textLen, unsigned mods)
{
    if (!ui)
        return 0;
    std::string text(reinterpret_cast<const char*>(web::scratch.data()),
                     static_cast<size_t>(std::clamp<int>(textLen, 0, static_cast<int>(web::scratch.size()))));
    auto& win = ui->editor->window();
    const int code = web::evdevCode(keysym);
    ui->held.insert(code);
    win.rawKey(code, true, keysym);
    return win.key(keysym, text.c_str(), mods) ? 1 : 0;
}

// A key comes up. Its keysym may not be the one it went down with (Shift let
// go first: '"' down, '2' up on some layouts); a release that matches no
// held key lets go of every held character key.
__attribute__((export_name("wclap_ui_key_up"))) int wclapUiKeyUp(unsigned keysym, unsigned)
{
    if (!ui)
        return 0;
    auto& win = ui->editor->window();
    const int code = web::evdevCode(keysym);
    if (ui->held.erase(code)) {
        win.rawKey(code, false, 0);
    } else {
        for (auto it = ui->held.begin(); it != ui->held.end();) {
            if (web::isModifierCode(*it)) {
                ++it;
                continue;
            }
            win.rawKey(*it, false, 0);
            it = ui->held.erase(it);
        }
    }
    return ui->editor->c64HasKeyboard() ? 1 : 0;
}

// Files dropped on the editor, copied by the page into /upload: their paths,
// one per line, in the buffer.
__attribute__((export_name("wclap_ui_files_dropped"))) void wclapUiFilesDropped(int size, float, float)
{
    if (!ui || size <= 0 || static_cast<size_t>(size) > web::scratch.size())
        return;
    const std::string list(reinterpret_cast<const char*>(web::scratch.data()), static_cast<size_t>(size));
    std::vector<std::string> paths;
    size_t at = 0;
    while (at < list.size()) {
        const auto nl = list.find('\n', at);
        const std::string p = list.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (!p.empty())
            paths.push_back(p);
        if (nl == std::string::npos)
            break;
        at = nl + 1;
    }
    ui->editor->window().filesDropped(paths);
}

// A message from the plugin, `size` bytes in the buffer.
__attribute__((export_name("wclap_ui_message"))) void wclapUiMessage(int size)
{
    if (!ui || size <= 0 || static_cast<size_t>(size) > web::scratch.size())
        return;
    if (ui->link.receive(web::scratch.data(), static_cast<size_t>(size)))
        ui->editor->markDirty();
}

} // extern "C"
