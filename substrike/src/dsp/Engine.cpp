#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace substrike::dsp {

namespace {
// A retrigger or a choke fades the old hit out over this long. Short enough
// not to smear the new transient, long enough not to click.
constexpr double kFadeMs = 3.0;
// Level, pan and output glide over this long, so automation does not zipper.
constexpr double kSmoothMs = 5.0;
// A chain that rings on after its voices is done once its output has stayed
// under -120 dB for this long; its state is then cleared.
constexpr double kTailQuietMs = 50.0;
constexpr float kQuiet = 1e-6f;

// Counts quiet samples from `from` on and returns the offset at which the
// count reaches `limit`, or -1. Samples before `from` are still sounding.
int findSilence(const float* l, const float* r, int from, int n, int& quiet, int limit)
{
    if (from > 0)
        quiet = 0;
    for (int i = from; i < n; ++i)
    {
        if (std::fabs(l[i]) < kQuiet && std::fabs(r[i]) < kQuiet)
        {
            if (++quiet >= limit)
                return i;
        }
        else
            quiet = 0;
    }
    return -1;
}

template <typename Voices> void fadeAll(Voices& voices, int samples)
{
    for (auto& v : voices)
        v.fadeOut(samples);
}

template <typename Voices> bool anyActive(const Voices& voices)
{
    for (const auto& v : voices)
        if (v.active())
            return true;
    return false;
}

// A free voice, or -- three hits inside one fade time -- the oldest fade,
// cut short.
template <typename Voices> auto& freeVoice(Voices& voices)
{
    for (auto& v : voices)
        if (!v.active())
            return v;
    return voices[0];
}
} // namespace

// ----------------------------------------------------------------------- Lane

void Lane::prepare(double sampleRate)
{
    for (ClickVoice& v : click_)
        v.prepare(sampleRate);
    quietLimit_ = std::max(1, static_cast<int>(kTailQuietMs * 0.001 * sampleRate));
    levelFall_ = std::exp(-1.0 / (0.05 * sampleRate));
    reset();
}

void Lane::prepareChain(double sampleRate, int oversampling)
{
    for (Slot& s : slots_)
        s.prepare(sampleRate, oversampling);
    tail_ = false;
    quiet_ = 0;
}

void Lane::reset()
{
    for (auto& v : body_)
        v.kill();
    for (auto& v : click_)
        v.kill();
    for (auto& v : noise_)
        v.kill();
    for (auto& v : resonator_)
        v.kill();
    pendingCount_ = 0;
    primed_ = false;
    snapGain_ = false;
    hits_ = 0;
    for (Slot& s : slots_)
        s.reset();
    tail_ = false;
    quiet_ = 0;
    preEnd_ = postEnd_ = 0;
    window_ = INT64_MAX / 2;
    duckEnv_ = 0.0;
    ducking_ = false;
    level_ = 0.0;
}

void Lane::trigger(int semitones, double velocity, int delay)
{
    if (pendingCount_ == kMaxPending)
        return;
    // A hit into silence takes its level and pan as they are now, rather
    // than gliding there from where they were while the lane slept: a
    // per-note modulation of the level has to reach the hit's first sample.
    if (!voicesActive() && !tail_)
        snapGain_ = true;
    pending_[static_cast<size_t>(pendingCount_++)] = {std::max(delay, 0), semitones, velocity};
}

void Lane::choke(int fadeSamples)
{
    pendingCount_ = 0;
    fadeAll(body_, fadeSamples);
    fadeAll(click_, fadeSamples);
    fadeAll(noise_, fadeSamples);
    fadeAll(resonator_, fadeSamples);
}

bool Lane::voicesActive() const
{
    return anyActive(body_) || anyActive(click_) || anyActive(noise_) || anyActive(resonator_);
}

bool Lane::active() const { return pendingCount_ > 0 || tail_ || voicesActive(); }

