// Long-hold stability: each preset (and Space offsets) is played for 40 s
// with a steady program; the late output level must not grow.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "dsp/ReverbEngine.h"
#include "plugin/Params.h"
#include "state/FfpImport.h"
#include "state/StateIO.h"

using namespace aurum;

struct Job
{
    std::string name;
    std::vector<double> values;
    double spaceOffset;
};

static bool runJob(const Job& j, std::string& msg)
{
    dsp::EngineParams p = buildEngineParams(j.values.data(), 120);
    p.space = std::clamp(p.space + j.spaceOffset, 0.0, 1.0);
    p.mix = 1.0;
    p.freeze = false;
    dsp::ReverbEngine e;
    e.setOffline(true);
    e.prepare(48000, 512);
    std::vector<float> l(512), r(512);
    dsp::Rng rng(11);
    double ph = 0;
    std::vector<double> rmsPerSec;
    for (int sec = 0; sec < 40; ++sec)
    {
        double acc = 0;
        for (int b = 0; b < 48000 / 512; ++b)
        {
            const bool on = (b / 12) % 3 == 0;
            for (int i = 0; i < 512; ++i)
            {
                ph += 2 * M_PI * 110 / 48000;
                const float x = on ? 0.25f * (0.4f * std::sin(ph) + 0.3f * std::sin(ph * 3.01) + 0.3f * rng.bipolar()) : 0.0f;
                l[i] = x;
                r[i] = 0.8f * x;
            }
            e.process(l.data(), r.data(), l.data(), r.data(), 512, p);
            for (int i = 0; i < 512; ++i)
            {
                if (!std::isfinite(l[i]) || !std::isfinite(r[i]))
                {
                    msg = "non-finite";
                    return false;
                }
                acc += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
            }
        }
        rmsPerSec.push_back(std::sqrt(acc / 96000));
    }
    double early = 0, late = 0;
    for (int s = 5; s < 15; ++s)
        early += rmsPerSec[static_cast<size_t>(s)];
    for (int s = 30; s < 40; ++s)
        late += rmsPerSec[static_cast<size_t>(s)];
    const double growthDb = 20 * std::log10((late + 1e-12) / (early + 1e-12));
    char buf[96];
    std::snprintf(buf, sizeof(buf), "growth %+.1f dB, late rms %.3f", growthDb, late / 10);
    msg = buf;
    return growthDb < 3.0 && late / 10 < 1.0;
}

int main(int argc, char** argv)
{
    std::vector<Job> jobs;
    if (argc > 1)
        for (auto& f : std::filesystem::recursive_directory_iterator(argv[1]))
        {
            if (f.path().extension() != ".ffp")
                continue;
            std::ifstream in(f.path());
            std::stringstream ss;
            ss << in.rdbuf();
            StateDocument d;
            if (!importFfpPreset(ss.str(), d))
                continue;
            for (double off : {0.0, 0.08, -0.08})
                jobs.push_back({f.path().stem().string(), d.values, off});
        }
    std::atomic<size_t> next{0};
    std::atomic<int> failures{0};
    std::mutex m;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < jobs.size();)
            {
                std::string msg;
                if (!runJob(jobs[i], msg))
                {
                    std::lock_guard<std::mutex> lock(m);
                    std::printf("FAIL %s (space %+.2f): %s\n", jobs[i].name.c_str(), jobs[i].spaceOffset, msg.c_str());
                    ++failures;
                }
            }
        });
    for (auto& t : pool)
        t.join();
    std::printf("long-run: %zu cases, %d failures\n", jobs.size(), failures.load());
    return failures ? 1 : 0;
}
