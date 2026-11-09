#pragma once

// The NMOS 6502/6510 core, shared by the C64 and the 1541.
//
// Instruction-stepped, but every bus access is exactly one cycle and the
// sequence of accesses -- dummy reads and the double write of a
// read-modify-write included -- is the real chip's. The bus advances time in
// read() and write(), so a device sees each access in the cycle it happens;
// `INC $D019` acknowledges the VIC with its first write, `LDA $DC0D,X` across a
// page reads the CIA twice.
//
// Interrupts are sampled after every cycle; the decision at the end of an
// instruction uses the sample of its penultimate cycle, which is what makes
// CLI/SEI/PLP and RTI behave as they do on the chip. A taken branch that does
// not cross a page polls before its last cycle.
//
// The Bus must provide:
//   uint8_t read(uint16_t addr);           one cycle
//   void    write(uint16_t addr, uint8_t);  one cycle
//   bool    irqLine() const;                level, active high
//   bool    nmiLine() const;                level, active high (edge detected here)
//   bool    trap(uint16_t pc);              optional hook before an opcode fetch;
//                                           true when it changed the CPU state

#include <cstdint>

namespace sidmidi::emu {

struct CpuState
{
    uint16_t pc = 0;
    uint8_t a = 0, x = 0, y = 0, sp = 0xfd, p = 0x24;
    bool nmiLast = false;   // NMI line level seen last cycle
    bool nmiLatch = false;  // edge seen, not yet serviced
    bool intPrev = false, intCur = false; // IRQ samples (penultimate, last cycle)
    bool nmiPrev = false, nmiCur = false; // NMI latch samples
    bool jammed = false;
};

template <class Bus>
class Cpu6502
{
public:
    enum : uint8_t { C = 0x01, Z = 0x02, I = 0x04, D = 0x08, B = 0x10, U = 0x20, V = 0x40, N = 0x80 };

    explicit Cpu6502(Bus& bus) : bus_(bus) {}

    CpuState s;

    // The reset sequence: three dummy stack reads, then the vector.
    void reset()
    {
        s.jammed = false;
        rd(s.pc);
        rd(s.pc);
        rd(0x100 | s.sp);
        s.sp--;
        rd(0x100 | s.sp);
        s.sp--;
        rd(0x100 | s.sp);
        s.sp--;
        s.p |= I | U;
        const uint8_t lo = rd(0xfffc);
        const uint8_t hi = rd(0xfffd);
        s.pc = static_cast<uint16_t>(lo | (hi << 8));
        s.intPrev = s.intCur = false;
        s.nmiPrev = s.nmiCur = false;
        s.nmiLatch = false;
    }

    // Executes one instruction, or one interrupt sequence.
    void step()
    {
        if (s.jammed) {
            rd(0xffff);
            return;
        }
        if (s.nmiPrev) {
            interrupt(0xfffa, false);
            return;
        }
        if (s.intPrev) {
            interrupt(0xfffe, false);
            return;
        }
        if (bus_.trap(s.pc))
            return;
        execute(fetch());
    }

    uint8_t flags() const { return s.p | U; }

private:
    Bus& bus_;

    // ------------------------------------------------------------- cycles
    void sample()
    {
        const bool nmi = bus_.nmiLine();
        if (nmi && !s.nmiLast)
            s.nmiLatch = true;
        s.nmiLast = nmi;
        s.nmiPrev = s.nmiCur;
        s.nmiCur = s.nmiLatch;
        s.intPrev = s.intCur;
        s.intCur = bus_.irqLine() && !(s.p & I);
    }
    uint8_t rd(uint16_t a)
    {
        const uint8_t v = bus_.read(a);
        sample();
        return v;
    }
    void wr(uint16_t a, uint8_t v)
    {
        bus_.write(a, v);
        sample();
    }
    uint8_t fetch() { return rd(s.pc++); }
    void push(uint8_t v)
    {
        wr(0x100 | s.sp, v);
        s.sp--;
    }
    uint8_t pull()
    {
        s.sp++;
        return rd(0x100 | s.sp);
    }

    // BRK, IRQ and NMI. An NMI that arrives before the vector is fetched
    // takes the vector over (it "hijacks" an IRQ or a BRK).
    void interrupt(uint16_t vector, bool brk)
    {
        if (!brk) {
            rd(s.pc);
            rd(s.pc);
        }
        push(static_cast<uint8_t>(s.pc >> 8));
        push(static_cast<uint8_t>(s.pc));
        // The NMI latch is looked at here, while P is pushed.
        const bool nmi = s.nmiLatch;
        push(static_cast<uint8_t>((s.p | U | (brk ? B : 0)) & ~(brk ? 0 : B)));
        s.p |= I;
        if (nmi) {
            vector = 0xfffa;
            s.nmiLatch = false;
        }
        const uint8_t lo = rd(vector);
        const uint8_t hi = rd(vector + 1);
        s.pc = static_cast<uint16_t>(lo | (hi << 8));
        // The first instruction of a handler always runs.
        s.intPrev = s.intCur = false;
        s.nmiPrev = false;
        s.nmiCur = s.nmiLatch;
    }

