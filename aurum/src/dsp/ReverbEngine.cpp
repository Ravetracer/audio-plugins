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

// Loudness calibration of the late tail per style (keeps styles comparable).
constexpr float kStyleWet[3] = {1.0f, 0.9f, 0.85f};

float panGain(double pan, int channel)
{
    const double p = clamp(pan, -1.0, 1.0);
    return static_cast<float>(channel == 0 ? std::min(1.0, 1.0 - p) : std::min(1.0, 1.0 + p));
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
    c.erStartMs = c.room.erStart * (0.6 + 1.4 * di);
    c.erLengthMs = c.room.erLength * (0.85 + 0.35 * di);
    c.erSparsity = clamp(0.55 * (1.0 - th) + 0.25 * echo - 0.2 * di, 0.0, 0.9);
    c.erDiffusion = 0.2 + 0.55 * di + 0.15 * th;
    c.erToneHz = clamp((3500.0 + 14000.0 * (1.0 - 0.7 * di)) * (0.55 + 0.9 * br), 1500.0, 20000.0);
    c.lateFeed = 1.0 - 0.65 * di;
    c.erFeed = 0.9 * di;
    c.lateDiffusion = clamp(c.room.diffusion * (0.7 + 0.45 * th) - 0.12 * echo, 0.3, 0.82);
    c.lateDiffScale = (0.7 + 0.8 * di) * std::sqrt(c.room.size / 30.0);

    // Thickness above 50 % adds saturation in front of the tank.
    const double sat = std::max(th - 0.5, 0.0) / 0.5;
    c.satDrive = 1.0 + 5.0 * sat * sat;

    // Width: 0 .. 0.5 mono to full cross-feed, 0.5 .. 1 to multi-mono, above
    // 1 the side signal is boosted.
    const double w = clamp(p.width, 0.0, 1.5);
    c.crossfeed = w <= 0.5 ? 1.0 : clamp(1.0 - (w - 0.5) / 0.5, 0.0, 1.0);
    c.sideGain = w <= 0.5 ? w / 0.5 : (w <= 1.0 ? 1.0 : 1.0 + (w - 1.0) * 2.0);

    // Brightness also tilts the wet tone a little.
    const double r = br * 2.0 - 1.0;
    c.toneHiDb = r < 0.0 ? 9.0 * r : 2.0 * r;
    c.toneLoDb = -0.75 * r;
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
    for (auto& st : compS_)
        for (auto& x : st)
            x.reset();
    satL_.reset();
    satR_.reset();
    for (auto& s : toneHiS_)
        s.reset();
    for (auto& s : toneLoS_)
        s.reset();
    postEq_.reset();
    ducker_.reset();
    gate_.reset();
    lastLanes_ = 0;
    samplesSinceRequest_ = 1 << 20;
    styleFadeDir_ = 0;
    styleGain_ = 1.0f;
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

    // Level compensation for the Decay Rate EQ: tail energy is proportional
    // to T60, so a band that scales T60 by 2^r gets -3 dB * r.
    numComp_ = 0;
    for (const DecayBand& b : p.decayBands)
    {
        if (!b.used || !b.enabled || std::fabs(b.rateLog2) < 1e-4)
            continue;
        const double g = clamp(-3.0103 * b.rateLog2, -12.0, 12.0);
        SvfCoeffs c;
        switch (b.shape)
        {
        case DecayShape::LowShelf: c = SvfCoeffs::lowShelf(b.freq, b.q * kShelfQScale, g, fs_); break;
        case DecayShape::HighShelf: c = SvfCoeffs::highShelf(b.freq, b.q * kShelfQScale, g, fs_); break;
        default: c = SvfCoeffs::bell(b.freq, b.q, g, fs_); break;
        }
        comp_[static_cast<size_t>(numComp_++)] = c;
    }
    if (numComp_ != lastNumComp_)
    {
        for (auto& st : compS_)
            for (auto& x : st)
                x.reset();
        lastNumComp_ = numComp_;
    }

    toneHi_ = SvfCoeffs::highShelf(5000.0, 0.6, ctl_.toneHiDb, fs_);
    toneLo_ = SvfCoeffs::lowShelf(220.0, 0.6, ctl_.toneLoDb, fs_);
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
    wetGain_.setTarget(wet * kStyleWet[static_cast<int>(style_)], ramp);
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
    if (lastLanes_ == 0)
    {
        // First block after (re)activation: settle everything immediately.
        if (p.style != style_)
            switchStyle(p.style);
        requestedStyle_ = p.style;
        updateControl(p, true);
        maybeRequestDesign(p, true);
    }

    int done = 0;
    while (done < n)
    {
        const int len = std::min(kBlock, n - done);
        processBlock(inL + done, inR + done, outL + done, outR + done, len, p);
        done += len;
    }
}

void ReverbEngine::processBlock(const float* inL, const float* inR, float* outL, float* outR, int n,
                                const EngineParams& p)
{
    // Style changes fade the wet signal out, swap structures, fade back in.
    if (p.style != requestedStyle_ && styleFadeDir_ == 0)
    {
        requestedStyle_ = p.style;
        styleFadeDir_ = -1;
        styleStep_ = static_cast<float>(1.0 / (0.015 * fs_));
    }
    if (styleFadeDir_ < 0 && styleGain_ <= 0.0f)
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
        // Decay Rate EQ changes the decay time, not the level of the tail.
        for (int k = 0; k < numComp_; ++k)
        {
            l = compS_[static_cast<size_t>(k)][0].process(l, comp_[static_cast<size_t>(k)]);
            r = compS_[static_cast<size_t>(k)][1].process(r, comp_[static_cast<size_t>(k)]);
        }
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

    for (int s = 0; s < n; ++s)
    {
        wetL[s] = toneLoS_[0].process(toneHiS_[0].process(wetL[s], toneHi_), toneLo_);
        wetR[s] = toneLoS_[1].process(toneHiS_[1].process(wetR[s], toneHi_), toneLo_);
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
