#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Keys.h"
#include "engine/Resources.h"
#include "plugin/CcMap.h"
#include "plugin/SidMidiPlugin.h"
#include "sidmidi.h"

namespace sidmidi::gui {

namespace {

std::string hex4(unsigned v)
{
    char b[16];
    std::snprintf(b, sizeof(b), "$%04X", v & 0xffffu);
    return b;
}

std::string dbText(double v)
{
    if (v <= -59.9)
        return "-inf";
    char b[16];
    std::snprintf(b, sizeof(b), "%+.1f dB", v);
    return b;
}

std::string panText(double v)
{
    if (std::fabs(v) < 0.005)
        return "C";
    char b[16];
    std::snprintf(b, sizeof(b), "%d %s", static_cast<int>(std::lround(std::fabs(v) * 100)), v < 0 ? "L" : "R");
    return b;
}

} // namespace

// ================================================================= ScreenView

class ScreenView : public Widget
{
public:
    explicit ScreenView(Editor& ed) : ed_(ed)
    {
        frame_.assign(static_cast<size_t>(emu::Vic::kWidth) * emu::Vic::kHeight, 0);
        surface_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, emu::Vic::kWidth, emu::Vic::kHeight);
    }
    ~ScreenView() override { cairo_surface_destroy(surface_); }

    // Pulls a new frame when there is one. True when the picture changed.
    bool update()
    {
        auto& engine = ed_.plugin().engine();
        const std::string& key = ed_.plugin().settings().palette;
        if (engine.frameSerial() == serial_ && key == paletteKey_)
            return false;
        serial_ = engine.copyFrame(frame_.data());
        paletteKey_ = key;
        const Palette& pal = palette(key);
        cairo_surface_flush(surface_);
        unsigned char* data = cairo_image_surface_get_data(surface_);
        const int stride = cairo_image_surface_get_stride(surface_);
        for (int y = 0; y < emu::Vic::kHeight; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(data + y * stride);
            const uint8_t* src = &frame_[static_cast<size_t>(y) * emu::Vic::kWidth];
            for (int x = 0; x < emu::Vic::kWidth; ++x)
                row[x] = pal.rgb[src[x] & 15];
        }
        cairo_surface_mark_dirty(surface_);
        return true;
    }

    Rect pictureRect() const
    {
        const float s = std::min(bounds_.w / emu::Vic::kWidth, bounds_.h / emu::Vic::kHeight);
        const float w = emu::Vic::kWidth * s, h = emu::Vic::kHeight * s;
        return {bounds_.x + (bounds_.w - w) * 0.5f, bounds_.y + (bounds_.h - h) * 0.5f, w, h};
    }

    void paintLive(cairo_t* cr)
    {
        if (!isVisible())
            return;
        const Rect r = pictureRect();
        cairo_save(cr);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_clip(cr);
        cairo_translate(cr, r.x, r.y);
        cairo_scale(cr, r.w / emu::Vic::kWidth, r.h / emu::Vic::kHeight);
        cairo_set_source_surface(cr, surface_, 0, 0);
        double dx = 1, dy = 1;
        cairo_user_to_device_distance(cr, &dx, &dy);
        // Whole device pixels per C64 pixel stay crisp; anything else is smoothed.
        const bool whole = std::fabs(dx - std::round(dx)) < 0.01;
        cairo_pattern_set_filter(cairo_get_source(cr), whole ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        const Color edge = ed_.c64HasKeyboard() ? theme::accent : theme::outline;
        strokeRounded(cr, {r.x - 2, r.y - 2, r.w + 4, r.h + 4}, 3, edge, ed_.c64HasKeyboard() ? 2.0f : 1.0f);
    }

    bool mouseDown(const MouseEvent& e) override
    {
        ed_.giveKeyboardToC64(true);
        buttons_ |= e.button == 1 ? 1 : e.button == 3 ? 2 : 0;
        pointer(e, true);
        return true;
    }
    void mouseUp(const MouseEvent& e) override
    {
        buttons_ &= ~(e.button == 1 ? 1 : e.button == 3 ? 2 : 0);
        if (e.button != 1)
            buttons_ = 0; // a captured drag ends with every button up
        pointer(e, true);
    }
    void mouseMove(const MouseEvent& e) override { pointer(e, false); }
    void mouseDrag(const MouseEvent& e) override { pointer(e, false); }
    void mouseLeave() override
    {
        buttons_ = 0;
        hasLast_ = false;
        sendButtons();
    }
    bool mouseWheel(const MouseEvent&) override { return false; }
    std::string tooltip() const override
    {
        return ed_.c64HasKeyboard() ? std::string() : "Click to type on the C64 (Esc = RUN/STOP, Page Up = RESTORE)";
    }

private:
    // The host mouse over the screen drives the 1351 or the paddles in
    // whichever port has one: movement in C64 pixels (the 1351 counts about
    // one unit per pixel), paddles by the pointer's position.
    void pointer(const MouseEvent& e, bool buttonsChanged)
    {
        auto& pl = ed_.plugin();
        const Rect r = pictureRect();
        const float px = (e.x - r.x) * emu::Vic::kWidth / std::max(1.0f, r.w);
        const float py = (e.y - r.y) * emu::Vic::kHeight / std::max(1.0f, r.h);
        for (int port = 0; port < 2; ++port) {
            const auto dev = pl.settings().portDevice[port];
            if (dev == emu::PortDevice::Mouse1351 && hasLast_) {
                const int dx = static_cast<int>(std::lround(px - lastX_));
                const int dy = static_cast<int>(std::lround(py - lastY_));
                if (dx || dy)
                    pl.mouseMove(port, dx, dy);
            } else if (dev == emu::PortDevice::Paddles) {
                const float tx = std::clamp(px / emu::Vic::kWidth, 0.0f, 1.0f);
                const float ty = std::clamp(py / emu::Vic::kHeight, 0.0f, 1.0f);
                // Turned right (clockwise), a paddle's value falls.
                pl.paddles(port, static_cast<uint8_t>(std::lround((1.0f - tx) * 255.0f)),
                           static_cast<uint8_t>(std::lround((1.0f - ty) * 255.0f)));
            }
        }
        // Whole units only, so slow movements still add up.
        if (!hasLast_) {
            lastX_ = px;
            lastY_ = py;
        } else {
            lastX_ += std::round(px - lastX_);
            lastY_ += std::round(py - lastY_);
        }
        hasLast_ = true;
        if (buttonsChanged)
            sendButtons();
    }
    void sendButtons()
    {
        auto& pl = ed_.plugin();
        for (int port = 0; port < 2; ++port) {
            const auto dev = pl.settings().portDevice[port];
            uint8_t bits = 0;
            if (dev == emu::PortDevice::Mouse1351)
                bits = static_cast<uint8_t>(((buttons_ & 1) ? emu::JoyFire : 0) | ((buttons_ & 2) ? emu::JoyUp : 0));
            else if (dev == emu::PortDevice::Paddles)
                bits = static_cast<uint8_t>(((buttons_ & 1) ? emu::JoyLeft : 0) | ((buttons_ & 2) ? emu::JoyRight : 0));
            if (dev != emu::PortDevice::Joystick)
                pl.deviceButtons(port, bits);
        }
    }

    int buttons_ = 0;
    float lastX_ = 0, lastY_ = 0;
    bool hasLast_ = false;
    Editor& ed_;
    std::vector<uint8_t> frame_;
    cairo_surface_t* surface_;
    uint64_t serial_ = ~0ull;
    std::string paletteKey_;
};

// ================================================================= RomPanel

// In place of the screen while the ROMs are missing: what is needed, where it
// goes, and two buttons. The editor looks again every two seconds by itself.
class RomPanel : public Widget
{
public:
    explicit RomPanel(Editor& ed) : ed_(ed)
    {
        openButton_ = add<Button>("Open the folder", [] { openFolder(romFolder()); });
        againButton_ = add<Button>("Look again", [this] {
            if (ed_.plugin().rescanRoms())
                ed_.notify("ROMs found: the C64 is starting");
            else
                ed_.notify("Still missing: " + ed_.plugin().romReport(), true);
        });
    }

