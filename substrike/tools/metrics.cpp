// Measures kicks: the numbers in the table in docs/PLAN.md, from WAV files.
// It knows nothing about Substrike, so the reference kicks and renders of the
// factory presets go through the same code.
//
//   substrike-metrics kick.wav more.wav folder/          one row per file
//   substrike-metrics --compare references/ renders/    how well the second set
//                                                       covers the first
//   substrike-metrics --csv ...                          the rows as CSV
//
// What it measures, from the hit's onset (the first sample within 40 dB of
// the peak), on the mid channel:
//
//   end Hz      final pitch: the median of the confident pitch frames from
//               150 ms on (or the last third of the body, if it is shorter)
//   start Hz    the pitch of the first confident frame
//   sweep ms    when the pitch is within 10 % of the final pitch for good
//   body ms     until the envelope (2 ms RMS) stays under -20 dB of its peak
//   tail ms     the same at -40 dB
//   click dB    energy above 1.5 kHz in the first 12 ms against all of it
//   harm dB     energy above 3.5 x the final pitch from 40 to 200 ms against
//               all of it
//   crest dB    peak against RMS from 30 to 200 ms
//   side dB     side against mid energy over the hit
//   drift ct    the spread (standard deviation, in cents) of the pitch over
//               the tail, from 200 ms on: high for a noisy or wobbling tail
//
// Pitch is YIN (de Cheveigne and Kawahara, 2002), hop 2.5 ms, 25 Hz to
// 2.5 kHz, over frames of three periods of the pitch found before (5 to
// 40 ms), so a fast sweep is followed rather than averaged; heavily
// distorted kicks still get a pitch, which the zero-crossing tracker of the
// first measurements could not do. The start pitch is the zero-crossing rate
// of the first periods, where no frame is short enough.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Audio
{
    double rate = 48000.0;
    std::vector<float> mid, side;
};

uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// PCM 16/24/32-bit, 32-bit float, plain or WAVE_FORMAT_EXTENSIBLE.
bool readWav(const std::string& path, Audio& a, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
    {
        error = "not a WAV file";
        return false;
    }
    int format = 0, channels = 0, bits = 0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;
    for (size_t pos = 12; pos + 8 <= b.size();)
    {
        const uint32_t size = u32(&b[pos + 4]);
        const unsigned char* body = &b[pos + 8];
        if (!std::memcmp(&b[pos], "fmt ", 4) && size >= 16)
        {
            format = u16(body);
            channels = u16(body + 2);
            a.rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && size >= 26)
                format = u16(body + 24);
        }
        else if (!std::memcmp(&b[pos], "data", 4))
        {
            data = body;
            dataSize = std::min<size_t>(size, b.size() - pos - 8);
        }
        pos += 8 + size + (size & 1);
    }
    if (!data || channels < 1 || !((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
    {
        error = "unsupported WAV format";
        return false;
    }
    const size_t frame = static_cast<size_t>(channels) * (bits / 8);
    const size_t n = dataSize / frame;
    auto sample = [&](size_t i, int c) -> double {
        const unsigned char* p = data + i * frame + static_cast<size_t>(c) * (bits / 8);
        if (format == 3)
        {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        if (bits == 16)
            return static_cast<int16_t>(u16(p)) / 32768.0;
        if (bits == 24)
            return static_cast<int32_t>((p[0] << 8) | (p[1] << 16) | (static_cast<uint32_t>(p[2]) << 24)) / 2147483648.0;
        return static_cast<int32_t>(u32(p)) / 2147483648.0;
    };
    a.mid.resize(n);
    a.side.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const double l = sample(i, 0), r = channels > 1 ? sample(i, 1) : l;
        a.mid[i] = static_cast<float>(0.5 * (l + r));
        a.side[i] = static_cast<float>(0.5 * (l - r));
    }
    return true;
}

// A Butterworth highpass, as two cascaded RBJ biquads (fourth order).
std::vector<float> highpass(const std::vector<float>& x, double f, double rate)
{
    std::vector<float> y = x;
    const double qs[2] = {0.5411961, 1.3065630};
    for (double q : qs)
    {
        const double w = 2.0 * kPi * std::min(f, 0.45 * rate) / rate, c = std::cos(w), al = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + al;
        const double b0 = (1.0 + c) / 2.0 / a0, b1 = -(1.0 + c) / a0, b2 = b0, a1 = -2.0 * c / a0, a2 = (1.0 - al) / a0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (float& v : y)
        {
            const double in = v, out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = out;
            v = static_cast<float>(out);
        }
    }
    return y;
}

double energy(const std::vector<float>& x, size_t a, size_t b)
{
    double e = 0.0;
    for (size_t i = a; i < std::min(b, x.size()); ++i)
        e += static_cast<double>(x[i]) * x[i];
    return e;
}

// Energy ratios in dB, floored at -60: below that a click, harmonics or a
// side signal are inaudible, and a mono file's -inf would swamp every
// comparison.
double db(double ratio) { return std::max(-60.0, 10.0 * std::log10(std::max(ratio, 1e-30))); }

// YIN over a frame of `w` samples starting at `at`, looking for periods up
// to `tauMax`; 0 when there is no confident pitch. The first dip under 0.15
// wins; failing that the deepest one, if it is under 0.35.
double yin(const std::vector<float>& x, size_t at, double rate, int w, int tauMax)
{
    const int tauMin = std::max(2, static_cast<int>(rate / 2500.0));
    tauMax = std::min(tauMax, static_cast<int>(rate / 25.0));
    if (tauMax <= tauMin + 2 || at + static_cast<size_t>(w + tauMax) >= x.size())
        return 0.0;
    std::vector<double> c(static_cast<size_t>(tauMax + 2), 1.0);
    double sum = 0.0;
    for (int tau = 1; tau <= tauMax + 1; ++tau)
    {
        double d = 0.0;
        for (int j = 0; j < w; ++j)
        {
            const double diff = x[at + static_cast<size_t>(j)] - x[at + static_cast<size_t>(j + tau)];
            d += diff * diff;
        }
        sum += d;
        c[static_cast<size_t>(tau)] = sum > 0.0 ? d * tau / sum : 1.0;
    }
    int best = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau)
        if (c[static_cast<size_t>(tau)] < 0.15)
        {
            while (tau + 1 <= tauMax && c[static_cast<size_t>(tau + 1)] < c[static_cast<size_t>(tau)])
                ++tau;
            best = tau;
            break;
        }
    if (best < 0)
    {
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (best < 0 || c[static_cast<size_t>(tau)] < c[static_cast<size_t>(best)])
                best = tau;
        if (best < 0 || c[static_cast<size_t>(best)] > 0.35 || best == tauMax)
            return 0.0;
    }
    // Parabolic interpolation around the dip.
    const double a = c[static_cast<size_t>(best - 1)], b = c[static_cast<size_t>(best)], e = c[static_cast<size_t>(best + 1)];
    const double den = a - 2.0 * b + e;
    const double t = best + (std::fabs(den) > 1e-12 ? 0.5 * (a - e) / den : 0.0);
    return rate / t;
}

// The pitch at the very start, from the first few periods: the time between
// upward zero crossings over the first three of them, from 1.5 ms after the
// onset (past a click) on the signal through two one-pole lowpasses at
// 2 kHz, so it is the body's sweep that is measured and not the click.
double startPitch(const std::vector<float>& x, size_t onset, double rate)
{
    const double k = 1.0 - std::exp(-2.0 * kPi * 2000.0 / rate);
    double lp1 = 0.0, lp = 0.0, prev = 0.0;
    std::vector<double> ups;
    const size_t skip = onset + static_cast<size_t>(0.0015 * rate);
    for (size_t i = onset; i < std::min(x.size(), onset + static_cast<size_t>(0.06 * rate)) && ups.size() < 4; ++i)
    {
        lp1 += k * (x[i] - lp1);
        lp += k * (lp1 - lp);
        if (i < skip)
        {
            prev = lp;
            continue;
        }
        if (prev < 0.0 && lp >= 0.0)
            ups.push_back(static_cast<double>(i) - prev / (lp - prev)); // interpolated crossing
        prev = lp;
    }
    if (ups.size() < 2)
        return 0.0;
    return rate * static_cast<double>(ups.size() - 1) / (ups.back() - ups.front());
}

struct Metrics
{
    std::string name;
    double endHz = 0, startHz = 0, sweepMs = 0, bodyMs = 0, tailMs = 0, clickDb = 0, harmDb = 0, crestDb = 0,
           sideDb = 0, driftCt = 0;
};

constexpr int kFields = 10;
const char* const kFieldNames[kFields] = {"end Hz",  "start Hz", "sweep ms", "body ms", "tail ms",
                                          "click dB", "harm dB", "crest dB", "side dB", "drift ct"};
double field(const Metrics& m, int i)
{
    const double v[kFields] = {m.endHz, m.startHz, m.sweepMs, m.bodyMs,  m.tailMs,
                               m.clickDb, m.harmDb, m.crestDb, m.sideDb, m.driftCt};
    return v[i];
}

bool measure(const std::string& path, Metrics& m, std::string& error)
{
    Audio a;
    if (!readWav(path, a, error))
        return false;
    m.name = std::filesystem::path(path).stem().string();
    const std::vector<float>& x = a.mid;
    const double rate = a.rate;
    float peak = 0.0f;
    for (float v : x)
        peak = std::max(peak, std::fabs(v));
    if (peak <= 0.0f)
    {
        error = "silent";
        return false;
    }
    size_t onset = 0;
    while (onset < x.size() && std::fabs(x[onset]) < peak * 0.01f)
        ++onset;
    auto at = [&](double ms) { return onset + static_cast<size_t>(ms * 0.001 * rate); };

    // Envelope: 2 ms RMS, hop 1 ms.
    const size_t hop = static_cast<size_t>(0.001 * rate), win = 2 * hop;
    std::vector<double> env;
    for (size_t i = onset; i + win < x.size(); i += hop)
        env.push_back(std::sqrt(energy(x, i, i + win) / static_cast<double>(win)));
    const double envPeak = env.empty() ? 1e-9 : *std::max_element(env.begin(), env.end());
    auto fallTo = [&](double dbDown) {
        const double level = envPeak * std::pow(10.0, -dbDown / 20.0);
        size_t last = 0;
        for (size_t i = 0; i < env.size(); ++i)
            if (env[i] >= level)
                last = i;
        return static_cast<double>(last + 1);
    };
    m.bodyMs = fallTo(20.0);
    m.tailMs = fallTo(40.0);

    // Pitch track over the body, down to -40 dB.
    struct Frame
    {
        double ms, hz;
    };
    std::vector<Frame> track;
    const size_t step = static_cast<size_t>(0.0025 * rate);
    const double first = startPitch(x, onset, rate);
    double guess = first > 0.0 ? first : 100.0;
    for (size_t i = onset; i < at(m.tailMs); i += step)
    {
        const int w = static_cast<int>(std::clamp(3.0 / guess, 0.005, 0.040) * rate);
        // Up to twice the guessed period, so a falling pitch is still found.
        const int tauMax = std::max(static_cast<int>(2.0 * rate / guess), static_cast<int>(0.005 * rate));
        double hz = yin(x, i, rate, w, std::min(tauMax, w));
        if (hz <= 0.0)
            hz = yin(x, i, rate, static_cast<int>(0.040 * rate), static_cast<int>(0.040 * rate));
        if (hz > 0.0)
        {
            track.push_back({(i - onset) * 1000.0 / rate + 0.5 * w * 1000.0 / rate, hz}); // a frame's centre
            guess = hz;
        }
    }
    std::vector<double> late;
    const double from = m.bodyMs > 225.0 ? 150.0 : m.bodyMs * 2.0 / 3.0;
    for (const Frame& f : track)
        if (f.ms >= from)
            late.push_back(f.hz);
    if (late.empty())
        for (const Frame& f : track)
            late.push_back(f.hz);
    if (!late.empty())
    {
        std::nth_element(late.begin(), late.begin() + static_cast<long>(late.size() / 2), late.end());
        m.endHz = late[late.size() / 2];
    }
    if (!track.empty())
    {
        m.startHz = std::max(first, track.front().hz);
        m.sweepMs = 0.0;
        for (const Frame& f : track)
            if (m.endHz > 0.0 && std::fabs(f.hz / m.endHz - 1.0) > 0.10)
                m.sweepMs = f.ms;
        // Pitch drift over the tail.
        double s = 0.0, s2 = 0.0;
        int n = 0;
        for (const Frame& f : track)
            if (f.ms >= 200.0 && m.endHz > 0.0)
            {
                const double ct = 1200.0 * std::log2(f.hz / m.endHz);
                s += ct;
                s2 += ct * ct;
                ++n;
            }
        m.driftCt = n > 1 ? std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n))) : 0.0;
    }

    const std::vector<float> high = highpass(x, 1500.0, rate);
    m.clickDb = db(energy(high, onset, at(12)) / std::max(energy(x, onset, at(12)), 1e-30));
    if (m.endHz > 0.0)
    {
        const std::vector<float> harm = highpass(x, 3.5 * m.endHz, rate);
        m.harmDb = db(energy(harm, at(40), at(200)) / std::max(energy(x, at(40), at(200)), 1e-30));
    }
    float bodyPeak = 0.0f;
    for (size_t i = at(30); i < std::min(at(200), x.size()); ++i)
        bodyPeak = std::max(bodyPeak, std::fabs(x[i]));
    const double bodyRms = std::sqrt(energy(x, at(30), at(200)) / std::max(1.0, (at(200) - at(30)) * 1.0));
    m.crestDb = 20.0 * std::log10(std::max(static_cast<double>(bodyPeak), 1e-12) / std::max(bodyRms, 1e-12));
    m.sideDb = db(energy(a.side, onset, at(m.tailMs)) / std::max(energy(x, onset, at(m.tailMs)), 1e-30));
    return true;
}

