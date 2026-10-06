#include "IrImport.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "dsp/DecayModel.h"
#include "dsp/ReverbEngine.h"
#include "dsp/RoomModel.h"
#include "dsp/Svf.h"
#include "plugin/Params.h"

namespace aurum {

namespace {

constexpr double kBands[] = {63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
constexpr int kNumBands = 9;

// Energy envelope of one band around fc (sum over channels): Q sqrt(2) is an
// octave, Q 4.32 a third of one.
std::vector<double> bandEnergy(const AudioData& ir, double fc, size_t start, double q)
{
    const size_t n = ir.frames() - start;
    std::vector<double> e(n, 0.0);
    const dsp::SvfCoeffs bp = dsp::SvfCoeffs::bandPass(fc, q, ir.sampleRate);
    for (const auto& ch : ir.channels)
    {
        dsp::SvfState a, b;
        for (size_t i = 0; i < n; ++i)
        {
            const float y = b.process(a.process(ch[start + i], bp), bp);
            e[i] += static_cast<double>(y) * y;
        }
    }
    return e;
}

// T60 from the Schroeder integral with noise-floor truncation.
double decayTime(const std::vector<double>& e, double fs)
{
    const size_t n = e.size();
    if (n < static_cast<size_t>(0.05 * fs))
        return 0.0;
    // Noise floor from the last 10 %.
    const size_t tailStart = n - n / 10;
    double noise = 0.0;
    for (size_t i = tailStart; i < n; ++i)
        noise += e[i];
    noise /= static_cast<double>(n - tailStart);
    // Smoothed level (10 ms) to find where the decay meets the noise.
    const size_t win = std::max<size_t>(1, static_cast<size_t>(0.01 * fs));
    size_t peak = 0;
    double peakVal = 0.0;
    std::vector<double> smooth(n / win + 1, 0.0);
    for (size_t i = 0; i < n; ++i)
        smooth[i / win] += e[i] / static_cast<double>(win);
    for (size_t k = 0; k < smooth.size(); ++k)
        if (smooth[k] > peakVal)
        {
            peakVal = smooth[k];
            peak = k;
        }
    if (peakVal <= 0.0)
        return 0.0;
    size_t trunc = smooth.size() - 1;
    for (size_t k = peak; k < smooth.size(); ++k)
        if (smooth[k] < noise * 2.0)
        {
            trunc = k;
            break;
        }
    const size_t end = std::min(n, trunc * win);
    std::vector<double> edc(end + 1, 0.0);
    for (size_t i = end; i-- > 0;)
        edc[i] = edc[i + 1] + std::max(e[i] - noise, 0.0);
    const double e0 = edc[peak * win < end ? peak * win : 0];
    if (e0 <= 0.0)
        return 0.0;
    // Fit -5..-25 dB (T20); fall back to -5..-15 dB (T10).
    for (double lowDb : {-25.0, -15.0})
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int cnt = 0;
        bool reached = false;
        for (size_t i = peak * win; i < end; ++i)
        {
            const double db = 10.0 * std::log10(edc[i] / e0 + 1e-30);
            if (db > -5.0)
                continue;
            if (db < lowDb)
            {
                reached = true;
                break;
            }
            const double t = static_cast<double>(i) / fs;
            sx += t;
            sy += db;
            sxx += t * t;
            sxy += t * db;
            ++cnt;
        }
        if (!reached || cnt < 20)
            continue;
        const double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
        if (slope < 0.0)
            return -60.0 / slope;
    }
    return 0.0;
}

// Greedy fit of up to `maxBells` bells (+ edge shelves) to target values given
// at the octave centres. `eval(shape, f, f0, gain, q)` returns the response.
struct FitBand
{
    int shape; // 0 bell, 1 low shelf, 2 high shelf
    double freq, gain, q;
};

template <typename Eval>
std::vector<FitBand> fitCurve(const std::vector<double>& freqs, const std::vector<double>& target,
                              const std::vector<bool>& valid, int maxBells, double threshold, double maxGain, Eval eval)
{
    std::vector<FitBand> bands;
    auto residual = [&](size_t i) {
        double r = target[i];
        for (const auto& b : bands)
            r -= eval(b, freqs[i]);
        return r;
    };
    auto meanOf = [&](std::initializer_list<size_t> idx) {
        double s = 0;
        int c = 0;
        for (size_t i : idx)
            if (i < freqs.size() && valid[i])
            {
                s += target[i];
                ++c;
            }
        return c ? s / c : 0.0;
    };
    const double lo = meanOf({0, 1});
    if (std::fabs(lo) > threshold)
        bands.push_back({1, 160.0, std::clamp(lo, -maxGain, maxGain), 0.7});
    const size_t last = freqs.size() - 1;
    const double hi = meanOf({last - 1, last});
    if (std::fabs(hi) > threshold)
        bands.push_back({2, 5000.0, std::clamp(hi, -maxGain, maxGain), 0.7});
    for (int k = 0; k < maxBells; ++k)
    {
        size_t worst = 0;
        double worstAbs = 0.0;
        for (size_t i = 1; i + 1 < freqs.size(); ++i)
            if (valid[i] && std::fabs(residual(i)) > worstAbs)
            {
                worstAbs = std::fabs(residual(i));
                worst = i;
            }
        if (worstAbs < threshold)
            break;
        bands.push_back({0, freqs[worst], std::clamp(residual(worst), -maxGain, maxGain), 1.4});
    }
    // Refine gains at their own frequencies.
    for (int it = 0; it < 4; ++it)
        for (auto& b : bands)
        {
            size_t nearest = 0;
            for (size_t i = 0; i < freqs.size(); ++i)
                if (std::fabs(std::log(freqs[i] / b.freq)) < std::fabs(std::log(freqs[nearest] / b.freq)))
                    nearest = i;
            if (b.shape == 1)
                nearest = valid[0] ? 0 : 1;
            if (b.shape == 2)
                nearest = valid[last] ? last : last - 1;
            if (!valid[nearest])
                continue;
            b.gain = std::clamp(b.gain + 0.7 * residual(nearest), -maxGain, maxGain);
        }
    return bands;
}

} // namespace

bool importImpulseResponse(const AudioData& ir, const std::vector<double>& current, StateDocument& out,
                           IrReport* report, std::string* error)
{
    auto fail = [&](const char* msg) {
        if (error)
            *error = msg;
        return false;
    };
    const double fs = ir.sampleRate;
    const size_t n = ir.frames();
    if (fs < 8000 || n < static_cast<size_t>(0.1 * fs) || ir.channels.empty())
        return fail("Impulse response is too short or has no audio");

    // Onset and direct sound.
    double peak = 0.0;
    size_t peakIdx = 0;
    for (size_t i = 0; i < n; ++i)
        for (const auto& ch : ir.channels)
            if (std::fabs(ch[i]) > peak)
            {
                peak = std::fabs(ch[i]);
                peakIdx = i;
            }
    if (peak <= 0.0)
        return fail("The file is silent");
    size_t onset = peakIdx;
    for (size_t i = 0; i < peakIdx; ++i)
    {
        bool hit = false;
        for (const auto& ch : ir.channels)
            hit |= std::fabs(ch[i]) > 0.1 * peak;
        if (hit)
        {
            onset = i;
            break;
        }
    }

    // Reject sine sweeps / non-IRs: energy must peak early and then decay.
    {
        const size_t frame = static_cast<size_t>(0.02 * fs);
        std::vector<double> lv;
        for (size_t s = onset; s + frame <= n; s += frame)
        {
            double acc = 0.0;
            for (const auto& ch : ir.channels)
                for (size_t i = s; i < s + frame; ++i)
                    acc += static_cast<double>(ch[i]) * ch[i];
            lv.push_back(10.0 * std::log10(acc / frame + 1e-30));
        }
        if (lv.size() < 5)
            return fail("Impulse response is too short");
        const auto mx = std::max_element(lv.begin(), lv.end());
        const size_t mxPos = static_cast<size_t>(mx - lv.begin());
        size_t loud = 0;
        for (double v : lv)
            loud += v > *mx - 10.0;
        const double loudSec = static_cast<double>(loud) * 0.02;
        if (mxPos * 0.02 > 0.4 || (loudSec > 0.6 && loud > lv.size() * 4 / 10))
            return fail("This looks like a sine sweep or music, not an impulse response");
    }

    // Per-band decay times and levels (from the onset).
    IrReport rep;
    std::vector<double> freqs, t60s, levels;
    std::vector<bool> valid;
    for (double fc : kBands)
    {
        if (fc > 0.42 * fs)
            break;
        // Levels over the whole octave; the decay time over a third of one,
        // so that a steep step in the decay (a high shelf, say) is measured
        // where it is and not smeared from the neighbouring band.
        const std::vector<double> e = bandEnergy(ir, fc, onset, std::sqrt(2.0));
        const double t = decayTime(bandEnergy(ir, fc, onset, 4.32), fs);
        const double total = std::accumulate(e.begin(), e.end(), 0.0);
        freqs.push_back(fc);
        t60s.push_back(t);
        levels.push_back(total);
        valid.push_back(t > 0.02);
    }
    // Fill unmeasurable bands from neighbours.
    for (size_t i = 0; i < t60s.size(); ++i)
        if (!valid[i])
        {
            double s = 0;
            int c = 0;
            for (size_t j = 0; j < t60s.size(); ++j)
                if (valid[j] && (j + 1 == i || j == i + 1))
                {
                    s += t60s[j];
                    ++c;
                }
            if (c)
                t60s[i] = s / c;
        }
    double mid = 0.0;
    int midCount = 0;
    for (size_t i = 0; i < freqs.size(); ++i)
        if ((freqs[i] == 500 || freqs[i] == 1000) && t60s[i] > 0)
        {
            mid += t60s[i];
            ++midCount;
        }
    if (!midCount)
        return fail("Could not measure the decay of this impulse response");
    mid /= midCount;

    // Base settings.
    const ParamTable& t = ParamTable::get();
    out.values = defaultValues();
    auto set = [&](uint32_t id, double v) { out.values[static_cast<size_t>(t.indexOf(id))] = v; };
    for (uint32_t id : {pid::Mix, pid::InputLevel, pid::InputPan, pid::OutputLevel, pid::OutputPan, pid::Bypass})
    {
        const int idx = t.indexOf(id);
        if (idx >= 0 && static_cast<size_t>(idx) < current.size())
            out.values[static_cast<size_t>(idx)] = current[static_cast<size_t>(idx)];
    }
    const auto& rooms = dsp::roomAnchors();
    double rate = 1.0;
    double space = dsp::spaceForT60(std::clamp(mid, rooms.front().t60, rooms.back().t60));
    if (mid > rooms.back().t60)
        rate = std::min(4.0, mid / rooms.back().t60);
    if (mid < rooms.front().t60)
        rate = std::max(0.25, mid / rooms.front().t60);
    set(pid::Space, space);
    set(pid::DecayRate, (std::log2(rate) + 2.0) / 4.0);
    set(pid::Style, 0.0);
    set(pid::Character, 0.15);
    set(pid::Brightness, 0.5);
    set(pid::Thickness, 0.5);

    // Decay Rate EQ: match the measured T60 relative to the room's own curve.
    dsp::EngineParams ep = buildEngineParams(out.values.data(), 120.0);
    const dsp::DecayModel base = dsp::ReverbEngine::decayModelFor(ep);
    std::vector<double> target(freqs.size());
    for (size_t i = 0; i < freqs.size(); ++i)
        target[i] = t60s[i] > 0 ? std::log2(t60s[i] / base.t60At(freqs[i])) : 0.0;
    auto decayEval = [](const FitBand& b, double f) {
        dsp::DecayBand d;
        d.used = true;
        d.shape = static_cast<dsp::DecayShape>(b.shape);
        d.freq = b.freq;
        d.rateLog2 = b.gain;
        d.q = b.q;
        return dsp::DecayModel::bandLog2(d, f);
    };
    const auto decayFit = fitCurve(freqs, target, valid, 4, 0.1, 3.0, decayEval);
    for (size_t k = 0; k < decayFit.size() && k < 6; ++k)
    {
        using namespace pid;
        const int b = static_cast<int>(k);
        set(decay(b, DUsed), 1);
        set(decay(b, DEnabled), 1);
        set(decay(b, DShape), decayFit[k].shape);
        set(decay(b, DFreq), conv::freqToValue(decayFit[k].freq));
        set(decay(b, DRate), conv::rateLog2ToValue(decayFit[k].gain));
        set(decay(b, DQ), conv::qToValue(decayFit[k].q));
    }

    // Post EQ: initial spectral level = band energy / T60.
    std::vector<double> lvl(freqs.size(), 0.0);
    double ref = 0.0;
    int refCount = 0;
    for (size_t i = 0; i < freqs.size(); ++i)
    {
        lvl[i] = 10.0 * std::log10(std::max(levels[i], 1e-30) / std::max(t60s[i], 0.02));
        if (freqs[i] >= 250 && freqs[i] <= 4000)
        {
            ref += lvl[i];
            ++refCount;
        }
    }
    ref /= std::max(refCount, 1);
    for (auto& v : lvl)
        v -= ref;
    std::vector<bool> lvlValid(freqs.size(), true);
    auto postEval = [](const FitBand& b, double f) {
        dsp::PostBand p;
        p.used = true;
        p.shape = b.shape == 1 ? dsp::PostShape::LowShelf : (b.shape == 2 ? dsp::PostShape::HighShelf : dsp::PostShape::Bell);
        p.freq = b.freq;
        p.gainDb = b.gain;
        p.q = b.q;
        return 10.0 * std::log10(dsp::PostEq::bandPow(p, f, 1e7));
    };
    const auto postFit = fitCurve(freqs, lvl, lvlValid, 3, 1.5, 12.0, postEval);
    int pb = 0;
    for (const auto& f : postFit)
    {
        using namespace pid;
        const int shape = f.shape == 1 ? 1 : (f.shape == 2 ? 2 : 0);
        set(post(pb, PUsed), 1);
        set(post(pb, PEnabled), 1);
        set(post(pb, PShape), shape);
        set(post(pb, PFreq), conv::freqToValue(f.freq));
        set(post(pb, PGain), conv::gainDbToValue(f.gain));
        set(post(pb, PQ), conv::qToValue(f.q));
        if (++pb >= 6)
            break;
    }

    // Clarity C50 -> Distance; inter-channel correlation -> Width.
    double early = 0.0, late = 0.0, lr = 0.0, ll = 0.0, rr = 0.0;
    const size_t c50 = onset + static_cast<size_t>(0.05 * fs);
    for (size_t i = onset; i < n; ++i)
    {
        double e = 0.0;
        for (const auto& ch : ir.channels)
            e += static_cast<double>(ch[i]) * ch[i];
        (i < c50 ? early : late) += e;
        if (ir.channels.size() >= 2 && i >= c50)
        {
            const double l = ir.channels[0][i], r = ir.channels[1][i];
            lr += l * r;
            ll += l * l;
            rr += r * r;
        }
    }
    rep.c50 = 10.0 * std::log10((early + 1e-30) / (late + 1e-30));
    set(pid::Distance, std::clamp(0.55 - rep.c50 / 16.0, 0.0, 1.0));
    if (ir.channels.size() >= 2 && ll > 0 && rr > 0)
    {
        rep.correlation = lr / std::sqrt(ll * rr);
        const double w = rep.correlation < 0.2 ? 1.0 : std::clamp(0.5 * (1.0 - rep.correlation) / 0.8, 0.0, 0.5);
        set(pid::Width, w / 1.5);
    }

    // Initial time gap -> Predelay.
    size_t gap = 0;
    const size_t from = peakIdx + static_cast<size_t>(0.001 * fs), to = std::min(n, peakIdx + static_cast<size_t>(0.1 * fs));
    for (size_t i = from; i < to && !gap; ++i)
        for (const auto& ch : ir.channels)
            if (std::fabs(ch[i]) > 0.25 * peak)
            {
                gap = i - peakIdx;
                break;
            }
    rep.predelayMs = std::min(80.0, gap * 1000.0 / fs);
    set(pid::Predelay, conv::predelayToValue(rep.predelayMs));

    rep.bandFreq = freqs;
    rep.bandT60 = t60s;
    rep.bandLevel = lvl;
    rep.t60Mid = mid;
    if (report)
        *report = rep;
    char desc[160];
    std::snprintf(desc, sizeof(desc), "Matched to an impulse response (T60 %.2f s, C50 %.1f dB).", mid, rep.c50);
    out.meta["description"] = desc;
    out.meta["tags"] = "impulse response";
    return true;
}

} // namespace aurum