    void layout() override
    {
        const float x = bounds_.x + 28, y = bounds_.bottom() - 56;
        openButton_->setBounds({x, y, 130, 28});
        againButton_->setBounds({x + 142, y, 100, 28});
    }

    void paint(cairo_t* cr) override
    {
        fillRounded(cr, bounds_, 6, theme::panel);
        strokeRounded(cr, bounds_, 6, theme::outline);
        float y = bounds_.y + 30;
        const float x = bounds_.x + 28, w = bounds_.w - 56;
        setFont(cr, 17.0f, true);
        drawText(cr, "The C64 ROMs are needed", {x, y, w, 24}, Align::Left, theme::text);
        y += 36;
        setFont(cr, 11.5f);
        const std::string text = romInstructions();
        size_t at = 0;
        bool folderShown = false;
        while (at < text.size()) {
            const auto nl = text.find('\n', at);
            const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? text.size() : nl + 1;
            const bool mono = line.rfind("  -", 0) == 0 || line.rfind("    ", 0) == 0;
            if (mono) {
                cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, 11.0);
            } else {
                setFont(cr, 11.5f);
            }
            drawText(cr, line, {x, y, w, 18}, Align::Left, mono ? theme::text : theme::textDim);
            y += line.empty() ? 10 : 19;
            if (!folderShown && line.find("into this folder") != std::string::npos) {
                // The folder itself, right under the sentence that asks for it.
                setFont(cr, 12.0f, true);
                drawText(cr, romFolder(), {x + 16, y, w - 16, 20}, Align::Left, theme::accentBright);
                y += 28;
                folderShown = true;
            }
        }
        const std::string report = ed_.plugin().romReport();
        if (!report.empty()) {
            setFont(cr, 11.0f);
            drawText(cr, "Missing now: " + report, {x, bounds_.bottom() - 84, w, 18}, Align::Left, theme::red);
        }
    }

private:
    Editor& ed_;
    Button* openButton_;
    Button* againButton_;
};

// ================================================================= MachinePage

class MachinePage : public Widget
{
public:
    explicit MachinePage(Editor& ed) : ed_(ed) {}

    // Rebuilds the controls when the SID count changed.
    void sync()
    {
        const int n = ed_.plugin().settings().sids.count;
        if (n != builtCount_)
            build();
    }

