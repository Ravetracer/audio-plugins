#include "ReverbEngine.h"

#include <algorithm>
#ifdef AURUM_DEBUG_STAGES
#include <cstdio>
#endif

namespace aurum::dsp {

namespace {

constexpr double kMaxRoomSizeMs = 95.0;
constexpr double kMaxSpread = 3.7;
constexpr double kMaxPredelayMs = 500.0;
constexpr float kLateDiffMs[4] = {1.31f, 2.07f, 3.43f, 5.29f};
constexpr double kChorusBaseMs[2] = {9.0, 13.5};
constexpr double kToneLpQ = 0.4;
// Plate calibration, see decayModelFor and EngineControl::compute.
constexpr double kPlateRateExp = 0.905;
constexpr double kPlateFloor = 0.38;
constexpr double kPlateDistanceDb = 3.3;

// Loudness calibration of the late tail per style (keeps styles comparable).
// Tone and level of each style at default settings, fitted (within 0.5 dB rms
// per octave) to reference renders of a 2.5 s and a 10 s room. Replaces the
// former per-style wet factors (1.0, 0.9, 0.85), which are folded in here.
struct Voicing
{
    double hiHz, hiDb, loHz, loDb, levelDb;
};
constexpr Voicing kVoicing[3] = {
    {5460.0, -5.0, 400.0, 2.0, 0.5},      // Natural
    {14288.0, 9.75, 400.0, 1.75, -6.68},  // Classic
    {13502.0, -6.75, 400.0, 0.75, -8.09}, // Plate
};

float panGain(double pan, int channel)
{
    const double p = clamp(pan, -1.0, 1.0);
    return static_cast<float>(channel == 0 ? std::min(1.0, 1.0 - p) : std::min(1.0, 1.0 + p));
}

} // namespace

namespace {

// Brightness as a tone change, per style, fitted to reference renders of a
// 2.5 s hall at Brightness -100 .. +100 % in 25 % steps: one high and one low
// shelf (Q 0.5), a lowpass (Q 0.4) and a level. Natural (+50 % interpolated)
// matches every octave from 63 Hz to 16 kHz within 0.4 dB. Plate and Classic
// match 63 Hz .. 8 kHz within 1.8 dB, mostly within 0.5 dB; at 16 kHz the
// darkest Plate settings reach the references' noise floor, and Classic at
// -100 % cuts that octave almost completely, which a lowpass this gentle
// does not follow (about 20 dB short there). What Brightness does to the
// decay time is in DecayModel; the energy that change adds or removes is
// already taken out of these gains.
struct ToneStep
{
    double hiHz, hiDb, loHz, loDb, levelDb;
    double lpHz = 0.0; // 0 = no lowpass
};
constexpr ToneStep kBrightnessTone[3][9] = {
    {
        // Natural
        {2362.0, -25.5, 400.0, 5.5, -3.1}, // -100 %
        {2849.0, -20.0, 400.0, 3.5, -1.4}, //  -75 %
        {3228.0, -14.5, 400.0, 1.0, 0.4},  //  -50 %
        {4145.0, -7.5, 400.0, 0.0, 0.5},   //  -25 %
        {5500.0, 0.0, 300.0, 0.0, 0.0},    //    0
        {6836.0, 7.5, 250.0, 0.0, -0.6},   //  +25 %
        {7541.0, 10.0, 250.0, -1.75, -0.75},
        {8246.0, 12.5, 250.0, -3.5, -0.9}, //  +75 %
        {7747.0, 12.5, 250.0, -6.0, -1.1}, // +100 %
    },
    {
        // Classic: a lowpass that closes in only towards -100 %, a gentle
        // high shelf above 0; most of the darkening is in the decay.
        {5500.0, 0.0, 400.0, 0.0, -0.01, 10106.0}, // -100 %
        {5500.0, 0.0, 400.0, 0.0, -0.25, 14007.0}, //  -75 %
        {5500.0, 0.0, 400.0, 0.0, -0.20, 15640.0}, //  -50 %
        {5500.0, 0.0, 400.0, 0.0, -0.22},          //  -25 %
        {5500.0, 0.0, 300.0, 0.0, 0.0},            //    0
        {4000.0, 0.16, 250.0, -0.08, 0.01},        //  +25 %
        {4000.0, 0.58, 250.0, -0.08, -0.01},
        {4000.0, 1.19, 250.0, -0.10, -0.04},       //  +75 %
        {4000.0, 2.03, 250.0, -0.04, -0.12},       // +100 %
    },
    {
        // Plate: a lowpass whose corner falls about an octave and a half
        // per 25 % below 0, with the level made up; above 0 a high shelf
        // rises and the lows are taken down.
        {5500.0, 0.0, 400.0, 0.0, 7.03, 267.0},  // -100 %
        {5500.0, 0.0, 400.0, 0.0, 3.92, 748.0},  //  -75 %
        {5500.0, 0.0, 400.0, 0.0, 2.31, 2156.0}, //  -50 %
        {5500.0, 0.0, 400.0, 0.0, 1.14, 6740.0}, //  -25 %
        {5500.0, 0.0, 300.0, 0.0, 0.0},          //    0
        {8000.0, 3.09, 250.0, 0.20, -0.12},      //  +25 %
        {8000.0, 5.15, 250.0, 0.30, -0.24},
        {8000.0, 11.55, 250.0, -5.08, 0.32},     //  +75 %
        {8000.0, 17.43, 250.0, -10.43, 0.86},    // +100 %
    },
};
// The lowpass is faded in from far above the audio band, so a step without
// one blends smoothly into a step with one.
constexpr double kToneLpOffHz = 40000.0;

ToneStep brightnessTone(Style style, double r)
{
    const ToneStep* table = kBrightnessTone[static_cast<int>(style)];
    const double pos = (clamp(r, -1.0, 1.0) + 1.0) * 4.0;
    const int i = std::min(static_cast<int>(pos), 7);
    const double t = pos - i;
    const ToneStep& a = table[i];
    const ToneStep& b = table[i + 1];
    auto mix = [t](double x, double y) { return x + (y - x) * t; };
    auto mixLog = [&](double x, double y) { return std::exp(mix(std::log(x), std::log(y))); };
    ToneStep out{mixLog(a.hiHz, b.hiHz), mix(a.hiDb, b.hiDb), mixLog(a.loHz, b.loHz), mix(a.loDb, b.loDb),
                 mix(a.levelDb, b.levelDb)};
    if (a.lpHz > 0.0 || b.lpHz > 0.0)
    {
        const double lp = mixLog(a.lpHz > 0.0 ? a.lpHz : kToneLpOffHz, b.lpHz > 0.0 ? b.lpHz : kToneLpOffHz);
        out.lpHz = lp < kToneLpOffHz * 0.999 ? lp : 0.0;
    }
    return out;
}

// Thickness is mostly density, heard as level: reference renders measured
// -14.4, -7.4, 0, +5.1 and +6.6 dB at -100, -50, 0, +50 and +100 %, the same
// in every octave and with the decay time unchanged. The top two steps are
// raised by what the saturation above +50 % takes off a full-scale impulse.
// The plate follows the same curve within a dB; the second row is what
// reference renders of Plate at -100 .. +100 % add to it.
double thicknessLevelDb(Style style, double t)
{
    constexpr double kDb[5] = {-14.4, -7.4, 0.0, 6.0, 8.2};
    constexpr double kPlateDb[5] = {0.4, 0.5, 0.0, -0.25, -1.2};
    const double pos = (clamp(t, -1.0, 1.0) + 1.0) * 2.0;
    const int i = std::min(static_cast<int>(pos), 3);
    double db = kDb[i] + (kDb[i + 1] - kDb[i]) * (pos - i);
    if (style == Style::Plate)
        db += kPlateDb[i] + (kPlateDb[i + 1] - kPlateDb[i]) * (pos - i);
    return db;
}

} // namespace

EngineControl EngineControl::compute(const EngineParams& p, double space)
{
    EngineControl c;
    c.room = roomAt(space);
    const double ch = clamp(p.character, 0.0, 1.0);
    const double th = clamp(p.thickness, 0.0, 1.0);
    const double di = clamp(p.distance, 0.0, 1.0);
    const double br = clamp(p.brightness, 0.0, 1.0);

    // Character: up to 50 % adds early reflection emphasis, late echoes and
    // gentle modulation; beyond that modulation grows into a chorus.
    const double lowPart = std::min(ch, 0.5) / 0.5;
    const double highPart = std::max(ch - 0.5, 0.0) / 0.5;
    double echo = 0.55 * lowPart - 0.25 * highPart;
    echo += std::max(0.5 - th, 0.0) * 0.5; // sparse = more distinct echoes
    echo = clamp(echo, 0.0, 0.8);
    c.theta = kPi * 0.25 * (1.0 - 0.42 * echo);
    c.modDepthMs = 0.35 * std::pow(lowPart, 1.5) + 2.4 * std::pow(highPart, 1.4);
    c.modRateHz = 0.25 + 0.6 * lowPart + 1.2 * highPart;
    c.chorusMix = 0.45 * std::pow(highPart, 1.5);

    // Distance: near = strong bright reflections, far = diffuse slow build-up.
    c.erLevelDb = c.room.erLevel + 5.0 * (1.0 - di) - 8.0 * di + 3.5 * lowPart - 2.0 * highPart;
    // The Natural tank builds up smoothly on its own, and reflections much
    // louder than it are heard as a hard early burst in front of a thin tail.
    // Fitted to reference renders of a Distance sweep (0 .. 100 %, build-up
    // shape over the first 320 ms) and confirmed by three presets at 0.70 to
    // 0.81, which needed 17 to 23 dB less than the line above.
    if (p.style == Style::Natural)
        c.erLevelDb -= 12.5 + 10.0 * di;
    c.erStartMs = c.room.erStart * (0.6 + 1.4 * di);
    c.erLengthMs = c.room.erLength * (0.85 + 0.35 * di);
    c.erSparsity = clamp(0.55 * (1.0 - th) + 0.25 * echo - 0.2 * di, 0.0, 0.9);
    c.erDiffusion = 0.2 + 0.55 * di + 0.15 * th;
    c.erToneHz = clamp((3500.0 + 14000.0 * (1.0 - 0.7 * di)) * (0.55 + 0.9 * br), 1500.0, 20000.0);
    c.lateFeed = 1.0 - 0.65 * di;
    c.erFeed = 0.9 * di;
    c.lateDiffusion = clamp(c.room.diffusion * (0.7 + 0.45 * th) - 0.12 * echo, 0.3, 0.82);
    c.lateDiffScale = (0.7 + 0.8 * di) * std::sqrt(c.room.size / 30.0);

    // Thickness above 50 % adds a subtle saturation in front of the tank.
    const double sat = std::max(th - 0.5, 0.0) / 0.5;
    c.satDrive = 1.0 + 1.0 * sat * sat;

    // Width: 0 .. 0.5 mono to full cross-feed, 0.5 .. 1 to multi-mono, above
    // 1 the side signal is boosted.
    const double w = clamp(p.width, 0.0, 1.5);
    c.crossfeed = w <= 0.5 ? 1.0 : clamp(1.0 - (w - 0.5) / 0.5, 0.0, 1.0);
    c.sideGain = w <= 0.5 ? w / 0.5 : (w <= 1.0 ? 1.0 : 1.0 + (w - 1.0) * 2.0);

    const ToneStep tone = brightnessTone(p.style, br * 2.0 - 1.0);
    c.toneLpHz = tone.lpHz;
    c.toneHiHz = tone.hiHz;
    c.toneHiDb = tone.hiDb;
    c.toneLoHz = tone.loHz;
    c.toneLoDb = tone.loDb;

    // A bigger room holds more energy: reference renders are 4.1 dB louder for
    // a 10 s room than for a 2.5 s one at the same settings. Kept within
    // +-4 dB until shorter and longer rooms have been measured too.
    const double roomDb = clamp(2.05 * std::log2(c.room.t60 / 2.5), -4.0, 4.0);
    c.wetLevelDb = tone.levelDb + thicknessLevelDb(p.style, th * 2.0 - 1.0) + roomDb;
    // The plate loses less level towards a far Distance than the feeds above
    // take off: reference renders at 0 .. 100 % span 3.1 dB, against 6.4 dB
    // without this.
    if (p.style == Style::Plate)
        c.wetLevelDb += kPlateDistanceDb * (di - 0.5);
    return c;
}

ReverbEngine::ReverbEngine() : syncSet_(std::make_unique<DecayCoeffSet>()) {}

ReverbEngine::~ReverbEngine() { release(); }

void ReverbEngine::prepare(double sampleRate, int /*maxBlock*/)
{
    release();
    fs_ = sampleRate;
    designer_.prepare(sampleRate);

    const double maxLine = (kMaxRoomSizeMs * std::sqrt(kMaxSpread) + 6.0) * 1e-3;
    for (int t = 0; t < kTanks; ++t)
    {
        fdn_[t].prepare(sampleRate, maxLine, 0x1234u + 77u * static_cast<uint32_t>(t));
        classic_[t].prepare(sampleRate, 1.7, 0x4321u + 13u * static_cast<uint32_t>(t));
        plate_[t].prepare(sampleRate, 2.0, 0x777u + 5u * static_cast<uint32_t>(t));
        for (auto& d : lateDiff_[t])
            d.allocate(static_cast<int>(0.03 * sampleRate) + 16);

        // Line positions in [-1, 1] (geometric spread), jittered and
        // shuffled so butterfly partners differ in length.
        Rng rng(0xabcdu + 991u * static_cast<uint32_t>(t));
        std::array<float, FdnTank::N> pos{};
        for (int i = 0; i < FdnTank::N; ++i)
            pos[i] = -1.0f + 2.0f * (i + 0.5f) / FdnTank::N + (rng.uniform() - 0.5f) * 0.09f;
        for (int i = FdnTank::N - 1; i > 0; --i)
            std::swap(pos[i], pos[rng.next() % static_cast<uint32_t>(i + 1)]);
        fdnPos_[t] = pos;
    }
    er_.prepare(sampleRate, 0.3);

    maxPredelay_ = static_cast<float>((kMaxPredelayMs + 20.0) * 1e-3 * sampleRate);
    predelayL_.allocate(static_cast<int>(maxPredelay_) + 8);
    predelayR_.allocate(static_cast<int>(maxPredelay_) + 8);
    glide_ = onePoleCoeff(0.06, sampleRate);
    chorusL_.allocate(static_cast<int>(0.04 * sampleRate));
    chorusSplitC_ = OnePoleCoeffs::make(250.0, sampleRate);
    chorusR_.allocate(static_cast<int>(0.04 * sampleRate));

    postEq_.prepare(sampleRate);
    ducker_.prepare(sampleRate);
    gate_.prepare(sampleRate);
    spaceCoeff_ = onePoleCoeff(0.08, sampleRate / kBlock);

    prepared_ = true;
    reset();
    designer_.startWorker();
}

void ReverbEngine::release()
{
    designer_.stopWorker();
    prepared_ = false;
}

void ReverbEngine::reset()
{
    for (int t = 0; t < kTanks; ++t)
    {
        fdn_[t].clear();
        classic_[t].clear();
        plate_[t].clear();
        for (auto& d : lateDiff_[t])
            d.clear();
    }
    er_.clear();
    predelayL_.clear();
    predelayR_.clear();
    chorusL_.clear();
    chorusR_.clear();
    for (auto& c : chorusSplit_)
        c.reset();
    satL_.reset();
    satR_.reset();
    for (auto& s : toneHiS_)
        s.reset();
    for (auto& s : toneLoS_)
        s.reset();
    for (auto& s : toneLpS_)
        s.reset();
    for (auto& s : voiceHiS_)
        s.reset();
    for (auto& s : voiceLoS_)
        s.reset();
    postEq_.reset();
    ducker_.reset();
    gate_.reset();
    lastLanes_ = 0;
    samplesSinceRequest_ = 1 << 20;
    styleFadeDir_ = 0;
    styleGain_ = 1.0f;
    cutting_ = false;
}

DecayModel ReverbEngine::decayModelFor(const EngineParams& p) { return decayModelFor(p, roomAt(p.space), p.style); }

DecayModel ReverbEngine::decayModelFor(const EngineParams& p, const Room& room, Style style)
{
    DecayModel m;
    m.baseT60 = room.t60;
    m.decayRate = clamp(p.decayRate, 0.25, 4.0);
    m.lfMult = room.lfMult;
    m.hfMult = room.hfMult;
    m.hfFreq = room.hfFreq;
    m.brightness = p.brightness;
    m.bands = p.decayBands;
    if (style == Style::Classic)
    {
        // Early digital units: darker, slightly longer low end.
        m.hfMult *= 0.85;
        m.lfMult *= 1.1;
    }
    else if (style == Style::Plate)
    {
        // Plates sustain highs and decay lows faster.
        m.hfMult = std::min(1.0, m.hfMult * 1.45);
        m.lfMult = 0.85;
        m.hfFreq *= 1.6;
    }
    // Measured correction on top of the curves above, per octave 63 Hz ..
    // 16 kHz: a log2 multiplier and an absorption in 1/s, solved exactly from
    // reference renders of a 2.5 s and a 10 s room at default settings. The
    // absorption is what keeps the highs from growing with Length, as they do
    // not in the references.
    static constexpr double kCalib[3][9] = {
        {-0.214, 0.067, 0.061, 0.064, 0.152, 0.340, 0.478, 0.426, 0.310},     // Natural
        {-0.734, -0.479, -0.330, -0.037, 0.023, 0.389, 0.865, 1.496, 1.886},  // Classic
        {0.890, 0.768, 0.655, 0.382, 0.160, 0.234, 0.219, 0.118, 0.194},     // Plate
    };
    static constexpr double kAbsorb[3][9] = {
        {0.0153, 0.0256, 0.0146, 0.0051, 0.0, 0.0, 0.0, 0.0, 0.0095},
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0453, 0.1259, 0.2740, 0.3702},
        {0.0, 0.0, 0.0, 0.0, 0.0174, 0.0764, 0.0405, 0.0441, 0.1998},
    };
    // Brightness and the decay, per style, from reference renders of a 2.5 s
    // and a long room at -100 .. +100 %. Natural is what DecayModel's curves
    // were fitted to. Classic darkens its decay the same way, a little less at
    // the top (within 6 % per octave, 500 Hz .. 8 kHz). Plate does not change
    // its decay at all, in a short room or a long one: there Brightness is a
    // tone control only.
    if (style == Style::Classic)
    {
        m.brightAbsScale = 1.08;
        m.brightAbsExp = 0.85;
    }
    else if (style == Style::Plate)
    {
        m.brightAbsScale = 0.0;
        m.brightLog2Scale = 0.0;
        // Decay Rate, from reference renders at 25 .. 400 % of a 2.5 s room:
        // longer settings lengthen the plate less than in proportion, and
        // shorter ones run into a floor near 40 % of the room's time, flat
        // across the octaves.
        if (m.decayRate > 1.0)
            m.decayRate = std::pow(m.decayRate, kPlateRateExp);
        m.floorT60 = kPlateFloor * room.t60;
    }
    for (size_t k = 0; k < m.calib.size(); ++k)
    {
        m.calib[k] = kCalib[static_cast<int>(style)][k];
        m.absorb[k] = kAbsorb[static_cast<int>(style)][k];
    }
    return m;
}