    // ------------------------------------------------------------- flags
    void nz(uint8_t v)
    {
        s.p = static_cast<uint8_t>((s.p & ~(N | Z)) | (v & N) | (v ? 0 : Z));
    }
    void setFlag(uint8_t f, bool on) { s.p = on ? (s.p | f) : (s.p & ~f); }

    // ------------------------------------------------------------- addressing
    // Each returns the effective address, having done exactly the bus
    // accesses the chip does before the operand access.
    uint16_t aZp() { return fetch(); }
    uint16_t aZpX()
    {
        const uint8_t b = fetch();
        rd(b);
        return static_cast<uint8_t>(b + s.x);
    }
    uint16_t aZpY()
    {
        const uint8_t b = fetch();
        rd(b);
        return static_cast<uint8_t>(b + s.y);
    }
    uint16_t aAbs()
    {
        const uint8_t lo = fetch();
        const uint8_t hi = fetch();
        return static_cast<uint16_t>(lo | (hi << 8));
    }
    // Indexed absolute for reads: the dummy read only on a page cross.
    uint16_t aAbsIdxR(uint8_t idx)
    {
        const uint8_t lo = fetch();
        const uint8_t hi = fetch();
        const uint16_t base = static_cast<uint16_t>(lo | (hi << 8));
        const uint16_t ea = static_cast<uint16_t>(base + idx);
        if ((base ^ ea) & 0xff00)
            rd(static_cast<uint16_t>((base & 0xff00) | (ea & 0x00ff)));
        return ea;
    }
    // Indexed absolute for writes and RMW: the dummy read always happens.
    uint16_t aAbsIdxW(uint8_t idx, uint8_t* hiOut = nullptr)
    {
        const uint8_t lo = fetch();
        const uint8_t hi = fetch();
        const uint16_t base = static_cast<uint16_t>(lo | (hi << 8));
        const uint16_t ea = static_cast<uint16_t>(base + idx);
        rd(static_cast<uint16_t>((base & 0xff00) | (ea & 0x00ff)));
        if (hiOut)
            *hiOut = hi;
        return ea;
    }
    uint16_t aIndX()
    {
        const uint8_t b = fetch();
        rd(b);
        const uint8_t z = static_cast<uint8_t>(b + s.x);
        const uint8_t lo = rd(z);
        const uint8_t hi = rd(static_cast<uint8_t>(z + 1));
        return static_cast<uint16_t>(lo | (hi << 8));
    }
    uint16_t aIndYR()
    {
        const uint8_t z = fetch();
        const uint8_t lo = rd(z);
        const uint8_t hi = rd(static_cast<uint8_t>(z + 1));
        const uint16_t base = static_cast<uint16_t>(lo | (hi << 8));
        const uint16_t ea = static_cast<uint16_t>(base + s.y);
        if ((base ^ ea) & 0xff00)
            rd(static_cast<uint16_t>((base & 0xff00) | (ea & 0x00ff)));
        return ea;
    }
    uint16_t aIndYW(uint8_t* hiOut = nullptr)
    {
        const uint8_t z = fetch();
        const uint8_t lo = rd(z);
        const uint8_t hi = rd(static_cast<uint8_t>(z + 1));
        const uint16_t base = static_cast<uint16_t>(lo | (hi << 8));
        const uint16_t ea = static_cast<uint16_t>(base + s.y);
        rd(static_cast<uint16_t>((base & 0xff00) | (ea & 0x00ff)));
        if (hiOut)
            *hiOut = hi;
        return ea;
    }

