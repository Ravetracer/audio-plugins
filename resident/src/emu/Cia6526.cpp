#include "Cia6526.h"

#include <cstring>

namespace resident::emu {

namespace {

uint8_t bcdInc(uint8_t v)
{
    v++;
    if ((v & 0x0f) > 9)
        v = static_cast<uint8_t>((v & 0xf0) + 0x10);
    return v;
}

} // namespace

void Cia6526::reset()
{
    const auto keepModel = newModel;
    s = CiaState{};
    newModel = keepModel;
    portsChanged();
}

uint8_t Cia6526::portBOut() const
{
    uint8_t v = static_cast<uint8_t>(s.prb | ~s.ddrb);
    // A timer with its PB-output bit set drives PB6 (A) or PB7 (B).
    if (s.cra & 0x02) {
        const bool on = (s.cra & 0x04) ? s.pb6 : s.pulseA;
        v = static_cast<uint8_t>((v & ~0x40) | (on ? 0x40 : 0));
    }
    if (s.crb & 0x02) {
        const bool on = (s.crb & 0x04) ? s.pb7 : s.pulseB;
        v = static_cast<uint8_t>((v & ~0x80) | (on ? 0x80 : 0));
    }
    return v;
}

void Cia6526::setIcr(uint8_t bits)
{
    s.icrData |= bits;
    if ((s.icrData & s.icrMask & 0x1f) && !(s.icrData & 0x80)) {
        s.icrData |= 0x80;
        if (newModel)
            s.irq = true;
        else
            s.irqDelay = 1;
    }
}

void Cia6526::underflowA()
{
    s.ta = s.latchA;
    s.pulseA = true;
    s.pb6 = !s.pb6;
    setIcr(TA);
    if ((s.cra & 0x08) || (s.oneShotA & 1)) {
        s.cra &= ~0x01;
        s.countA = 0;
    }
    // The serial port shifts on every second underflow of timer A in output mode.
    if ((s.cra & 0x40) && (s.sdrBits || s.sdrPending)) {
        s.sdrToggle = !s.sdrToggle;
        if (!s.sdrToggle) {
            if (s.sdrBits == 0 && s.sdrPending) {
                s.sdrShift = s.sdr;
                s.sdrBits = 8;
                s.sdrPending = false;
            }
            if (s.sdrBits && --s.sdrBits == 0) {
                setIcr(SP);
                if (s.sdrPending) {
                    s.sdrShift = s.sdr;
                    s.sdrBits = 8;
                    s.sdrPending = false;
                }
            }
        }
    }
    // Timer B in "count timer A underflows" mode.
    if ((s.crb & 0x61) == 0x41 || (s.crb & 0x61) == 0x61) {
        if (s.tb == 0)
            underflowB();
        else
            s.tb--;
    }
}

void Cia6526::underflowB()
{
    s.tb = s.latchB;
    s.pulseB = true;
    s.pb7 = !s.pb7;
    setIcr(TB);
    if ((s.crb & 0x08) || (s.oneShotB & 1)) {
        s.crb &= ~0x01;
        s.countB = 0;
    }
}

void Cia6526::tick()
{
    // Both timers stopped with nothing in their pipelines: only the clock runs.
    if (!((s.cra | s.crb) & 0x01) && !(s.countA | s.countB | s.loadA | s.loadB | s.irqDelay) && !s.pulseA &&
        !s.pulseB) {
        if (++s.todCycles >= cyclesPerPowerTick) {
            s.todCycles = 0;
            tickTod();
        }
        return;
    }
    if (s.irqDelay) {
        if (--s.irqDelay == 0)
            s.irq = true;
    }
    s.pulseA = s.pulseB = false;

    // Timer A, counting system cycles (CNT input is idle high on the C64).
    const bool runA = (s.cra & 0x21) == 0x01;
    s.countA = static_cast<uint8_t>((s.countA >> 1) | (runA ? 0x04 : 0));
    if (s.loadA & 1)
        s.ta = s.latchA;
    else if ((s.countA & 1) && runA) {
        if (s.ta == 0)
            underflowA();
        else
            s.ta--;
    }
    s.loadA >>= 1;
    s.oneShotA >>= 1;

    // Timer B counting system cycles; the timer-A-chained modes count in underflowA().
    const bool runB = (s.crb & 0x61) == 0x01;
    s.countB = static_cast<uint8_t>((s.countB >> 1) | (runB ? 0x04 : 0));
    if (s.loadB & 1)
        s.tb = s.latchB;
    else if ((s.countB & 1) && runB) {
        if (s.tb == 0)
            underflowB();
        else
            s.tb--;
    }
    s.loadB >>= 1;
    s.oneShotB >>= 1;

    if (++s.todCycles >= cyclesPerPowerTick) {
        s.todCycles = 0;
        tickTod();
    }
}

bool Cia6526::todMatchesAlarm() const
{
    return std::memcmp(s.tod, s.alarm, 4) == 0;
}

void Cia6526::tickTod()
{
    if (s.todStopped)
        return;
    const uint8_t div = (s.cra & 0x80) ? 5 : 6;
    if (++s.todTicks < div)
        return;
    s.todTicks = 0;
    // Tenths 0-9, seconds and minutes 00-59, hours 1-12 with PM in bit 7.
    s.tod[0] = static_cast<uint8_t>((s.tod[0] + 1) & 0x0f);
    if (s.tod[0] == 10) {
        s.tod[0] = 0;
        s.tod[1] = bcdInc(s.tod[1]);
        if (s.tod[1] == 0x60) {
            s.tod[1] = 0;
            s.tod[2] = bcdInc(s.tod[2]);
            if (s.tod[2] == 0x60) {
                s.tod[2] = 0;
                uint8_t h = s.tod[3] & 0x1f;
                uint8_t pm = s.tod[3] & 0x80;
                if (h == 0x11)
                    pm ^= 0x80;
                if (h == 0x12)
                    h = 0x01;
                else
                    h = bcdInc(h);
                s.tod[3] = static_cast<uint8_t>(h | pm);
            }
        }
    }
    if (todMatchesAlarm())
        setIcr(ALRM);
}

uint8_t Cia6526::peek(uint8_t reg) const
{
    switch (reg & 0x0f) {
    case 0x0: return static_cast<uint8_t>((s.pra | ~s.ddra) & readPortA());
    case 0x1: return static_cast<uint8_t>(portBOut() & readPortB());
    case 0x2: return s.ddra;
    case 0x3: return s.ddrb;
    case 0x4: return static_cast<uint8_t>(s.ta);
    case 0x5: return static_cast<uint8_t>(s.ta >> 8);
    case 0x6: return static_cast<uint8_t>(s.tb);
    case 0x7: return static_cast<uint8_t>(s.tb >> 8);
    case 0x8: return s.latched ? s.latch[0] : s.tod[0];
    case 0x9: return s.latched ? s.latch[1] : s.tod[1];
    case 0xa: return s.latched ? s.latch[2] : s.tod[2];
    case 0xb: return s.latched ? s.latch[3] : s.tod[3];
    case 0xc: return s.sdr;
    case 0xd: return s.icrData;
    case 0xe: return static_cast<uint8_t>(s.cra & ~0x10);
    case 0xf: return static_cast<uint8_t>(s.crb & ~0x10);
    }
    return 0xff;
}

uint8_t Cia6526::read(uint8_t reg)
{
    reg &= 0x0f;
    switch (reg) {
    case 0x8: {
        const uint8_t v = s.latched ? s.latch[0] : s.tod[0];
        s.latched = false;
        return v;
    }
    case 0xb:
        // Reading the hours freezes the visible time until the tenths are read.
        if (!s.latched) {
            std::memcpy(s.latch, s.tod, 4);
            s.latched = true;
        }
        return s.latch[3];
    case 0xd: {
        const uint8_t v = s.icrData;
        s.icrData = 0;
        s.irq = false;
        s.irqDelay = 0;
        return v;
    }
    default:
        return peek(reg);
    }
}

void Cia6526::write(uint8_t reg, uint8_t v)
{
    reg &= 0x0f;
    switch (reg) {
    case 0x0: s.pra = v; portsChanged(); break;
    case 0x1: s.prb = v; portsChanged(); break;
    case 0x2: s.ddra = v; portsChanged(); break;
    case 0x3: s.ddrb = v; portsChanged(); break;
    case 0x4: s.latchA = static_cast<uint16_t>((s.latchA & 0xff00) | v); break;
    case 0x5:
        s.latchA = static_cast<uint16_t>((s.latchA & 0x00ff) | (v << 8));
        if (!(s.cra & 0x01))
            s.loadA |= 0x01 << 1;
        break;
    case 0x6: s.latchB = static_cast<uint16_t>((s.latchB & 0xff00) | v); break;
    case 0x7:
        s.latchB = static_cast<uint16_t>((s.latchB & 0x00ff) | (v << 8));
        if (!(s.crb & 0x01))
            s.loadB |= 0x01 << 1;
        break;
    case 0x8: case 0x9: case 0xa: case 0xb: {
        uint8_t* dst = (s.crb & 0x80) ? s.alarm : s.tod;
        const int i = reg - 0x8;
        static constexpr uint8_t masks[4] = {0x0f, 0x7f, 0x7f, 0x9f};
        uint8_t val = static_cast<uint8_t>(v & masks[i]);
        if (i == 3 && !(s.crb & 0x80)) {
            // Writing 12 into the hours flips AM/PM on the real chip.
            if ((val & 0x1f) == 0x12)
                val ^= 0x80;
            s.todStopped = true;
        }
        dst[i] = val;
        if (i == 0 && !(s.crb & 0x80)) {
            s.todStopped = false;
            s.todTicks = 0;
        }
        if (todMatchesAlarm())
            setIcr(ALRM);
        break;
    }
    case 0xc:
        s.sdr = v;
        if (s.cra & 0x40)
            s.sdrPending = true;
        break;
    case 0xd:
        if (v & 0x80)
            s.icrMask |= v & 0x1f;
        else
            s.icrMask &= ~(v & 0x1f);
        if ((s.icrData & s.icrMask & 0x1f) && !(s.icrData & 0x80))
            setIcr(0);
        break;
    case 0xe:
        if ((v & 0x01) && !(s.cra & 0x01))
            s.pb6 = true; // the toggle output starts high
        if (v & 0x10)
            s.loadA |= 0x01 << 1;
        if (v & 0x08)
            s.oneShotA = 0xff;
        if ((v ^ s.cra) & 0x40) {
            s.sdrBits = 0;
            s.sdrPending = false;
            s.sdrToggle = false;
        }
        // A newly started timer counts from the third cycle after the write.
        if ((v & 0x01) && !(s.cra & 0x01))
            s.countA &= 0x03;
        s.cra = static_cast<uint8_t>(v & ~0x10);
        portsChanged();
        break;
    case 0xf:
        if ((v & 0x01) && !(s.crb & 0x01))
            s.pb7 = true;
        if (v & 0x10)
            s.loadB |= 0x01 << 1;
        if (v & 0x08)
            s.oneShotB = 0xff;
        if ((v & 0x01) && !(s.crb & 0x01))
            s.countB &= 0x03;
        s.crb = static_cast<uint8_t>(v & ~0x10);
        portsChanged();
        break;
    }
}

} // namespace resident::emu
