#include "Pic12F509.h"

#include <cstdlib>
#include <sstream>

namespace sidmidi::emu {

namespace {

// The watchdog's nominal period with the prescaler at 1:1.
constexpr uint32_t kWdtCycles = 18000;

} // namespace

bool Pic12F509::loadHex(const std::string& hex)
{
    rom_.fill(0x0fff); // erased flash reads as XORLW 0xFF
    std::istringstream in(hex);
    std::string line;
    uint32_t base = 0;
    bool any = false;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != ':')
            continue;
        auto byteAt = [&](size_t i) { return static_cast<uint32_t>(std::strtoul(line.substr(i, 2).c_str(), nullptr, 16)); };
        const uint32_t n = byteAt(1);
        const uint32_t a = (byteAt(3) << 8) | byteAt(5);
        const uint32_t type = byteAt(7);
        if (type == 1)
            break;
        if (type == 4) {
            base = ((byteAt(9) << 8) | byteAt(11)) << 16;
            continue;
        }
        if (type != 0)
            continue;
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            const uint32_t byteAddr = base + a + i;
            const uint32_t word = (byteAt(9 + 2 * i) | (byteAt(11 + 2 * i) << 8)) & 0x0fff;
            const uint32_t wordAddr = byteAddr / 2;
            if (wordAddr < rom_.size()) {
                rom_[wordAddr] = static_cast<uint16_t>(word);
                any = true;
            }
        }
    }
    loaded_ = any;
    return any;
}

void Pic12F509::reset()
{
    const uint64_t clk = s.clk;
    const auto ram = s.ram;
    const bool wdtReset = s.wdt >= kWdtCycles;
    s = PicState{};
    s.ram = ram; // RAM keeps its contents through a reset
    s.clk = clk;
    // The reset vector is the last word (MOVLW <oscillator calibration>),
    // from where the program counter rolls over to 0.
    s.w = 0x70;
    s.pc = 0;
    s.status = static_cast<uint8_t>(wdtReset ? 0x08 : 0x18);
}

uint8_t Pic12F509::gpio() const
{
    const uint8_t pins = readPins() & 0x3f;
    return static_cast<uint8_t>(((s.tris & pins) | (~s.tris & s.latch)) & 0x3f);
}

int Pic12F509::addr(uint8_t f) const
{
    f &= 0x1f;
    if (f == 0) {
        const int a = s.fsr & 0x3f;
        return (a & 0x1f) < 0x10 ? (a & 0x0f) : a;
    }
    return f < 0x10 ? f : ((s.fsr & 0x20) | f);
}

uint8_t Pic12F509::rd(uint8_t f) const
{
    const int a = addr(f);
    switch (a) {
    case 0: return 0; // INDF pointing at itself
    case 1: return s.tmr0;
    case 2: return static_cast<uint8_t>(s.pc);
    case 3: return s.status;
    case 4: return static_cast<uint8_t>(s.fsr | 0xc0);
    case 5: return s.osccal;
    case 6: return gpio();
    default: return s.ram[static_cast<size_t>(a)];
    }
}

void Pic12F509::wr(uint8_t f, uint8_t v)
{
    const int a = addr(f);
    switch (a) {
    case 0: break;
    case 1: s.tmr0 = v; break;
    case 2:
        s.pc = static_cast<uint16_t>(((s.status & 0x20) << 4) | v);
        pcWritten_ = true;
        break;
    case 3: s.status = static_cast<uint8_t>((s.status & 0x18) | (v & 0xe7)); break;
    case 4: s.fsr = v; break;
    case 5: s.osccal = v; break;
    case 6: s.latch = v & 0x3f; break;
    default: s.ram[static_cast<size_t>(a)] = v; break;
    }
}

