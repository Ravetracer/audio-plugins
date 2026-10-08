#include "Player.h"

#include <algorithm>
#include <cmath>

namespace substrike {

namespace {
// An LFO's Sync choices past the delays' (which end at 1/1): 2/1 and 4/1.
double lfoBeats(int choice)
{
    if (choice >= 16)
        return choice == 16 ? 8.0 : 16.0;
    return dsp::syncBeats(choice);
}
} // namespace

void Player::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    engine_.prepare(sampleRate);
    // Sized here, on the main thread, so a fresh start never allocates.
    waitingValues_.reserve(static_cast<size_t>(ParamTable::get().count()));
    // The envelopes' lengths are in samples.
    if (!plain_.empty())
    {
        const std::vector<double> v = plain_;
        setValues(v.data());
    }
    reset();
}

void Player::reset()
{
    engine_.reset();
    freshLeft_ = 0;
    deferredCount_ = 0;
    clock_ = 0;
    beatsClock_ = 0;
    beatsAnchor_ = 0.0;
    for (int k = 0; k < pid::kNumLfos; ++k)
        anchorLfo(k, lfoSetup_[static_cast<size_t>(k)].phase);
    for (dsp::ModEnvelope& e : envs_)
        e.reset();
    velocity_ = note_ = random_ = 0.0;
    rng_ = dsp::Rng(0x6D6F64ull);
    if (routeCount_ > 0)
    {
        params_ = base_;
        modulate();
    }
}

void Player::setTapLanes(bool tap)
{
    tap_ = tap;
    base_.tapLanes = params_.tapLanes = tap;
}

void Player::setCurve(int index, const dsp::Curve& c)
{
    if (freshLeft_ > 0)
    {
        waitingCurves_[static_cast<size_t>(index)] = c;
        curveWaiting_[static_cast<size_t>(index)] = true;
        return;
    }
    curve(index) = c;
}

dsp::Curve& Player::curve(int index)
{
    if (index < dsp::kNumLaneCurves)
        return engine_.curve(index);
    return envCurves_[static_cast<size_t>(index - dsp::kNumLaneCurves)];
}

void Player::setValues(const double* values)
{
    const ParamTable& t = ParamTable::get();
    const int count = t.count();
    if (freshLeft_ > 0)
    {
        waitingValues_.assign(values, values + count);
        valuesWaiting_ = true;
        return;
    }
    plain_.assign(values, values + count);
    values_ = plain_;
    base_ = buildEngineParams(values);
    base_.tapLanes = tap_;
    auto plain = [&](uint32_t id) {
        const int i = t.indexOf(id);
        return ParamTable::toPlain(t.def(i), values[i]);
    };

    for (int m = 0; m < pid::kNumMacros; ++m)
        macros_[static_cast<size_t>(m)] = std::clamp(values[t.indexOf(pid::macro(m))], 0.0, 1.0);
    for (int k = 0; k < pid::kNumLfos; ++k)
    {
        LfoSetup s;
        s.shape = static_cast<dsp::LfoShape>(static_cast<int>(plain(pid::lfo(k, pid::LfoShape))));
        s.rate = plain(pid::lfo(k, pid::LfoRate));
        s.beats = lfoBeats(static_cast<int>(plain(pid::lfo(k, pid::LfoSync))));
        s.phase = plain(pid::lfo(k, pid::LfoPhase)) / 360.0;
        s.retrigger = plain(pid::lfo(k, pid::LfoRetrigger)) > 0.5;
        LfoSetup& old = lfoSetup_[static_cast<size_t>(k)];
        // A new speed carries on from where the LFO is.
        const bool moved = s.rate != old.rate || s.beats != old.beats || s.retrigger != old.retrigger;
        const double here = lfoPosition(k);
        const double phaseShift = s.phase - old.phase;
        old = s;
        if (moved || phaseShift != 0.0)
            anchorLfo(k, here + phaseShift);
    }
    for (int k = 0; k < pid::kNumModEnvs; ++k)
    {
        envLength_[static_cast<size_t>(k)] = plain(pid::modEnv(k, pid::EnvTime)) * 0.001 * sampleRate_;
        envLoop_[static_cast<size_t>(k)] = plain(pid::modEnv(k, pid::EnvLoop)) > 0.5;
    }

    routeCount_ = 0;
    for (int r = 0; r < pid::kNumRoutes; ++r)
    {
        const int source = static_cast<int>(plain(pid::route(r, pid::RSource)));
        const int dest = static_cast<int>(plain(pid::route(r, pid::RDest)));
        const double amount = plain(pid::route(r, pid::RAmount)) / 100.0;
        if (source <= 0 || dest <= 0 || dest > static_cast<int>(t.destinations().size()) || amount == 0.0)
            continue;
        Route& out = routes_[static_cast<size_t>(routeCount_++)];
        out.source = static_cast<ModSource>(source);
        out.dest = t.destinations()[static_cast<size_t>(dest - 1)];
        out.amount = amount;
        // +100 %: the square root of the square root, rising early; -100 %:
        // the fourth power, rising late.
        out.power = std::exp2(-2.0 * plain(pid::route(r, pid::RCurve)) / 100.0);
    }
    params_ = base_;
    if (routeCount_ > 0)
        modulate();
}

