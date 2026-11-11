// resident-enginetest: the two SID engines, chip by chip (SidChip).
//
//   resident-enginetest
//
// 1. Sample count: ten minutes of CPU cycles make the samples the host rate
//    asks for, within two (Light: one of its own internal rate), PAL and
//    NTSC, from 44.1 to 192 kHz. A count that runs ahead fills SidBank's
//    FIFO until the engine drops samples, one that falls behind empties it:
//    SIDLite's own clock ratio ran 0.13 % ahead, reSIDfp's resamplers
//    0.02-0.05 % (both patched, sidlite 0001 and residfp 0004).
// 2. Pitch: a triangle at A4 measures within 0.5 cent of the frequency its
//    register asks for.
// 3. State: a Light chip saved and restored into a new one plays on bit for
//    bit, at 48 and at 96 kHz (where it is upsampled); a state of the other
//    engine does not restore.
// 4. Voice mask: three Light chips hearing one voice each add up to the chip
//    hearing all three; a masked voice 3 still drives OSC3.
// 5. Two runs of the same stream are identical.
//
// Exit code 0 when everything passed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "emu/SidChip.h"

using namespace resident::emu;

namespace {

int failures = 0;
void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

constexpr double kPal = 985248.0, kNtsc = 1022727.0;

const char* name(SidEngine e) { return e == SidEngine::Lite ? "Light" : "reSIDfp"; }

std::unique_ptr<SidChip> makeChip(SidEngine e, SidVariant v, double clock, double rate)
{
    auto c = makeSidChip(e);
    SidChipConfig cc;
    cc.variant = v;
    cc.tuning = sidVariantTuning(v);
    c->tune(cc, cc, false);
    c->setSampling(clock, SidResampling::Sinc, rate);
    c->reset();
    return c;
}

// Runs `cycles` in SidBank's chunk size; appends the samples to `out` if given.
size_t run(SidChip& c, uint64_t cycles, std::vector<int16_t>* out = nullptr)
{
    std::vector<int16_t> buf(1 << 16);
    size_t total = 0;
    while (cycles) {
        const unsigned n = static_cast<unsigned>(std::min<uint64_t>(cycles, 20000));
        cycles -= n;
        const int got = c.clock(n, buf.data());
        total += static_cast<size_t>(got);
        if (out)
            out->insert(out->end(), buf.begin(), buf.begin() + got);
    }
    return total;
}

// Voice 1..3 on (mask), sustained triangle at `reg`, no filter, volume 15.
void playTriangle(SidChip& c, uint16_t reg, int voices = 1)
{
    for (int v = 0; v < voices; ++v) {
        const uint8_t b = static_cast<uint8_t>(7 * v);
        const uint16_t f = static_cast<uint16_t>(reg + 371 * v);
        c.write(b + 0, static_cast<uint8_t>(f));
        c.write(b + 1, static_cast<uint8_t>(f >> 8));
        c.write(b + 5, 0x00); // attack 0, decay 0
        c.write(b + 6, 0xf0); // sustain 15, release 0
        c.write(b + 4, 0x11); // triangle, gate
    }
    c.write(0x18, 0x0f);
}

// Frequency from the rising zero crossings (linear interpolation), DC removed.
double measureHz(const std::vector<int16_t>& s, double rate)
{
    double mean = 0;
    for (int16_t x : s)
        mean += x;
    mean /= static_cast<double>(s.size());
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        const double a = s[i - 1] - mean, b = s[i] - mean;
        if (a < 0 && b >= 0) {
            const double t = static_cast<double>(i - 1) + a / (a - b);
            if (first < 0)
                first = t;
            else
                ++count;
            last = t;
        }
    }
    return count > 0 ? count * rate / (last - first) : 0.0;
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

double maxAbsDiff(const std::vector<int16_t>& a, const std::vector<int16_t>& b)
{
    double d = a.size() == b.size() ? 0.0 : 1e9;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        d = std::max(d, std::fabs(static_cast<double>(a[i]) - b[i]));
    return d;
}

// RMS level in dB full scale, DC removed (as resident-selftest measures).
double rmsDb(const std::vector<int16_t>& s)
{
    double mean = 0;
    for (int16_t x : s)
        mean += x;
    mean /= static_cast<double>(std::max<size_t>(1, s.size()));
    double e = 0;
    for (int16_t x : s)
        e += (x - mean) * (x - mean);
    return 10.0 * std::log10(e / std::max<size_t>(1, s.size()) / (32768.0 * 32768.0) + 1e-20);
}

} // namespace

