// IR import round trip (render -> WAV -> import -> compare T60 curves) and
// bulk conversion of .ffp presets (with a stability render).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "WavFile.h"
#include "dsp/ReverbEngine.h"
#include "plugin/Params.h"
#include "state/AudioFile.h"
#include "state/IrImport.h"
#include "state/FfpImport.h"
#include "state/StateIO.h"

using namespace aurum;

static std::vector<float> renderIr(const dsp::EngineParams& p, double secs, std::vector<float>& right)
{
    dsp::ReverbEngine e;
    e.setOffline(true);
    e.prepare(48000, 512);
    const int n = static_cast<int>(secs * 48000);
    std::vector<float> l(static_cast<size_t>(n)), in(512), inR(512);
    right.assign(static_cast<size_t>(n), 0.0f);
    for (int off = 0; off < n; off += 512)
    {
        std::fill(in.begin(), in.end(), 0.0f);
        std::fill(inR.begin(), inR.end(), 0.0f);
        if (off == 0)
            in[0] = inR[0] = 1.0f;
        const int len = std::min(512, n - off);
        e.process(in.data(), inR.data(), &l[static_cast<size_t>(off)], &right[static_cast<size_t>(off)], len, p);
    }
    return l;
}

int main(int argc, char** argv)
{
    const std::string tmp = argc > 1 ? argv[1] : "/tmp";
    int failures = 0;

    // 1) IR round trip.
    struct Case { const char* name; double space; double rate; bool eq; };
    const Case cases[] = {{"room", 0.25, 1.0, false}, {"hall", 0.6, 1.0, true}, {"cathedral", 0.95, 1.2, false}};
    for (const Case& c : cases)
    {
        std::vector<double> v = defaultValues();
        const ParamTable& t = ParamTable::get();
        v[static_cast<size_t>(t.indexOf(pid::Space))] = c.space;
        v[static_cast<size_t>(t.indexOf(pid::DecayRate))] = (std::log2(c.rate) + 2) / 4;
        v[static_cast<size_t>(t.indexOf(pid::Mix))] = 1.0;
        if (c.eq)
        {
            using namespace pid;
            v[static_cast<size_t>(t.indexOf(decay(0, DUsed)))] = 1;
            v[static_cast<size_t>(t.indexOf(decay(0, DShape)))] = 2;
            v[static_cast<size_t>(t.indexOf(decay(0, DFreq)))] = conv::freqToValue(3000);
            v[static_cast<size_t>(t.indexOf(decay(0, DRate)))] = conv::rateLog2ToValue(-1.0);
            v[static_cast<size_t>(t.indexOf(decay(0, DQ)))] = conv::qToValue(0.7);
        }
        dsp::EngineParams p = buildEngineParams(v.data(), 120);
        const dsp::DecayModel orig = dsp::ReverbEngine::decayModelFor(p);
        std::vector<float> r;
        std::vector<float> l = renderIr(p, orig.nominalT60() * 2.0 + 0.5, r);
        const std::string wav = tmp + "/ir_" + c.name + ".wav";
        writeWav(wav, l, r, 48000);

        AudioData a;
        std::string err;
        StateDocument doc;
        IrReport rep;
        if (!readAudioFile(wav, a, &err) || !importImpulseResponse(a, v, doc, &rep, &err))
        {
            printf("IR %s: FAILED %s\n", c.name, err.c_str());
            ++failures;
            continue;
        }
        const dsp::DecayModel imp = dsp::ReverbEngine::decayModelFor(buildEngineParams(doc.values.data(), 120));
        printf("IR %-10s T60mid %.2f  band: target / measured / imported\n", c.name, rep.t60Mid);
        double worst = 0;
        for (size_t i = 0; i < rep.bandFreq.size(); ++i)
        {
            const double f = rep.bandFreq[i];
            const double err2 = imp.t60At(f) / orig.t60At(f) - 1.0;
            if (f >= 125 && f <= 8000)
                worst = std::max(worst, std::fabs(err2));
            printf("   %5.0f  %6.2f  %6.2f  %6.2f  (%+.0f%%)\n", f, orig.t60At(f), rep.bandT60[i], imp.t60At(f), 100 * err2);
        }
        printf("   worst imported-vs-original 125..8k: %.0f%%  width %.2f dist %.2f\n", 100 * worst,
               doc.values[static_cast<size_t>(t.indexOf(pid::Width))] * 1.5,
               doc.values[static_cast<size_t>(t.indexOf(pid::Distance))]);
        if (worst > 0.25)
            ++failures;
    }

    // 2) Sweep rejection.
    {
        AudioData a;
        a.sampleRate = 48000;
        a.channels.assign(1, std::vector<float>(48000 * 3));
        double ph = 0;
        for (size_t i = 0; i < a.channels[0].size(); ++i)
        {
            const double f = 20 * std::pow(1000.0, i / double(a.channels[0].size()));
            ph += 2 * M_PI * f / 48000;
            a.channels[0][i] = static_cast<float>(0.5 * std::sin(ph));
        }
        StateDocument d;
        std::string err;
        const bool ok = importImpulseResponse(a, defaultValues(), d, nullptr, &err);
        printf("sweep rejected: %s (%s)\n", ok ? "NO" : "yes", err.c_str());
        failures += ok;
    }

    // 3) .ffp presets.
    if (argc > 2)
    {
        int count = 0, bad = 0;
        for (auto& f : std::filesystem::recursive_directory_iterator(argv[2]))
        {
            if (f.path().extension() != ".ffp")
                continue;
            std::ifstream in(f.path());
            std::stringstream ss;
            ss << in.rdbuf();
            StateDocument d;
            if (!importFfpPreset(ss.str(), d))
            {
                ++bad;
                continue;
            }
            ++count;
            dsp::EngineParams p = buildEngineParams(d.values.data(), 120);
            std::vector<float> r;
            std::vector<float> l = renderIr(p, 2.0, r);
            float peak = 0;
            bool finite = true;
            for (size_t i = 0; i < l.size(); ++i)
            {
                finite &= std::isfinite(l[i]) && std::isfinite(r[i]);
                peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
            }
            if (!finite || peak > 4)
            {
                printf("preset %s: unstable (peak %.2f)\n", f.path().filename().c_str(), peak);
                ++bad;
            }
        }
        printf(".ffp presets converted: %d, problems: %d\n", count, bad);
        failures += bad;
    }
    printf(failures ? "FAILURES: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
