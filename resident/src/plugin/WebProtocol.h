#pragma once

// Messages between the WCLAP's plugin (module.wasm, in the host's audio
// worklet) and its page (ui.wasm, the editor), carried by clap.webview: the
// page posts to the plugin, the plugin answers through
// clap_host_webview.send(). Binary, little endian. The page asks, the plugin
// answers: it never sends unasked, so a closed page costs nothing.
//
// The page polls about 30 times a second with the serial of every part of
// the plugin's state it holds; the answer carries only the parts whose
// serial moved. The screen goes as the lines that changed since the frame
// the page has (or whole, when it has none), each line run-length coded or
// packed two pixels to a byte, whichever is smaller, and only while the page
// shows it and the VIC has drawn a new frame.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace resident::web {

enum PageMessage : uint8_t
{
    // [type][u8 flags: 1 = the screen is shown][u16 0]
    // [u32 serials: values, settings, CC map, media, notice, screen]
    PagePoll = 1,
    // A parameter: [type][u8 0 begin, 1 value, 2 end][u16 0][u32 id][f64 value]
    PageParam = 2,
    // The machine settings (MachineSettings::toText): [type][0 0 0][text]
    PageSettings = 3,
    // The CC map (CcMap::toText): [type][0 0 0][text]
    PageCcMap = 4,
    // Keyboard, joystick, mouse, paddles, as the native window sends them:
    // [type][u8 InputKind][u8 a][u8 b][i16 c][i16 d]
    PageInput = 5,
    // A button: [type][u8 Command][u16 0][string argument]
    PageCommand = 6,
    // A file, in chunks of at most kUploadChunk bytes:
    // [type][u8 UploadKind][u16 0][u32 total][u32 offset][string name][bytes]
    // The chunk at offset 0 starts a file; the last one completes it.
    PageUpload = 7,
    // The editor wants another window size (logical pixels): [type][0 0 0][u32 w][u32 h]
    PageResize = 8,
};

enum InputKind : uint8_t
{
    InputKey = 1,        // a matrix position, b down
    InputJoystick = 2,   // a port (0, 1), b bits
    InputRestore = 3,
    InputMouse = 4,      // a port, c dx, d dy
    InputPaddles = 5,    // a port, c paddle A, d paddle B
    InputButtons = 6,    // a port, b bits
};

enum Command : uint8_t
{
    CommandReset = 1,
    CommandPower = 2,
    CommandWarpOn = 3,
    CommandWarpOff = 4,
    CommandEjectDisk = 5,
    CommandEjectCartridge = 6,
    CommandFreeze = 7,
    CommandStartFromDisk = 8, // argument: the PETSCII name, "*" for the first file
    CommandExportDisk = 9,    // argument: the file name; answered with PluginDiskImage
};

enum UploadKind : uint8_t
{
    UploadLoad = 0, // a program, cartridge or disk, started as the Load button does
    UploadDisk = 1, // a disk, only inserted
    UploadRom = 2,  // a ROM image, kept in the project
};

constexpr uint32_t kUploadChunk = 32768;
constexpr uint32_t kPollSize = 28;

enum PluginMessage : uint8_t
{
    // [type][u8 StatusFlags][u8 ROMs in use][u8 ROMs in the project]
    // [u32 MIDI in count][u32 MIDI out count][u16 sections][u16 0]
    // then each section: [u8 Section][u8 0][u16 0][u32 serial][u32 size][payload]
    PluginStatus = 1,
    // The inserted disk as a .d64: [type][0 0 0][string name][bytes]
    PluginDiskImage = 2,
};

constexpr uint32_t kStatusHeader = 16;
constexpr uint32_t kSectionHeader = 12;

enum StatusFlags : uint8_t
{
    StatusHasRoms = 1,
    StatusHasDriveRom = 2,
    StatusWarping = 4,
    StatusDriveLed = 8,
    StatusFreezable = 16,
    StatusDisk = 32,
};

enum Section : uint8_t
{
    // f64 per parameter, in the plugin's parameter order.
    SectionValues = 1,
    // MachineSettings::toText.
    SectionSettings = 2,
    // CcMap::toText.
    SectionCcMap = 3,
    // [u32 disks inserted from the page][string summary][string disk title]
    // [string ROM report][u16 files], per file:
    // [string petscii][string display][string type][u16 blocks]
    SectionMedia = 4,
    // [u8 error][text]: something for the status line.
    SectionNotice = 5,
    // [u32 base serial, 0 = whole][u16 lines][u16 0], per line:
    // [u16 y][u8 ScreenLine][u8 0][u16 bytes][data]
    SectionScreen = 6,
};

