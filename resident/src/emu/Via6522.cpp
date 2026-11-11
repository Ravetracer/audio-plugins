#include "Via6522.h"

namespace resident::emu {

void Via6522::setCA1(bool level)
{
    if (level == s.ca1)
        return;
    const bool positive = s.pcr & 0x01;
    if (level == positive) {
        s.ifr |= CA1;
        s.paLatch = readPortA();
    }
    s.ca1 = level;
}

void Via6522::setCB1(bool level)
{
    if (level == s.cb1)
        return;
    const bool positive = s.pcr & 0x10;
    if (level == positive)
        s.ifr |= CB1;
    s.cb1 = level;
}

void Via6522::pulseCA1()
{
    s.ifr |= CA1;
    s.paLatch = readPortA();
}

uint8_t Via6522::peek(uint8_t reg) const
{
    switch (reg & 0x0f) {
    case 0x0: return static_cast<uint8_t>((s.orb & s.ddrb) | (readPortB() & ~s.ddrb));
    case 0x1:
    case 0xf: {
        const uint8_t pins = (s.acr & 0x01) ? s.paLatch : readPortA();
        return static_cast<uint8_t>((s.ora & s.ddra) | (pins & ~s.ddra));
    }
    case 0x2: return s.ddrb;
    case 0x3: return s.ddra;
    case 0x4: return static_cast<uint8_t>(s.t1);
    case 0x5: return static_cast<uint8_t>(s.t1 >> 8);
    case 0x6: return static_cast<uint8_t>(s.t1Latch);
    case 0x7: return static_cast<uint8_t>(s.t1Latch >> 8);
    case 0x8: return static_cast<uint8_t>(s.t2);
    case 0x9: return static_cast<uint8_t>(s.t2 >> 8);
    case 0xa: return s.sr;
    case 0xb: return s.acr;
    case 0xc: return s.pcr;
    case 0xd: return static_cast<uint8_t>((s.ifr & 0x7f) | (irq() ? 0x80 : 0));
    case 0xe: return static_cast<uint8_t>(s.ier | 0x80);
    }
    return 0xff;
}

uint8_t Via6522::read(uint8_t reg)
{
    reg &= 0x0f;
    const uint8_t v = peek(reg);
    switch (reg) {
    case 0x0: s.ifr &= static_cast<uint8_t>(~(CB1 | CB2)); break;
    case 0x1: s.ifr &= static_cast<uint8_t>(~(CA1 | CA2)); break;
    case 0x4: s.ifr &= static_cast<uint8_t>(~T1); break;
    case 0x8: s.ifr &= static_cast<uint8_t>(~T2); break;
    default: break;
    }
    return v;
}

void Via6522::write(uint8_t reg, uint8_t v)
{
    reg &= 0x0f;
    switch (reg) {
    case 0x0:
        s.orb = v;
        s.ifr &= static_cast<uint8_t>(~(CB1 | CB2));
        outputsChanged();
        break;
    case 0x1:
        s.ora = v;
        s.ifr &= static_cast<uint8_t>(~(CA1 | CA2));
        outputsChanged();
        break;
    case 0xf:
        s.ora = v;
        outputsChanged();
        break;
    case 0x2: s.ddrb = v; outputsChanged(); break;
    case 0x3: s.ddra = v; outputsChanged(); break;
    case 0x4:
    case 0x6: s.t1Latch = static_cast<uint16_t>((s.t1Latch & 0xff00) | v); break;
    case 0x5:
        s.t1Latch = static_cast<uint16_t>((s.t1Latch & 0x00ff) | (v << 8));
        s.t1 = s.t1Latch;
        s.t1Reload = false;
        s.t1Armed = true;
        s.ifr &= static_cast<uint8_t>(~T1);
        break;
    case 0x7:
        s.t1Latch = static_cast<uint16_t>((s.t1Latch & 0x00ff) | (v << 8));
        s.ifr &= static_cast<uint8_t>(~T1);
        break;
    case 0x8: s.t2LatchLo = v; break;
    case 0x9:
        s.t2 = static_cast<uint16_t>((v << 8) | s.t2LatchLo);
        s.t2Armed = true;
        s.ifr &= static_cast<uint8_t>(~T2);
        break;
    case 0xa: s.sr = v; break;
    case 0xb: s.acr = v; break;
    case 0xc: s.pcr = v; outputsChanged(); break;
    case 0xd: s.ifr &= static_cast<uint8_t>(~(v & 0x7f)); break;
    case 0xe:
        if (v & 0x80)
            s.ier |= v & 0x7f;
        else
            s.ier &= static_cast<uint8_t>(~(v & 0x7f));
        break;
    }
}

} // namespace resident::emu