void Lane::fire(const Pending& pending, const Context& c)
{
    const LaneParams& p = *c.params;
    fadeAll(body_, c.fadeSamples);
    fadeAll(click_, c.fadeSamples);
    fadeAll(noise_, c.fadeSamples);
    fadeAll(resonator_, c.fadeSamples);

    Hit hit;
    hit.semitones = pending.semitones;
    hit.level = 1.0 - std::clamp(p.velocity, 0.0, 1.0) * (1.0 - std::clamp(pending.velocity, 0.0, 1.0));
    // Without variation every hit gets the same seed, so the noise, the drift
    // and everything else random repeat exactly. Lanes never share a seed.
    const uint64_t laneSeed = static_cast<uint64_t>(c.index + 1) * 0x100000001B3ull;
    const double v = std::clamp(p.variation, 0.0, 1.0);
    hit.seed = v > 0.0 ? laneSeed ^ ((hits_ + 1) * 0x9E3779B97F4A7C15ull) : laneSeed;
    if (v > 0.0)
    {
        // Up to 50 cents, 3 dB and 20 % of every decay, either way.
        Rng rng(hit.seed ^ 0x5851F42D4C957F2Dull);
        hit.pitchRatio = std::exp2(v * 50.0 / 1200.0 * rng.bipolar());
        hit.level *= std::pow(10.0, v * 3.0 * rng.bipolar() / 20.0);
        hit.timeScale = 1.0 + 0.2 * v * rng.bipolar();
    }
    ++hits_;

    const double transpose = std::exp2(p.transpose / 12.0);
    const bool linked = c.track.curve != &pitchCurve_;
    switch (p.source)
    {
    case Source::Body: freeVoice(body_).start(hit, p.body.phase); break;
    case Source::Click:
    {
        const double pitch = (linked ? c.track.endFreq(hit.semitones) : p.click.pitch * transpose) * hit.pitchRatio;
        freeVoice(click_).start(hit, p.click, pitch, p.click.cutoff * transpose, c.sampleRate);
        break;
    }
    case Source::Noise: freeVoice(noise_).start(hit); break;
    case Source::Bus: break; // its sound comes in from the other lanes
    case Source::Resonator:
    {
        const ResonatorParams& r = p.resonator;
        const double tune = linked ? c.track.endFreq(hit.semitones)
                                   : r.tune * std::exp2(hit.semitones * r.keyTrack / 12.0) * transpose;
        freeVoice(resonator_).start(hit, r, tune * hit.pitchRatio, c.sampleRate);
        break;
    }
    }
}

void Lane::renderVoices(int from, int to, const Context& c)
{
    const int n = to - from;
    if (n <= 0)
        return;
    const LaneParams& p = *c.params;
    float* l = l_.data() + from;
    float* r = r_.data() + from;
    int sounded = 0;
    auto track = [&](int rendered) { sounded = std::max(sounded, rendered); };
    for (BodyVoice& v : body_)
        if (v.active())
            track(v.render(l, r, n, c.sampleRate, p.body, c.track, ampCurve_));
    for (ClickVoice& v : click_)
        if (v.active())
            track(v.render(l, r, n));
    const double transpose = std::exp2(p.transpose / 12.0);
    for (NoiseVoice& v : noise_)
        if (v.active())
            track(v.render(l, r, n, c.sampleRate, p.noise, transpose));
    for (ResonatorVoice& v : resonator_)
        if (v.active())
            track(v.render(l, r, n));
    if (sounded > 0)
        soundEnd_ = std::max(soundEnd_, from + sounded);
}

