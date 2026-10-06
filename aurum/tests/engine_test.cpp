// Engine measurements: T60 per octave band vs. the decay model, stability
// under random parameter changes, and CPU cost.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "WavFile.h"
#include "dsp/ReverbEngine.h"

using namespace aurum::dsp;

namespace {

constexpr double kFs = 48000.0;

struct Ir
{
    std::vector<float> l, r;
};

Ir renderImpulse(const EngineParams& p, double seconds)
{
    ReverbEngine e;
    e.setOffline(true);
    e.prepare(kFs, 512);
    const int n = static_cast<int>(seconds * kFs);
    Ir ir;
    ir.l.assign(static_cast<size_t>(n), 0.0f);
    ir.r.assign(static_cast<size_t>(n), 0.0f);
    std::vector<float> inL(512), inR(512);
    // Let parameter smoothing settle before the impulse.
    for (int i = 0; i < 20; ++i)
    {
        std::fill(inL.begin(), inL.end(), 0.0f);
        std::fill(inR.begin(), inR.end(), 0.0f);
        e.process(inL.data(), inR.data(), inL.data(), inR.data(), 512, p);
    }
    for (int off = 0; off < n; off += 512)
    {
        const int len = std::min(512, n - off);
        std::fill(inL.begin(), inL.end(), 0.0f);
        std::fill(inR.begin(), inR.end(), 0.0f);
        if (off == 0)
            inL[0] = inR[0] = 1.0f;
        e.process(inL.data(), inR.data(), &ir.l[static_cast<size_t>(off)], &ir.r[static_cast<size_t>(off)], len, p);
    }
    return ir;
}

// T60 from Schroeder backward integration (T20 fit, -5..-25 dB) of an
// octave-filtered response.
double measureT60(const std::vector<float>& x, double fc)
{
    SvfCoeffs bp = SvfCoeffs::bandPass(fc, 1.4, kFs);
    SvfState s1, s2, s3;
    std::vector<double> y(x.size());
    for (size_t i = 0; i < x.size(); ++i)
        y[i] = s3.process(s2.process(s1.process(x[i], bp), bp), bp);
    std::vector<double> edc(y.size());
    double acc = 0.0;
    for (size_t i = y.size(); i-- > 0;)
    {
        acc += y[i] * y[i];
        edc[i] = acc;
    }
    const double e0 = edc[0];
    if (e0 <= 0.0)
        return 0.0;
    // Least squares fit of dB vs time between -5 and -25 dB.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0;
    for (size_t i = 0; i < edc.size(); ++i)
    {
        const double db = 10.0 * std::log10(edc[i] / e0 + 1e-30);
        if (db > -5.0)
            continue;
        if (db < -25.0)
            break;
        const double t = i / kFs;
        sx += t;
        sy += db;
        sxx += t * t;
        sxy += t * db;
        ++cnt;
    }
    if (cnt < 10)
        return 0.0;
    const double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
    return -60.0 / slope;
}

const char* styleName(Style s) { return s == Style::Natural ? "Natural" : s == Style::Classic ? "Classic" : "Plate"; }

int t60Test(bool writeFiles)
{
    struct Case
    {
        const char* name;
        Style style;
        double space;
        double rate;
        bool eq;
    };
    const Case cases[] = {
        {"room", Style::Natural, 0.25, 1.0, false},  {"hall", Style::Natural, 0.65, 1.0, false},
        {"cathedral", Style::Natural, 1.0, 1.0, false}, {"hall+eq", Style::Natural, 0.65, 1.0, true},
        {"hall-classic", Style::Classic, 0.65, 1.0, false}, {"hall-plate", Style::Plate, 0.65, 1.0, false},
        {"plate+eq", Style::Plate, 0.5, 1.5, true},  {"classic-long", Style::Classic, 0.9, 1.5, false},
    };
    const double bands[] = {125, 250, 500, 1000, 2000, 4000, 8000};
    double worst = 0.0;
    for (const Case& c : cases)
    {
        EngineParams p;
        p.mix = 1.0;
        p.space = c.space;
        p.decayRate = c.rate;
        p.style = c.style;
        p.character = 0.0;
        p.distance = 0.3;
        if (c.eq)
        {
            p.decayBands[0] = {true, true, DecayShape::Bell, 500.0, 1.0, 1.0};
            p.decayBands[1] = {true, true, DecayShape::HighShelf, 4000.0, -1.0, 0.7};
        }
        const DecayModel m = ReverbEngine::decayModelFor(p);
        const double secs = std::min(30.0, m.nominalT60() * 2.5 + 1.0);
        Ir ir = renderImpulse(p, secs);
        std::vector<float> mono(ir.l.size());
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] = 0.5f * (ir.l[i] + ir.r[i]);
        printf("%-14s %-7s T60 nominal %.2fs\n   band  target  measured  err\n", c.name, styleName(c.style),
               m.nominalT60());
        for (double fc : bands)
        {
            const double target = m.t60At(fc);
            const double meas = measureT60(mono, fc);
            const double err = meas / target - 1.0;
            worst = std::max(worst, std::fabs(err));
            printf("  %5.0f  %6.2f  %8.2f  %+5.1f%%\n", fc, target, meas, 100.0 * err);
        }
        double peak = 0.0, energy = 0.0;
        for (size_t i = 0; i < mono.size(); ++i)
        {
            peak = std::max(peak, static_cast<double>(std::fabs(mono[i])));
            energy += static_cast<double>(ir.l[i]) * ir.l[i] + static_cast<double>(ir.r[i]) * ir.r[i];
        }
        printf("   peak %.3f  energy %.3f\n", peak, energy);
        if (writeFiles)
            writeWav(std::string("ir_") + c.name + ".wav", ir.l, ir.r, static_cast<int>(kFs));
    }
    printf("worst T60 error %.1f%%\n", 100.0 * worst);
    return 0;
}