    // ------------------------------------------------------------- ALU
    void adc(uint8_t v)
    {
        const unsigned c = s.p & C;
        if (s.p & D) {
            // NMOS decimal mode: Z from the binary sum, N and V from the
            // intermediate result after the low-nibble correction.
            unsigned lo = (s.a & 0x0f) + (v & 0x0f) + c;
            unsigned hi = (s.a & 0xf0) + (v & 0xf0);
            if (lo > 0x09) {
                hi += 0x10;
                lo += 0x06;
            }
            setFlag(Z, static_cast<uint8_t>(s.a + v + c) == 0);
            setFlag(N, hi & 0x80);
            setFlag(V, (~(s.a ^ v) & (s.a ^ hi) & 0x80) != 0);
            if (hi > 0x90)
                hi += 0x60;
            setFlag(C, hi > 0xff);
            s.a = static_cast<uint8_t>((lo & 0x0f) | (hi & 0xf0));
        } else {
            const unsigned r = s.a + v + c;
            setFlag(V, (~(s.a ^ v) & (s.a ^ r) & 0x80) != 0);
            setFlag(C, r > 0xff);
            s.a = static_cast<uint8_t>(r);
            nz(s.a);
        }
    }
    void sbc(uint8_t v)
    {
        const unsigned borrow = (s.p & C) ? 0 : 1;
        const unsigned r = s.a - v - borrow;
        if (s.p & D) {
            // Flags from the binary subtraction; the result decimal-adjusted.
            unsigned lo = (s.a & 0x0f) - (v & 0x0f) - borrow;
            unsigned hi = (s.a & 0xf0) - (v & 0xf0);
            if (lo & 0x10) {
                lo -= 6;
                hi -= 0x10;
            }
            if (hi & 0x0100)
                hi -= 0x60;
            setFlag(C, r < 0x100);
            setFlag(V, ((s.a ^ v) & (s.a ^ r) & 0x80) != 0);
            nz(static_cast<uint8_t>(r));
            s.a = static_cast<uint8_t>((lo & 0x0f) | (hi & 0xf0));
        } else {
            setFlag(C, r < 0x100);
            setFlag(V, ((s.a ^ v) & (s.a ^ r) & 0x80) != 0);
            s.a = static_cast<uint8_t>(r);
            nz(s.a);
        }
    }
    void cmp(uint8_t reg, uint8_t v)
    {
        const unsigned r = reg - v;
        setFlag(C, reg >= v);
        nz(static_cast<uint8_t>(r));
    }
    uint8_t asl(uint8_t v)
    {
        setFlag(C, v & 0x80);
        v = static_cast<uint8_t>(v << 1);
        nz(v);
        return v;
    }
    uint8_t lsr(uint8_t v)
    {
        setFlag(C, v & 0x01);
        v >>= 1;
        nz(v);
        return v;
    }
    uint8_t rol(uint8_t v)
    {
        const uint8_t c = s.p & C;
        setFlag(C, v & 0x80);
        v = static_cast<uint8_t>((v << 1) | c);
        nz(v);
        return v;
    }
    uint8_t ror(uint8_t v)
    {
        const uint8_t c = (s.p & C) ? 0x80 : 0;
        setFlag(C, v & 0x01);
        v = static_cast<uint8_t>((v >> 1) | c);
        nz(v);
        return v;
    }
    void bit(uint8_t v)
    {
        s.p = static_cast<uint8_t>((s.p & ~(N | V | Z)) | (v & (N | V)) | ((s.a & v) ? 0 : Z));
    }

    // ------------------------------------------------------------- op shapes
    template <class F> void readOp(uint16_t ea, F f) { f(rd(ea)); }
    template <class F> void rmwOp(uint16_t ea, F f)
    {
        uint8_t v = rd(ea);
        wr(ea, v); // the dummy write of the unmodified value
        v = f(v);
        wr(ea, v);
    }
    void branch(bool cond)
    {
        const int8_t off = static_cast<int8_t>(fetch());
        if (!cond)
            return;
        // Interrupts are polled before this cycle when no page is crossed.
        const bool ip = s.intPrev, np = s.nmiPrev;
        rd(s.pc);
        const uint16_t target = static_cast<uint16_t>(s.pc + off);
        if ((target ^ s.pc) & 0xff00) {
            rd(static_cast<uint16_t>((s.pc & 0xff00) | (target & 0x00ff)));
        } else {
            s.intPrev = ip;
            s.nmiPrev = np;
        }
        s.pc = target;
    }
    // SHA/SHX/SHY/TAS: the value is ANDed with the high byte of the base + 1,
    // and on a page cross the high byte of the address is replaced too.
    void shStore(uint16_t ea, uint8_t hiBase, uint8_t val)
    {
        const uint8_t v = static_cast<uint8_t>(val & (hiBase + 1));
        uint16_t addr = ea;
        if (((hiBase << 8) ^ ea) & 0xff00)
            addr = static_cast<uint16_t>((v << 8) | (ea & 0xff));
        wr(addr, v);
    }