int Lane::process(const Bus& main, const Bus& aux, int n, const Context& c)
{
    const LaneParams& p = *c.params;
    const double sign = p.invert ? -1.0 : 1.0;
    const double targetL = sign * p.gain * std::min(1.0, 1.0 - p.pan);
    const double targetR = sign * p.gain * std::min(1.0, 1.0 + p.pan);
    const int busEnd = c.busL ? std::min(c.busEnd, n) : 0;
    if (!primed_ || !active() || snapGain_)
    {
        gainL_ = targetL;
        gainR_ = targetR;
        primed_ = true;
        snapGain_ = false;
        if (!active() && busEnd <= 0)
        {
            preEnd_ = postEnd_ = 0;
            // Sample by sample, as when it sounds, so the value does not
            // depend on how the time was cut into blocks.
            for (int i = 0; i < n && level_ > 0.0; ++i)
                level_ = level_ * levelFall_ < 1e-9 ? 0.0 : level_ * levelFall_;
            return 0;
        }
    }
    std::fill(l_.begin(), l_.begin() + n, 0.0f);
    std::fill(r_.begin(), r_.begin() + n, 0.0f);
    soundEnd_ = 0;
    firedCount_ = 0;

    // Render up to each scheduled hit that falls inside this chunk, fire it on
    // its sample, and carry on.
    int pos = 0;
    for (;;)
    {
        int which = -1, due = n;
        for (int i = 0; i < pendingCount_; ++i)
            if (pending_[static_cast<size_t>(i)].at < due)
            {
                due = pending_[static_cast<size_t>(i)].at;
                which = i;
            }
        renderVoices(pos, due, c);
        if (which < 0)
            break;
        const Pending hit = pending_[static_cast<size_t>(which)];
        std::copy(pending_.begin() + which + 1, pending_.begin() + pendingCount_, pending_.begin() + which);
        --pendingCount_;
        fire(hit, c);
        fired_[static_cast<size_t>(firedCount_++)] = due;
        pos = due;
    }
    for (int i = 0; i < pendingCount_; ++i)
        pending_[static_cast<size_t>(i)].at -= n;

    // A bus lane's input joins whatever its old source still fades out.
    if (busEnd > 0)
    {
        for (int i = 0; i < busEnd; ++i)
        {
            l_[static_cast<size_t>(i)] += c.busL[i];
            r_[static_cast<size_t>(i)] += c.busR[i];
        }
        soundEnd_ = std::max(soundEnd_, busEnd);
    }
    std::copy(l_.begin(), l_.begin() + n, preL_.begin());
    std::copy(r_.begin(), r_.begin() + n, preR_.begin());
    preEnd_ = soundEnd_;

    // The chain, then the check for the end of its tail. The tail ends on the
    // sample its output has been quiet for long enough, counted from where the
    // voices stopped, so where it ends does not depend on the block size.
    const SlotEnv env{c.sampleRate, p.xoverLow, p.xoverHigh, c.tempo};
    bool live = false;
    int hold = 0;
    for (int s = 0; s < kNumSlots; ++s)
    {
        Slot& slot = slots_[static_cast<size_t>(s)];
        slot.process(l_.data(), r_.data(), n, p.slots[static_cast<size_t>(s)], env, fired_.data(), firedCount_);
        live |= slot.live();
        hold = std::max(hold, slot.silentHold());
    }
    // An input that sounds to the end of the chunk may go on into the next.
    const bool sounding = voicesActive() || busEnd >= n;
    int end = sounding ? n : soundEnd_;
    tail_ = false;
    if (live)
    {
        const int from = sounding ? n : soundEnd_;
        // A delay waits for its next echo in silence: the chain has rung
        // out only once it has been quiet for longer than that.
        const int silent = findSilence(l_.data(), r_.data(), from, n, quiet_, quietLimit_ + hold);
        if (silent >= 0)
        {
            std::fill(l_.begin() + silent + 1, l_.begin() + n, 0.0f);
            std::fill(r_.begin() + silent + 1, r_.begin() + n, 0.0f);
            for (Slot& s : slots_)
                s.reset();
            quiet_ = 0;
            end = silent + 1;
        }
        else
        {
            tail_ = true;
            end = n;
        }
    }
    else
        quiet_ = 0;
    // The guard comes after the check for the end, so a lane held silent by
    // its window does not count as rung out.
    guard(n, c);
    postEnd_ = end;
    for (int i = 0; i < n; ++i)
    {
        const double x = std::max(std::fabs(l_[static_cast<size_t>(i)]), std::fabs(r_[static_cast<size_t>(i)]));
        level_ = x > level_ ? x : (level_ * levelFall_ < 1e-9 ? 0.0 : level_ * levelFall_);
    }

    const bool toMain = p.output != Output::Aux;
    const bool toAux = p.output != Output::Main || c.tap;
    for (int i = 0; i < n; ++i)
    {
        gainL_ += c.smoothCoef * (targetL - gainL_);
        gainR_ += c.smoothCoef * (targetR - gainR_);
        const float left = static_cast<float>(l_[static_cast<size_t>(i)] * gainL_);
        const float right = static_cast<float>(r_[static_cast<size_t>(i)] * gainR_);
        if (toMain)
        {
            main.l[i] += left;
            main.r[i] += right;
        }
        if (toAux)
        {
            aux.l[i] += left;
            aux.r[i] += right;
        }
    }
    return end;
}