    void build()
    {
        clearChildren();
        auto& pl = ed_.plugin();
        builtCount_ = pl.settings().sids.count;

        auto edit = [&pl](auto fn) {
            MachineSettings s = pl.settings();
            fn(s);
            pl.applySettings(s);
        };

        add<Choice>("Video", std::vector<std::string>{"PAL (6569)", "NTSC (6567R8)"},
                    [&pl] { return static_cast<int>(pl.settings().standard); },
                    [edit](int i) { edit([i](MachineSettings& s) { s.standard = static_cast<emu::VideoStandard>(i); }); })
            ->setTooltip("The C64's video standard: its clock, and so the SID's pitch");
        std::vector<std::string> palNames;
        for (const auto& p : palettes())
            palNames.push_back(p.name);
        add<Choice>("Palette", palNames,
                    [&pl] {
                        const auto& list = palettes();
                        for (size_t i = 0; i < list.size(); ++i)
                            if (list[i].key == pl.settings().palette)
                                return static_cast<int>(i);
                        return 0;
                    },
                    [edit](int i) {
                        edit([i](MachineSettings& s) { s.palette = palettes()[static_cast<size_t>(i)].key; });
                    });
        add<Choice>("MIDI clock", std::vector<std::string>{"Off", "From the host"},
                    [&pl] { return pl.settings().sendClock ? 1 : 0; },
                    [edit](int i) { edit([i](MachineSettings& s) { s.sendClock = i == 1; }); })
            ->setTooltip("Send MIDI clock, start, stop and song position from the host's transport");
        add<Choice>("Drive 8", std::vector<std::string>{"Virtual", "True 1541", "None"},
                    [&pl] { return static_cast<int>(pl.settings().driveMode); },
                    [edit, this](int i) {
                        if (i == 1 && !ed_.plugin().hasDriveRom())
                            ed_.notify("The True 1541 needs the 1541 DOS ROM in " + romFolder(), true);
                        edit([i](MachineSettings& s) { s.driveMode = static_cast<emu::DriveMode>(i); });
                    })
            ->setTooltip("Virtual: instant, through the KERNAL. True 1541: the real drive, for programs with "
                         "their own loaders (needs the 1541 ROM).");
        for (int port = 0; port < 2; ++port)
            add<Choice>(port == 0 ? "Port 1" : "Port 2", std::vector<std::string>{"Joystick", "Mouse 1351", "Paddles"},
                        [&pl, port] { return static_cast<int>(pl.settings().portDevice[port]); },
                        [edit, port](int i) {
                            edit([i, port](MachineSettings& s) { s.portDevice[port] = static_cast<emu::PortDevice>(i); });
                        })
                ->setTooltip("Mouse or paddles: driven by the host mouse over the C64 screen (buttons: left, right)");
        std::vector<std::string> counts;
        for (int i = 1; i <= 8; ++i)
            counts.push_back(std::to_string(i));
        add<Choice>("SID chips", counts, [&pl] { return pl.settings().sids.count - 1; },
                    [edit](int i) { edit([i](MachineSettings& s) { s.sids.count = i + 1; }); })
            ->setTooltip("How many SIDs the machine has. Programs find the extra ones at their addresses.");
        add<Choice>("Keyboard", std::vector<std::string>{"Symbolic", "Positional"},
                    [&pl] { return static_cast<int>(pl.settings().keyMapping); },
                    [edit](int i) { edit([i](MachineSettings& s) { s.keyMapping = static_cast<KeyMapping>(i); }); })
            ->setTooltip("Symbolic: a key types the character your layout prints on it. Positional: a key is the C64 "
                         "key in its place on a C64's keyboard.");
        add<Choice>("Resampling", std::vector<std::string>{"Sinc (best)", "Fast"},
                    [&pl] { return static_cast<int>(pl.settings().sids.resampling); },
                    [edit](int i) { edit([i](MachineSettings& s) { s.sids.resampling = static_cast<emu::SidResampling>(i); }); })
            ->setTooltip("Sinc: clean, the reference. Fast: much cheaper with many SIDs, but it aliases (harsh highs).");
        add<Choice>("Auto warp", std::vector<std::string>{"Start and disk", "Start only", "Disk only", "Off"},
                    [&pl] {
                        const auto& s = pl.settings();
                        return (s.warpOnBoot ? 0 : 2) + (s.warpOnDiskAccess ? 0 : 1);
                    },
                    [edit](int i) {
                        edit([i](MachineSettings& s) {
                            s.warpOnBoot = i < 2;
                            s.warpOnDiskAccess = i % 2 == 0;
                        });
                    })
            ->setTooltip("When the machine warps on its own (muted): while a program starts, and while the "
                         "True 1541's motor runs. The Warp button stops a running one.");
        add<Choice>("Voice outputs", std::vector<std::string>{"Off", "On"},
                    [&pl] { return pl.settings().sids.voiceOutputs ? 1 : 0; },
                    [edit](int i) { edit([i](MachineSettings& s) { s.sids.voiceOutputs = i == 1; }); })
            ->setTooltip("Each SID voice on its own output (SID 1 Voice 1 ... SID 8 Voice 3), with its own filter. "
                         "About four times the processing per SID in use.");

        for (const char* h : {"", "Address", "Model", "Curve", "Range", "Caps", "Combined", "Level", "Pan"})
            add<Label>(h, 10.0f, Align::Left, theme::textFaint);

        std::vector<std::string> models;
        for (int v = 0; v < static_cast<int>(emu::SidVariant::Count); ++v)
            models.push_back(emu::sidVariantName(static_cast<emu::SidVariant>(v)));
        for (int i = 0; i < builtCount_; ++i) {
            const size_t ci = static_cast<size_t>(i);
            add<Label>("SID " + std::to_string(i + 1), 11.0f, Align::Left, theme::text);
            auto* addr = add<Button>(hex4(pl.settings().sids.chips[ci].address), [this, i] { addressMenu(i); });
            addr->fontSize = 10.5f;
            addr->setEnabled(i > 0);
            addr->setTooltip(i == 0 ? "SID 1 is always at $D400" : "Where the program finds this SID");
            add<Choice>("", models, [&pl, ci] { return static_cast<int>(pl.settings().sids.chips[ci].variant); },
                        [edit, ci](int v) {
                            edit([v, ci](MachineSettings& s) {
                                auto& c = s.sids.chips[ci];
                                c.variant = static_cast<emu::SidVariant>(v);
                                c.tuning = emu::sidVariantTuning(c.variant);
                            });
                        })
                ->setTooltip("Chip revision. Sets the filter and combined-waveform starting points.");
            add<Slider>("", 0.0, 1.0, 0.5, [&pl, ci] { return static_cast<double>(pl.settings().sids.chips[ci].tuning.filterCurve); },
                        [edit, ci](double v) {
                            edit([v, ci](MachineSettings& s) { s.sids.chips[ci].tuning.filterCurve = static_cast<float>(v); });
                        },
                        [](double v) {
                            char b[16];
                            std::snprintf(b, sizeof(b), "%.0f%%", v * 100);
                            return std::string(b);
                        })
                ->setTooltip("Filter curve: how dark (left) or bright (right) this chip's filter is. 6581s vary a lot.");
            add<Slider>("", 0.0, 1.0, 19.0 / 39.0,
                        [&pl, ci] { return static_cast<double>(pl.settings().sids.chips[ci].tuning.filterRange); },
                        [edit, ci](double v) {
                            edit([v, ci](MachineSettings& s) { s.sids.chips[ci].tuning.filterRange = static_cast<float>(v); });
                        },
                        [&pl, ci](double v) {
                            if (emu::sidVariantIs8580(pl.settings().sids.chips[ci].variant))
                                return std::string("-");
                            char b[16];
                            std::snprintf(b, sizeof(b), "%.0f%%", v * 100);
                            return std::string(b);
                        })
                ->setTooltip("6581 only: the filter's range and offset, dark (left) to bright (right). "
                             "Double click for the default.");
            add<Choice>("", std::vector<std::string>{"470 pF", "2200 pF"},
                        [&pl, ci] { return pl.settings().sids.chips[ci].tuning.oldCaps ? 1 : 0; },
                        [edit, ci](int v) { edit([v, ci](MachineSettings& s) { s.sids.chips[ci].tuning.oldCaps = v == 1; }); })
                ->setTooltip("6581 only: the filter capacitors. 2200 pF were fitted to early boards (ASSY 326298): "
                             "a much darker filter.");
            add<Choice>("", std::vector<std::string>{"Average", "Weak", "Strong"},
                        [&pl, ci] { return static_cast<int>(pl.settings().sids.chips[ci].tuning.combined); },
                        [edit, ci](int v) {
                            edit([v, ci](MachineSettings& s) {
                                s.sids.chips[ci].tuning.combined = static_cast<emu::CombinedWaves>(v);
                            });
                        })
                ->setTooltip("How loud combined waveforms (e.g. saw + triangle) come out");
            const uint32_t lid = pid::Level + static_cast<uint32_t>(i), pid_ = pid::Pan + static_cast<uint32_t>(i);
            auto* lvl = add<Slider>("", -60.0, 12.0, 0.0, [&pl, lid] { return pl.paramValue(lid); },
                                    [&pl, lid](double v) { pl.guiSetParam(lid, v); }, dbText);
            lvl->onBegin = [&pl, lid] { pl.guiBeginParam(lid); };
            lvl->onEnd = [&pl, lid] { pl.guiEndParam(lid); };
            auto* pan = add<Slider>("", -1.0, 1.0, 0.0, [&pl, pid_] { return pl.paramValue(pid_); },
                                    [&pl, pid_](double v) { pl.guiSetParam(pid_, v); }, panText);
            pan->onBegin = [&pl, pid_] { pl.guiBeginParam(pid_); };
            pan->onEnd = [&pl, pid_] { pl.guiEndParam(pid_); };
        }
        auto* master = add<Slider>("Volume", -60.0, 12.0, 0.0, [&pl] { return pl.paramValue(pid::Master); },
                                   [&pl](double v) { pl.guiSetParam(pid::Master, v); }, dbText);
        master->onBegin = [&pl] { pl.guiBeginParam(pid::Master); };
        master->onEnd = [&pl] { pl.guiEndParam(pid::Master); };
        add<Label>("Model sets a starting point for Curve and Combined; tune Curve, Range and Caps by ear to your chip.", 10.0f,
                   Align::Left, theme::textFaint);
        layout();
        if (root())
            root()->markDirty();
    }