double ReverbEngine::tailSeconds(const EngineParams& p)
{
    if (p.freeze)
        return -1.0;
    const DecayModel m = decayModelFor(p);
    double t = 0.0;
    for (double f = 31.25; f < 16000.0; f *= 2.0)
        t = std::max(t, m.t60At(f));
    return p.predelayMs * 1e-3 + 0.15 + t * 1.1;
}

int ReverbEngine::gatherSegments(double* delays) const
{
    int n = 0;
    switch (style_)
    {
    case Style::Natural:
        for (int t = 0; t < kTanks; ++t)
        {
            fdn_[t].currentDelays(delays + n);
            n += FdnTank::N;
        }
        break;
    case Style::Classic:
        for (int t = 0; t < kTanks; ++t)
        {
            classic_[t].currentDelays(delays + n);
            n += classic_[t].numSegments();
        }
        break;
    case Style::Plate:
        for (int t = 0; t < kTanks; ++t)
        {
            plate_[t].currentDelays(delays + n);
            n += plate_[t].numSegments();
        }
        break;
    }
    return n;
}

void ReverbEngine::applyCoeffs(const DecayCoeffSet& set)
{
    if (set.tag != styleTag_)
        return; // computed for a previous structure
    switch (style_)
    {
    case Style::Natural:
        for (int t = 0; t < kTanks; ++t)
            fdn_[t].loadCoeffs(set, t * FdnTank::N);
        break;
    case Style::Classic:
        for (int t = 0; t < kTanks; ++t)
            classic_[t].loadCoeffs(set, t * ClassicTank::kBranches);
        break;
    case Style::Plate:
        for (int t = 0; t < kTanks; ++t)
            plate_[t].loadCoeffs(set, t * 2);
        break;
    }
}

