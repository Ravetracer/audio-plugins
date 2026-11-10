#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace sidmidi {

Engine::Engine()
{
    for (auto& f : frames_)
        f.assign(static_cast<size_t>(emu::Vic::kWidth) * emu::Vic::kHeight, 0);
    scratch_.resize(8192);
    txBytes_.reserve(emu::MidiCart::TxQueue::capacity());
    // A short default pan spread so a second SID is audibly a second SID.
    for (int i = 0; i < emu::SidBank::kMaxChips; ++i)
        mix[i].pan = i == 0 ? 0.0f : ((i & 1) ? 0.5f : -0.5f);
}

void Engine::setSampleRate(double rate)
{
    rate_ = rate;
    cyclesPerSample_ = c64_.clockHz() / rate_;
    c64_.sids.configure(c64_.sids.config(), c64_.clockHz(), rate_, c64_.clk());
    resyncAudio();
}

void Engine::configureSids(const emu::SidBankConfig& cfg)
{
    c64_.sids.configure(cfg, c64_.clockHz(), rate_, c64_.clk());
    resyncAudio();
}

void Engine::setStandard(emu::VideoStandard std)
{
    c64_.setStandard(std);
    cyclesPerSample_ = c64_.clockHz() / rate_;
    c64_.sids.configure(c64_.sids.config(), c64_.clockHz(), rate_, c64_.clk());
    resyncAudio();
}

void Engine::resync()
{
    holdAuto_ = false; // a new start warps again
    resyncAudio();
}

void Engine::resyncAudio()
{
    c64_.sids.sync(c64_.clk());
    c64_.sids.discard();
    wasWarping_ = true; // primes the FIFOs at the next block
}