void Player::setTransport(double tempo, bool hasBeats, double beats)
{
    if (tempo > 0.0 && tempo != tempo_)
    {
        // The synced LFOs carry on from where they are at the new speed.
        std::array<double, pid::kNumLfos> here{};
        for (int k = 0; k < pid::kNumLfos; ++k)
            here[static_cast<size_t>(k)] = lfoPosition(k);
        anchorBeats();
        tempo_ = tempo;
        for (int k = 0; k < pid::kNumLfos; ++k)
            if (lfoSetup_[static_cast<size_t>(k)].beats > 0.0)
                anchorLfo(k, here[static_cast<size_t>(k)]);
    }
    engine_.setTempo(tempo_);
    if (hasBeats)
    {
        beatsClock_ = clock_;
        beatsAnchor_ = beats;
    }
}

void Player::anchorBeats()
{
    beatsAnchor_ = beatsNow();
    beatsClock_ = clock_;
}

double Player::beatsNow() const
{
    return beatsAnchor_ + static_cast<double>(clock_ - beatsClock_) * tempo_ / (60.0 * sampleRate_);
}

void Player::anchorLfo(int k, double position)
{
    lfoClock_[static_cast<size_t>(k)] = clock_;
    lfoAnchor_[static_cast<size_t>(k)] = position;
}

double Player::lfoPosition(int k) const
{
    const LfoSetup& s = lfoSetup_[static_cast<size_t>(k)];
    // Synced and free running: locked to the song.
    if (s.beats > 0.0 && !s.retrigger)
        return beatsNow() / s.beats + s.phase;
    const double elapsed = static_cast<double>(clock_ - lfoClock_[static_cast<size_t>(k)]);
    const double perSample = s.beats > 0.0 ? tempo_ / (60.0 * sampleRate_ * s.beats) : s.rate / sampleRate_;
    return lfoAnchor_[static_cast<size_t>(k)] + elapsed * perSample;
}

double Player::source(ModSource s) const
{
    const int i = static_cast<int>(s);
    const int lfo = static_cast<int>(ModSource::Lfo1), env = static_cast<int>(ModSource::Env1);
    const int mac = static_cast<int>(ModSource::Macro1), fol = static_cast<int>(ModSource::Follow1);
    switch (s)
    {
    case ModSource::Velocity: return velocity_;
    case ModSource::Note: return note_;
    case ModSource::Random: return random_;
    default: break;
    }
    if (i >= lfo && i < env)
    {
        dsp::Lfo l = lfos_[static_cast<size_t>(i - lfo)];
        l.position = lfoPosition(i - lfo);
        return l.value(lfoSetup_[static_cast<size_t>(i - lfo)].shape);
    }
    if (i >= env && i < mac)
    {
        const size_t k = static_cast<size_t>(i - env);
        return envs_[k].value(envCurves_[k], envLength_[k], envLoop_[k]);
    }
    if (i >= mac && i < fol)
        return macros_[static_cast<size_t>(i - mac)];
    if (i >= fol && i < static_cast<int>(ModSource::Count))
        return std::min(engine_.laneLevel(i - fol), 1.0);
    return 0.0;
}

void Player::modulate()
{
    std::array<double, static_cast<size_t>(ModSource::Count)> src{};
    std::array<bool, static_cast<size_t>(ModSource::Count)> have{};
    std::array<int, pid::kNumRoutes> dests{};
    std::array<double, pid::kNumRoutes> sums{};
    int n = 0;
    for (int r = 0; r < routeCount_; ++r)
    {
        const Route& route = routes_[static_cast<size_t>(r)];
        const size_t si = static_cast<size_t>(route.source);
        if (!have[si])
        {
            src[si] = source(route.source);
            have[si] = true;
        }
        const double x = src[si];
        const double shaped = route.power == 1.0 ? x : std::copysign(std::pow(std::fabs(x), route.power), x);
        int slot = 0;
        while (slot < n && dests[static_cast<size_t>(slot)] != route.dest)
            ++slot;
        if (slot == n)
        {
            dests[static_cast<size_t>(n)] = route.dest;
            sums[static_cast<size_t>(n++)] = 0.0;
        }
        sums[static_cast<size_t>(slot)] += route.amount * shaped;
    }
    for (int i = 0; i < n; ++i)
    {
        const size_t d = static_cast<size_t>(dests[static_cast<size_t>(i)]);
        values_[d] = std::clamp(plain_[d] + sums[static_cast<size_t>(i)], 0.0, 1.0);
        assignParam(params_, static_cast<int>(d), values_.data());
    }
}

