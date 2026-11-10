// sidmidi-run: the machine without a host.
//
// Boots the C64, autostarts a program, plays it a script of MIDI notes, key
// presses and typed text, and writes what came out: a WAV of the SIDs,
// screenshots, and the MIDI the program transmitted. It drives the machine
// through the same Engine the plugin uses.
//
//   sidmidi-run [options]
//     --roms DIR            ROM folder (default: SidMidi's ROM folder, see Resources.h)
//     --prg FILE | --d64 FILE | --crt FILE
//     --attach FILE         attach a .d64 without starting anything
//     --seconds S           how long to run (default 10)
//     --rate HZ             sample rate (default 48000)
//     --block N             block size (default 256)
//     --ntsc
//     --midi NAME           datel|sequential|passport|namesoft|maplin|moog|hermidi|none
//     --sids N              number of SIDs (addresses $D400, $D420, $D440, ...)
//     --sid-addr I:HEX      address of SID I (2..8)
//     --model I:NAME        6581|6581R3|6581R4|6581R4AR|8580R5
//     --filter-range I:V    6581 filter range of SID I, 0..1 (after --model)
//     --old-caps I          SID I has the 2200 pF filter caps (after --model)
//     --fast-resampling     nearest sample instead of the sinc resampler
//     --note T:KEY:VEL:LEN  a note on channel 1 at T seconds, LEN seconds long
//     --cc T:CC:VAL         a controller on channel 1
//     --bytes T:HEX..       raw MIDI bytes, e.g. 4.0:903C64
//     --key T:NAME[:LEN]    press a C64 key (F1, RETURN, SPACE, A, 1, RUNSTOP ...)
//     --type T:TEXT         type text into the keyboard buffer ("\n" = RETURN)
//     --shot T:FILE.png     screenshot at T seconds (repeatable)
//     --wav FILE            write the main output as 32-bit float stereo
//     --palette KEY         palette for screenshots (default colodore)
//     --trace               print PC and frame count every second
//     --port N:DEV          port 1 or 2: joystick, mouse or paddles
//     --drive MODE          virtual (default), true (the emulated 1541) or none
//     --no-video            do not draw the screen (as the plugin with its window closed)
//     --export-d64 FILE     write the disk image at the end (what the program saved included)
//     --paddle T:N:A:B      set port N's paddles to A and B at T seconds
//     --mouse T:N:DX:DY     move port N's mouse
//     --peek T:HEX          print what the CPU would read at an address
//     --sidregs T           print the registers last written to SID 1 at T seconds

#include <cairo/cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "engine/Engine.h"
#include "engine/Resources.h"

using namespace sidmidi;

namespace {

struct Scripted
{
    double t;
    int kind; // 0 midi bytes, 1 key down, 2 key up, 3 type, 4 shot, 5 SID registers, 6 paddle, 7 mouse, 8 peek
    std::vector<uint8_t> bytes;
    uint8_t key = 0;
    std::string text;
};

const std::map<std::string, uint8_t>& keyNames()
{
    using namespace emu::keys;
    static const std::map<std::string, uint8_t> m = {
        {"F1", F1}, {"F3", F3}, {"F5", F5}, {"F7", F7}, {"RETURN", Return}, {"SPACE", Space}, {"DEL", Del},
        {"RUNSTOP", RunStop}, {"LSHIFT", LShift}, {"RSHIFT", RShift}, {"CBM", Commodore}, {"CTRL", Ctrl},
        {"HOME", Home}, {"CRSRDOWN", CrsrDown}, {"CRSRRIGHT", CrsrRight}, {"LEFTARROW", LeftArrow},
        {"UPARROW", UpArrow}, {"PLUS", Plus}, {"MINUS", Minus}, {"ASTERISK", Asterisk}, {"POUND", Pound},
        {"EQUAL", Equal}, {"COLON", Colon}, {"SEMICOLON", Semicolon}, {"COMMA", Comma}, {"PERIOD", Period},
        {"SLASH", Slash}, {"AT", At},
        {"A", A}, {"B", B}, {"C", C}, {"D", D}, {"E", E}, {"F", F}, {"G", G}, {"H", H}, {"I", I}, {"J", J},
        {"K", K}, {"L", L}, {"M", M}, {"N", N}, {"O", O}, {"P", P}, {"Q", Q}, {"R", R}, {"S", S}, {"T", T},
        {"U", U}, {"V", V}, {"W", W}, {"X", X}, {"Y", Y}, {"Z", Z},
        {"0", K0}, {"1", K1}, {"2", K2}, {"3", K3}, {"4", K4}, {"5", K5}, {"6", K6}, {"7", K7}, {"8", K8}, {"9", K9},
    };
    return m;
}

bool writePng(const std::string& path, const uint8_t* frame, const Palette& pal)
{
    const int w = emu::Vic::kWidth, h = emu::Vic::kHeight;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    unsigned char* data = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(data + y * stride);
        for (int x = 0; x < w; ++x)
            row[x] = pal.rgb[frame[y * w + x] & 15];
    }
    cairo_surface_mark_dirty(s);
#if CAIRO_HAS_PNG_FUNCTIONS
    const bool ok = cairo_surface_write_to_png(s, path.c_str()) == CAIRO_STATUS_SUCCESS;
#else
    // The Windows cross-built Cairo has no PNG support: a PPM instead.
    bool ok = false;
    if (FILE* f = std::fopen((path + ".ppm").c_str(), "wb")) {
        std::fprintf(f, "P6 %d %d 255\n", w, h);
        for (int i = 0; i < w * h; ++i) {
            const uint32_t c = pal.rgb[frame[i] & 15];
            const unsigned char px[3] = {static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8),
                                         static_cast<unsigned char>(c)};
            std::fwrite(px, 1, 3, f);
        }
        ok = std::fclose(f) == 0;
    }