    void layout() override
    {
        const auto& ch = children();
        if (ch.empty())
            return;
        const float x0 = bounds_.x + 16, y0 = bounds_.y + 14;
        const float w = bounds_.w - 32;
        const float rowH = 26, gap = 8;
        size_t k = 0;
        // Machine row: video, palette, clock, drive.
        const float cw = (w - 3 * gap) / 4;
        for (int i = 0; i < 4 && k < ch.size(); ++i, ++k)
            ch[k]->setBounds({x0 + i * (cw + gap), y0, cw, rowH});
        // Second row: the two control ports, the SID count, the keyboard;
        // third: the resampling, the automatic warp, the voice outputs.
        for (int i = 0; i < 4 && k < ch.size(); ++i, ++k)
            ch[k]->setBounds({x0 + i * (cw + gap), y0 + rowH + gap, cw, rowH});
        for (int i = 0; i < 3 && k < ch.size(); ++i, ++k)
            ch[k]->setBounds({x0 + i * (cw + gap), y0 + 2 * (rowH + gap), cw, rowH});
        // The SID table.
        constexpr int nc = 9;
        const float cols[nc] = {0.07f, 0.08f, 0.13f, 0.13f, 0.11f, 0.10f, 0.11f, 0.14f, 0.13f};
        float cx[nc], cwid[nc];
        float acc = x0;
        for (int c = 0; c < nc; ++c) {
            cx[c] = acc;
            cwid[c] = cols[c] * w - 6;
            acc += cols[c] * w;
        }
        float y = y0 + 3 * (rowH + gap) + 6;
        for (int c = 0; c < nc && k < ch.size(); ++c, ++k)
            ch[k]->setBounds({cx[c], y, cwid[c], 16});
        y += 20;
        const float sidRow = std::min(rowH, std::max(18.0f, (bounds_.bottom() - y - 80) / std::max(1, builtCount_) - 4));
        for (int i = 0; i < builtCount_; ++i) {
            for (int c = 0; c < nc && k < ch.size(); ++c, ++k)
                ch[k]->setBounds({cx[c], y, cwid[c], sidRow});
            y += sidRow + 4;
        }
        y += 8;
        if (k < ch.size())
            ch[k++]->setBounds({x0, y, cw * 1.5f, rowH});
        y += rowH + 6;
        if (k < ch.size())
            ch[k++]->setBounds({x0, y, w, 16});
    }

    void paint(cairo_t* cr) override
    {
        fillRounded(cr, bounds_, 6, theme::panel);
        strokeRounded(cr, bounds_, 6, theme::outline);
    }

private:
    void addressMenu(int i)
    {
        auto& pl = ed_.plugin();
        const uint16_t cur = pl.settings().sids.chips[static_cast<size_t>(i)].address;
        std::vector<MenuItem> groups;
        for (unsigned page : {0xd400u, 0xd500u, 0xd600u, 0xd700u, 0xde00u, 0xdf00u}) {
            MenuItem g;
            g.label = hex4(page).substr(0, 3) + "xx";
            for (unsigned a = page; a < page + 0x100; a += 0x20) {
                if (a == 0xd400)
                    continue;
                MenuItem mi;
                mi.label = hex4(a);
                mi.checked = a == cur;
                mi.action = [this, &pl, i, a] {
                    MachineSettings s = pl.settings();
                    s.sids.chips[static_cast<size_t>(i)].address = static_cast<uint16_t>(a);
                    pl.applySettings(s);
                    build();
                };
                g.submenu.push_back(std::move(mi));
            }
            groups.push_back(std::move(g));
        }
        showMenu(root(), std::move(groups), bounds_.x + 120, bounds_.y + 60);
    }

    Editor& ed_;
    int builtCount_ = -1;
};

// ================================================================= CcPage

// The CC map: which map, whether loading a program picks one, and the 64
// slots -- controller, name (with value labels after a '|') and value.
class CcPage : public Widget
{
public:
    explicit CcPage(Editor& ed) : ed_(ed) {}

    // Rebuilds when the map changed (another map, a program that picked one,
    // a project load, an edit).
    void sync()
    {
        if (ed_.plugin().ccMapSerial() != builtSerial_)
            build();
    }

    void build()
    {
        clearChildren();
        auto& pl = ed_.plugin();
        builtSerial_ = pl.ccMapSerial();
        maps_ = availableCcMaps();
        const CcMap& cur = pl.ccMap();
        const std::string curText = cur.toText();

        // The map list: none, the factory maps, the user's, and the
        // project's own when it is none of them (edited, or from a file
        // that is gone).
        std::vector<std::string> items{"None"};
        selected_ = cur.empty() && cur.name.empty() ? 0 : -1;
        for (size_t i = 0; i < maps_.size(); ++i) {
            items.push_back(maps_[i].map.name + (maps_[i].factory ? "" : "  (yours)"));
            if (selected_ < 0 && maps_[i].map.toText() == curText)
                selected_ = static_cast<int>(i + 1);
        }
        if (selected_ < 0) {
            items.push_back((cur.name.empty() ? std::string("Unnamed") : cur.name) + "  (edited)");
            selected_ = static_cast<int>(items.size() - 1);
        }
        add<Choice>("Map", items, [this] { return selected_; },
                    [this, &pl](int i) {
                        if (i == 0)
                            pl.setCcMap(CcMap{});
                        else if (i - 1 < static_cast<int>(maps_.size()))
                            pl.setCcMap(maps_[static_cast<size_t>(i - 1)].map);
                    })
            ->setTooltip("Which controllers the program listens to. The slots are host parameters named after it.");
        add<Choice>("Follow program", std::vector<std::string>{"Off", "On"},
                    [&pl] { return pl.settings().ccFollowProgram ? 1 : 0; },
                    [&pl](int i) {
                        MachineSettings s = pl.settings();
                        s.ccFollowProgram = i == 1;
                        pl.applySettings(s);
                    })
            ->setTooltip("Loading a program picks the map made for it");
        std::vector<std::string> channels;
        for (int c = 1; c <= 16; ++c)
            channels.push_back(std::to_string(c));
        add<Choice>("Channel", channels, [&pl] { return pl.ccMap().channel; },
                    [&pl](int c) {
                        CcMap m = pl.ccMap();
                        m.channel = c;
                        pl.setCcMap(m);
                    })
            ->setTooltip("The MIDI channel the slots send on (a slot can name its own: CC@channel)");
        add<Button>("Save as", [this] { saveAs(); })->setTooltip("Save this map into your CC map folder");
        add<Button>("Folder", [] {
            makeFolder(ccMapFolder());
            openFolder(ccMapFolder());
        })->setTooltip("Open the folder with your CC maps (*.ccmap, plain text)");

        for (int i = 0; i < kCcSlots; ++i) {
            const auto& s = cur.slots[i];
            const uint32_t id = pid::CcSlot + static_cast<uint32_t>(i);
            add<Label>(std::to_string(i + 1), 10.0f, Align::Right, theme::textFaint);
            auto* cc = add<Button>(s.used() ? std::to_string(s.cc) + (s.channel >= 0 ? "@" + std::to_string(s.channel + 1) : "") : "-",
                                   [this, i] { editCc(i); });
            cc->fontSize = 10.5f;
            cc->setTooltip("The controller this slot sends, as CC or CC@channel; empty clears the slot");
            auto* name = add<Button>(s.used() ? cur.slotName(i) : "", [this, i] { editName(i); });
            name->fontSize = 10.5f;
            name->setEnabled(s.used());
            name->setTooltip("The slot's name; value names after a '|': Vib On | 0 Off, 64 On");
            auto* val = add<Slider>("", 0.0, 127.0, 0.0, [&pl, id] { return pl.paramValue(id); },
                                    [&pl, id](double v) { pl.guiSetParam(id, std::round(v)); },
                                    [&pl, i](double v) { return pl.ccMap().valueText(i, static_cast<int>(std::lround(v))); });
            val->onBegin = [&pl, id] { pl.guiBeginParam(id); };
            val->onEnd = [&pl, id] { pl.guiEndParam(id); };
        }
        layout();
        if (root())
            root()->markDirty();
    }