void Lane::follow(const float* l, const float* r, int end, int n, const LaneParams& p, double sampleRate)
{
    const int src = p.guard.duckSource;
    ducking_ = src >= 0 && p.guard.duckDepth > 0.0;
    if (!ducking_)
    {
        duckEnv_ = 0.0;
        return;
    }
    // A peak follower: half a millisecond up, the release down.
    const double up = 1.0 - std::exp(-1.0 / (0.0005 * sampleRate));
    const double down = std::exp(-1.0 / std::max(p.guard.duckReleaseMs * 0.001 * sampleRate, 1.0));
    const double depth = std::clamp(p.guard.duckDepth, 0.0, 1.0);
    for (int i = 0; i < n; ++i)
    {
        const double x = i < end ? std::max(std::fabs(l[i]), std::fabs(r[i])) : 0.0;
        duckEnv_ = x > duckEnv_ ? duckEnv_ + up * (x - duckEnv_) : duckEnv_ * down;
        if (duckEnv_ < 1e-9)
            duckEnv_ = 0.0;
        duck_[static_cast<size_t>(i)] = static_cast<float>(1.0 - depth * std::min(duckEnv_, 1.0));
    }
}

void Lane::guard(int n, const Context& c)
{
    const GuardParams& g = c.params->guard;
    const int64_t delay = static_cast<int64_t>(std::lround(g.delayMs * 0.001 * c.sampleRate));
    const int64_t fade = static_cast<int64_t>(std::lround(g.fadeMs * 0.001 * c.sampleRate));
    const bool windowed = delay > 0 || fade > 0;
    int h = 0;
    for (int i = 0; i < n; ++i)
    {
        while (h < firedCount_ && fired_[static_cast<size_t>(h)] <= i)
        {
            window_ = 0;
            ++h;
        }
        double gain = 1.0;
        if (windowed && window_ < delay + fade)
        {
            if (window_ < delay)
                gain = 0.0;
            else
            {
                // A raised cosine from 0 to 1 over the fade.
                const double x = static_cast<double>(window_ - delay) / static_cast<double>(fade);
                gain = 0.5 - 0.5 * std::cos(3.14159265358979323846 * x);
            }
        }
        if (window_ < INT64_MAX / 2)
            ++window_;
        if (ducking_)
            gain *= duck_[static_cast<size_t>(i)];
        if (gain != 1.0)
        {
            l_[static_cast<size_t>(i)] = static_cast<float>(l_[static_cast<size_t>(i)] * gain);
            r_[static_cast<size_t>(i)] = static_cast<float>(r_[static_cast<size_t>(i)] * gain);
        }
    }
}

// --------------------------------------------------------------------- Engine

void Engine::prepare(double sampleRate)
{
    sampleRate_ = sampleRate;
    smoothCoef_ = 1.0 - std::exp(-1.0 / (kSmoothMs * 0.001 * sampleRate));
    fadeSamples_ = std::max(1, static_cast<int>(std::lround(kFadeMs * 0.001 * sampleRate)));
    quietLimit_ = std::max(1, static_cast<int>(kTailQuietMs * 0.001 * sampleRate));
    for (Lane& l : lanes_)
    {
        l.prepare(sampleRate);
        l.prepareChain(sampleRate, oversampling_);
    }
    for (Slot& s : master_)
        s.prepare(sampleRate, oversampling_);
    clipL_.setFactor(oversampling_);
    clipR_.setFactor(oversampling_);
    prepareLimit();
    reset();
}