#endif
    cairo_surface_destroy(s);
    return ok;
}

bool writeWav(const std::string& path, const std::vector<float>& lr, int rate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = static_cast<uint32_t>(lr.size() * 4);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(3); // float
    u16(2);
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * 8));
    u16(8);
    u16(32);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    std::fwrite(lr.data(), 4, lr.size(), f);
    return std::fclose(f) == 0;
}

std::vector<uint8_t> hexBytes(const std::string& s)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
    return out;
}

emu::SidVariant variantByName(const std::string& n)
{
    for (int i = 0; i < static_cast<int>(emu::SidVariant::Count); ++i)
        if (n == emu::sidVariantName(static_cast<emu::SidVariant>(i)))
            return static_cast<emu::SidVariant>(i);
    std::fprintf(stderr, "unknown SID model %s\n", n.c_str());
    std::exit(2);
}

} // namespace

int main(int argc, char** argv)
{
    std::string romDir, prg, d64, crt, wav, attach, paletteKey = "colodore";
    double seconds = 10.0;
    int rate = 48000, block = 256;
    bool ntsc = false, trace = false;
    emu::MidiInterface iface = emu::MidiInterface::Datel;
    emu::SidBankConfig sidCfg;
    emu::PortDevice portDevices[2] = {emu::PortDevice::Joystick, emu::PortDevice::Joystick};
    emu::DriveMode driveMode = emu::DriveMode::Virtual;
    bool noVideo = false;
    std::string exportD64;
    std::vector<Scripted> script;

    auto split = [](const std::string& s) {
        std::vector<std::string> parts;
        size_t at = 0;
        for (;;) {
            const auto c = s.find(':', at);
            parts.push_back(s.substr(at, c == std::string::npos ? std::string::npos : c - at));
            if (c == std::string::npos)
                break;
            at = c + 1;
        }
        return parts;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--roms") romDir = next();
        else if (a == "--prg") prg = next();
        else if (a == "--d64") d64 = next();
        else if (a == "--attach") attach = next();
        else if (a == "--crt") crt = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--rate") rate = std::atoi(next().c_str());
        else if (a == "--block") block = std::max(1, std::atoi(next().c_str()));
        else if (a == "--ntsc") ntsc = true;
        else if (a == "--trace") trace = true;
        else if (a == "--palette") paletteKey = next();
        else if (a == "--wav") wav = next();
        else if (a == "--midi") {
            const std::string n = next();
            static const std::map<std::string, emu::MidiInterface> m = {
                {"none", emu::MidiInterface::None}, {"datel", emu::MidiInterface::Datel},
                {"sequential", emu::MidiInterface::Sequential}, {"passport", emu::MidiInterface::Passport},
                {"namesoft", emu::MidiInterface::Namesoft}, {"maplin", emu::MidiInterface::Maplin},
                {"moog", emu::MidiInterface::MoogSP}, {"hermidi", emu::MidiInterface::HerMIDI}};
            iface = m.at(n);
        } else if (a == "--sids") {
            sidCfg.count = std::clamp(std::atoi(next().c_str()), 1, 8);
            for (int k = 1; k < 8; ++k)
                sidCfg.chips[k].address = static_cast<uint16_t>(0xd400 + 0x20 * k);
        } else if (a == "--sid-addr") {
            const auto p = split(next());
            sidCfg.chips[std::atoi(p[0].c_str()) - 1].address = static_cast<uint16_t>(std::strtoul(p[1].c_str(), nullptr, 16));
        } else if (a == "--model") {
            const auto p = split(next());
            auto& c = sidCfg.chips[std::atoi(p[0].c_str()) - 1];
            c.variant = variantByName(p[1]);
            c.tuning = emu::sidVariantTuning(c.variant);
        } else if (a == "--filter-range") {
            const auto p = split(next());
            sidCfg.chips[std::atoi(p[0].c_str()) - 1].tuning.filterRange = static_cast<float>(std::atof(p[1].c_str()));
        } else if (a == "--old-caps") {
            sidCfg.chips[std::atoi(next().c_str()) - 1].tuning.oldCaps = true;
        } else if (a == "--fast-resampling") {
            sidCfg.resampling = emu::SidResampling::Fast;
        } else if (a == "--note") {
            const auto p = split(next());
            const double t = std::atof(p[0].c_str());
            const uint8_t key = static_cast<uint8_t>(std::atoi(p[1].c_str()));
            const uint8_t vel = static_cast<uint8_t>(p.size() > 2 ? std::atoi(p[2].c_str()) : 100);
            const double len = p.size() > 3 ? std::atof(p[3].c_str()) : 0.5;
            script.push_back({t, 0, {0x90, key, vel}});
            script.push_back({t + len, 0, {0x80, key, 0}});
        } else if (a == "--cc") {
            const auto p = split(next());
            script.push_back({std::atof(p[0].c_str()), 0,
                              {0xb0, static_cast<uint8_t>(std::atoi(p[1].c_str())), static_cast<uint8_t>(std::atoi(p[2].c_str()))}});
        } else if (a == "--bytes") {
            const auto p = split(next());
            script.push_back({std::atof(p[0].c_str()), 0, hexBytes(p[1])});
        } else if (a == "--key") {
            const auto p = split(next());
            const auto it = keyNames().find(p[1]);
            if (it == keyNames().end()) {
                std::fprintf(stderr, "unknown key %s\n", p[1].c_str());
                return 2;
            }
            const double t = std::atof(p[0].c_str());
            const double len = p.size() > 2 ? std::atof(p[2].c_str()) : 0.1;
            Scripted down{t, 1, {}};
            down.key = it->second;
            Scripted up{t + len, 2, {}};
            up.key = it->second;
            script.push_back(down);
            script.push_back(up);
        } else if (a == "--type") {
            const std::string v = next();
            const auto c = v.find(':');
            Scripted s{std::atof(v.substr(0, c).c_str()), 3, {}};
            for (size_t k = c + 1; k < v.size(); ++k) {
                if (v[k] == '\\' && k + 1 < v.size() && v[k + 1] == 'n') {
                    s.text.push_back('\r');
                    ++k;
                } else {
                    s.text.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(v[k]))));
                }
            }
            script.push_back(s);
        } else if (a == "--drive") {
            const std::string m = next();
            driveMode = m == "true" ? emu::DriveMode::True1541 : m == "none" ? emu::DriveMode::None : emu::DriveMode::Virtual;
        } else if (a == "--export-d64") {
            exportD64 = next();
        } else if (a == "--no-video") {
            noVideo = true;
        } else if (a == "--port") {
            const auto p = split(next());
            const int port = std::atoi(p[0].c_str()) - 1;
            portDevices[port & 1] = p[1] == "mouse" ? emu::PortDevice::Mouse1351
                                    : p[1] == "paddles" ? emu::PortDevice::Paddles : emu::PortDevice::Joystick;
        } else if (a == "--paddle" || a == "--mouse") {
            const auto p = split(next());
            Scripted s{std::atof(p[0].c_str()), a == "--paddle" ? 6 : 7, {}};
            s.key = static_cast<uint8_t>(std::atoi(p[1].c_str()) - 1);
            s.bytes = {static_cast<uint8_t>(std::atoi(p[2].c_str())), static_cast<uint8_t>(std::atoi(p[3].c_str()))};
            script.push_back(s);
        } else if (a == "--peek") {
            const auto p = split(next());
            Scripted s{std::atof(p[0].c_str()), 8, {}};
            s.text = p[1];
            script.push_back(s);
        } else if (a == "--sidregs") {
            script.push_back({std::atof(next().c_str()), 5, {}});
        } else if (a == "--shot") {
            const std::string v = next();
            const auto c = v.find(':');
            Scripted s{std::atof(v.substr(0, c).c_str()), 4, {}};
            s.text = v.substr(c + 1);
            script.push_back(s);
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    std::stable_sort(script.begin(), script.end(), [](const Scripted& a, const Scripted& b) { return a.t < b.t; });

    emu::Roms roms;
    if (romDir.empty())
        romDir = romFolder();
    std::string report;
    if (!romsFromFolder(romDir, roms, report)) {
        std::fprintf(stderr, "ROMs missing in %s: %s\n", romDir.c_str(), report.c_str());
        return 1;
    }

    auto enginePtr = std::make_unique<Engine>(); // large; keep it off the stack
    Engine& engine = *enginePtr;
    engine.realtime = false; // fixed warp: renders repeat exactly
    auto& c64 = engine.machine();
    c64.setRoms(roms);
    engine.setStandard(ntsc ? emu::VideoStandard::NTSC : emu::VideoStandard::PAL);
    engine.setSampleRate(rate);
    engine.configureSids(sidCfg);
    c64.setMidiInterface(iface);
    c64.setPortDevice(0, portDevices[0]);
    c64.setPortDevice(1, portDevices[1]);
    c64.setDriveMode(driveMode);
    if (driveMode == emu::DriveMode::True1541 && !c64.drive1541.hasRom()) {
        std::fprintf(stderr, "the true 1541 needs the 1541 DOS ROM in %s\n", romDir.c_str());
        return 1;
    }
    engine.videoWanted = !noVideo;
    c64.powerOn();

    std::string err;
    std::vector<uint8_t> data;
    if (!prg.empty()) {
        if (!readFile(prg, data) || !c64.attachPrg(data, true, err)) {
            std::fprintf(stderr, "%s: %s\n", prg.c_str(), err.empty() ? "cannot read" : err.c_str());
            return 1;
        }
    }
    if (!d64.empty()) {
        if (!readFile(d64, data) || !c64.attachDisk(data, prg.empty(), err)) {
            std::fprintf(stderr, "%s: %s\n", d64.c_str(), err.empty() ? "cannot read" : err.c_str());
            return 1;
        }
    }
    if (!attach.empty()) {
        if (!readFile(attach, data) || !c64.attachDisk(data, false, err)) {
            std::fprintf(stderr, "%s: %s\n", attach.c_str(), err.empty() ? "cannot read" : err.c_str());
            return 1;
        }
    }
    if (!crt.empty()) {
        if (!readFile(crt, data) || !c64.attachCartridge(data, crt, err)) {
            std::fprintf(stderr, "%s: %s\n", crt.c_str(), err.empty() ? "cannot read" : err.c_str());
            return 1;
        }
    }
    engine.resync();

    const Palette& pal = palette(paletteKey);
    std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block)), wavData;
    std::vector<MidiEvent> events, midiOut;
    midiOut.reserve(4096); // Engine::process appends only up to the capacity
    std::vector<uint8_t> frame(static_cast<size_t>(emu::Vic::kWidth) * emu::Vic::kHeight);
    const uint64_t total = static_cast<uint64_t>(seconds * rate);
    uint64_t done = 0;
    size_t next = 0;
    double nextTrace = 1.0;
    // The script's clock starts when the program has been started (the warp
    // that boots the machine takes no host time).
    uint64_t warpBlocks = 0;
    while (done < total) {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(block), total - done));
        events.clear();
        if (!engine.warping()) {
            const double t0 = static_cast<double>(done) / rate, t1 = static_cast<double>(done + n) / rate;
            while (next < script.size() && script[next].t < t1) {
                const auto& s = script[next];
                const uint32_t off = static_cast<uint32_t>(std::max(0.0, (s.t - t0) * rate));
                switch (s.kind) {
                case 0:
                    for (size_t k = 0; k < s.bytes.size(); k += 3) {
                        MidiEvent e;
                        e.offset = std::min(off, n - 1);
                        e.size = static_cast<uint8_t>(std::min<size_t>(3, s.bytes.size() - k));
                        std::memcpy(e.data, &s.bytes[k], e.size);
                        events.push_back(e);
                    }
                    break;
                case 1: c64.setKey(s.key, true); break;
                case 2: c64.setKey(s.key, false); break;
                case 3: c64.typeText(s.text); break;
                case 5: {
                    std::printf("SID1 @%.2fs:", s.t);
                    for (int r = 0; r < 25; ++r)
                        std::printf(" %02X", c64.sids.lastWritten(0, static_cast<uint8_t>(r)));
                    for (int v = 0; v < 3; ++v) {
                        const int f = c64.sids.lastWritten(0, static_cast<uint8_t>(7 * v)) |
                                      (c64.sids.lastWritten(0, static_cast<uint8_t>(7 * v + 1)) << 8);
                        std::printf("  v%d %.1fHz", v + 1, f * c64.clockHz() / 16777216.0);
                    }
                    std::printf("\n");
                    break;
                }
                case 6: c64.setPaddles(s.key, s.bytes[0], s.bytes[1]); break;
                case 7: c64.moveMouse(s.key, static_cast<int8_t>(s.bytes[0]), static_cast<int8_t>(s.bytes[1])); break;
                case 8: {
                    const auto addr = static_cast<uint16_t>(std::strtoul(s.text.c_str(), nullptr, 16));
                    std::printf("peek @%.2fs $%04X = $%02X (%d)\n", s.t, addr, c64.peek(addr), c64.peek(addr));
                    break;
                }
                case 4:
                    engine.copyFrame(frame.data());
                    if (writePng(s.text, frame.data(), pal))
                        std::printf("shot %.2fs -> %s\n", s.t, s.text.c_str());
                    break;
                }
                ++next;
            }
        }
        midiOut.clear();
        Engine::Outputs out;
        out.left = l.data();
        out.right = r.data();
        const bool warp = engine.warping();
        engine.process(n, events.data(), events.size(), out, midiOut);
        if (warp) {
            if (++warpBlocks > 100000) {
                std::fprintf(stderr, "never left warp: the program did not start\n");
                return 1;
            }
            continue;
        }
        for (uint32_t k = 0; k < n; ++k) {
            wavData.push_back(l[k]);
            wavData.push_back(r[k]);
        }
        for (const auto& e : midiOut)
            std::printf("midi out @%.4fs: %02X\n", static_cast<double>(done + e.offset) / rate, e.data[0]);
        done += n;
        if (trace && static_cast<double>(done) / rate >= nextTrace) {
            std::printf("t=%.0fs pc=%04X frames=%llu clk=%llu\n", nextTrace, c64.cpu.s.pc,
                        static_cast<unsigned long long>(c64.vic.frameCount()), static_cast<unsigned long long>(c64.clk()));
            nextTrace += 1.0;
        }
    }
    // Shots scheduled past the end.
    for (; next < script.size(); ++next) {
        if (script[next].kind == 4) {
            engine.copyFrame(frame.data());
            writePng(script[next].text, frame.data(), pal);
            std::printf("shot (end) -> %s\n", script[next].text.c_str());
        }
    }

    float peak = 0.0f;
    double sum = 0.0;
    for (float v : wavData) {
        peak = std::max(peak, std::fabs(v));
        sum += static_cast<double>(v) * v;
    }
    const double rms = wavData.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(wavData.size()));
    std::printf("audio: %.2fs, peak %.1f dBFS, rms %.1f dBFS; warp blocks %llu; pc=%04X\n",
                static_cast<double>(done) / rate, 20.0 * std::log10(std::max(peak, 1e-9f)),
                20.0 * std::log10(std::max(rms, 1e-12)), static_cast<unsigned long long>(warpBlocks), c64.cpu.s.pc);
    if (!exportD64.empty()) {
        if (!writeFile(exportD64, c64.diskImage().data()))
            std::fprintf(stderr, "cannot write %s\n", exportD64.c_str());
        else
            std::printf("disk -> %s\n", exportD64.c_str());
    }
    if (!wav.empty() && !writeWav(wav, wavData, rate)) {
        std::fprintf(stderr, "cannot write %s\n", wav.c_str());
        return 1;
    }
    return 0;
}