std::vector<uint8_t> Engine::saveAudioState() const
{
    std::vector<uint8_t> out;
    auto put = [&out](const void* p, size_t n) {
        out.insert(out.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    };
    const uint32_t magic = 0x41444d53; // "SMDA"
    put(&magic, 4);
    put(&clkFrac_, sizeof(clkFrac_));
    const uint8_t warm = wasWarping_ ? 1 : 0;
    put(&warm, 1);
    const uint32_t n = static_cast<uint32_t>(c64_.sids.count());
    put(&n, 4);
    for (int i = 0; i < c64_.sids.count(); ++i) {
        const auto p = c64_.sids.pending(i);
        const uint32_t k = static_cast<uint32_t>(p.size());
        put(&k, 4);
        put(p.data(), p.size() * sizeof(int16_t));
    }
    // The voice outputs' queues after them (older builds stop reading before).
    for (int i = 0; i < c64_.sids.count(); ++i) {
        if (!c64_.sids.hasVoices(i))
            continue;
        for (int v = 0; v < emu::SidBank::kVoices; ++v) {
            const auto p = c64_.sids.pendingVoice(i, v);
            const uint32_t k = static_cast<uint32_t>(p.size());
            put(&k, 4);
            put(p.data(), p.size() * sizeof(int16_t));
        }
    }
    return out;
}

bool Engine::loadAudioState(const std::vector<uint8_t>& data)
{
    size_t at = 0;
    auto get = [&](void* p, size_t n) {
        if (at + n > data.size())
            return false;
        std::memcpy(p, data.data() + at, n);
        at += n;
        return true;
    };
    uint32_t magic = 0, n = 0;
    double frac = 0;
    uint8_t warm = 1;
    if (!get(&magic, 4) || magic != 0x41444d53 || !get(&frac, sizeof(frac)) || !get(&warm, 1) || !get(&n, 4) ||
        static_cast<int>(n) != c64_.sids.count())
        return false;
    std::vector<std::vector<int16_t>> fifos(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t k = 0;
        if (!get(&k, 4) || k > emu::SidBank::kFifoSize)
            return false;
        fifos[i].resize(k);
        if (!get(fifos[i].data(), k * sizeof(int16_t)))
            return false;
    }
    clkFrac_ = frac;
    wasWarping_ = warm != 0;
    for (uint32_t i = 0; i < n; ++i)
        c64_.sids.setPending(static_cast<int>(i), fifos[i]);
    // Voice queues, when the project has them; else setPending's silence stays.
    std::vector<int16_t> q;
    for (uint32_t i = 0; i < n; ++i) {
        if (!c64_.sids.hasVoices(static_cast<int>(i)))
            continue;
        for (int v = 0; v < emu::SidBank::kVoices; ++v) {
            uint32_t k = 0;
            if (!get(&k, 4) || k > emu::SidBank::kFifoSize)
                return true;
            q.resize(k);
            if (!get(q.data(), k * sizeof(int16_t)))
                return true;
            c64_.sids.setPendingVoice(static_cast<int>(i), v, q);
        }
    }
    return true;
}

void Engine::runTo(uint64_t clk)
{
    c64_.run(clk);
    if (c64_.vic.frameCount() != lastFrame_) {
        lastFrame_ = c64_.vic.frameCount();
        publishFrame();
    }
}

void Engine::publishFrame()
{
    std::memcpy(frames_[back_].data(), c64_.vic.frame(), frames_[back_].size());
    back_ = middle_.exchange(back_, std::memory_order_acq_rel);
    fresh_.store(true, std::memory_order_release);
    frameSerial_.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t Engine::copyFrame(uint8_t* dst)
{
    const uint64_t serial = frameSerial_.load(std::memory_order_acquire);
    if (fresh_.exchange(false, std::memory_order_acq_rel))
        front_ = middle_.exchange(front_, std::memory_order_acq_rel);
    std::memcpy(dst, frames_[front_].data(), frames_[front_].size());
    return serial;
}

void Engine::process(uint32_t frames, const MidiEvent* events, size_t numEvents, const Outputs& out,
                     std::vector<MidiEvent>& midiOut)
{
    if (frames == 0)
        return;
    const int nSids = c64_.sids.count();
    const uint64_t start = c64_.clk();
    c64_.vic.renderVideo = videoWanted.load(std::memory_order_relaxed);
    if (holdAuto_ && !autoWarping())
        holdAuto_ = false; // the stopped warp is over; the next one runs again

    if (warping()) {
        // Cycles in bulk, audio and incoming MIDI dropped; the program has
        // not started and nothing it would do with them is meaningful.
        // The SIDs are not clocked in warp (nobody hears them); they skip ahead.
        c64_.sids.setSkipping(true);
        const double blockCycles = frames * cyclesPerSample_;
        if (realtime) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto budget = std::chrono::duration<double>(0.5 * frames / rate_);
            const uint64_t end = start + static_cast<uint64_t>(blockCycles * kWarpFactorMax);
            while (c64_.clk() < end && warping() && std::chrono::steady_clock::now() - t0 < budget)
                runTo(std::min(end, c64_.clk() + 10000));
        } else {
            const uint64_t end = start + static_cast<uint64_t>(blockCycles * kWarpFactor);
            while (c64_.clk() < end && warping())
                runTo(std::min(end, c64_.clk() + 20000));
        }
        c64_.sids.sync(c64_.clk());
        c64_.sids.setSkipping(false);
        c64_.sids.discard();
        c64_.midiFlush();
        if (out.left)
            std::fill(out.left, out.left + frames, 0.0f);
        if (out.right)
            std::fill(out.right, out.right + frames, 0.0f);
        for (int i = 0; i < emu::SidBank::kMaxChips; ++i) {
            if (out.sid[i])
                std::fill(out.sid[i], out.sid[i] + frames, 0.0f);
            for (float* v : out.voice[i])
                if (v)
                    std::fill(v, v + frames, 0.0f);
        }
        wasWarping_ = true;
        driveLed.store(c64_.drive.busy() || (c64_.trueDriveActive() && c64_.drive1541.ledOn()), std::memory_order_relaxed);
        return;
    }
    if (wasWarping_) {
        wasWarping_ = false;
        c64_.sids.sync(c64_.clk());
        c64_.sids.discard();
        clkFrac_ = 0.0;
    }

    // Events at their cycles.
    auto clkAt = [&](uint32_t offset) {
        return start + static_cast<uint64_t>(std::floor((offset + clkFrac_ / cyclesPerSample_) * cyclesPerSample_));
    };
    for (size_t i = 0; i < numEvents; ++i) {
        const auto& e = events[i];
        const uint64_t at = clkAt(std::min(e.offset, frames));
        runTo(at);
        if ((e.ext ? e.extSize : static_cast<uint32_t>(e.size)) > c64_.midiRoom())
            continue; // whole or not at all
        if (e.ext) {
            for (uint32_t k = 0; k < e.extSize; ++k)
                c64_.midiReceive(e.ext[k]);
        } else {
            for (int k = 0; k < e.size; ++k)
                c64_.midiReceive(e.data[k]);
        }
        midiInCount.fetch_add(1, std::memory_order_relaxed);
    }
    const double exact = frames * cyclesPerSample_ + clkFrac_;
    const uint64_t cycles = static_cast<uint64_t>(exact);
    clkFrac_ = exact - static_cast<double>(cycles);
    const uint64_t end = start + cycles;
    runTo(end);
    c64_.sids.sync(c64_.clk());

    // Mix. Each FIFO is drained so that kSidLatency samples remain.
    if (out.left)
        std::fill(out.left, out.left + frames, 0.0f);
    if (out.right)
        std::fill(out.right, out.right + frames, 0.0f);
    if (scratch_.size() < frames)
        scratch_.resize(frames);
    // Takes `frames` samples from a FIFO into `buf`, keeping kSidLatency queued.
    auto drain = [&](size_t avail, auto pop, float* buf) {
        if (avail < frames + kSidLatency) {
            // Not enough yet (just primed): pad at the front.
            const size_t have = avail > kSidLatency ? avail - kSidLatency : 0;
            const size_t pad = frames - std::min<size_t>(frames, have);
            std::fill(buf, buf + pad, 0.0f);
            pop(buf + pad, frames - pad);
        } else {
            // More than needed (should not happen): drop the excess first.
            const size_t excess = avail - frames - kSidLatency;
            for (size_t left = excess > 4 ? excess : 0; left;) {
                const size_t n = std::min<size_t>(left, frames);
                pop(buf, n);
                left -= n;
            }
            pop(buf, frames);
        }
    };
    for (int i = 0; i < emu::SidBank::kMaxChips; ++i) {
        float* buf = scratch_.data();
        const bool voices = i < nSids && c64_.sids.hasVoices(i);
        for (int v = 0; v < emu::SidBank::kVoices; ++v) {
            float* dst = out.voice[i][v];
            if (voices && dst)
                drain(c64_.sids.availableVoice(i, v), [&](float* b, size_t n) { c64_.sids.popVoice(i, v, b, n); }, dst);
            else if (voices) // nobody listens: keep the queue at its latency
                drain(c64_.sids.availableVoice(i, v), [&](float* b, size_t n) { c64_.sids.popVoice(i, v, b, n); }, buf);
            else if (dst)
                std::fill(dst, dst + frames, 0.0f);
        }
        if (i >= nSids) {
            if (out.sid[i])
                std::fill(out.sid[i], out.sid[i] + frames, 0.0f);
            continue;
        }
        drain(c64_.sids.available(i), [&](float* b, size_t n) { c64_.sids.pop(i, b, n); }, buf);
        if (out.sid[i])
            std::memcpy(out.sid[i], buf, frames * sizeof(float));
        const float g = mix[i].level * masterGain;
        const float p = std::clamp(mix[i].pan, -1.0f, 1.0f);
        // Constant-power pan.
        const float angle = (p + 1.0f) * 0.25f * 3.14159265f;
        const float gl = g * std::cos(angle) * 1.41421356f;
        const float gr = g * std::sin(angle) * 1.41421356f;
        for (uint32_t k = 0; k < frames; ++k) {
            if (out.left)
                out.left[k] += buf[k] * gl;
            if (out.right)
                out.right[k] += buf[k] * gr;
        }
    }

    txBytes_.clear();
    c64_.midi.takeTransmitted(txBytes_);
    for (const auto& b : txBytes_) {
        if (midiOut.size() >= midiOut.capacity())
            break; // the caller reserves; growing here would allocate in the audio thread
        MidiEvent e;
        const double off = (static_cast<double>(b.clk) - static_cast<double>(start)) / cyclesPerSample_;
        e.offset = static_cast<uint32_t>(std::clamp(off, 0.0, static_cast<double>(frames ? frames - 1 : 0)));
        e.size = 1;
        e.data[0] = b.byte;
        midiOut.push_back(e);
        midiOutCount.fetch_add(1, std::memory_order_relaxed);
    }
    driveLed.store(c64_.drive.busy() || (c64_.trueDriveActive() && c64_.drive1541.ledOn()), std::memory_order_relaxed);
}

} // namespace sidmidi