    void layout() override
    {
        const auto& ch = children();
        if (ch.empty())
            return;
        const float x0 = bounds_.x + 12, y0 = bounds_.y + 12;
        const float w = bounds_.w - 24;
        const float rowH = 26, gap = 8;
        size_t k = 0;
        // Map, follow, channel, save as, folder.
        const float widths[5] = {w * 0.36f, w * 0.22f, w * 0.16f, w * 0.12f, w * 0.14f - 4 * gap};
        float x = x0;
        for (int i = 0; i < 5 && k < ch.size(); ++i, ++k) {
            ch[k]->setBounds({x, y0, widths[i], rowH});
            x += widths[i] + gap;
        }
        // Three columns of slots.
        constexpr int rows = (kCcSlots + 2) / 3;
        const float top = y0 + rowH + 10;
        const float colGap = 14;
        const float cw = (w - 2 * colGap) / 3;
        const float sh = std::max(14.0f, std::min(24.0f, (bounds_.bottom() - 8 - top) / rows - 2));
        for (int i = 0; i < kCcSlots && k + 3 < ch.size(); ++i) {
            const float cx = x0 + static_cast<float>(i / rows) * (cw + colGap);
            const float cy = top + static_cast<float>(i % rows) * (sh + 2);
            ch[k++]->setBounds({cx, cy, 18, sh});
            ch[k++]->setBounds({cx + 22, cy, 40, sh});
            ch[k++]->setBounds({cx + 66, cy, cw * 0.45f, sh});
            ch[k++]->setBounds({cx + 70 + cw * 0.45f, cy, cw - 70 - cw * 0.45f, sh});
        }
    }

    void paint(cairo_t* cr) override
    {
        fillRounded(cr, bounds_, 6, theme::panel);
        strokeRounded(cr, bounds_, 6, theme::outline);
    }

private:
    // A one-line text field over the clicked control.
    void ask(int childIndex, const std::string& initial, std::function<void(const std::string&)> done)
    {
        const auto& ch = children();
        if (childIndex < 0 || childIndex >= static_cast<int>(ch.size()) || !root())
            return;
        Rect r = ch[static_cast<size_t>(childIndex)]->bounds();
        r.w = std::max(r.w, 220.0f);
        r.h = std::max(r.h, 26.0f);
        auto te = std::make_unique<TextEditor>(initial, std::move(done));
        te->setBounds(r);
        Widget* raw = te.get();
        root()->pushOverlay(std::move(te));
        root()->setKeyFocus(raw);
        if (root()->grabKeyboard)
            root()->grabKeyboard();
    }

    static constexpr int kHeaderControls = 5;
    int slotChild(int slot, int part) const { return kHeaderControls + slot * 4 + part; }

    void editCc(int slot)
    {
        const auto& s = ed_.plugin().ccMap().slots[slot];
        const std::string cur = s.used() ? std::to_string(s.cc) + (s.channel >= 0 ? "@" + std::to_string(s.channel + 1) : "") : "";
        ask(slotChild(slot, 1), cur, [this, slot](const std::string& text) {
            auto& pl = ed_.plugin();
            CcSlot s = pl.ccMap().slots[slot];
            std::string t;
            for (char c : text)
                if (c != ' ')
                    t += c;
            if (t.empty() || t == "-") {
                pl.setCcSlot(slot, CcSlot{});
                return;
            }
            // Parsed the way a map file is.
            const CcMap probe = CcMap::fromText("1 = " + t + " x");
            if (!probe.slots[0].used()) {
                ed_.notify("A controller is 0..127, optionally with @channel (1..16)", true);
                return;
            }
            s.cc = probe.slots[0].cc;
            s.channel = probe.slots[0].channel;
            pl.setCcSlot(slot, s);
        });
    }

    void editName(int slot)
    {
        const auto& s = ed_.plugin().ccMap().slots[slot];
        std::string cur = s.name;
        if (!s.labels.empty()) {
            cur += " |";
            for (size_t k = 0; k < s.labels.size(); ++k)
                cur += (k ? ", " : " ") + std::to_string(s.labels[k].first) + " " + s.labels[k].second;
        }
        ask(slotChild(slot, 2), cur, [this, slot](const std::string& text) {
            auto& pl = ed_.plugin();
            CcSlot s = pl.ccMap().slots[slot];
            const CcMap probe = CcMap::fromText("1 = " + std::to_string(s.cc) + " " + text);
            s.name = probe.slots[0].name;
            s.labels = probe.slots[0].labels;
            pl.setCcSlot(slot, s);
        });
    }

    void saveAs()
    {
        const CcMap& cur = ed_.plugin().ccMap();
        ask(3, cur.name.empty() ? std::string("My map") : cur.name, [this](const std::string& name) {
            if (name.empty())
                return;
            auto& pl = ed_.plugin();
            CcMap m = pl.ccMap();
            m.name = name;
            std::string err;
            const std::string path = saveUserCcMap(m, err);
            if (path.empty()) {
                ed_.notify(err, true);
                return;
            }
            pl.setCcMap(m);
            ed_.notify("Saved " + path);
        });
    }

    Editor& ed_;
    std::vector<CcMapEntry> maps_;
    int selected_ = 0;
    uint32_t builtSerial_ = ~0u;
};

// ================================================================= Editor