int main()
{
    // 1. Sample count over ten minutes.
    for (SidEngine e : {SidEngine::ReSidFp, SidEngine::Lite})
        for (double clock : {kPal, kNtsc})
            for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
                auto c = makeChip(e, SidVariant::MOS8580R5, clock, rate);
                const uint64_t cycles = static_cast<uint64_t>(600.0 * clock);
                const double want = static_cast<double>(cycles) * rate / clock;
                const double got = static_cast<double>(run(*c, cycles));
                char what[160];
                std::snprintf(what, sizeof(what), "%s %s %6.0f Hz: 10 min make %.0f samples, %+.0f off", name(e),
                              clock == kPal ? "PAL " : "NTSC", rate, got, got - want);
                const double up = e == SidEngine::Lite ? std::ceil(rate / 48000.0 - 1e-9) : 1.0; // Light's upsampling
                check(std::fabs(got - want) <= 1.0 + up, what);
            }

    // 2. Pitch: A4, register 7493 on PAL (440.03 Hz).
    {
        const uint16_t reg = 7493;
        const double want = reg * kPal / 16777216.0;
        for (SidEngine e : {SidEngine::ReSidFp, SidEngine::Lite})
            for (double rate : {44100.0, 48000.0, 96000.0}) {
                auto c = makeChip(e, SidVariant::MOS8580R5, kPal, rate);
                playTriangle(*c, reg);
                run(*c, static_cast<uint64_t>(0.5 * kPal)); // attack and DC settle
                std::vector<int16_t> s;
                run(*c, static_cast<uint64_t>(10.0 * kPal), &s);
                const double hz = measureHz(s, rate);
                char what[160];
                std::snprintf(what, sizeof(what), "%s %6.0f Hz: A4 at %.4f Hz (wanted %.4f), %+.3f cent", name(e), rate,
                              hz, want, cents(hz, want));
                check(std::fabs(cents(hz, want)) < 0.5, what);
            }
    }

    // 3. State round trip on Light; a reSIDfp state does not restore.
    for (double rate : {48000.0, 96000.0}) {
        auto a = makeChip(SidEngine::Lite, SidVariant::MOS6581R4AR, kPal, rate);
        playTriangle(*a, 5000, 3);
        a->write(0x17, 0xf3); // voices 1, 2 through the filter, resonance 15
        a->write(0x16, 0x40);
        a->write(0x18, 0x1f); // lowpass
        run(*a, static_cast<uint64_t>(1.234 * kPal));
        std::vector<char> st(static_cast<size_t>(a->stateSize()));
        a->saveState(st.data());
        auto b = makeChip(SidEngine::Lite, SidVariant::MOS6581R4AR, kPal, rate);
        SidChipConfig cc;
        const bool restored = b->restoreState(st.data(), static_cast<int>(st.size()), cc);
        std::vector<int16_t> sa, sb;
        for (auto* v : {&sa, &sb}) {
            SidChip& c = v == &sa ? *a : *b;
            c.write(0x00, 0x34); // the stream goes on: a write after the restore
            run(c, static_cast<uint64_t>(1.0 * kPal), v);
            c.write(0x04, 0x10); // gate off
            run(c, static_cast<uint64_t>(1.0 * kPal), v);
        }
        char what[160];
        std::snprintf(what, sizeof(what), "Light %6.0f Hz: a restored chip plays on exactly (max difference %.0f, %.1f dB)",
                      rate, maxAbsDiff(sa, sb), rmsDb(sa));
        check(restored && maxAbsDiff(sa, sb) == 0.0 && rmsDb(sa) > -40.0, what);
    }
    {
        auto r = makeChip(SidEngine::ReSidFp, SidVariant::MOS6581R4AR, kPal, 48000.0);
        auto l = makeChip(SidEngine::Lite, SidVariant::MOS6581R4AR, kPal, 48000.0);
        std::vector<char> st(static_cast<size_t>(r->stateSize()));
        r->saveState(st.data());
        std::vector<char> lst(static_cast<size_t>(l->stateSize()));
        l->saveState(lst.data());
        SidChipConfig cc;
        check(!l->restoreState(st.data(), static_cast<int>(st.size()), cc) &&
                  !r->restoreState(lst.data(), static_cast<int>(lst.size()), cc),
              "a state of the other engine does not restore");
    }

    // 4. Voice mask on Light: the three taps add up to the whole chip.
    {
        auto whole = makeChip(SidEngine::Lite, SidVariant::MOS8580R5, kPal, 48000.0);
        std::vector<std::unique_ptr<SidChip>> taps;
        for (int v = 0; v < 3; ++v) {
            taps.push_back(makeChip(SidEngine::Lite, SidVariant::MOS8580R5, kPal, 48000.0));
            taps.back()->setVoiceMask(static_cast<uint8_t>(1u << v));
        }
        for (SidChip* c : {whole.get(), taps[0].get(), taps[1].get(), taps[2].get()}) {
            playTriangle(*c, 4000, 3);
            run(*c, static_cast<uint64_t>(0.5 * kPal)); // the volume's own highpass settles
        }
        std::vector<int16_t> w, t[3];
        run(*whole, static_cast<uint64_t>(2.0 * kPal), &w);
        for (int v = 0; v < 3; ++v)
            run(*taps[static_cast<size_t>(v)], static_cast<uint64_t>(2.0 * kPal), &t[v]);
        std::vector<int16_t> resid(w.size());
        for (size_t i = 0; i < w.size(); ++i)
            resid[i] = static_cast<int16_t>(t[0][i] + t[1][i] + t[2][i] - w[i]);
        char what[160];
        std::snprintf(what, sizeof(what), "Light voice taps: %.1f / %.1f / %.1f dB; their sum minus the chip %.1f dB (chip %.1f dB)",
                      rmsDb(t[0]), rmsDb(t[1]), rmsDb(t[2]), rmsDb(resid), rmsDb(w));
        check(rmsDb(t[0]) > -40.0 && rmsDb(resid) < rmsDb(w) - 60.0, what);
        // Voice 3 masked still drives OSC3 (programs read it as a random source).
        auto m = makeChip(SidEngine::Lite, SidVariant::MOS8580R5, kPal, 48000.0);
        m->setVoiceMask(0x03);
        playTriangle(*m, 4000, 3);
        int distinct = 0, prev = -1;
        for (int k = 0; k < 50; ++k) {
            run(*m, 997);
            const int o = m->read(0x1b);
            distinct += o != prev;
            prev = o;
        }
        check(distinct > 20, "a masked voice 3 still drives OSC3 (" + std::to_string(distinct) + " changes in 50 reads)");
    }

    // 5. Determinism.
    for (SidEngine e : {SidEngine::ReSidFp, SidEngine::Lite}) {
        std::vector<int16_t> s[2];
        for (auto& v : s) {
            auto c = makeChip(e, SidVariant::MOS6581R4AR, kPal, 48000.0);
            playTriangle(*c, 3000, 3);
            c->write(0x17, 0xf7);
            c->write(0x18, 0x3f);
            run(*c, static_cast<uint64_t>(2.0 * kPal), &v);
        }
        check(maxAbsDiff(s[0], s[1]) == 0.0, std::string(name(e)) + ": two runs of the same stream are identical");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