enum ScreenLine : uint8_t
{
    // A byte per run: (length - 1) << 4 | colour, runs of 1 to 16 pixels.
    LineRuns = 0,
    // Two pixels per byte, the left one in the low nibble.
    LinePacked = 1,
    // Against the same line of the page's frame (never in a whole screen):
    // 0x00-0x7f skips 1 to 128 unchanged pixels; 0x80-0xff is a run of
    // ((b >> 4) & 7) + 1 pixels of colour b & 15.
    LineDiff = 2,
};

// A string is [u16 length][bytes], no terminator.
inline void putString(std::vector<uint8_t>& out, const std::string& s)
{
    const uint16_t n = static_cast<uint16_t>(s.size() > 0xffff ? 0xffff : s.size());
    out.push_back(static_cast<uint8_t>(n));
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.insert(out.end(), s.begin(), s.begin() + n);
}

inline bool getString(const uint8_t* in, size_t size, size_t& pos, std::string& s)
{
    if (pos + 2 > size)
        return false;
    const uint16_t n = static_cast<uint16_t>(in[pos] | (in[pos + 1] << 8));
    if (pos + 2 + n > size)
        return false;
    s.assign(reinterpret_cast<const char*>(in + pos + 2), n);
    pos += 2u + n;
    return true;
}

template <typename T> inline void put(std::vector<uint8_t>& out, T v)
{
    uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.insert(out.end(), b, b + sizeof(T));
}

template <typename T> inline T get(const uint8_t* in, size_t at)
{
    T v;
    std::memcpy(&v, in + at, sizeof(T));
    return v;
}

// One screen line (`width` palette indices) in whichever form is smaller;
// `prev`, when there is one, is the line as the page has it.
inline void encodeLine(const uint8_t* px, int width, std::vector<uint8_t>& out, uint8_t& mode,
                       const uint8_t* prev = nullptr)
{
    const size_t start = out.size();
    size_t diffSize = static_cast<size_t>(width);
    std::vector<uint8_t> diff;
    if (prev) {
        for (int x = 0; x < width && diff.size() < diffSize;) {
            if ((px[x] & 15) == (prev[x] & 15)) {
                int n = 1;
                while (x + n < width && n < 128 && (px[x + n] & 15) == (prev[x + n] & 15))
                    ++n;
                x += n;
                if (x < width)
                    diff.push_back(static_cast<uint8_t>(n - 1)); // a skip at the end is implied
                continue;
            }
            const uint8_t c = px[x] & 15;
            int n = 1;
            while (x + n < width && n < 8 && (px[x + n] & 15) == c)
                ++n;
            diff.push_back(static_cast<uint8_t>(0x80 | ((n - 1) << 4) | c));
            x += n;
        }
        diffSize = diff.size();
    }
    for (int x = 0; x < width;) {
        const uint8_t c = px[x] & 15;
        int n = 1;
        while (x + n < width && n < 16 && (px[x + n] & 15) == c)
            ++n;
        out.push_back(static_cast<uint8_t>(((n - 1) << 4) | c));
        x += n;
    }
    const size_t runsSize = out.size() - start;
    if (prev && diffSize < runsSize && diffSize < static_cast<size_t>(width / 2)) {
        out.resize(start);
        out.insert(out.end(), diff.begin(), diff.end());
        mode = LineDiff;
        return;
    }
    if (runsSize <= static_cast<size_t>(width / 2)) {
        mode = LineRuns;
        return;
    }
    out.resize(start);
    for (int x = 0; x < width; x += 2)
        out.push_back(static_cast<uint8_t>((px[x] & 15) | ((x + 1 < width ? px[x + 1] & 15 : 0) << 4)));
    mode = LinePacked;
}

// Back into palette indices, over the line as it was for LineDiff; false
// when the data does not fit the line.
inline bool decodeLine(const uint8_t* in, size_t size, uint8_t mode, uint8_t* px, int width)
{
    if (mode == LineDiff) {
        int x = 0;
        for (size_t i = 0; i < size; ++i) {
            if (!(in[i] & 0x80)) {
                x += in[i] + 1;
                continue;
            }
            const int n = ((in[i] >> 4) & 7) + 1;
            if (x + n > width)
                return false;
            for (int k = 0; k < n; ++k)
                px[x++] = in[i] & 15;
        }
        return x <= width;
    }
    if (mode == LinePacked) {
        if (size < static_cast<size_t>((width + 1) / 2))
            return false;
        for (int x = 0; x < width; ++x)
            px[x] = (x & 1) ? (in[x / 2] >> 4) : (in[x / 2] & 15);
        return true;
    }
    int x = 0;
    for (size_t i = 0; i < size && x < width; ++i) {
        const int n = (in[i] >> 4) + 1;
        const uint8_t c = in[i] & 15;
        for (int k = 0; k < n && x < width; ++k)
            px[x++] = c;
    }
    return x == width;
}

} // namespace resident::web