int stabilityTest(bool offline, int startBlock)
{
    ReverbEngine e;
    e.setOffline(offline);
    e.prepare(kFs, 512);
    Rng rng(42);
    EngineParams p;
    std::vector<float> l(256), r(256);
    double maxAbs = 0.0, setPeak = 0.0;
    bool bad = false;
    const int blocks = static_cast<int>(120.0 * kFs / 256);
    for (int b = 0; b < blocks; ++b)
    {
        if (b % 40 == 0)
        {
            if (b > 0 && getenv("VERBOSE"))
                printf("set@%d: style %d space %.2f freeze %d thick %.2f char %.2f peak %.2f\n", b - 40, (int)p.style,
                       p.space, p.freeze, p.thickness, p.character, setPeak);
            setPeak = 0.0;
            p.space = rng.uniform();
            p.decayRate = std::pow(2.0, rng.bipolar() * 2.0);
            p.style = static_cast<Style>(rng.next() % 3);
            p.character = rng.uniform();
            p.brightness = rng.uniform();
            p.distance = rng.uniform();
            p.thickness = rng.uniform();
            p.width = rng.uniform() * 1.5;
            p.predelayMs = rng.uniform() * 500.0;
            p.ducking = rng.uniform() < 0.3 ? rng.uniform() : 0.0;
            p.gateOn = rng.uniform() < 0.2;
            p.freeze = rng.uniform() < 0.05;
            p.mix = 1.0;
            for (auto& d : p.decayBands)
            {
                d.used = rng.uniform() < 0.4;
                d.shape = static_cast<DecayShape>(rng.next() % 4);
                d.freq = 30.0 * std::pow(600.0, rng.uniform());
                d.rateLog2 = rng.bipolar() * 3.0;
                d.q = 0.2 * std::pow(50.0, rng.uniform());
            }
            for (auto& q : p.postBands)
            {
                q.used = rng.uniform() < 0.4;
                q.shape = static_cast<PostShape>(rng.next() % 5);
                q.freq = 20.0 * std::pow(1000.0, rng.uniform());
                q.gainDb = rng.bipolar() * 18.0;
                q.q = 0.2 * std::pow(50.0, rng.uniform());
                q.slope = static_cast<int>(rng.next() % 9);
                q.placement = static_cast<Placement>(rng.next() % 5);
            }
        }
        for (int i = 0; i < 256; ++i)
        {
            const bool burst = (b % 200) < 20;
            l[i] = burst ? 0.5f * rng.bipolar() : 0.0f;
            r[i] = burst ? 0.5f * rng.bipolar() : 0.0f;
        }
        if (b < startBlock)
            continue;
        if (b == startBlock && startBlock > 0)
        {
            e.reset();
            printf("start set: space %.4f rate %.4f style %d char %.4f bright %.4f dist %.4f thick %.4f width %.4f pre %.1f\n",
                   p.space, p.decayRate, (int)p.style, p.character, p.brightness, p.distance, p.thickness, p.width, p.predelayMs);
            for (auto& d : p.decayBands)
                if (d.used) printf("  decay used %d en %d shape %d f %.2f r %.3f q %.3f\n", d.used, d.enabled, (int)d.shape, d.freq, d.rateLog2, d.q);
        }
        e.process(l.data(), r.data(), l.data(), r.data(), 256, p);
        for (int i = 0; i < 256; ++i)
        {
            if (!std::isfinite(l[i]) || !std::isfinite(r[i]))
                bad = true;
            maxAbs = std::max(maxAbs, static_cast<double>(std::max(std::fabs(l[i]), std::fabs(r[i]))));
            setPeak = std::max(setPeak, static_cast<double>(std::max(std::fabs(l[i]), std::fabs(r[i]))));
        }
        static bool reported = false;
        if (maxAbs > 50.0 && !reported)
        {
            reported = true;
            bad = true;
        }
        if (bad)
        {
            printf("non-finite or runaway output (%.1f) at block %d\n", maxAbs, b);
            printf(" space %.3f rate %.3f style %d char %.2f bright %.2f dist %.2f thick %.2f width %.2f pre %.1f duck %.2f gate %d freeze %d\n",
                   p.space, p.decayRate, (int)p.style, p.character, p.brightness, p.distance, p.thickness, p.width,
                   p.predelayMs, p.ducking, p.gateOn, p.freeze);
            for (auto& d : p.decayBands)
                if (d.used) printf("  decay shape %d f %.1f r %.2f q %.2f\n", (int)d.shape, d.freq, d.rateLog2, d.q);
            for (auto& q : p.postBands)
                if (q.used) printf("  post shape %d f %.1f g %.1f q %.2f slope %d place %d\n", (int)q.shape, q.freq, q.gainDb, q.q, q.slope, (int)q.placement);
            return 1;
        }
    }
    printf("stability: 120 s random sweep, max |out| = %.2f %s\n", maxAbs, maxAbs < 20.0 ? "OK" : "TOO LOUD");
    return maxAbs < 20.0 ? 0 : 1;
}

int cpuTest()
{
    for (int st = 0; st < 3; ++st)
    {
        ReverbEngine e;
        e.prepare(kFs, 512);
        EngineParams p;
        p.style = static_cast<Style>(st);
        p.space = 0.7;
        p.character = 0.4;
        std::vector<float> l(256), r(256);
        Rng rng(1);
        const int blocks = static_cast<int>(20.0 * kFs / 256);
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 256; ++i)
            {
                l[i] = 0.1f * rng.bipolar();
                r[i] = 0.1f * rng.bipolar();
            }
            e.process(l.data(), r.data(), l.data(), r.data(), 256, p);
        }
        auto t1 = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(t1 - t0).count();
        printf("cpu %-7s: %.2f%% of one core (48 kHz stereo)\n", styleName(p.style), 100.0 * sec / 20.0);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (mode == "t60" || mode == "all" || mode == "wav")
        rc |= t60Test(mode == "wav");
    if (mode == "stability" || mode == "all")
        rc |= stabilityTest(false, 0);
    if (mode == "stability-offline")
        rc |= stabilityTest(true, argc > 2 ? atoi(argv[2]) : 0);
    if (mode == "cpu" || mode == "all")
        rc |= cpuTest();
    return rc;
}
