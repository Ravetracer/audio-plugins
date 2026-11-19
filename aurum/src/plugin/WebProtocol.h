#pragma once

// Messages between the WCLAP's DSP module (module.wasm, the plugin) and its
// page (ui.wasm, the editor), carried by clap.webview: the page posts to the
// plugin, the plugin answers through clap_host_webview.send(). Binary, little
// endian, fixed offsets. The page asks, the plugin answers: it never sends
// unasked, so a closed page costs nothing.
//
// Values are the plugin's stored units (ParamTable), by parameter index.

#include <cstdint>
#include <cstring>
#include <string>

namespace aurum::web {

enum PageMessage : uint8_t
{
    // [type][0 0 0][u32 serial]: answered by Status. Serial 0 asks for the
    // values even when nothing changed (the page just opened).
    PagePoll = 1,
    // [type][0 0 0][u32 index]
    PageBegin = 2,
    // [type][0 0 0][u32 index][f64 value]
    PageSet = 3,
    // [type][0 0 0][u32 index]
    PageEnd = 4,
    // A whole new sound (preset, IR import, A/B): the plugin cuts the tail and
    // takes all values at once. The preset's name follows as PagePreset.
    // [type][0 0 0][u32 count][f64 values...]
    PageApply = 5,
    // The preset the editor's session is on, for the plugin's state.
    // [type][0 0 0][string name][string path]
    PagePreset = 6,
    // The editor wants another window size (logical pixels).
    // [type][0 0 0][u32 width][u32 height]
    PageResize = 7,
    // The parameter under the pointer, -1 for none (clap.param-hovered).
    // [type][0 0 0][i32 index]
    PageHover = 8,
};

enum PluginMessage : uint8_t
{
    // [type][u8 hasValues][u16 count][u32 serial][f64 sampleRate][f64 tempo]
    // then, when hasValues: [f64 values...][string name][string path]
    PluginStatus = 1,
};

constexpr uint32_t kStatusHeader = 24;

// A string is [u16 length][bytes], no terminator.
inline size_t putString(uint8_t* out, const std::string& s)
{
    const uint16_t n = static_cast<uint16_t>(s.size() > 0xffff ? 0xffff : s.size());
    std::memcpy(out, &n, 2);
    std::memcpy(out + 2, s.data(), n);
    return 2u + n;
}

inline bool getString(const uint8_t* in, size_t size, size_t& pos, std::string& s)
{
    if (pos + 2 > size)
        return false;
    uint16_t n;
    std::memcpy(&n, in + pos, 2);
    if (pos + 2 + n > size)
        return false;
    s.assign(reinterpret_cast<const char*>(in + pos + 2), n);
    pos += 2u + n;
    return true;
}

} // namespace aurum::web