int Pic12F509::step()
{
    const uint16_t op = rom_[s.pc & 0x3ff];
    s.pc = static_cast<uint16_t>((s.pc + 1) & 0x3ff);
    pcWritten_ = false;
    int cycles = 1;
    const uint8_t f = op & 0x1f;
    const bool toF = op & 0x20;
    auto store = [&](uint8_t v) {
        if (toF)
            wr(f, v);
        else
            s.w = v;
    };
    auto skip = [&] {
        s.pc = static_cast<uint16_t>((s.pc + 1) & 0x3ff);
        cycles = 2;
    };

    switch (op >> 8) {
    case 0x0: {
        const uint8_t sub = (op >> 6) & 0x3;
        if (op == 0x000) { // NOP
        } else if (op == 0x002) { // OPTION
            s.option = s.w;
        } else if (op == 0x003) { // SLEEP: not used by HerMIDI; treat as a wait
            s.pc = static_cast<uint16_t>((s.pc - 1) & 0x3ff);
        } else if (op == 0x004) { // CLRWDT
            s.wdt = 0;
            s.status |= 0x18;
        } else if ((op & 0xff8) == 0x000) { // TRIS f (6 = GPIO)
            if ((op & 7) == 6)
                s.tris = s.w & 0x3f;
        } else if (sub == 0 && toF) { // MOVWF
            wr(f, s.w);
        } else if (sub == 1) { // CLRW / CLRF
            store(0);
            setZ(true);
        } else if (sub == 2) { // SUBWF
            const uint8_t a = rd(f);
            const int r = a - s.w;
            setC(r >= 0);
            setDC((a & 0x0f) >= (s.w & 0x0f));
            setZ(static_cast<uint8_t>(r) == 0);
            store(static_cast<uint8_t>(r));
        } else if (sub == 3) { // DECF
            const uint8_t r = static_cast<uint8_t>(rd(f) - 1);
            setZ(r == 0);
            store(r);
        }
        break;
    }
    case 0x1: {
        const uint8_t sub = (op >> 6) & 0x3;
        const uint8_t a = rd(f);
        uint8_t r;
        if (sub == 0)
            r = a | s.w; // IORWF
        else if (sub == 1)
            r = a & s.w; // ANDWF
        else if (sub == 2)
            r = a ^ s.w; // XORWF
        else { // ADDWF
            const int sum = a + s.w;
            setC(sum > 0xff);
            setDC(((a & 0x0f) + (s.w & 0x0f)) > 0x0f);
            r = static_cast<uint8_t>(sum);
        }
        setZ(r == 0);
        store(r);
        break;
    }
    case 0x2: {
        const uint8_t sub = (op >> 6) & 0x3;
        const uint8_t a = rd(f);
        if (sub == 0) { // MOVF
            setZ(a == 0);
            store(a);
        } else if (sub == 1) { // COMF
            const uint8_t r = static_cast<uint8_t>(~a);
            setZ(r == 0);
            store(r);
        } else if (sub == 2) { // INCF
            const uint8_t r = static_cast<uint8_t>(a + 1);
            setZ(r == 0);
            store(r);
        } else { // DECFSZ
            const uint8_t r = static_cast<uint8_t>(a - 1);
            store(r);
            if (r == 0)
                skip();
        }
        break;
    }
    case 0x3: {
        const uint8_t sub = (op >> 6) & 0x3;
        const uint8_t a = rd(f);
        const uint8_t c = s.status & 0x01;
        if (sub == 0) { // RRF
            setC(a & 1);
            store(static_cast<uint8_t>((a >> 1) | (c << 7)));
        } else if (sub == 1) { // RLF
            setC(a & 0x80);
            store(static_cast<uint8_t>((a << 1) | c));
        } else if (sub == 2) { // SWAPF
            store(static_cast<uint8_t>((a << 4) | (a >> 4)));
        } else { // INCFSZ
            const uint8_t r = static_cast<uint8_t>(a + 1);
            store(r);
            if (r == 0)
                skip();
        }
        break;
    }
    case 0x4: case 0x5: case 0x6: case 0x7: {
        const uint8_t bit = static_cast<uint8_t>(1 << ((op >> 5) & 7));
        const uint8_t a = rd(f);
        switch ((op >> 8) & 3) {
        case 0: wr(f, static_cast<uint8_t>(a & ~bit)); break; // BCF
        case 1: wr(f, static_cast<uint8_t>(a | bit)); break;  // BSF
        case 2: if (!(a & bit)) skip(); break;                // BTFSC
        case 3: if (a & bit) skip(); break;                   // BTFSS
        }
        break;
    }
    case 0x8: // RETLW
        s.w = static_cast<uint8_t>(op);
        s.pc = s.stack[0];
        s.stack[0] = s.stack[1];
        cycles = 2;
        break;
    case 0x9: // CALL (into the first half of the page)
        s.stack[1] = s.stack[0];
        s.stack[0] = s.pc;
        s.pc = static_cast<uint16_t>(((s.status & 0x20) << 4) | (op & 0xff));
        cycles = 2;
        break;
    case 0xa: case 0xb: // GOTO
        s.pc = static_cast<uint16_t>(((s.status & 0x20) << 4) | (op & 0x1ff));
        cycles = 2;
        break;
    case 0xc: s.w = static_cast<uint8_t>(op); break; // MOVLW
    case 0xd: s.w |= static_cast<uint8_t>(op); setZ(s.w == 0); break;
    case 0xe: s.w &= static_cast<uint8_t>(op); setZ(s.w == 0); break;
    case 0xf: s.w ^= static_cast<uint8_t>(op); setZ(s.w == 0); break;
    }
    if (pcWritten_)
        cycles = 2;

    s.clk += static_cast<uint64_t>(cycles);
    if (!(s.option & 0x20))
        s.tmr0 = static_cast<uint8_t>(s.tmr0 + cycles);
    s.wdt += static_cast<uint32_t>(cycles);
    if (s.wdt >= kWdtCycles)
        reset();
    return cycles;
}

} // namespace sidmidi::emu