    void execute(uint8_t op);
};

template <class Bus>
void Cpu6502<Bus>::execute(uint8_t op)
{
    auto& r = s;
    switch (op) {
    // ---------------------------------------------------------- loads
    case 0xa9: nz(r.a = fetch()); break;
    case 0xa5: readOp(aZp(), [&](uint8_t v) { nz(r.a = v); }); break;
    case 0xb5: readOp(aZpX(), [&](uint8_t v) { nz(r.a = v); }); break;
    case 0xad: readOp(aAbs(), [&](uint8_t v) { nz(r.a = v); }); break;
    case 0xbd: readOp(aAbsIdxR(r.x), [&](uint8_t v) { nz(r.a = v); }); break;
    case 0xb9: readOp(aAbsIdxR(r.y), [&](uint8_t v) { nz(r.a = v); }); break;
    case 0xa1: readOp(aIndX(), [&](uint8_t v) { nz(r.a = v); }); break;
    case 0xb1: readOp(aIndYR(), [&](uint8_t v) { nz(r.a = v); }); break;
    case 0xa2: nz(r.x = fetch()); break;
    case 0xa6: readOp(aZp(), [&](uint8_t v) { nz(r.x = v); }); break;
    case 0xb6: readOp(aZpY(), [&](uint8_t v) { nz(r.x = v); }); break;
    case 0xae: readOp(aAbs(), [&](uint8_t v) { nz(r.x = v); }); break;
    case 0xbe: readOp(aAbsIdxR(r.y), [&](uint8_t v) { nz(r.x = v); }); break;
    case 0xa0: nz(r.y = fetch()); break;
    case 0xa4: readOp(aZp(), [&](uint8_t v) { nz(r.y = v); }); break;
    case 0xb4: readOp(aZpX(), [&](uint8_t v) { nz(r.y = v); }); break;
    case 0xac: readOp(aAbs(), [&](uint8_t v) { nz(r.y = v); }); break;
    case 0xbc: readOp(aAbsIdxR(r.x), [&](uint8_t v) { nz(r.y = v); }); break;
    // ---------------------------------------------------------- stores
    case 0x85: wr(aZp(), r.a); break;
    case 0x95: wr(aZpX(), r.a); break;
    case 0x8d: wr(aAbs(), r.a); break;
    case 0x9d: wr(aAbsIdxW(r.x), r.a); break;
    case 0x99: wr(aAbsIdxW(r.y), r.a); break;
    case 0x81: wr(aIndX(), r.a); break;
    case 0x91: wr(aIndYW(), r.a); break;
    case 0x86: wr(aZp(), r.x); break;
    case 0x96: wr(aZpY(), r.x); break;
    case 0x8e: wr(aAbs(), r.x); break;
    case 0x84: wr(aZp(), r.y); break;
    case 0x94: wr(aZpX(), r.y); break;
    case 0x8c: wr(aAbs(), r.y); break;
    // ---------------------------------------------------------- transfers
    case 0xaa: rd(r.pc); nz(r.x = r.a); break;
    case 0xa8: rd(r.pc); nz(r.y = r.a); break;
    case 0x8a: rd(r.pc); nz(r.a = r.x); break;
    case 0x98: rd(r.pc); nz(r.a = r.y); break;
    case 0xba: rd(r.pc); nz(r.x = r.sp); break;
    case 0x9a: rd(r.pc); r.sp = r.x; break;
    // ---------------------------------------------------------- stack
    case 0x48: rd(r.pc); push(r.a); break;
    case 0x08: rd(r.pc); push(r.p | B | U); break;
    case 0x68: rd(r.pc); rd(0x100 | r.sp); nz(r.a = pull()); break;
    case 0x28: rd(r.pc); rd(0x100 | r.sp); r.p = static_cast<uint8_t>((pull() & ~B) | U); break;
    // ---------------------------------------------------------- logic
    case 0x29: nz(r.a &= fetch()); break;
    case 0x25: readOp(aZp(), [&](uint8_t v) { nz(r.a &= v); }); break;
    case 0x35: readOp(aZpX(), [&](uint8_t v) { nz(r.a &= v); }); break;
    case 0x2d: readOp(aAbs(), [&](uint8_t v) { nz(r.a &= v); }); break;
    case 0x3d: readOp(aAbsIdxR(r.x), [&](uint8_t v) { nz(r.a &= v); }); break;
    case 0x39: readOp(aAbsIdxR(r.y), [&](uint8_t v) { nz(r.a &= v); }); break;
    case 0x21: readOp(aIndX(), [&](uint8_t v) { nz(r.a &= v); }); break;
    case 0x31: readOp(aIndYR(), [&](uint8_t v) { nz(r.a &= v); }); break;
    case 0x09: nz(r.a |= fetch()); break;
    case 0x05: readOp(aZp(), [&](uint8_t v) { nz(r.a |= v); }); break;
    case 0x15: readOp(aZpX(), [&](uint8_t v) { nz(r.a |= v); }); break;
    case 0x0d: readOp(aAbs(), [&](uint8_t v) { nz(r.a |= v); }); break;
    case 0x1d: readOp(aAbsIdxR(r.x), [&](uint8_t v) { nz(r.a |= v); }); break;
    case 0x19: readOp(aAbsIdxR(r.y), [&](uint8_t v) { nz(r.a |= v); }); break;
    case 0x01: readOp(aIndX(), [&](uint8_t v) { nz(r.a |= v); }); break;
    case 0x11: readOp(aIndYR(), [&](uint8_t v) { nz(r.a |= v); }); break;
    case 0x49: nz(r.a ^= fetch()); break;
    case 0x45: readOp(aZp(), [&](uint8_t v) { nz(r.a ^= v); }); break;
    case 0x55: readOp(aZpX(), [&](uint8_t v) { nz(r.a ^= v); }); break;
    case 0x4d: readOp(aAbs(), [&](uint8_t v) { nz(r.a ^= v); }); break;
    case 0x5d: readOp(aAbsIdxR(r.x), [&](uint8_t v) { nz(r.a ^= v); }); break;
    case 0x59: readOp(aAbsIdxR(r.y), [&](uint8_t v) { nz(r.a ^= v); }); break;
    case 0x41: readOp(aIndX(), [&](uint8_t v) { nz(r.a ^= v); }); break;
    case 0x51: readOp(aIndYR(), [&](uint8_t v) { nz(r.a ^= v); }); break;
    case 0x24: readOp(aZp(), [&](uint8_t v) { bit(v); }); break;
    case 0x2c: readOp(aAbs(), [&](uint8_t v) { bit(v); }); break;
    // ---------------------------------------------------------- arithmetic
    case 0x69: adc(fetch()); break;
    case 0x65: readOp(aZp(), [&](uint8_t v) { adc(v); }); break;
    case 0x75: readOp(aZpX(), [&](uint8_t v) { adc(v); }); break;
    case 0x6d: readOp(aAbs(), [&](uint8_t v) { adc(v); }); break;
    case 0x7d: readOp(aAbsIdxR(r.x), [&](uint8_t v) { adc(v); }); break;
    case 0x79: readOp(aAbsIdxR(r.y), [&](uint8_t v) { adc(v); }); break;
    case 0x61: readOp(aIndX(), [&](uint8_t v) { adc(v); }); break;
    case 0x71: readOp(aIndYR(), [&](uint8_t v) { adc(v); }); break;
    case 0xe9: case 0xeb: sbc(fetch()); break;
    case 0xe5: readOp(aZp(), [&](uint8_t v) { sbc(v); }); break;
    case 0xf5: readOp(aZpX(), [&](uint8_t v) { sbc(v); }); break;
    case 0xed: readOp(aAbs(), [&](uint8_t v) { sbc(v); }); break;
    case 0xfd: readOp(aAbsIdxR(r.x), [&](uint8_t v) { sbc(v); }); break;
    case 0xf9: readOp(aAbsIdxR(r.y), [&](uint8_t v) { sbc(v); }); break;
    case 0xe1: readOp(aIndX(), [&](uint8_t v) { sbc(v); }); break;
    case 0xf1: readOp(aIndYR(), [&](uint8_t v) { sbc(v); }); break;
    case 0xc9: cmp(r.a, fetch()); break;
    case 0xc5: readOp(aZp(), [&](uint8_t v) { cmp(r.a, v); }); break;
    case 0xd5: readOp(aZpX(), [&](uint8_t v) { cmp(r.a, v); }); break;
    case 0xcd: readOp(aAbs(), [&](uint8_t v) { cmp(r.a, v); }); break;
    case 0xdd: readOp(aAbsIdxR(r.x), [&](uint8_t v) { cmp(r.a, v); }); break;
    case 0xd9: readOp(aAbsIdxR(r.y), [&](uint8_t v) { cmp(r.a, v); }); break;
    case 0xc1: readOp(aIndX(), [&](uint8_t v) { cmp(r.a, v); }); break;
    case 0xd1: readOp(aIndYR(), [&](uint8_t v) { cmp(r.a, v); }); break;
    case 0xe0: cmp(r.x, fetch()); break;
    case 0xe4: readOp(aZp(), [&](uint8_t v) { cmp(r.x, v); }); break;
    case 0xec: readOp(aAbs(), [&](uint8_t v) { cmp(r.x, v); }); break;
    case 0xc0: cmp(r.y, fetch()); break;
    case 0xc4: readOp(aZp(), [&](uint8_t v) { cmp(r.y, v); }); break;
    case 0xcc: readOp(aAbs(), [&](uint8_t v) { cmp(r.y, v); }); break;
    // ---------------------------------------------------------- inc/dec
    case 0xe6: rmwOp(aZp(), [&](uint8_t v) { v++; nz(v); return v; }); break;
    case 0xf6: rmwOp(aZpX(), [&](uint8_t v) { v++; nz(v); return v; }); break;
    case 0xee: rmwOp(aAbs(), [&](uint8_t v) { v++; nz(v); return v; }); break;
    case 0xfe: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { v++; nz(v); return v; }); break;
    case 0xc6: rmwOp(aZp(), [&](uint8_t v) { v--; nz(v); return v; }); break;
    case 0xd6: rmwOp(aZpX(), [&](uint8_t v) { v--; nz(v); return v; }); break;
    case 0xce: rmwOp(aAbs(), [&](uint8_t v) { v--; nz(v); return v; }); break;
    case 0xde: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { v--; nz(v); return v; }); break;
    case 0xe8: rd(r.pc); nz(++r.x); break;
    case 0xc8: rd(r.pc); nz(++r.y); break;
    case 0xca: rd(r.pc); nz(--r.x); break;
    case 0x88: rd(r.pc); nz(--r.y); break;
    // ---------------------------------------------------------- shifts
    case 0x0a: rd(r.pc); r.a = asl(r.a); break;
    case 0x06: rmwOp(aZp(), [&](uint8_t v) { return asl(v); }); break;
    case 0x16: rmwOp(aZpX(), [&](uint8_t v) { return asl(v); }); break;
    case 0x0e: rmwOp(aAbs(), [&](uint8_t v) { return asl(v); }); break;
    case 0x1e: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { return asl(v); }); break;
    case 0x4a: rd(r.pc); r.a = lsr(r.a); break;
    case 0x46: rmwOp(aZp(), [&](uint8_t v) { return lsr(v); }); break;
    case 0x56: rmwOp(aZpX(), [&](uint8_t v) { return lsr(v); }); break;
    case 0x4e: rmwOp(aAbs(), [&](uint8_t v) { return lsr(v); }); break;
    case 0x5e: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { return lsr(v); }); break;
    case 0x2a: rd(r.pc); r.a = rol(r.a); break;
    case 0x26: rmwOp(aZp(), [&](uint8_t v) { return rol(v); }); break;
    case 0x36: rmwOp(aZpX(), [&](uint8_t v) { return rol(v); }); break;
    case 0x2e: rmwOp(aAbs(), [&](uint8_t v) { return rol(v); }); break;
    case 0x3e: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { return rol(v); }); break;
    case 0x6a: rd(r.pc); r.a = ror(r.a); break;
    case 0x66: rmwOp(aZp(), [&](uint8_t v) { return ror(v); }); break;
    case 0x76: rmwOp(aZpX(), [&](uint8_t v) { return ror(v); }); break;
    case 0x6e: rmwOp(aAbs(), [&](uint8_t v) { return ror(v); }); break;
    case 0x7e: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { return ror(v); }); break;
    // ---------------------------------------------------------- jumps
    case 0x4c: r.pc = aAbs(); break;
    case 0x6c: {
        const uint16_t ptr = aAbs();
        const uint8_t lo = rd(ptr);
        const uint8_t hi = rd(static_cast<uint16_t>((ptr & 0xff00) | ((ptr + 1) & 0x00ff)));
        r.pc = static_cast<uint16_t>(lo | (hi << 8));
        break;
    }
    case 0x20: {
        const uint8_t lo = fetch();
        rd(0x100 | r.sp);
        push(static_cast<uint8_t>(r.pc >> 8));
        push(static_cast<uint8_t>(r.pc));
        const uint8_t hi = rd(r.pc);
        r.pc = static_cast<uint16_t>(lo | (hi << 8));
        break;
    }
    case 0x60: {
        rd(r.pc);
        rd(0x100 | r.sp);
        const uint8_t lo = pull();
        const uint8_t hi = pull();
        r.pc = static_cast<uint16_t>(lo | (hi << 8));
        rd(r.pc);
        r.pc++;
        break;
    }
    case 0x40: {
        rd(r.pc);
        rd(0x100 | r.sp);
        r.p = static_cast<uint8_t>((pull() & ~B) | U);
        const uint8_t lo = pull();
        const uint8_t hi = pull();
        r.pc = static_cast<uint16_t>(lo | (hi << 8));
        break;
    }
    case 0x00:
        fetch(); // the padding byte
        interrupt(0xfffe, true);
        break;
    // ---------------------------------------------------------- branches
    case 0x10: branch(!(r.p & N)); break;
    case 0x30: branch(r.p & N); break;
    case 0x50: branch(!(r.p & V)); break;
    case 0x70: branch(r.p & V); break;
    case 0x90: branch(!(r.p & C)); break;
    case 0xb0: branch(r.p & C); break;
    case 0xd0: branch(!(r.p & Z)); break;
    case 0xf0: branch(r.p & Z); break;
    // ---------------------------------------------------------- flags
    case 0x18: rd(r.pc); r.p &= ~C; break;
    case 0x38: rd(r.pc); r.p |= C; break;
    case 0x58: rd(r.pc); r.p &= ~I; break;
    case 0x78: rd(r.pc); r.p |= I; break;
    case 0xb8: rd(r.pc); r.p &= ~V; break;
    case 0xd8: rd(r.pc); r.p &= ~D; break;
    case 0xf8: rd(r.pc); r.p |= D; break;
    // ---------------------------------------------------------- NOPs
    case 0xea: case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa:
        rd(r.pc);
        break;
    case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2: fetch(); break;
    case 0x04: case 0x44: case 0x64: rd(aZp()); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4: rd(aZpX()); break;
    case 0x0c: rd(aAbs()); break;
    case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc: rd(aAbsIdxR(r.x)); break;
    // ---------------------------------------------------------- illegal: combined
    // SLO: ASL then ORA
    case 0x07: rmwOp(aZp(), [&](uint8_t v) { v = asl(v); nz(r.a |= v); return v; }); break;
    case 0x17: rmwOp(aZpX(), [&](uint8_t v) { v = asl(v); nz(r.a |= v); return v; }); break;
    case 0x0f: rmwOp(aAbs(), [&](uint8_t v) { v = asl(v); nz(r.a |= v); return v; }); break;
    case 0x1f: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { v = asl(v); nz(r.a |= v); return v; }); break;
    case 0x1b: rmwOp(aAbsIdxW(r.y), [&](uint8_t v) { v = asl(v); nz(r.a |= v); return v; }); break;
    case 0x03: rmwOp(aIndX(), [&](uint8_t v) { v = asl(v); nz(r.a |= v); return v; }); break;
    case 0x13: rmwOp(aIndYW(), [&](uint8_t v) { v = asl(v); nz(r.a |= v); return v; }); break;
    // RLA: ROL then AND
    case 0x27: rmwOp(aZp(), [&](uint8_t v) { v = rol(v); nz(r.a &= v); return v; }); break;
    case 0x37: rmwOp(aZpX(), [&](uint8_t v) { v = rol(v); nz(r.a &= v); return v; }); break;
    case 0x2f: rmwOp(aAbs(), [&](uint8_t v) { v = rol(v); nz(r.a &= v); return v; }); break;
    case 0x3f: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { v = rol(v); nz(r.a &= v); return v; }); break;
    case 0x3b: rmwOp(aAbsIdxW(r.y), [&](uint8_t v) { v = rol(v); nz(r.a &= v); return v; }); break;
    case 0x23: rmwOp(aIndX(), [&](uint8_t v) { v = rol(v); nz(r.a &= v); return v; }); break;
    case 0x33: rmwOp(aIndYW(), [&](uint8_t v) { v = rol(v); nz(r.a &= v); return v; }); break;
    // SRE: LSR then EOR
    case 0x47: rmwOp(aZp(), [&](uint8_t v) { v = lsr(v); nz(r.a ^= v); return v; }); break;
    case 0x57: rmwOp(aZpX(), [&](uint8_t v) { v = lsr(v); nz(r.a ^= v); return v; }); break;
    case 0x4f: rmwOp(aAbs(), [&](uint8_t v) { v = lsr(v); nz(r.a ^= v); return v; }); break;
    case 0x5f: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { v = lsr(v); nz(r.a ^= v); return v; }); break;
    case 0x5b: rmwOp(aAbsIdxW(r.y), [&](uint8_t v) { v = lsr(v); nz(r.a ^= v); return v; }); break;
    case 0x43: rmwOp(aIndX(), [&](uint8_t v) { v = lsr(v); nz(r.a ^= v); return v; }); break;
    case 0x53: rmwOp(aIndYW(), [&](uint8_t v) { v = lsr(v); nz(r.a ^= v); return v; }); break;
    // RRA: ROR then ADC
    case 0x67: rmwOp(aZp(), [&](uint8_t v) { v = ror(v); adc(v); return v; }); break;
    case 0x77: rmwOp(aZpX(), [&](uint8_t v) { v = ror(v); adc(v); return v; }); break;
    case 0x6f: rmwOp(aAbs(), [&](uint8_t v) { v = ror(v); adc(v); return v; }); break;
    case 0x7f: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { v = ror(v); adc(v); return v; }); break;
    case 0x7b: rmwOp(aAbsIdxW(r.y), [&](uint8_t v) { v = ror(v); adc(v); return v; }); break;
    case 0x63: rmwOp(aIndX(), [&](uint8_t v) { v = ror(v); adc(v); return v; }); break;
    case 0x73: rmwOp(aIndYW(), [&](uint8_t v) { v = ror(v); adc(v); return v; }); break;
    // DCP: DEC then CMP
    case 0xc7: rmwOp(aZp(), [&](uint8_t v) { v--; cmp(r.a, v); return v; }); break;
    case 0xd7: rmwOp(aZpX(), [&](uint8_t v) { v--; cmp(r.a, v); return v; }); break;
    case 0xcf: rmwOp(aAbs(), [&](uint8_t v) { v--; cmp(r.a, v); return v; }); break;
    case 0xdf: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { v--; cmp(r.a, v); return v; }); break;
    case 0xdb: rmwOp(aAbsIdxW(r.y), [&](uint8_t v) { v--; cmp(r.a, v); return v; }); break;
    case 0xc3: rmwOp(aIndX(), [&](uint8_t v) { v--; cmp(r.a, v); return v; }); break;
    case 0xd3: rmwOp(aIndYW(), [&](uint8_t v) { v--; cmp(r.a, v); return v; }); break;
    // ISC: INC then SBC
    case 0xe7: rmwOp(aZp(), [&](uint8_t v) { v++; sbc(v); return v; }); break;
    case 0xf7: rmwOp(aZpX(), [&](uint8_t v) { v++; sbc(v); return v; }); break;
    case 0xef: rmwOp(aAbs(), [&](uint8_t v) { v++; sbc(v); return v; }); break;
    case 0xff: rmwOp(aAbsIdxW(r.x), [&](uint8_t v) { v++; sbc(v); return v; }); break;
    case 0xfb: rmwOp(aAbsIdxW(r.y), [&](uint8_t v) { v++; sbc(v); return v; }); break;
    case 0xe3: rmwOp(aIndX(), [&](uint8_t v) { v++; sbc(v); return v; }); break;
    case 0xf3: rmwOp(aIndYW(), [&](uint8_t v) { v++; sbc(v); return v; }); break;
    // LAX
    case 0xa7: readOp(aZp(), [&](uint8_t v) { nz(r.a = r.x = v); }); break;
    case 0xb7: readOp(aZpY(), [&](uint8_t v) { nz(r.a = r.x = v); }); break;
    case 0xaf: readOp(aAbs(), [&](uint8_t v) { nz(r.a = r.x = v); }); break;
    case 0xbf: readOp(aAbsIdxR(r.y), [&](uint8_t v) { nz(r.a = r.x = v); }); break;
    case 0xa3: readOp(aIndX(), [&](uint8_t v) { nz(r.a = r.x = v); }); break;
    case 0xb3: readOp(aIndYR(), [&](uint8_t v) { nz(r.a = r.x = v); }); break;
    // SAX
    case 0x87: wr(aZp(), r.a & r.x); break;
    case 0x97: wr(aZpY(), r.a & r.x); break;
    case 0x8f: wr(aAbs(), r.a & r.x); break;
    case 0x83: wr(aIndX(), r.a & r.x); break;
    // ---------------------------------------------------------- illegal: immediate
    case 0x0b: case 0x2b: // ANC
        nz(r.a &= fetch());
        setFlag(C, r.a & 0x80);
        break;
    case 0x4b: // ALR
        r.a &= fetch();
        r.a = lsr(r.a);
        break;
    case 0x6b: { // ARR
        const uint8_t v = static_cast<uint8_t>(r.a & fetch());
        if (r.p & D) {
            // Decimal ARR, as measured on NMOS parts.
            uint8_t res = static_cast<uint8_t>((v >> 1) | ((r.p & C) ? 0x80 : 0));
            setFlag(N, r.p & C);
            setFlag(Z, res == 0);
            setFlag(V, (v ^ res) & 0x40);
            if ((v & 0x0f) + (v & 0x01) > 5)
                res = static_cast<uint8_t>((res & 0xf0) | ((res + 6) & 0x0f));
            if ((v & 0xf0) + (v & 0x10) > 0x50) {
                res = static_cast<uint8_t>(res + 0x60);
                r.p |= C;
            } else {
                r.p &= ~C;
            }
            r.a = res;
        } else {
            r.a = static_cast<uint8_t>((v >> 1) | ((r.p & C) ? 0x80 : 0));
            nz(r.a);
            setFlag(C, r.a & 0x40);
            setFlag(V, ((r.a >> 6) ^ (r.a >> 5)) & 1);
        }
        break;
    }
    case 0xcb: { // SBX (AXS)
        const uint8_t v = fetch();
        const uint8_t ax = r.a & r.x;
        setFlag(C, ax >= v);
        nz(r.x = static_cast<uint8_t>(ax - v));
        break;
    }
    case 0x8b: // ANE: unstable; the common "magic" constant is $EE
        nz(r.a = static_cast<uint8_t>((r.a | 0xee) & r.x & fetch()));
        break;
    case 0xab: // LXA: unstable, magic $EE
        nz(r.a = r.x = static_cast<uint8_t>((r.a | 0xee) & fetch()));
        break;
    case 0xbb: // LAS
        readOp(aAbsIdxR(r.y), [&](uint8_t v) { nz(r.a = r.x = r.sp = static_cast<uint8_t>(v & r.sp)); });
        break;
    // ---------------------------------------------------------- illegal: unstable stores
    case 0x9f: { uint8_t hi; const uint16_t ea = aAbsIdxW(r.y, &hi); shStore(ea, hi, r.a & r.x); break; } // SHA abs,y
    case 0x93: { uint8_t hi; const uint16_t ea = aIndYW(&hi); shStore(ea, hi, r.a & r.x); break; }       // SHA (zp),y
    case 0x9e: { uint8_t hi; const uint16_t ea = aAbsIdxW(r.y, &hi); shStore(ea, hi, r.x); break; }       // SHX
    case 0x9c: { uint8_t hi; const uint16_t ea = aAbsIdxW(r.x, &hi); shStore(ea, hi, r.y); break; }       // SHY
    case 0x9b: { // TAS
        uint8_t hi;
        const uint16_t ea = aAbsIdxW(r.y, &hi);
        r.sp = r.a & r.x;
        shStore(ea, hi, r.sp);
        break;
    }
    // ---------------------------------------------------------- JAM
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
    case 0x62: case 0x72: case 0x92: case 0xb2: case 0xd2: case 0xf2:
        r.jammed = true;
        r.pc--;
        break;
    default:
        break;
    }
}

} // namespace sidmidi::emu
