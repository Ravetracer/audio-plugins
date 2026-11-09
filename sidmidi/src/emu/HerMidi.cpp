#include "HerMidi.h"

#include "HerMidiFirmware.h"

namespace sidmidi::emu {

namespace {
constexpr uint64_t kBitCycles = 32; // 31250 baud at the PIC's 1 MHz
} // namespace

HerMidi::HerMidi()
{
    pic.loadHex(kHerMidiFirmwareHex);
    pic.readPins = [this] {
        uint8_t v = 0x01 | 0x02; // GP0 pulled up; GP1 (LED) reads its latch when driven
        if (!lineData)
            v |= 0x04;
        if (midiLevel(pic.s.clk))
            v |= 0x08;
        if (!lineAtn)
            v |= 0x10;
        if (!lineClk)
            v |= 0x20;
        return v;
    };
}

void HerMidi::reset()
{
    pic.s = PicState{};
    pic.reset();
    st = HerMidiState{};
    queue_.clear();
}

void HerMidi::receive(uint8_t byte, uint64_t at)
{
    queue_.push_back({at, byte});
}

// The cable: idle high, then per byte a low start bit, eight data bits (LSB
// first) and a high stop bit.
bool HerMidi::midiLevel(uint64_t t)
{
    for (;;) {
        if (st.inFrame) {
            const uint64_t bit = (t - st.frameStart) / kBitCycles;
            if (bit == 0)
                return false;
            if (bit <= 8)
                return (st.frameByte >> (bit - 1)) & 1;
            if (bit == 9)
                return true;
            // Frame over: the next byte may follow at once.
            st.inFrame = false;
            const uint64_t end = st.frameStart + 10 * kBitCycles;
            if (!queue_.empty() && queue_.front().at < end)
                queue_.front().at = end;
        }
        if (queue_.empty() || t < queue_.front().at)
            return true;
        st.inFrame = true;
        st.frameStart = queue_.front().at;
        st.frameByte = queue_.front().byte;
        queue_.pop_front();
    }
}

void HerMidi::runUntil(uint64_t picCycle)
{
    if (!pic.hasProgram()) {
        pic.s.clk = picCycle;
        return;
    }
    while (pic.s.clk < picCycle)
        pic.step();
}

} // namespace sidmidi::emu
