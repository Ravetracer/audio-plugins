#pragma once

// Messages between the WCLAP's DSP module (module.wasm, the plugin) and its
// page (ui.wasm, the editor), carried by clap.webview: the page posts to the
// plugin, the plugin answers through clap_host_webview.send(). Binary, little
// endian. The page asks, the plugin answers: it never sends unasked, so a
// closed page costs nothing.
//
// Values are stored values (ParamTable), by parameter index; curves by
// dsp::curveIndex(). The plugin counts its changes with a serial: a poll names
// the serial the page has seen and the answer carries only what changed since
// (all of it for serial 0), so a still plugin answers with the header alone.
//
// Every answer goes to every open page; a page keeps only those that carry its
// own page id, which it chose at random.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "dsp/Curve.h"

namespace substrike::web {

enum PageMessage : uint8_t
{
    // [type][0 0 0][u32 page][u32 serial][u32 edits]: answered by Status.
    // `edits` counts the edits this page has sent so far; the answer echoes
    // it, so the page can tell a value of its own still on the way from one
    // the plugin changed.
    PagePoll = 1,
    // A gesture on a parameter: [type][0 0 0][u32 index], Set adds [f64 value].
    PageBegin = 2,
    PageSet = 3,
    PageEnd = 4,
    // A curve: [type][0 0 0][u32 index][u32 count][count x (f64 x, f64 y, f64 k)]
    PageCurve = 5,
    // A whole new state (a preset): every value, every curve, the name.
    // [type][0 0 0][u32 count][f64 values...]
    // [every curve in order: u8 points, points x (f64 x, f64 y, f64 k)][string name]
    PageLoad = 6,
    // [type][0 0 0]
    PageAudition = 7,
    PageCheckpoint = 8,
    PageUndo = 9,
    PageRedo = 10,
    // The editor wants another window size (logical pixels).
    // [type][0 0 0][u32 width][u32 height]
    PageResize = 11,
};

enum PluginMessage : uint8_t
{
    // Header, kStatusHeader bytes:
    //   [0]  type        [1] flags (StatusFlags)    [2] u16 reserved
    //   [4]  u32 page    [8] u32 serial             [12] u32 edits (echoed)
    //   [16] f64 sampleRate
    //   [24] u16 values  [26] u16 curves  [28] u16 modulated  [30] u16 scope bins
    // then, in this order:
    //   values     count x [u16 index][f64 value]
    //   curves     count x [u8 index][u8 points][points x (f64 x, f64 y, f64 k)]
    //   preset     [string name], when StatusPreset is set
    //   modulated  count x [u16 index][f64 value], the parameters the matrix
    //              moves now, where it has them; when StatusModulation is
    //              set, and then the whole list (an empty one clears it)
    //   scope      count x [i16 lo][i16 hi], kScopeScale per unit, oldest first
    PluginStatus = 1,
};

enum StatusFlags : uint8_t
{
    StatusCanUndo = 1,
    StatusCanRedo = 2,
    StatusPreset = 4,
    StatusModulation = 8,
};

constexpr uint32_t kStatusHeader = 32;
// Scope bins as int16: +-2.0 (6 dB over full scale) fits.
constexpr float kScopeScale = 16384.0f;

// A string is [u16 length][bytes], no terminator.
inline void putString(std::vector<uint8_t>& out, const std::string& s)
{
    const uint16_t n = static_cast<uint16_t>(s.size() > 0xffff ? 0xffff : s.size());
    const size_t at = out.size();
    out.resize(at + 2 + n);
    std::memcpy(out.data() + at, &n, 2);
    std::memcpy(out.data() + at + 2, s.data(), n);
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

template <typename T> void put(std::vector<uint8_t>& out, T v)
{
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
}

template <typename T> bool get(const uint8_t* in, size_t size, size_t& pos, T& v)
{
    if (pos + sizeof(T) > size)
        return false;
    std::memcpy(&v, in + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

// A curve's points: [count][count x (f64 x, f64 y, f64 k)], the count as T.
template <typename T> void putCurve(std::vector<uint8_t>& out, const dsp::Curve& c)
{
    put<T>(out, static_cast<T>(c.count()));
    for (int i = 0; i < c.count(); ++i)
    {
        put(out, c.point(i).x);
        put(out, c.point(i).y);
        put(out, c.point(i).k);
    }
}

template <typename T> bool getCurve(const uint8_t* in, size_t size, size_t& pos, dsp::Curve& c)
{
    T count;
    if (!get(in, size, pos, count) || count < 2 || count > dsp::Curve::kMaxPoints)
        return false;
    dsp::CurvePoint points[dsp::Curve::kMaxPoints];
    for (T i = 0; i < count; ++i)
        if (!get(in, size, pos, points[i].x) || !get(in, size, pos, points[i].y) || !get(in, size, pos, points[i].k))
            return false;
    c.set(points, static_cast<int>(count));
    return true;
}

} // namespace substrike::web
