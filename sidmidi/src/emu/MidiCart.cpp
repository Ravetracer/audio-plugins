#include "MidiCart.h"

namespace sidmidi::emu {

namespace {

constexpr uint8_t RDRF = 0x01, TDRE = 0x02, OVRN = 0x20, IRQ = 0x80;

} // namespace

const char* midiInterfaceName(MidiInterface m)
{
    switch (m) {
    case MidiInterface::None: return "None";
    case MidiInterface::Datel: return "Datel / Kerberos";
    case MidiInterface::Sequential: return "Sequential";
    case MidiInterface::Passport: return "Passport / Syntech";
    case MidiInterface::Namesoft: return "Namesoft";
    case MidiInterface::Maplin: return "Maplin";
    case MidiInterface::MoogSP: return "Moog Song Producer";
    case MidiInterface::HerMIDI: return "HerMIDI (serial port)";
    default: return "?";
    }
}

void MidiCart::reset()
{
    s = MidiCartState{};
    flush();
}

void MidiCart::flush()
{
    rx_.clear();
    tx_.clear();
    s.txShifting = false;
    s.txFull = false;
}

uint16_t MidiCart::base() const
{
    switch (type) {
    case MidiInterface::Datel: return 0xde04;
    case MidiInterface::Sequential: return 0xde00;
    case MidiInterface::Passport: return 0xde08;
    case MidiInterface::Namesoft: return 0xde00;
    case MidiInterface::Maplin: return 0xdf00;
    case MidiInterface::MoogSP: return 0xde00;
    default: return 0;
    }
}

// Sequential, Datel and Namesoft decode four addresses (control and TX on
// write at +0/+1, status and RX on read at +2/+3); the others two.
bool MidiCart::fourRegisters() const
{
    return type == MidiInterface::Datel || type == MidiInterface::Sequential || type == MidiInterface::Namesoft;
}

bool MidiCart::decodes(uint16_t addr) const
{
    if (type == MidiInterface::None || type == MidiInterface::HerMIDI)
        return false;
    const uint16_t b = base();
    return addr >= b && addr < b + (fourRegisters() ? 4 : 2);
}

bool MidiCart::irqAsserted() const
{
    if (s.inReset)
        return false;
    const bool rie = s.cr & 0x80;
    const bool tie = (s.cr & 0x60) == 0x20;
    return (rie && (s.sr & (RDRF | OVRN))) || (tie && (s.sr & TDRE));
}

void MidiCart::receive(uint8_t byte, uint64_t clk)
{
    uint64_t done = clk + cyclesPerFrame;
    if (!rx_.empty() && rx_.back().clk + cyclesPerFrame > done)
        done = rx_.back().clk + cyclesPerFrame;
    if (s.rxLineFreeClk > done)
        done = s.rxLineFreeClk;
    rx_.push_back({done, byte});
}

void MidiCart::service(uint64_t clk)
{
    while (!rx_.empty() && clk >= rx_.front().clk) {
        const MidiByte b = rx_.front();
        rx_.pop_front();
        s.rxLineFreeClk = b.clk;
        if (s.inReset)
            continue; // a held reset ignores the line
        if (s.sr & RDRF) {
            s.sr |= OVRN; // the previous byte is kept, this one is lost
        } else {
            s.rdr = b.byte;
            s.sr |= RDRF;
        }
    }
    if (s.txShifting && clk >= s.txDoneClk) {
        tx_.push_back({s.txDoneClk, s.txShift});
        s.txShifting = false;
        if (s.txFull) {
            s.txFull = false;
            s.txShift = s.tdr;
            s.txShifting = true;
            s.txDoneClk = clk + cyclesPerFrame;
            s.sr |= TDRE;
        }
    }
}

uint8_t MidiCart::peek(uint16_t addr) const
{
    const uint16_t off = static_cast<uint16_t>(addr - base());
    const bool data = off & 1;
    if (!data) {
        uint8_t v = s.sr & ~IRQ;
        if (irqAsserted())
            v |= IRQ;
        return v;
    }
    return s.rdr;
}

uint8_t MidiCart::read(uint16_t addr, uint64_t clk)
{
    service(clk);
    const uint16_t off = static_cast<uint16_t>(addr - base());
    if (off & 1) {
        const uint8_t v = s.rdr;
        s.sr &= ~(RDRF | OVRN);
        return v;
    }
    return peek(addr);
}

void MidiCart::write(uint16_t addr, uint8_t v, uint64_t clk)
{
    service(clk);
    const uint16_t off = static_cast<uint16_t>(addr - base());
    if (!(off & 1)) {
        s.cr = v;
        if ((v & 0x03) == 0x03) {
            // Master reset: status cleared, nothing in flight.
            s.inReset = true;
            s.sr = 0;
            s.txFull = false;
            s.txShifting = false;
        } else if (s.inReset) {
            s.inReset = false;
            s.sr = TDRE;
        }
        return;
    }
    if (s.inReset)
        return;
    s.tdr = v;
    if (!s.txShifting) {
        s.txShift = v;
        s.txShifting = true;
        s.txDoneClk = clk + cyclesPerFrame;
        s.sr |= TDRE; // the TDR empties into the shifter at once
    } else {
        s.txFull = true;
        s.sr &= ~TDRE;
    }
}

void MidiCart::takeTransmitted(std::vector<MidiByte>& out)
{
    for (size_t i = 0; i < tx_.size() && out.size() < out.capacity(); ++i)
        out.push_back(tx_[i]);
    tx_.clear();
}

} // namespace sidmidi::emu