void ReverbEngine::maybeRequestDesign(const EngineParams& p, bool sync)
{
    DecayRequest req;
    // Design for the smoothed room and the structure that is actually running.
    req.model = decayModelFor(p, ctl_.room, style_);
    if (style_ == Style::Plate)
    {
        // Allpasses inside the plate loop concentrate modes where their group
        // delay peaks; those decay slower than the nominal loop length implies
        // (measured: about +8 % broadband, more at low frequencies).
        req.model.corrScale = 1.0 / 1.085;
        req.model.corrLfScale = 1.0 / 1.18;
        req.model.corrLfFreq = 220.0;
    }
    req.numLanes = gatherSegments(req.delays);
    req.freeze = p.freeze;
    req.tag = styleTag_;

    bool changed = sync || req.numLanes != lastLanes_ || req.freeze != lastFreeze_ || req.tag != lastTag_;
    const DecayModel& a = req.model;
    const DecayModel& b = lastModel_;
    auto rel = [](double x, double y) { return std::fabs(x - y) > 1e-4 * std::max(std::fabs(x), std::fabs(y)); };
    if (!changed)
        changed = rel(a.baseT60, b.baseT60) || rel(a.decayRate, b.decayRate) || rel(a.lfMult, b.lfMult) ||
                  rel(a.hfMult, b.hfMult) || rel(a.hfFreq, b.hfFreq) || std::fabs(a.brightness - b.brightness) > 1e-5;
    if (!changed)
        for (int k = 0; k < kNumDecayBands && !changed; ++k)
        {
            const DecayBand& x = a.bands[static_cast<size_t>(k)];
            const DecayBand& y = b.bands[static_cast<size_t>(k)];
            changed = x.used != y.used || x.enabled != y.enabled || x.shape != y.shape || rel(x.freq, y.freq) ||
                      std::fabs(x.rateLog2 - y.rateLog2) > 1e-5 || rel(x.q, y.q);
        }
    if (!changed)
        for (int i = 0; i < req.numLanes && !changed; ++i)
            changed = std::fabs(req.delays[i] - lastDelays_[static_cast<size_t>(i)]) > 0.002 * req.delays[i];
    if (!changed)
        return;

    const int minInterval = static_cast<int>(0.004 * fs_);
    if (!sync && !offline_ && samplesSinceRequest_ < minInterval)
        return;

    lastModel_ = req.model;
    for (int i = 0; i < req.numLanes; ++i)
        lastDelays_[static_cast<size_t>(i)] = req.delays[i];
    lastLanes_ = req.numLanes;
    lastFreeze_ = req.freeze;
    lastTag_ = req.tag;
    samplesSinceRequest_ = 0;

    if (sync || offline_)
    {
        designer_.compute(req, *syncSet_);
        applyCoeffs(*syncSet_);
    }
    else
    {
        designer_.post(req);
    }
}