void Engine::prepareLimit()
{
    outLimit_.prepare(sampleRate_);
    outLimit_.setFixed(std::pow(10.0, -0.3 / 20.0), 60.0);
}

void Engine::resetMaster()
{
    for (Slot& s : master_)
        s.reset();
    monoL_.reset();
    monoR_.reset();
    clipL_.reset();
    clipR_.reset();
    outLimit_.reset();
    masterHit_ = false;
    masterTail_ = false;
    masterQuiet_ = 0;
}

void Engine::reset()
{
    for (Lane& l : lanes_)
        l.reset();
    resetMaster();
    primed_ = false;
}

void Engine::noteOn(int key, double velocity, const EngineParams& p)
{
    const int semitones = key - p.rootNote;
    // Into silence the output level is taken as it is, like a lane's.
    if (idle())
        snapOutput_ = true;
    masterHit_ = true;
    for (int l = 0; l < kNumLanes; ++l)
    {
        const LaneParams& lp = p.lanes[static_cast<size_t>(l)];
        if (!lp.enabled || (lp.note >= 0 && lp.note != key))
            continue;
        const int delay = static_cast<int>(std::lround(lp.delayMs * 0.001 * sampleRate_));
        lanes_[static_cast<size_t>(l)].trigger(semitones, velocity, delay);
    }
}

void Engine::choke()
{
    for (Lane& l : lanes_)
        l.choke(fadeSamples_);
}

bool Engine::idle() const
{
    if (masterTail_)
        return false;
    for (const Lane& l : lanes_)
        if (l.active())
            return false;
    return true;
}

void Engine::master(const Bus& main, int n, int lanesEnd, const EngineParams& p)
{
    const int hit = 0;
    const bool hitNow = masterHit_;
    masterHit_ = false;
    const SlotEnv env{sampleRate_, p.masterXoverLow, p.masterXoverHigh, tempo_};
    bool live = false;
    int hold = 0;
    for (int s = 0; s < kNumSlots; ++s)
    {
        Slot& slot = master_[static_cast<size_t>(s)];
        slot.process(main.l, main.r, n, p.master[static_cast<size_t>(s)], env, &hit, hitNow ? 1 : 0);
        live |= slot.live();
        hold = std::max(hold, slot.silentHold());
    }

    // Mono below: the low band of an LR4 split goes to both sides as its mid.
    if (p.monoBelow > 0.0)
    {
        if (p.monoBelow != monoFreq_)
        {
            if (monoFreq_ <= 0.0)
            {
                monoL_.reset();
                monoR_.reset();
            }
            monoFreq_ = p.monoBelow;
            monoL_.setup(monoFreq_, sampleRate_);
            monoR_.setup(monoFreq_, sampleRate_);
        }
        for (int i = 0; i < n; ++i)
        {
            double lowL, highL, lowR, highR;
            monoL_.split(main.l[i], lowL, highL);
            monoR_.split(main.r[i], lowR, highR);
            const double mid = 0.5 * (lowL + lowR);
            main.l[i] = static_cast<float>(mid + highL);
            main.r[i] = static_cast<float>(mid + highR);
        }
        live = true;
    }
    else
        monoFreq_ = 0.0;

    // The output clip, at full scale and oversampled like the drive slots;
    // or the limiter, at the base rate so its ceiling is exact.
    if (p.clip == OutputClip::Limit)
    {
        if (clip_ != p.clip)
            outLimit_.reset();
        outLimit_.process(main.l, main.r, n);
        live = true;
    }
    else if (p.clip != OutputClip::Off)
    {
        if (clip_ != p.clip)
        {
            clipL_.reset();
            clipR_.reset();
        }
        const double knee = p.clip == OutputClip::Soft ? 0.3 : 0.0;
        const int f = clipL_.factor();
        for (int pos = 0; pos < n; pos += Slot::kStep)
        {
            const int m = std::min(Slot::kStep, n - pos);
            clipL_.up(main.l + pos, clipBufL_.data(), m);
            clipR_.up(main.r + pos, clipBufR_.data(), m);
            for (int i = 0; i < m * f; ++i)
            {
                clipBufL_[static_cast<size_t>(i)] =
                    static_cast<float>(ClipperFx::shape(clipBufL_[static_cast<size_t>(i)], knee));
                clipBufR_[static_cast<size_t>(i)] =
                    static_cast<float>(ClipperFx::shape(clipBufR_[static_cast<size_t>(i)], knee));
            }
            clipL_.down(clipBufL_.data(), main.l + pos, m);
            clipR_.down(clipBufR_.data(), main.r + pos, m);
        }
        live = true;
    }
    clip_ = p.clip;

    masterTail_ = false;
    if (!live)
    {
        masterQuiet_ = 0;
        return;
    }
    const int silent = findSilence(main.l, main.r, lanesEnd, n, masterQuiet_, quietLimit_ + hold);
    if (silent >= 0)
    {
        std::fill(main.l + silent + 1, main.l + n, 0.0f);
        std::fill(main.r + silent + 1, main.r + n, 0.0f);
        resetMaster();
    }
    else
        masterTail_ = true;
}