void Player::setClock(uint64_t sample)
{
    if (sample >= clock_)
    {
        advance(static_cast<int>(std::min<uint64_t>(sample - clock_, 1u << 30)));
        clock_ = sample;
        return;
    }
    // The host's counter went back: carry everything on from where it is.
    std::array<double, pid::kNumLfos> here{};
    for (int k = 0; k < pid::kNumLfos; ++k)
        here[static_cast<size_t>(k)] = lfoPosition(k);
    const double beats = beatsNow();
    clock_ = sample;
    for (int k = 0; k < pid::kNumLfos; ++k)
        anchorLfo(k, here[static_cast<size_t>(k)]);
    beatsClock_ = clock_;
    beatsAnchor_ = beats;
}

void Player::advance(int n)
{
    clock_ += static_cast<uint64_t>(n);
    for (dsp::ModEnvelope& e : envs_)
        e.advance(n);
}

void Player::freshStart()
{
    if (engine_.idle())
    {
        // Nothing sounds: start clean at once.
        engine_.reset();
        return;
    }
    freshLength_ = std::max(1, static_cast<int>(kFreshFadeMs * 0.001 * sampleRate_));
    freshLeft_ = freshLength_;
}

void Player::finishFreshStart()
{
    freshLeft_ = 0;
    engine_.reset();
    for (int i = 0; i < dsp::kNumCurves; ++i)
        if (curveWaiting_[static_cast<size_t>(i)])
        {
            curve(i) = waitingCurves_[static_cast<size_t>(i)];
            curveWaiting_[static_cast<size_t>(i)] = false;
        }
    if (valuesWaiting_)
    {
        valuesWaiting_ = false;
        setValues(waitingValues_.data());
    }
    const int count = deferredCount_;
    deferredCount_ = 0;
    for (int i = 0; i < count; ++i)
        noteOn(deferred_[static_cast<size_t>(i)].key, deferred_[static_cast<size_t>(i)].velocity);
}

void Player::noteOn(int key, double velocity)
{
    if (freshLeft_ > 0)
    {
        if (deferredCount_ < static_cast<int>(deferred_.size()))
            deferred_[static_cast<size_t>(deferredCount_++)] = {key, velocity};
        return;
    }
    velocity_ = std::clamp(velocity, 0.0, 1.0);
    note_ = std::clamp((key - base_.rootNote) / 24.0, -1.0, 1.0);
    random_ = rng_.bipolar();
    for (int k = 0; k < pid::kNumLfos; ++k)
        if (lfoSetup_[static_cast<size_t>(k)].retrigger)
            anchorLfo(k, lfoSetup_[static_cast<size_t>(k)].phase);
    for (dsp::ModEnvelope& e : envs_)
        e.trigger();
    if (routeCount_ > 0)
        modulate();
    engine_.noteOn(key, velocity, params_);
}

void Player::process(const dsp::Bus* buses, int n)
{
    // A fresh start: the old sound fades out, then the engine resets and the
    // notes that came in the meantime play.
    if (freshLeft_ > 0)
    {
        const int m = std::min(n, freshLeft_);
        render(buses, m);
        for (int b = 0; b < dsp::Engine::kNumBuses; ++b)
            for (int i = 0; i < m; ++i)
            {
                const float g = static_cast<float>(freshLeft_ - i - 1) / static_cast<float>(freshLength_);
                buses[b].l[i] *= g;
                buses[b].r[i] *= g;
            }
        freshLeft_ -= m;
        if (freshLeft_ == 0)
            finishFreshStart();
        if (m < n)
        {
            dsp::Bus rest[dsp::Engine::kNumBuses];
            for (int b = 0; b < dsp::Engine::kNumBuses; ++b)
                rest[b] = {buses[b].l + m, buses[b].r + m};
            render(rest, n - m);
        }
        return;
    }
    render(buses, n);
}

void Player::render(const dsp::Bus* buses, int n)
{
    if (routeCount_ == 0)
    {
        engine_.process(buses, n, params_);
        advance(n);
        return;
    }
    int pos = 0;
    while (pos < n)
    {
        const int phase = static_cast<int>(clock_ % kStep);
        if (phase == 0)
            modulate();
        const int m = std::min(n - pos, kStep - phase);
        dsp::Bus sub[dsp::Engine::kNumBuses];
        for (int b = 0; b < dsp::Engine::kNumBuses; ++b)
            sub[b] = {buses[b].l + pos, buses[b].r + pos};
        engine_.process(sub, m, params_);
        advance(m);
        pos += m;
    }
}

} // namespace substrike