std::vector<std::string> wavFiles(const std::string& arg)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (std::filesystem::is_directory(arg, ec))
    {
        for (const auto& e : std::filesystem::directory_iterator(arg, ec))
            if (e.is_regular_file(ec) && (e.path().extension() == ".wav" || e.path().extension() == ".WAV"))
                out.push_back(e.path().string());
        std::sort(out.begin(), out.end());
    }
    else
        out.push_back(arg);
    return out;
}

std::vector<Metrics> measureAll(const std::vector<std::string>& args)
{
    std::vector<Metrics> out;
    for (const std::string& a : args)
        for (const std::string& path : wavFiles(a))
        {
            Metrics m;
            std::string error;
            if (measure(path, m, error))
                out.push_back(m);
            else
                std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
        }
    return out;
}

void printRows(const std::vector<Metrics>& rows, bool csv)
{
    if (csv)
    {
        std::printf("name");
        for (const char* f : kFieldNames)
            std::printf(",%s", f);
        std::printf("\n");
        for (const Metrics& m : rows)
        {
            std::printf("\"%s\"", m.name.c_str());
            for (int i = 0; i < kFields; ++i)
                std::printf(",%.2f", field(m, i));
            std::printf("\n");
        }
        return;
    }
    std::printf("%-30s", "");
    for (const char* f : kFieldNames)
        std::printf("%9s", f);
    std::printf("\n");
    for (const Metrics& m : rows)
    {
        std::printf("%-30.30s", m.name.c_str());
        for (int i = 0; i < kFields; ++i)
            std::printf("%9.1f", field(m, i));
        std::printf("\n");
    }
}

