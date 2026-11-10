#include "SidBank.h"

#include <algorithm>
#include <cstring>

#include "SidChip.h"

namespace sidmidi::emu {

const char* sidVariantName(SidVariant v)
{
    switch (v) {
    case SidVariant::MOS6581: return "6581";
    case SidVariant::MOS6581R3: return "6581R3";
    case SidVariant::MOS6581R4: return "6581R4";
    case SidVariant::MOS6581R4AR: return "6581R4AR";
    case SidVariant::MOS8580R5: return "8580R5";
    default: return "?";
    }
}

bool sidVariantIs8580(SidVariant v)
{
    return v == SidVariant::MOS8580R5;
}

// Starting points per revision. They are educated guesses from listening
// reports and the spread reSIDfp's controls were made to cover, which is why
// every one of them is a control the user can change:
//  - R3 (the classic breadbin chip): bright filter, average combined waves.
//  - R4: a little darker, weaker combined waves.
//  - R4AR (late breadbin / early C64C): the dark, distorting filter, strong
//    combined waves, louder digis.
//  - 8580R5: reSIDfp's 8580 defaults, strong combined waves, little leakage.
SidTuning sidVariantTuning(SidVariant v)
{
    SidTuning t;
    switch (v) {
    case SidVariant::MOS6581:
        t.filterCurve = 0.5f;
        t.combined = CombinedWaves::Average;
        break;
    case SidVariant::MOS6581R3:
        t.filterCurve = 0.62f;
        t.combined = CombinedWaves::Average;
        t.digiOffset = 0.8f;
        break;
    case SidVariant::MOS6581R4:
        t.filterCurve = 0.45f;
        t.combined = CombinedWaves::Weak;
        t.digiOffset = 0.9f;
        break;
    case SidVariant::MOS6581R4AR:
        t.filterCurve = 0.32f;
        t.combined = CombinedWaves::Strong;
        t.digiOffset = 1.0f;
        break;
    case SidVariant::MOS8580R5:
        t.filterCurve = 0.5f;
        t.combined = CombinedWaves::Strong;
        t.dacLeakage = 0.3f;
        t.digiOffset = 0.0f;
        break;
    default:
        break;
    }
    return t;
}

SidBank::SidBank()
{
    for (auto& c : chips_) {
        c.fifo.buf.assign(kFifoSize, 0);
        for (auto& f : c.voiceFifo)
            f.buf.assign(kFifoSize, 0);
    }
    scratch_.resize(1 << 16);
}

void SidBank::Fifo::pop(float* out, size_t n)
{
    constexpr float k = 1.0f / 32768.0f;
    for (size_t i = 0; i < n; ++i) {
        if (rd == wr) {
            out[i] = 0.0f;
            continue;
        }
        out[i] = static_cast<float>(buf[rd]) * k;
        rd = (rd + 1) % kFifoSize;
    }
}

SidBank::~SidBank() = default;

void SidBank::applyTuning(int i, bool force)
{
    const bool valid = appliedValid_[i] && !force;
    chips_[i].sid->tune(cfg_.chips[i], applied_[i], valid);
    for (auto& t : chips_[i].taps)
        if (t)
            t->tune(cfg_.chips[i], applied_[i], valid);
    applied_[i] = cfg_.chips[i];
    appliedValid_[i] = true;
}

void SidBank::setSampling(SidChip& sid) const
{
    sid.setSampling(clockHz_, cfg_.resampling, sampleRate_);
}

namespace {
// How long a chip restarted on a running machine runs unheard first.
constexpr double kRestartSettleSeconds = 0.5;
} // namespace

void SidBank::settleChip(Chip& c)
{
    // A chip nobody has written to has nothing to settle.
    if (!c.written)
        return;
    auto run = [this](SidChip& sid) {
        for (auto todo = static_cast<uint64_t>(kRestartSettleSeconds * clockHz_); todo;) {
            const unsigned n = static_cast<unsigned>(std::min<uint64_t>(todo, 20000));
            todo -= n;
            sid.clock(n, scratch_.data());
        }
    };
    run(*c.sid);
    for (auto& t : c.taps)
        if (t)
            run(*t);
}

void SidBank::settle()
{
    for (int i = 0; i < cfg_.count; ++i)
        settleChip(chips_[i]);
}

void SidBank::createTaps(int i)
{
    auto& c = chips_[i];
    // The chip as it is now: registers, oscillators, envelopes and filter.
    std::vector<char> state(static_cast<size_t>(c.sid->stateSize()));
    c.sid->saveState(state.data());
    const auto& cc = cfg_.chips[i];
    for (int v = 0; v < kVoices; ++v) {
        auto t = makeSidChip(c.sid->engine());
        t->tune(cc, cc, false);
        setSampling(*t);
        t->reset();
        std::vector<char> copy = state; // restoreState wants a mutable buffer
        t->restoreState(copy.data(), static_cast<int>(copy.size()), cc);
        t->setVoiceMask(static_cast<unsigned char>(1u << v));
        c.taps[static_cast<size_t>(v)] = std::move(t);
        // As far behind as the chip's own output, so the voices line up with it.
        auto& f = c.voiceFifo[static_cast<size_t>(v)];
        f.clear();
        for (size_t k = c.fifo.available(); k > 0; --k)
            f.push(0);
    }
}

void SidBank::configure(const SidBankConfig& cfg, double clockHz, double sampleRate, uint64_t clk)
{
    const bool rateChanged = clockHz != clockHz_ || sampleRate != sampleRate_ ||
                             (chips_[0].sid && cfg.resampling != cfg_.resampling);
    const int oldCount = chips_[0].sid ? cfg_.count : 0;
    cfg_ = cfg;
    cfg_.count = std::clamp(cfg_.count, 1, kMaxChips);
    clockHz_ = clockHz;
    sampleRate_ = sampleRate;
    for (int i = 0; i < cfg_.count; ++i) {
        auto& c = chips_[i];
        bool fresh = false, switched = false;
        if (c.sid && c.sid->engine() != cfg_.engine) {
            switched = true;
            // A new engine: new chips, which start from the registers last
            // written (oscillators and envelopes start over).
            c.sid.reset();
            for (auto& t : c.taps)
                t.reset();
        }
        if (!c.sid) {
            c.sid = makeSidChip(cfg_.engine);
            fresh = true;
        }
        applyTuning(i, fresh);
        if (fresh || rateChanged) {
            setSampling(*c.sid);
            for (auto& t : c.taps)
                if (t)
                    setSampling(*t);
        }
        if (fresh || rateChanged || i >= oldCount) {
            // A chip that was not running starts at the machine's time.
            c.clk = clk;
            c.resetClk = clk;
            c.fifo.clear();
            for (auto& f : c.voiceFifo)
                f.clear();
        }
        if (fresh) {
            c.sid->reset();
            for (int r = 0; r < 25; ++r)
                c.sid->write(r, regs_[i][r]);
            // A chip that replaces one on the other engine picks up a running
            // machine (see settle()).
            if (switched)
                settleChip(c);
        }
        if (cfg_.voiceOutputs && !c.taps[0])
            createTaps(i);
        else if (!cfg_.voiceOutputs && c.taps[0])
            for (auto& t : c.taps)
                t.reset();
    }
    setPots(potX_, potY_);
}

void SidBank::reset(uint64_t clk)
{
    for (int i = 0; i < cfg_.count; ++i) {
        auto& c = chips_[i];
        catchUp(c, clk);
        c.sid->reset();
        for (auto& t : c.taps)
            if (t)
                t->reset();
        regs_[i].fill(0);
        c.written = false;
        c.resetClk = clk;
    }
}

int SidBank::chipAt(uint16_t addr) const
{
    const uint16_t slot = addr & 0xffe0;
    for (int i = 1; i < cfg_.count; ++i)
        if (cfg_.chips[i].address == slot)
            return i;
    if (addr >= 0xd400 && addr < 0xd800)
        return 0;
    return -1;
}

namespace {
// One second: long enough for the output's DC to settle through the C64's
// coupling filter (time constant about 0.1 s), so going silent is inaudible.
constexpr double kSettleSeconds = 1.0;
} // namespace

void SidBank::pushSilence(Chip& c, uint64_t cycles)
{
    const double exact = static_cast<double>(cycles) * sampleRate_ / clockHz_ + c.silentFrac;
    const auto n = static_cast<uint64_t>(exact);
    c.silentFrac = exact - static_cast<double>(n);
    const bool voices = c.taps[0] != nullptr;
    for (uint64_t k = 0; k < n; ++k) {
        c.fifo.push(0);
        if (voices)
            for (auto& f : c.voiceFifo)
                f.push(0);
    }
}

int SidBank::dormantCount(uint64_t clk) const
{
    int n = 0;
    for (int i = 0; i < cfg_.count; ++i) {
        const auto& c = chips_[i];
        if (!c.written && clk - c.resetClk > static_cast<uint64_t>(kSettleSeconds * clockHz_))
            ++n;
    }
    return n;
}

void SidBank::catchUp(Chip& c, uint64_t clk)
{
    if (clk <= c.clk)
        return;
    if (skipping_) {
        c.clk = clk;
        return;
    }
    if (!c.written) {
        const uint64_t settled = c.resetClk + static_cast<uint64_t>(kSettleSeconds * clockHz_);
        if (c.clk >= settled) {
            pushSilence(c, clk - c.clk);
            c.clk = clk;
            return;
        }
        if (clk > settled) {
            // Clock up to the moment it settles, then go quiet.
            catchUp(c, settled);
            pushSilence(c, clk - settled);
            c.clk = clk;
            return;
        }
    }
    uint64_t todo = clk - c.clk;
    c.clk = clk;
    // Chunks small enough for the scratch buffer at any host rate.
    while (todo) {
        const unsigned n = static_cast<unsigned>(std::min<uint64_t>(todo, 20000));
        todo -= n;
        const int got = c.sid->clock(n, scratch_.data());
        for (int k = 0; k < got; ++k)
            c.fifo.push(scratch_[k]);
        for (int v = 0; v < kVoices; ++v) {
            if (!c.taps[static_cast<size_t>(v)])
                continue;
            const int gv = c.taps[static_cast<size_t>(v)]->clock(n, scratch_.data());
            for (int k = 0; k < gv; ++k)
                c.voiceFifo[static_cast<size_t>(v)].push(scratch_[k]);
        }
    }
}

uint8_t SidBank::read(int chip, uint8_t reg, uint64_t clk)
{
    auto& c = chips_[chip];
    catchUp(c, clk);
    return c.sid->read(reg & 0x1f);
}

uint8_t SidBank::peek(int chip, uint8_t reg) const
{
    return chips_[chip].sid->peek(reg & 0x1f);
}

void SidBank::write(int chip, uint8_t reg, uint8_t v, uint64_t clk)
{
    auto& c = chips_[chip];
    catchUp(c, clk);
    c.written = true;
    regs_[chip][reg & 0x1f] = v;
    c.sid->write(reg & 0x1f, v);
    for (auto& t : c.taps)
        if (t)
            t->write(reg & 0x1f, v);
}

void SidBank::setPots(uint8_t x, uint8_t y)
{
    potX_ = x;
    potY_ = y;
    if (chips_[0].sid)
        chips_[0].sid->setPaddle(x, y);
}

void SidBank::sync(uint64_t clk)
{
    for (int i = 0; i < cfg_.count; ++i)
        catchUp(chips_[i], clk);
}

void SidBank::discard()
{
    for (auto& c : chips_) {
        c.fifo.rd = c.fifo.wr;
        for (auto& f : c.voiceFifo)
            f.rd = f.wr;
    }
}

std::vector<int16_t> SidBank::pending(int chip) const
{
    const auto& f = chips_[chip].fifo;
    std::vector<int16_t> out;
    for (size_t i = f.rd; i != f.wr; i = (i + 1) % kFifoSize)
        out.push_back(f.buf[i]);
    return out;
}

void SidBank::setPending(int chip, const std::vector<int16_t>& samples)
{
    auto& c = chips_[chip];
    c.fifo.clear();
    for (size_t i = 0; i < samples.size() && i + 1 < kFifoSize; ++i)
        c.fifo.push(samples[i]);
    // The voices' own queues are not in the state: silence, as long.
    for (auto& f : c.voiceFifo) {
        f.clear();
        if (c.taps[0])
            for (size_t i = 0; i < c.fifo.available(); ++i)
                f.push(0);
    }
}

std::vector<int16_t> SidBank::pendingVoice(int chip, int v) const
{
    const auto& f = chips_[chip].voiceFifo[static_cast<size_t>(v)];
    std::vector<int16_t> out;
    for (size_t i = f.rd; i != f.wr; i = (i + 1) % kFifoSize)
        out.push_back(f.buf[i]);
    return out;
}

void SidBank::setPendingVoice(int chip, int v, const std::vector<int16_t>& samples)
{
    auto& f = chips_[chip].voiceFifo[static_cast<size_t>(v)];
    f.clear();
    for (size_t i = 0; i < samples.size() && i + 1 < kFifoSize; ++i)
        f.push(samples[i]);
}

std::vector<char> SidBank::saveState() const
{
    std::vector<char> out;
    for (int i = 0; i < cfg_.count; ++i) {
        const auto& sid = *chips_[i].sid;
        const int n = sid.stateSize();
        const size_t at = out.size();
        out.resize(at + 4 + static_cast<size_t>(n));
        std::memcpy(out.data() + at, &n, 4);
        sid.saveState(out.data() + at + 4);
    }
    // The voice taps after all chips (older builds stop reading before them).
    for (int i = 0; i < cfg_.count; ++i)
        for (const auto& t : chips_[i].taps) {
            if (!t)
                continue;
            const int n = t->stateSize();
            const size_t at = out.size();
            out.resize(at + 4 + static_cast<size_t>(n));
            std::memcpy(out.data() + at, &n, 4);
            t->saveState(out.data() + at + 4);
        }
    return out;
}

bool SidBank::restoreState(const std::vector<char>& data, uint64_t clk)
{
    size_t at = 0;
    for (int i = 0; i < cfg_.count; ++i) {
        auto& c = chips_[i];
        int n = 0;
        if (at + 4 > data.size())
            return false;
        std::memcpy(&n, data.data() + at, 4);
        at += 4;
        if (n != c.sid->stateSize() || at + static_cast<size_t>(n) > data.size())
            return false;
        std::vector<char> buf(data.begin() + static_cast<long>(at), data.begin() + static_cast<long>(at + n));
        // Another engine's state (a project saved with the other one) does not
        // restore: the caller then starts the chips from their registers.
        if (!c.sid->restoreState(buf.data(), n, cfg_.chips[static_cast<size_t>(i)]))
            return false;
        at += static_cast<size_t>(n);
        c.clk = clk;
        c.fifo.clear();
        c.written = true; // nothing is known about its history: keep it running
    }
    // Voice taps: their own states when the project has them, else copies of
    // their chip (from a project saved without voice outputs).
    for (int i = 0; i < cfg_.count; ++i) {
        auto& c = chips_[i];
        if (!c.taps[0])
            continue;
        bool own = true;
        size_t probe = at;
        for (int v = 0; v < kVoices && own; ++v) {
            int n = 0;
            if (probe + 4 > data.size()) {
                own = false;
                break;
            }
            std::memcpy(&n, data.data() + probe, 4);
            own = n == c.taps[static_cast<size_t>(v)]->stateSize() && probe + 4 + static_cast<size_t>(n) <= data.size();
            probe += 4 + static_cast<size_t>(n);
        }
        if (!own) {
            for (auto& t : c.taps)
                t.reset();
            createTaps(i);
            continue;
        }
        const auto& cc = cfg_.chips[static_cast<size_t>(i)];
        for (int v = 0; v < kVoices; ++v) {
            auto& t = *c.taps[static_cast<size_t>(v)];
            int n = 0;
            std::memcpy(&n, data.data() + at, 4);
            at += 4;
            std::vector<char> buf(data.begin() + static_cast<long>(at), data.begin() + static_cast<long>(at + n));
            t.restoreState(buf.data(), n, cc);
            at += static_cast<size_t>(n);
            c.voiceFifo[static_cast<size_t>(v)].clear();
        }
    }
    return true;
}

} // namespace sidmidi::emu