void ReverbEngine::switchStyle(Style s)
{
    style_ = s;
    ++styleTag_;
    for (int t = 0; t < kTanks; ++t)
    {
        fdn_[t].clear();
        classic_[t].clear();
        plate_[t].clear();
        for (auto& d : lateDiff_[t])
            d.clear();
    }
}

void ReverbEngine::updateControl(const EngineParams& p, bool snap)
{
    space_ = snap ? p.space : p.space + (space_ - p.space) * spaceCoeff_;
    ctl_ = EngineControl::compute(p, space_);
    const Room& room = ctl_.room;
    const float ms = static_cast<float>(fs_ * 1e-3);
    const float modDepth = static_cast<float>(ctl_.modDepthMs) * ms;

    for (int t = 0; t < kTanks; ++t)
    {
        float d[FdnTank::N];
        for (int i = 0; i < FdnTank::N; ++i)
            d[i] = static_cast<float>(room.size * ms * std::pow(room.spread, 0.5 * fdnPos_[t][i]) *
                                      (t == 0 ? 1.0 : 1.037));
        fdn_[t].setTargetDelays(d);
        fdn_[t].setDiffusionAngle(static_cast<float>(ctl_.theta));
        fdn_[t].setModulation(modDepth, static_cast<float>(ctl_.modRateHz));

        classic_[t].setScale(static_cast<float>(room.size / 60.0 * (t == 0 ? 1.0 : 1.043)));
        classic_[t].setModulation(modDepth + 0.4f * ms, static_cast<float>(0.6 + ctl_.modRateHz));
        classic_[t].setDiffusion(static_cast<float>(p.thickness));

        plate_[t].setScale(static_cast<float>(clamp(room.size / 40.0, 0.45, 1.8) * (t == 0 ? 1.0 : 1.051)));
        plate_[t].setModulation(modDepth * 0.5f + 0.25f * ms, static_cast<float>(0.5 + ctl_.modRateHz));
        plate_[t].setDiffusion(static_cast<float>(p.thickness));
        if (snap)
        {
            fdn_[t].snapDelays();
            classic_[t].snapScale();
            plate_[t].snapScale();
        }
    }

    er_.setShape(ctl_.erLengthMs, ctl_.erStartMs, ctl_.erSparsity, ctl_.erDiffusion, ctl_.erToneHz);
    if (snap)
        er_.snap();

    predelayTarget_ = clamp(static_cast<float>(p.predelayMs * 1e-3 * fs_), 0.0f, maxPredelay_ - 8.0f);
    if (snap)
        predelay_ = predelayTarget_;

    // No level compensation for the Decay Rate EQ: a band that lengthens the
    // decay also makes the tail louder there, as a longer decay does. The
    // former -3 dB per doubling left presets that shorten their lows with up
    // to 11 dB too much bass against reference renders.

    toneHi_ = SvfCoeffs::highShelf(ctl_.toneHiHz, 0.5, ctl_.toneHiDb, fs_);
    toneLo_ = SvfCoeffs::lowShelf(ctl_.toneLoHz, 0.5, ctl_.toneLoDb, fs_);
    // Close to Nyquist the lowpass is left out rather than pushed against it.
    toneLpOn_ = ctl_.toneLpHz > 0.0 && ctl_.toneLpHz < 0.45 * fs_;
    if (toneLpOn_)
        toneLp_ = SvfCoeffs::lowPass(ctl_.toneLpHz, kToneLpQ, fs_);
    const Voicing& v = kVoicing[static_cast<int>(style_)];
    voiceHi_ = SvfCoeffs::highShelf(v.hiHz, 0.5, v.hiDb, fs_);
    voiceLo_ = SvfCoeffs::lowShelf(v.loHz, 0.5, v.loDb, fs_);
    postEq_.setBands(p.postBands);
    ducker_.setAmount(static_cast<float>(p.ducking));
    gate_.setEnabled(p.gateOn);
    gate_.setHold(p.gateHoldMs * 1e-3);

    const int ramp = snap ? 0 : kBlock;
    const float inG = static_cast<float>(dbToGain(p.inGainDb));
    const float outG = static_cast<float>(dbToGain(p.outGainDb));
    inGainL_.setTarget(inG * panGain(p.inPan, 0), ramp);
    inGainR_.setTarget(inG * panGain(p.inPan, 1), ramp);
    outGainL_.setTarget(outG * panGain(p.outPan, 0), ramp);
    outGainR_.setTarget(outG * panGain(p.outPan, 1), ramp);
    float dry, wet;
    equalPower(static_cast<float>(clamp(p.mix, 0.0, 1.0)), dry, wet);
    dryGain_.setTarget(dry, ramp);
    wetGain_.setTarget(wet * static_cast<float>(dbToGain(ctl_.wetLevelDb + v.levelDb)), ramp);
    erGain_.setTarget(static_cast<float>(dbToGain(ctl_.erLevelDb)), ramp);
    lateFeed_.setTarget(static_cast<float>(ctl_.lateFeed), ramp);
    erFeed_.setTarget(static_cast<float>(ctl_.erFeed), ramp);
    // Freeze closes the input over ~30 ms (one block ramp repeated).
    inputMute_.setTarget(p.freeze ? 0.0f : 1.0f, snap ? 0 : static_cast<int>(0.03 * fs_));
    sideGain_.setTarget(static_cast<float>(ctl_.sideGain), ramp);
    crossfeed_.setTarget(static_cast<float>(ctl_.crossfeed), ramp);
    bypassMix_.setTarget(p.bypass ? 0.0f : 1.0f, snap ? 0 : static_cast<int>(0.02 * fs_));
    chorusMix_.setTarget(static_cast<float>(ctl_.chorusMix), ramp);
    satDrive_.setTarget(static_cast<float>(ctl_.satDrive), ramp);
}