Editor::Editor(SidMidiPlugin& plugin) : plugin_(plugin), window_(this)
{
    scale_ = NativeWindow::systemScale();

    loadButton_ = add<Button>("Load", [this] { openLoadDialog(); });
    loadButton_->setTooltip("Load a .prg, .d64, .crt or .bin and start it (or drop the file on the window)");
    diskButton_ = add<Button>("Media", [this] { showDiskMenu(); });
    diskButton_->setTooltip("Insert a disk without starting it, start a file from the disk, export the disk "
                            "with what the program saved, press a freezer cartridge's button, eject");
    resetButton_ = add<Button>("Reset", [this] { plugin_.resetButton(); });
    resetButton_->setTooltip("The reset button: the program restarts its machine, memory survives");
    powerButton_ = add<Button>("Power", [this] { plugin_.powerCycle(); });
    powerButton_->setTooltip("Switch off and on: the attached program starts again");
    warpButton_ = add<Button>("Warp", [this] { plugin_.setWarp(!warpButton_->isActive()); });
    warpButton_->setTooltip("Run as fast as possible (muted), e.g. while a program unpacks itself. Lit by an automatic warp: "
                            "click to stop it (Machine page: Auto warp)");

    std::vector<std::string> ifaces;
    for (int i = 0; i < static_cast<int>(emu::MidiInterface::Count); ++i)
        ifaces.push_back(emu::midiInterfaceName(static_cast<emu::MidiInterface>(i)));
    midiChoice_ = add<Choice>("MIDI", ifaces, [this] { return static_cast<int>(plugin_.settings().midi); },
                              [this](int i) {
                                  MachineSettings s = plugin_.settings();
                                  s.midi = static_cast<emu::MidiInterface>(i);
                                  plugin_.applySettings(s);
                              });
    midiChoice_->setTooltip("The MIDI cartridge in the expansion port. Set the program to the same type.");
    joyChoice_ = add<Choice>("Joy", std::vector<std::string>{"Off", "Numpad", "Cursor keys"},
                             [this] { return static_cast<int>(plugin_.settings().joyKeys); },
                             [this](int i) {
                                 MachineSettings s = plugin_.settings();
                                 s.joyKeys = static_cast<JoyKeys>(i);
                                 plugin_.applySettings(s);
                             });
    joyChoice_->setTooltip("Joystick on the keyboard: numpad (0 or 5 = fire) or cursor keys (right Ctrl = fire)");
    portChoice_ = add<Choice>("Port", std::vector<std::string>{"1", "2"},
                              [this] { return plugin_.settings().joyPort == 1 ? 0 : 1; },
                              [this](int i) {
                                  MachineSettings s = plugin_.settings();
                                  s.joyPort = i == 0 ? 1 : 2;
                                  plugin_.applySettings(s);
                              });
    machineButton_ = add<Button>("Machine", [this] { toggleMachinePage(); });
    machineButton_->setTooltip("Video, palette, MIDI clock, the SIDs and their mix");
    ccButton_ = add<Button>("CC", [this] { toggleCcPage(); });
    ccButton_->setTooltip("The CC map: the program's controllers as host parameters");

    screen_ = add<ScreenView>(*this);
    page_ = add<MachinePage>(*this);
    page_->setVisible(false);
    ccPage_ = add<CcPage>(*this);
    ccPage_->setVisible(false);
    romPanel_ = add<RomPanel>(*this);
    romPanel_->setVisible(!plugin_.hasRoms());
    screen_->setVisible(plugin_.hasRoms());

    grabKeyboard = [this] { window_.grabKeyboard(); };
    setBounds({0, 0, logicalW_, logicalH_});
}

Editor::~Editor()
{
    detach();
    if (staticLayer_)
        cairo_surface_destroy(staticLayer_);
}

void Editor::layout()
{
    const float pad = 8, h = 26, y = (kBarH - h) * 0.5f;
    float x = pad;
    auto place = [&](Widget* w, float width) {
        w->setBounds({x, y, width, h});
        x += width + 6;
    };
    place(loadButton_, 52);
    place(diskButton_, 56);
    place(resetButton_, 52);
    place(powerButton_, 54);
    place(warpButton_, 50);
    x += 8;
    place(midiChoice_, 150);
    place(joyChoice_, 118);
    place(portChoice_, 64);
    machineButton_->setBounds({bounds_.w - pad - 74, y, 74, h});
    ccButton_->setBounds({bounds_.w - pad - 74 - 6 - 40, y, 40, h});
    const Rect content{pad, kBarH + pad, bounds_.w - 2 * pad, bounds_.h - kBarH - kStatusH - 2 * pad};
    screen_->setBounds(content);
    page_->setBounds(content);
    ccPage_->setBounds(content);
    romPanel_->setBounds(content);
}

void Editor::paint(cairo_t* cr)
{
    setColor(cr, theme::background);
    cairo_paint(cr);
    setColor(cr, theme::panel);
    cairo_rectangle(cr, 0, 0, bounds_.w, kBarH);
    cairo_fill(cr);
    setColor(cr, theme::outline);
    cairo_rectangle(cr, 0, kBarH - 1, bounds_.w, 1);
    cairo_fill(cr);
}

void Editor::paintStatus(cairo_t* cr)
{
    const Rect bar{0, bounds_.h - kStatusH, bounds_.w, kStatusH};
    setColor(cr, theme::panel);
    cairo_rectangle(cr, bar.x, bar.y, bar.w, bar.h);
    cairo_fill(cr);
    // Indicators on the right: drive, MIDI in, MIDI out.
    auto led = [&](float x, bool on, const Color& c, const char* label) {
        setColor(cr, on ? c : theme::outline);
        cairo_arc(cr, x, bar.cy(), 3.5, 0, 2 * M_PI);
        cairo_fill(cr);
        setFont(cr, 10.0f);
        drawText(cr, label, {x + 7, bar.y, 40, bar.h}, Align::Left, theme::textDim);
    };
    const double now = nowSeconds();
    float x = bar.right() - 170;
    led(x, driveLit_, theme::red, "Drive");
    led(x + 56, now < midiInLit_, theme::green, "In");
    led(x + 92, now < midiOutLit_, theme::accentBright, "Out");
    led(x + 132, warpLit_, theme::amp, "Warp");
    // Which build this is, beside the indicators.
    const float verW = 84;
    setFont(cr, 10.0f);
    drawText(cr, std::string(kPluginName) + " " + kPluginVersion, {x - 16 - verW, bar.y, verW, bar.h}, Align::Right,
             theme::textFaint);
    x -= verW + 8;
    setFont(cr, 10.5f);
    std::string text = notice_;
    Color c = noticeError_ ? theme::red : theme::text;
    if (text.empty()) {
        text = media_.empty() ? std::string("Drop a .prg, .d64 or .crt here") : media_;
        if (keyboardToC64_)
            text += "   |   Keyboard goes to the C64 (Esc = RUN/STOP, PgUp = RESTORE)";
        c = theme::textDim;
    }
    cairo_save(cr);
    cairo_rectangle(cr, 10, bar.y, x - 24, bar.h);
    cairo_clip(cr);
    drawText(cr, text, {10, bar.y, x - 24, bar.h}, Align::Left, c);
    cairo_restore(cr);
}

void Editor::notify(const std::string& text, bool error)
{
    notice_ = text;
    noticeError_ = error;
    noticeUntil_ = nowSeconds() + (error ? 8.0 : 4.0);
    markAnimDirty();
}

void Editor::giveKeyboardToC64(bool on)
{
    if (on == keyboardToC64_)
        return;
    keyboardToC64_ = on;
    if (on) {
        window_.grabKeyboard();
    } else {
        keymap_.clear();
        sendKeyState();
    }
    markAnimDirty();
}