void Engine::process(const Bus* buses, int n, const EngineParams& p)
{
    if (!primed_ || snapOutput_)
    {
        outGain_ = p.outGain;
        primed_ = true;
        snapOutput_ = false;
    }
    // A new quality setting re-prepares every chain; it is not automatable,
    // so this happens when someone picks it, not in the middle of a song.
    if (p.oversampling != oversampling_)
    {
        oversampling_ = p.oversampling;
        for (Lane& l : lanes_)
            l.prepareChain(sampleRate_, oversampling_);
        for (Slot& s : master_)
            s.prepare(sampleRate_, oversampling_);
        clipL_.setFactor(oversampling_);
        clipR_.setFactor(oversampling_);
        prepareLimit();
        resetMaster();
    }

    // Resolve every lane's pitch link once per call. A link reads the named
    // lane's Body pitch, whatever that lane plays and whether it is on; links
    // do not chain.
    std::array<Lane::Context, kNumLanes> ctx;
    for (int i = 0; i < kNumLanes; ++i)
    {
        const LaneParams& lp = p.lanes[static_cast<size_t>(i)];
        const int src = lp.pitchLink >= 0 && lp.pitchLink < kNumLanes ? lp.pitchLink : i;
        const LaneParams& sp = p.lanes[static_cast<size_t>(src)];
        PitchTrack t;
        t.start = sp.body.pitchStart;
        t.end = sp.body.pitchEnd;
        t.sweepMs = sp.body.sweepMs;
        t.sweepCurve = sp.body.sweepCurve;
        t.keyTrack = sp.body.keyTrack;
        t.ratio = std::exp2(lp.transpose / 12.0) * (src != i ? std::exp2(sp.transpose / 12.0) : 1.0);
        t.curve = &lanes_[static_cast<size_t>(src)].pitchCurve();
        ctx[static_cast<size_t>(i)] = {sampleRate_, smoothCoef_, fadeSamples_, i,      &lp, t, p.tapLanes,
                                       tempo_,      nullptr,     nullptr,      0};
    }

    // The order the lanes run in: every lane after the lanes it reads (a bus
    // lane's inputs, a duck's source), otherwise by number. A lane in a loop
    // runs when nothing else can, and reads silence from the lanes after it.
    std::array<int, kNumLanes> order{};
    std::array<int, kNumLanes> rank{};
    {
        auto reads = [&](int i, int j) {
            const LaneParams& lp = p.lanes[static_cast<size_t>(i)];
            if (i == j)
                return false;
            return (lp.source == Source::Bus && lp.bus.from[static_cast<size_t>(j)]) || lp.guard.duckSource == j;
        };
        std::array<bool, kNumLanes> done{};
        for (int k = 0; k < kNumLanes; ++k)
        {
            int pick = -1;
            for (int i = 0; i < kNumLanes && pick < 0; ++i)
            {
                if (done[static_cast<size_t>(i)])
                    continue;
                bool ready = true;
                for (int j = 0; j < kNumLanes; ++j)
                    ready &= done[static_cast<size_t>(j)] || !reads(i, j);
                if (ready)
                    pick = i;
            }
            for (int i = 0; i < kNumLanes && pick < 0; ++i)
                if (!done[static_cast<size_t>(i)])
                    pick = i;
            done[static_cast<size_t>(pick)] = true;
            order[static_cast<size_t>(k)] = pick;
            rank[static_cast<size_t>(pick)] = k;
        }
    }

    for (int pos = 0; pos < n; pos += Lane::kChunk)
    {
        const int m = std::min(Lane::kChunk, n - pos);
        for (int b = 0; b < kNumBuses; ++b)
        {
            std::fill(buses[b].l + pos, buses[b].l + pos + m, 0.0f);
            std::fill(buses[b].r + pos, buses[b].r + pos + m, 0.0f);
        }
        const Bus main{buses[0].l + pos, buses[0].r + pos};
        int lanesEnd = 0;
        for (int k = 0; k < kNumLanes; ++k)
        {
            const int i = order[static_cast<size_t>(k)];
            const LaneParams& lp = p.lanes[static_cast<size_t>(i)];
            Lane& lane = lanes_[static_cast<size_t>(i)];
            Lane::Context& c = ctx[static_cast<size_t>(i)];
            // A lane switched off while it sounds fades out rather than
            // stopping dead.
            if (!lp.enabled)
                lane.choke(fadeSamples_);
            // Only the lanes that ran before this one in this chunk have
            // anything to give it.
            auto ran = [&](int j) { return j != i && rank[static_cast<size_t>(j)] < k; };
            // A lane switched off takes no more input; what its chain still
            // holds rings out like any lane's tail.
            if (lp.source == Source::Bus && lp.enabled)
            {
                int busEnd = 0;
                std::fill(busL_.begin(), busL_.begin() + m, 0.0f);
                std::fill(busR_.begin(), busR_.begin() + m, 0.0f);
                for (int j = 0; j < kNumLanes; ++j)
                {
                    if (!lp.bus.from[static_cast<size_t>(j)] || !ran(j))
                        continue;
                    const Lane& from = lanes_[static_cast<size_t>(j)];
                    const int end = lp.bus.post ? from.postEnd() : from.preEnd();
                    const float* fl = lp.bus.post ? from.postL() : from.preL();
                    const float* fr = lp.bus.post ? from.postR() : from.preR();
                    for (int s = 0; s < end; ++s)
                    {
                        busL_[static_cast<size_t>(s)] += fl[s];
                        busR_[static_cast<size_t>(s)] += fr[s];
                    }
                    busEnd = std::max(busEnd, end);
                }
                c.busL = busL_.data();
                c.busR = busR_.data();
                c.busEnd = busEnd;
            }
            const int duck = lp.guard.duckSource;
            if (duck >= 0 && duck < kNumLanes && ran(duck))
            {
                const Lane& from = lanes_[static_cast<size_t>(duck)];
                lane.follow(from.postL(), from.postR(), from.postEnd(), m, lp, sampleRate_);
            }
            else
                lane.follow(nullptr, nullptr, 0, m, lp, sampleRate_);
            const Bus aux{buses[1 + i].l + pos, buses[1 + i].r + pos};
            const int end = lane.process(main, aux, m, c);
            // Only a lane that reaches the main output keeps the master busy.
            if (lp.output != Output::Aux)
                lanesEnd = std::max(lanesEnd, end);
        }
        if (lanesEnd > 0 || masterTail_ || masterHit_)
            master(main, m, lanesEnd, p);
        // The master level is the main output's; the aux outputs carry the
        // lanes as they leave them.
        for (int i = 0; i < m; ++i)
        {
            outGain_ += smoothCoef_ * (p.outGain - outGain_);
            main.l[i] = static_cast<float>(main.l[i] * outGain_);
            main.r[i] = static_cast<float>(main.r[i] * outGain_);
        }
    }
}

} // namespace substrike::dsp