void ReverbEngine::process(const float* inL, const float* inR, float* outL, float* outR, int n, const EngineParams& p)
{
    if (!prepared_)
    {
        std::copy(inL, inL + n, outL);
        std::copy(inR, inR + n, outR);
        return;
    }
    if (cutRequested_)
    {
        cutRequested_ = false;
        // Nothing has played since the last reset: there is no tail to cut.
        if (lastLanes_ != 0 && !cutting_)
        {
            cutting_ = true;
            cutParams_ = lastParams_;
            styleFadeDir_ = -1;
            styleStep_ = static_cast<float>(1.0 / (0.010 * fs_));
        }
    }
    if (lastLanes_ == 0)
        settle(p);

    int done = 0;
    while (done < n)
    {
        if (cutting_ && styleGain_ <= 0.0f)
        {
            // Silent now: drop everything and start over from the new settings.
            cutting_ = false;
            reset();
            settle(p);
        }
        const int len = std::min(kBlock, n - done);
        if (cutting_)
        {
            // The reverb fades out as it was; the levels and the mix already
            // follow the new settings, so the dry signal does not jump when
            // the fade ends.
            cutParams_.mix = p.mix;
            cutParams_.inGainDb = p.inGainDb;
            cutParams_.inPan = p.inPan;
            cutParams_.outGainDb = p.outGainDb;
            cutParams_.outPan = p.outPan;
            cutParams_.bypass = p.bypass;
            processBlock(inL + done, inR + done, outL + done, outR + done, len, cutParams_);
        }
        else
            processBlock(inL + done, inR + done, outL + done, outR + done, len, p);
        done += len;
    }
    lastParams_ = p;
}