void Editor::sendKeyState()
{
    keymap_.joyKeys = plugin_.settings().joyKeys;
    const auto m = keymap_.matrix();
    for (int col = 0; col < 8; ++col) {
        const uint8_t diff = m[static_cast<size_t>(col)] ^ sentMatrix_[static_cast<size_t>(col)];
        for (int row = 0; row < 8; ++row)
            if (diff & (1 << row))
                plugin_.key(static_cast<uint8_t>(col * 8 + row), (m[static_cast<size_t>(col)] >> row) & 1);
    }
    sentMatrix_ = m;
    const uint8_t joy = keymap_.joystickBits();
    const int port = plugin_.settings().joyPort - 1;
    if (port != sentJoyPort_) {
        plugin_.joystick(sentJoyPort_, 0);
        sentJoyPort_ = port;
        sentJoy_ = 0xff;
    }
    if (joy != sentJoy_) {
        plugin_.joystick(port, joy);
        sentJoy_ = joy;
    }
}

void Editor::onRawKey(int code, bool down, unsigned keysym)
{
    if (!keyboardToC64_ || hasOverlay())
        return;
    if (Keymap::isRestore(code)) {
        if (down)
            plugin_.restoreKey();
        return;
    }
    keymap_.mapping = plugin_.settings().keyMapping;
    if (down)
        keymap_.press(code, keysym);
    else
        keymap_.release(code);
    sendKeyState();
}

bool Editor::onKey(const KeyEvent& e)
{
    // With the keyboard at the C64, the raw events carry everything.
    if (keyboardToC64_ && !hasOverlay())
        return true;
    return handleKey(e);
}

void Editor::onFocusLost()
{
    keymap_.clear();
    sendKeyState();
}

void Editor::onMouseDown(const MouseEvent& e)
{
    // A click anywhere but the screen takes the keyboard back from the C64.
    if (!screen_->isVisible() || !screen_->bounds().contains(e.x, e.y))
        giveKeyboardToC64(false);
    handleMouseDown(e);
}

void Editor::onFilesDropped(const std::vector<std::string>& paths)
{
    if (!paths.empty())
        openMedia(paths.front());
}

namespace {
std::string fileNameOf(const std::string& path) { return path.substr(path.find_last_of("/\\") + 1); }
bool isD64(const std::string& path)
{
    const std::string n = fileNameOf(path);
    return n.size() > 4 && (n.substr(n.size() - 4) == ".d64" || n.substr(n.size() - 4) == ".D64");
}
} // namespace

void Editor::openMedia(const std::string& path)
{
    std::string err;
    if (isD64(path)) {
        // In the drive first, without touching the machine; then which file.
        if (!plugin_.attachDiskFile(path, err)) {
            notify(err, true);
            return;
        }
        notify("Disk inserted: " + fileNameOf(path));
        showDiskPicker();
        return;
    }
    if (plugin_.loadFile(path, err)) {
        const std::string notice = plugin_.loadNotice();
        notify(notice.empty() ? "Loaded " + fileNameOf(path) : notice);
    } else {
        notify(err, true);
    }
}

std::vector<MenuItem> Editor::diskStartItems()
{
    std::vector<MenuItem> items;
    const auto files = plugin_.diskDirectory();
    auto start = [this](std::string petscii, std::string label) {
        return [this, petscii, label] {
            std::string err;
            if (plugin_.startFromDisk(petscii, err))
                notify("Starting " + label);
            else
                notify(err, true);
        };
    };
    items.push_back({"Run the first file", start("*", "the first file")});
    std::vector<MenuItem> runnable;
    for (const auto& f : files) {
        if (f.type != "PRG")
            continue;
        const std::string label = f.display + "   (" + std::to_string(f.blocks) + (f.blocks == 1 ? " block)" : " blocks)");
        runnable.push_back({label, start(f.petscii, "\"" + f.display + "\"")});
    }
    if (runnable.empty())
        return items;
    MenuItem sep;
    sep.separator = true;
    items.push_back(sep);
    constexpr size_t kPerMenu = 20; // what fits in the window
    if (runnable.size() <= kPerMenu) {
        items.insert(items.end(), runnable.begin(), runnable.end());
    } else {
        for (size_t i = 0; i < runnable.size(); i += kPerMenu) {
            MenuItem group;
            const size_t end = std::min(runnable.size(), i + kPerMenu);
            group.label = "Files " + std::to_string(i + 1) + " to " + std::to_string(end);
            group.submenu.assign(runnable.begin() + static_cast<long>(i), runnable.begin() + static_cast<long>(end));
            items.push_back(std::move(group));
        }
    }
    return items;
}

// After a disk is loaded: leave it inserted, or start a file from it.
void Editor::showDiskPicker()
{
    std::vector<MenuItem> items;
    MenuItem title;
    const std::string t = plugin_.diskTitle();
    title.label = "Disk: " + (t.empty() ? std::string("(no title)") : t);
    title.enabled = false;
    items.push_back(title);
    items.push_back({"Leave it in the drive", [] {}});
    for (auto& it : diskStartItems())
        items.push_back(std::move(it));
    giveKeyboardToC64(false);
    showMenu(this, std::move(items), bounds_.w * 0.5f - 130, kBarH + 30);
}

void Editor::openLoadDialog()
{
    if (dialog_)
        return;
    dialog_ = std::make_unique<FileDialog>();
    dialogFor_ = DialogFor::Load;
    if (!dialog_->start(FileDialog::Mode::OpenFile, "Load a C64 program",
                        "C64 programs | *.prg *.PRG *.d64 *.D64 *.crt *.CRT *.bin *.BIN *.p00 *.P00")) {
        dialog_.reset();
        notify("No file chooser available (install zenity or kdialog); drop the file on the window instead", true);
    }
}

void Editor::openAttachDialog()
{
    if (dialog_)
        return;
    dialog_ = std::make_unique<FileDialog>();
    dialogFor_ = DialogFor::Attach;
    if (!dialog_->start(FileDialog::Mode::OpenFile, "Insert a disk", "Disk images | *.d64 *.D64")) {
        dialog_.reset();
        notify("No file chooser available (install zenity or kdialog); drop the file on the window instead", true);
    }
}

void Editor::openExportDialog()
{
    if (dialog_)
        return;
    dialog_ = std::make_unique<FileDialog>();
    dialogFor_ = DialogFor::Export;
    if (!dialog_->start(FileDialog::Mode::SaveFile, "Export the disk image", "Disk images | *.d64")) {
        dialog_.reset();
        notify("No file chooser available (install zenity or kdialog)", true);
    }
}