// How a value sits on a metric's scale for distances: the times and the
// pitches on a log scale, the levels as they are.
double scaled(int i, double v)
{
    switch (i)
    {
    case 0:
    case 1: return std::log2(std::max(v, 10.0)) * 6.0;  // an octave = 6
    case 2:
    case 3:
    case 4: return std::log2(std::max(v, 5.0)) * 3.0;   // a doubling = 3
    case 9: return std::log2(std::max(v, 1.0)) * 1.0;
    default: return v / 3.0;                             // 3 dB = 1
    }
}

void compare(const std::vector<Metrics>& refs, const std::vector<Metrics>& set)
{
    std::printf("%-10s %28s   %28s   %s\n", "", "reference: min  median  max", "covered by: min  median  max",
                "references outside");
    for (int i = 0; i < kFields; ++i)
    {
        auto stats = [&](const std::vector<Metrics>& v, double& lo, double& med, double& hi) {
            std::vector<double> x;
            for (const Metrics& m : v)
                x.push_back(field(m, i));
            std::sort(x.begin(), x.end());
            lo = x.front();
            hi = x.back();
            med = x[x.size() / 2];
        };
        double rl, rm, rh, sl, sm, sh;
        stats(refs, rl, rm, rh);
        stats(set, sl, sm, sh);
        int outside = 0;
        for (const Metrics& m : refs)
            outside += field(m, i) < sl || field(m, i) > sh;
        std::printf("%-10s %9.1f %8.1f %9.1f   %10.1f %8.1f %9.1f   %d of %zu\n", kFieldNames[i], rl, rm, rh, sl, sm, sh,
                    outside, refs.size());
    }
    // For every reference: the nearest of the set, and how near.
    std::printf("\nnearest to each reference (distance: about one per octave of pitch, per doubling of time, per 3 dB)\n");
    double total = 0.0;
    for (const Metrics& r : refs)
    {
        double best = 1e300;
        const Metrics* near = nullptr;
        for (const Metrics& m : set)
        {
            double d = 0.0;
            for (int i = 0; i < kFields; ++i)
            {
                const double e = scaled(i, field(r, i)) - scaled(i, field(m, i));
                d += e * e;
            }
            d = std::sqrt(d / kFields);
            if (d < best)
            {
                best = d;
                near = &m;
            }
        }
        total += best;
        std::printf("  %-30.30s -> %-30.30s %5.2f\n", r.name.c_str(), near ? near->name.c_str() : "-", best);
    }
    std::printf("mean distance %.2f\n", total / static_cast<double>(refs.size()));
}

} // namespace

int main(int argc, char** argv)
{
    bool csv = false;
    std::vector<std::string> args;
    std::string compareRefs, compareSet;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--csv")
            csv = true;
        else if (a == "--compare" && i + 2 < argc)
        {
            compareRefs = argv[++i];
            compareSet = argv[++i];
        }
        else
            args.push_back(a);
    }
    if (!compareRefs.empty())
    {
        const std::vector<Metrics> refs = measureAll({compareRefs}), set = measureAll({compareSet});
        if (refs.empty() || set.empty())
        {
            std::fprintf(stderr, "nothing to compare\n");
            return 1;
        }
        compare(refs, set);
        return 0;
    }
    if (args.empty())
    {
        std::fprintf(stderr, "usage: substrike-metrics [--csv] file.wav|folder ...\n"
                             "       substrike-metrics --compare references/ renders/\n");
        return 2;
    }
    printRows(measureAll(args), csv);
    return 0;
}