// Everything at its target at once: the first block after (re)activation,
// and the first after a cut.
void ReverbEngine::settle(const EngineParams& p)
{
    if (p.style != style_)
        switchStyle(p.style);
    requestedStyle_ = p.style;
    updateControl(p, true);
    maybeRequestDesign(p, true);
}

void ReverbEngine::processBlock(const float* inL, const float* inR, float* outL, float* outR, int n,
                                const EngineParams& p)
{
    // Style changes fade the wet signal out, swap structures, fade back in.
    // A cut owns the fade while it runs.
    if (!cutting_ && p.style != requestedStyle_ && styleFadeDir_ == 0)
    {
        requestedStyle_ = p.style;
        styleFadeDir_ = -1;
        styleStep_ = static_cast<float>(1.0 / (0.015 * fs_));
    }
    if (!cutting_ && styleFadeDir_ < 0 && styleGain_ <= 0.0f)
    {
        switchStyle(requestedStyle_);
        updateControl(p, false);
        // The new structure was just cleared: start it at its target size.
        for (int t = 0; t < kTanks; ++t)
        {
            fdn_[t].snapDelays();
            classic_[t].snapScale();
            plate_[t].snapScale();
        }
        maybeRequestDesign(p, true);
        styleFadeDir_ = 1;
    }
    if (styleFadeDir_ > 0 && styleGain_ >= 1.0f)
    {
        styleFadeDir_ = 0;
        styleGain_ = 1.0f;
        if (requestedStyle_ != style_ || p.style != style_)
        {
            requestedStyle_ = p.style;
            styleFadeDir_ = p.style != style_ ? -1 : 0;
        }
    }

    updateControl(p, false);
    if (const DecayCoeffSet* set = designer_.poll())
        applyCoeffs(*set);
    maybeRequestDesign(p, false);
    samplesSinceRequest_ += n;

    alignas(32) float dryL[kBlock], dryR[kBlock];
    alignas(32) float preL[kBlock], preR[kBlock];
    alignas(32) float duck[kBlock], gateG[kBlock];
    alignas(32) float erL[kBlock], erR[kBlock];
    alignas(32) float tankIn[kTanks][kBlock];
    alignas(32) float tankL[kTanks][kBlock], tankR[kTanks][kBlock];
    alignas(32) float wetL[kBlock], wetR[kBlock];

    // Input stage, detectors, predelay, saturation.
    const float glideBlock = std::pow(glide_, static_cast<float>(n));
    const float pd0 = predelay_;
    predelay_ = predelayTarget_ + (predelay_ - predelayTarget_) * glideBlock;
    const float pdStep = (predelay_ - pd0) / static_cast<float>(n);
    for (int s = 0; s < n; ++s)
    {
        const float xl = inL[s] * inGainL_.next();
        const float xr = inR[s] * inGainR_.next();
        dryL[s] = xl;
        dryR[s] = xr;
        const float key = std::max(std::fabs(xl), std::fabs(xr));
        duck[s] = ducker_.process(key);
        gateG[s] = gate_.process(key);

        const float m = inputMute_.next();
        predelayL_.push(xl * m);
        predelayR_.push(xr * m);
        const float pd = std::max(pd0 + pdStep * static_cast<float>(s + 1), 1.0f);
        float pl = predelayL_.readHermite(pd);
        float pr = predelayR_.readHermite(pd);
        const float drive = satDrive_.next();
        if (drive > 1.0001f)
        {
            pl = satL_.process(pl * drive) / drive;
            pr = satR_.process(pr * drive) / drive;
        }
        preL[s] = pl;
        preR[s] = pr;
    }

    er_.process(preL, preR, erL, erR, n);

    // Late input: width cross-feed, distance blend with the reflections.
    for (int s = 0; s < n; ++s)
    {
        const float c = crossfeed_.next();
        const float lf = lateFeed_.next();
        const float ef = erFeed_.next();
        const float a = preL[s] * (1.0f - 0.5f * c) + preR[s] * 0.5f * c;
        const float b = preR[s] * (1.0f - 0.5f * c) + preL[s] * 0.5f * c;
        tankIn[0][s] = a * lf + erL[s] * ef;
        tankIn[1][s] = b * lf + erR[s] * ef;
    }
    // Rewind cross-feed smoother for the output matrix below.
    const float cOut = crossfeed_.current;

    if (style_ != Style::Plate)
    {
        const float ms = static_cast<float>(fs_ * 1e-3);
        const float g = static_cast<float>(ctl_.lateDiffusion);
        for (int t = 0; t < kTanks; ++t)
            for (int k = 0; k < kLateDiffusers; ++k)
            {
                const float d = clamp(kLateDiffMs[k] * static_cast<float>(ctl_.lateDiffScale) * ms *
                                          (t == 0 ? 1.0f : 1.09f),
                                      2.0f, static_cast<float>(0.028 * fs_));
                AllpassDiffuser& ap = lateDiff_[t][static_cast<size_t>(k)];
                for (int s = 0; s < n; ++s)
                    tankIn[t][s] = ap.processFrac(tankIn[t][s], d, (k & 1) ? -g : g);
            }
    }

    for (int t = 0; t < kTanks; ++t)
    {
        switch (style_)
        {
        case Style::Natural: fdn_[t].process(tankIn[t], tankL[t], tankR[t], n); break;
        case Style::Classic: classic_[t].process(tankIn[t], tankL[t], tankR[t], n); break;
        case Style::Plate: plate_[t].process(tankIn[t], tankL[t], tankR[t], n); break;
        }
    }

#ifdef AURUM_DEBUG_STAGES
    {
        auto pk = [&](const float* x) { float m = 0; for (int i = 0; i < n; ++i) m = std::max(m, std::fabs(x[i])); return m; };
        const float tk = std::max({pk(tankL[0]), pk(tankR[0]), pk(tankL[1]), pk(tankR[1])});
        const float ti = std::max(pk(tankIn[0]), pk(tankIn[1]));
        if (tk > 5.0f || ti > 5.0f)
            fprintf(stderr, "stage: style %d tankIn %.2f tankOut %.2f er %.2f pre %.2f\n", (int)style_, ti, tk,
                    std::max(pk(erL), pk(erR)), std::max(pk(preL), pk(preR)));
    }
#endif
    // Output matrix, reflections, width.
    const float kNorm = 0.70710678f;
    for (int s = 0; s < n; ++s)
    {
        const float c = cOut;
        float l = (tankL[0][s] + c * tankL[1][s] + (1.0f - c) * tankR[0][s]) * kNorm;
        float r = (tankR[1][s] + c * tankR[0][s] + (1.0f - c) * tankL[1][s]) * kNorm;
        if (styleFadeDir_ != 0)
        {
            styleGain_ = clamp(styleGain_ + styleStep_ * static_cast<float>(styleFadeDir_), 0.0f, 1.0f);
            l *= styleGain_;
            r *= styleGain_;
        }
        const float eg = erGain_.next();
        l += erL[s] * eg;
        r += erR[s] * eg;
        const float mid = 0.5f * (l + r);
        const float side = 0.5f * (l - r) * sideGain_.next();
        wetL[s] = mid + side;
        wetR[s] = mid - side;
    }

    // Chorus for high Character settings (outside the loop, Hermite reads).
    {
        const float ms = static_cast<float>(fs_ * 1e-3);
        const double inc = 0.8 / fs_;
        for (int s = 0; s < n; ++s)
        {
            // Only the band above ~250 Hz is chorused, so the low end stays
            // solid (the voices are combined with opposite signs).
            const float lowL = chorusSplit_[0].lowpass(wetL[s], chorusSplitC_);
            const float lowR = chorusSplit_[1].lowpass(wetR[s], chorusSplitC_);
            const float highL = wetL[s] - lowL, highR = wetR[s] - lowR;
            chorusL_.push(highL);
            chorusR_.push(highR);
            const float cm = chorusMix_.next();
            if (cm <= 0.0f)
                continue;
            chorusPhase_ += inc;
            if (chorusPhase_ >= 1.0)
                chorusPhase_ -= 1.0;
            const float ph = static_cast<float>(chorusPhase_ * kTwoPi);
            const float depth = 2.5f * ms;
            const float d1 = static_cast<float>(kChorusBaseMs[0]) * ms + depth * std::sin(ph);
            const float d2 = static_cast<float>(kChorusBaseMs[1]) * ms + depth * std::sin(ph + 2.1f);
            const float cl = chorusL_.readHermite(d1) - chorusR_.readHermite(d2);
            const float cr = chorusR_.readHermite(d1) - chorusL_.readHermite(d2);
            wetL[s] = lowL + highL * (1.0f - 0.5f * cm) + cl * cm * 0.6f;
            wetR[s] = lowR + highR * (1.0f - 0.5f * cm) + cr * cm * 0.6f;
        }
    }

    if (toneLpOn_)
        for (int s = 0; s < n; ++s)
        {
            wetL[s] = toneLpS_[0].process(wetL[s], toneLp_);
            wetR[s] = toneLpS_[1].process(wetR[s], toneLp_);
        }
    for (int s = 0; s < n; ++s)
    {
        wetL[s] = toneLoS_[0].process(toneHiS_[0].process(wetL[s], toneHi_), toneLo_);
        wetR[s] = toneLoS_[1].process(toneHiS_[1].process(wetR[s], toneHi_), toneLo_);
        wetL[s] = voiceLoS_[0].process(voiceHiS_[0].process(wetL[s], voiceHi_), voiceLo_);
        wetR[s] = voiceLoS_[1].process(voiceHiS_[1].process(wetR[s], voiceHi_), voiceLo_);
    }
    postEq_.process(wetL, wetR, n);

    for (int s = 0; s < n; ++s)
    {
        const float wg = wetGain_.next() * duck[s] * gateG[s];
        const float dg = dryGain_.next();
        float l = dryL[s] * dg + wetL[s] * wg;
        float r = dryR[s] * dg + wetR[s] * wg;
        l *= outGainL_.next();
        r *= outGainR_.next();
        const float b = bypassMix_.next();
        l = inL[s] + (l - inL[s]) * b;
        r = inR[s] + (r - inR[s]) * b;
        outL[s] = l;
        outR[s] = r;
    }
}

} // namespace aurum::dsp