void Editor::showDiskMenu()
{
    std::vector<MenuItem> items;
    const bool disk = plugin_.diskPresent();
    items.push_back({"Load...", [this] { openLoadDialog(); }});
    items.push_back({"Insert disk...", [this] { openAttachDialog(); }});
    MenuItem startMenu;
    startMenu.label = "Start from disk";
    startMenu.enabled = disk;
    if (disk)
        startMenu.submenu = diskStartItems();
    items.push_back(std::move(startMenu));
    MenuItem sep;
    sep.separator = true;
    items.push_back(sep);
    MenuItem exportItem{"Export disk image...", [this] { openExportDialog(); }};
    exportItem.enabled = disk;
    items.push_back(exportItem);
    MenuItem ejectDisk{"Eject disk", [this] { plugin_.ejectDisk(); }};
    ejectDisk.enabled = disk;
    items.push_back(ejectDisk);
    MenuItem freeze{"Freeze (cartridge button)", [this] { plugin_.freezeButton(); }};
    freeze.enabled = plugin_.cartridgeFreezable();
    items.push_back(freeze);
    items.push_back({"Eject cartridge", [this] { plugin_.ejectCartridge(); }});
    const Rect b = diskButton_->bounds();
    showMenu(this, std::move(items), b.x, b.bottom());
}

void Editor::toggleMachinePage()
{
    showPage(page_->isVisible() ? nullptr : page_);
}

void Editor::showPage(Page p)
{
    showPage(p == Page::Machine ? static_cast<Widget*>(page_) : p == Page::Cc ? static_cast<Widget*>(ccPage_) : nullptr);
}

void Editor::toggleCcPage()
{
    showPage(ccPage_->isVisible() ? nullptr : ccPage_);
}

// One of the pages over the screen, or none (the screen).
void Editor::showPage(Widget* which)
{
    page_->setVisible(which == page_);
    ccPage_->setVisible(which == ccPage_);
    screen_->setVisible(!which && plugin_.hasRoms());
    romPanel_->setVisible(!which && !plugin_.hasRoms());
    machineButton_->setActive(which == page_);
    ccButton_->setActive(which == ccPage_);
    if (which) {
        giveKeyboardToC64(false);
        if (which == page_)
            page_->build();
        else
            ccPage_->build();
    }
    markDirty();
}

// ------------------------------------------------------------ window

bool Editor::attach(uintptr_t parentWindow)
{
    int w, h;
    physicalSize(w, h);
    window_.setScale(scale_);
    return window_.attach(parentWindow, w, h);
}

void Editor::detach() { window_.destroy(); }

void Editor::setScale(double s)
{
    scale_ = std::clamp(s, 0.5, 4.0);
    window_.setScale(scale_);
    markDirty();
}

void Editor::physicalSize(int& w, int& h) const
{
    w = static_cast<int>(std::lround(logicalW_ * scale_));
    h = static_cast<int>(std::lround(logicalH_ * scale_));
}

void Editor::setPhysicalSize(int w, int h)
{
    logicalW_ = std::max(kMinW, static_cast<float>(w / scale_));
    logicalH_ = std::max(kMinH, static_cast<float>(h / scale_));
    setBounds({0, 0, logicalW_, logicalH_});
    window_.setSize(w, h);
    markDirty();
}

void Editor::show() { window_.show(); }
void Editor::hide() { window_.hide(); }

void Editor::onTimer()
{
    window_.processEvents();
    const double t = nowSeconds();
    if (!plugin_.hasRoms() && t - romScanAt_ > 2.0) {
        romScanAt_ = t;
        if (plugin_.rescanRoms())
            notify("ROMs found: the C64 is starting");
        markDirty();
    }
    const bool pageShown = page_->isVisible() || ccPage_->isVisible();
    if (!pageShown && romPanel_->isVisible() == plugin_.hasRoms()) {
        romPanel_->setVisible(!plugin_.hasRoms());
        screen_->setVisible(plugin_.hasRoms());
        markDirty();
    }
    if (screen_->isVisible() && screen_->update())
        markAnimDirty();
    if (page_->isVisible())
        page_->sync();
    if (ccPage_->isVisible())
        ccPage_->sync();

    auto& engine = plugin_.engine();
    const uint32_t in = engine.midiInCount.load(std::memory_order_relaxed);
    const uint32_t out = engine.midiOutCount.load(std::memory_order_relaxed);
    if (in != lastMidiIn_) {
        lastMidiIn_ = in;
        midiInLit_ = t + 0.08;
    }
    if (out != lastMidiOut_) {
        lastMidiOut_ = out;
        midiOutLit_ = t + 0.08;
    }
    driveLit_ = engine.driveLed.load(std::memory_order_relaxed);
    const bool warp = plugin_.warping();
    if (warp != warpLit_) {
        warpLit_ = warp;
        warpButton_->setActive(warp);
        markDirty();
    }
    if (t - mediaPolledAt_ > 0.5) {
        mediaPolledAt_ = t;
        const std::string m = plugin_.mediaSummary();
        if (m != media_) {
            media_ = m;
            markAnimDirty();
        }
    }
    if (dialog_) {
        std::string result;
        if (dialog_->poll(result)) {
            const DialogFor purpose = dialogFor_;
            dialog_.reset();
            if (!result.empty()) {
                std::string err;
                if (purpose == DialogFor::Attach) {
                    if (plugin_.attachDiskFile(result, err))
                        notify("Disk inserted: " + fileNameOf(result));
                    else
                        notify(err, true);
                } else if (purpose == DialogFor::Load) {
                    openMedia(result);
                } else {
                    if (result.size() < 4 || (result.substr(result.size() - 4) != ".d64" && result.substr(result.size() - 4) != ".D64"))
                        result += ".d64";
                    if (plugin_.exportDisk(result, err))
                        notify("Saved " + fileNameOf(result));
                    else
                        notify(err, true);
                }
            }
        }
    }
    if (!notice_.empty() && t > noticeUntil_) {
        notice_.clear();
        markAnimDirty();
    }
    // The LEDs blink without anything else changing.
    if (t - lastLedRepaint_ > 0.05) {
        lastLedRepaint_ = t;
        markAnimDirty();
    }
    if (takeDirty())
        window_.render();
}

void Editor::onPaint(cairo_t* cr, int, int) { renderTo(cr); }

void Editor::renderTo(cairo_t* cr)
{
    double sx = 1, sy = 1;
    cairo_user_to_device_distance(cr, &sx, &sy);
    const int w = static_cast<int>(std::ceil(bounds_.w * sx)), h = static_cast<int>(std::ceil(bounds_.h * sx));
    if (!staticLayer_ || staticW_ != w || staticH_ != h) {
        if (staticLayer_)
            cairo_surface_destroy(staticLayer_);
        staticLayer_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        staticW_ = w;
        staticH_ = h;
        staticStale_ = true;
    }
    if (staticStale_) {
        cairo_t* s = cairo_create(staticLayer_);
        cairo_scale(s, sx, sx);
        paintAll(s);
        cairo_destroy(s);
        staticStale_ = false;
    }
    cairo_save(cr);
    cairo_scale(cr, 1.0 / sx, 1.0 / sx);
    cairo_set_source_surface(cr, staticLayer_, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    screen_->paintLive(cr);
    paintStatus(cr);
    paintOverlays(cr);
    paintTooltip(cr);
}

} // namespace sidmidi::gui
