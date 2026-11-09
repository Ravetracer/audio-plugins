#include "Vic.h"

#include <algorithm>
#include <cstring>

namespace sidmidi::emu {

Vic::Vic()
{
    setStandard(VideoStandard::PAL);
}

void Vic::setStandard(VideoStandard std)
{
    std_ = std;
    if (std == VideoStandard::PAL) {
        cyclesPerLine_ = 63;
        lines_ = 312;
        firstVisibleLine_ = 16;
        static constexpr int pal[8] = {58, 60, 62, 1, 3, 5, 7, 9};
        std::copy(pal, pal + 8, spriteCycle_);
    } else {
        cyclesPerLine_ = 65;
        lines_ = 263;
        firstVisibleLine_ = 28;
        static constexpr int ntsc[8] = {58, 60, 62, 64, 1, 3, 5, 7};
        std::copy(ntsc, ntsc + 8, spriteCycle_);
    }
    std::memset(baMask_, 0, sizeof(baMask_));
    for (int n = 0; n < 8; ++n) {
        for (int k = -3; k <= 1; ++k) {
            int c = spriteCycle_[n] + k;
            while (c < 1)
                c += cyclesPerLine_;
            while (c > cyclesPerLine_)
                c -= cyclesPerLine_;
            baMask_[c] |= static_cast<uint8_t>(1 << n);
        }
    }
    work_.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    last_.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    if (s.line >= lines_)
        s.line = 0;
    if (s.cycle > cyclesPerLine_)
        s.cycle = 1;
}

void Vic::reset()
{
    const uint64_t frames = s.frames;
    s = VicState{};
    s.frames = frames;
    ba_ = false;
}

uint8_t Vic::fetch(uint16_t a14)
{
    a14 &= 0x3fff;
    uint8_t v;
    if (ultimaxRom && (a14 & 0x3000) == 0x3000)
        v = ultimaxRom[a14 & 0x1fff];
    else if (!(bankBase & 0x4000) && (a14 & 0x3000) == 0x1000)
        v = charRom[a14 & 0x0fff];
    else
        v = ram[(bankBase | a14) & 0xffff];
    s.busByte = v;
    return v;
}

void Vic::checkRasterIrq()
{
    const int cmp = s.regs[0x12] | ((s.regs[0x11] & 0x80) << 1);
    if (s.line == cmp && !s.rasterIrqDone) {
        s.irqFlags |= 0x01;
        s.rasterIrqDone = true;
    }
}

void Vic::cAccess()
{
    if (s.vmli >= 40)
        return;
    s.lineChar[s.vmli] = fetch(static_cast<uint16_t>(((s.regs[0x18] & 0xf0) << 6) | s.vc));
    s.lineColor[s.vmli] = colorRam[s.vc & 0x3ff] & 0x0f;
}

void Vic::gAccess()
{
    const uint8_t r11 = s.regs[0x11];
    uint8_t data, ch = 0, col = 0;
    if (s.display) {
        const int i = s.vmli < 40 ? s.vmli : 39;
        ch = s.lineChar[i];
        col = s.lineColor[i];
        uint16_t addr;
        if (r11 & 0x20)
            addr = static_cast<uint16_t>(((s.regs[0x18] & 0x08) << 10) | (s.vc << 3) | s.rc);
        else
            addr = static_cast<uint16_t>(((s.regs[0x18] & 0x0e) << 10) | (ch << 3) | s.rc);
        if (r11 & 0x40)
            addr &= 0x39ff;
        data = fetch(addr);
        s.vc = (s.vc + 1) & 0x3ff;
        s.vmli = (s.vmli + 1) & 0x3f;
    } else {
        data = fetch((r11 & 0x40) ? 0x39ff : 0x3fff);
    }

    const int mode = ((r11 & 0x60) | (s.regs[0x16] & 0x10)) >> 4; // ECM BMM MCM
    uint8_t* px = &gfx_[8 + 8 * (s.cycle - 16) + (s.regs[0x16] & 7)];
    constexpr uint8_t FG = 0x80, EX = 0x40;

    auto hires = [&](uint8_t on, uint8_t off) {
        for (int k = 0; k < 8; ++k)
            px[k] = (data & (0x80 >> k)) ? on : off;
    };
    auto multi = [&](const uint8_t codes[4]) {
        for (int k = 0; k < 4; ++k) {
            const uint8_t c = codes[(data >> (6 - 2 * k)) & 3];
            px[2 * k] = px[2 * k + 1] = c;
        }
    };

    switch (mode) {
    case 0: // standard text
        hires(static_cast<uint8_t>(FG | EX | (col & 0x0f)), 0x00);
        break;
    case 1: // multicolour text
        if (col & 0x08) {
            const uint8_t codes[4] = {0x00, 0x01, static_cast<uint8_t>(FG | 0x02), static_cast<uint8_t>(FG | EX | (col & 7))};
            multi(codes);
        } else {
            hires(static_cast<uint8_t>(FG | EX | (col & 7)), 0x00);
        }
        break;
    case 2: // standard bitmap
        hires(static_cast<uint8_t>(FG | EX | (ch >> 4)), static_cast<uint8_t>(EX | (ch & 0x0f)));
        break;
    case 3: { // multicolour bitmap
        const uint8_t codes[4] = {0x00, static_cast<uint8_t>(EX | (ch >> 4)), static_cast<uint8_t>(FG | EX | (ch & 0x0f)),
                                  static_cast<uint8_t>(FG | EX | (col & 0x0f))};
        multi(codes);
        break;
    }
    case 4: // extended colour text
        hires(static_cast<uint8_t>(FG | EX | (col & 0x0f)), static_cast<uint8_t>(ch >> 6));
        break;
    case 5: // invalid: ECM + multicolour text -- black, collisions as usual
        if (col & 0x08) {
            const uint8_t codes[4] = {EX, EX, FG | EX, FG | EX};
            multi(codes);
        } else {
            hires(FG | EX, EX);
        }
        break;
    case 6: // invalid: ECM + bitmap
        hires(FG | EX, EX);
        break;
    default: { // invalid: ECM + multicolour bitmap
        const uint8_t codes[4] = {EX, EX, FG | EX, FG | EX};
        multi(codes);
        break;
    }
    }
}

void Vic::spriteFetch(int n)
{
    const uint8_t ptr = fetch(static_cast<uint16_t>(((s.regs[0x18] & 0xf0) << 6) | 0x3f8 | n));
    if (!(s.dma & (1 << n))) {
        fetch(0x3fff);
        return;
    }
    const uint16_t a = static_cast<uint16_t>(ptr << 6);
    uint32_t d = 0;
    for (int k = 0; k < 3; ++k) {
        d = (d << 8) | fetch(static_cast<uint16_t>(a | s.mc[n]));
        s.mc[n] = (s.mc[n] + 1) & 0x3f;
    }
    s.fetched[n] = d;
}

uint8_t Vic::resolve(uint8_t code) const
{
    if (code & 0x40)
        return code & 0x0f;
    return s.regs[0x21 + (code & 3)] & 0x0f;
}

void Vic::outputCycle()
{
    const int c = s.cycle;
    const int x0 = 8 * c - 104;
    const int fbLine = s.line - firstVisibleLine_;
    const bool draw = fbLine >= 0 && fbLine < kHeight && (renderVideo || s.lineDisplay);
    const uint8_t r11 = s.regs[0x11], r16 = s.regs[0x16];
    const int leftCmp = (r16 & 0x08) ? 24 : 31;
    const int rightCmp = (r16 & 0x08) ? 344 : 335;
    const int top = (r11 & 0x08) ? 51 : 55;
    const int bottom = (r11 & 0x08) ? 251 : 247;

    if (!draw) {
        // Only the border flip-flops need to follow along.
        if (rightCmp >= x0 && rightCmp < x0 + 8)
            s.mainBorder = true;
        if (leftCmp >= x0 && leftCmp < x0 + 8) {
            if (s.line == bottom)
                s.vertBorder = true;
            else if (s.line == top && (r11 & 0x10))
                s.vertBorder = false;
            if (!s.vertBorder)
                s.mainBorder = false;
        }
        return;
    }

    uint8_t* row = &work_[static_cast<size_t>(fbLine) * kWidth + static_cast<size_t>(8 * (c - 12))];
    const int lineWidth = cyclesPerLine_ * 8;
    int sx[8];
    const uint8_t active = s.lineDisplay;
    if (active) {
        for (int n = 0; n < 8; ++n) {
            int x = s.regs[2 * n] | (((s.regs[0x10] >> n) & 1) << 8);
            if (x >= lineWidth - 16)
                x -= lineWidth;
            sx[n] = x;
        }
    }

    for (int i = 0; i < 8; ++i) {
        const int X = x0 + i;
        if (X == rightCmp)
            s.mainBorder = true;
        if (X == leftCmp) {
            if (s.line == bottom)
                s.vertBorder = true;
            else if (s.line == top && (r11 & 0x10))
                s.vertBorder = false;
            if (!s.vertBorder)
                s.mainBorder = false;
        }

        const int gi = X - 16;
        const uint8_t code = (gi >= 0 && gi < 400) ? gfx_[gi] : 0;
        const bool fg = code & 0x80;
        uint8_t color = resolve(code);

        if (active) {
            uint8_t hit = 0;
            int first = -1;
            uint8_t firstColor = 0;
            for (int n = 0; n < 8; ++n) {
                if (!(active & (1 << n)))
                    continue;
                const int dx = X - sx[n];
                const bool xexp = (s.regs[0x1d] >> n) & 1;
                if (dx < 0 || dx >= (xexp ? 48 : 24))
                    continue;
                const int bx = xexp ? dx >> 1 : dx;
                const uint32_t d = s.lineData[n];
                uint8_t sc = 0;
                bool on = false;
                if ((s.regs[0x1c] >> n) & 1) {
                    const int pair = (d >> (22 - (bx & ~1))) & 3;
                    if (pair) {
                        on = true;
                        sc = pair == 1 ? s.regs[0x25] : pair == 2 ? s.regs[0x27 + n] : s.regs[0x26];
                    }
                } else if ((d >> (23 - bx)) & 1) {
                    on = true;
                    sc = s.regs[0x27 + n];
                }
                if (!on)
                    continue;
                hit |= static_cast<uint8_t>(1 << n);
                if (first < 0) {
                    first = n;
                    firstColor = sc & 0x0f;
                }
            }
            if (hit) {
                if (hit & (hit - 1)) {
                    if (!s.collSS)
                        s.irqFlags |= 0x04;
                    s.collSS |= hit;
                }
                if (fg) {
                    if (!s.collSB)
                        s.irqFlags |= 0x02;
                    s.collSB |= hit;
                }
                if (!((s.regs[0x1b] >> first) & 1) || !fg)
                    color = firstColor;
            }
        }
        if (s.mainBorder)
            color = s.regs[0x20] & 0x0f;
        row[i] = color;
    }
}

void Vic::endOfLine()
{
    const uint8_t r11 = s.regs[0x11];
    const int top = (r11 & 0x08) ? 51 : 55;
    const int bottom = (r11 & 0x08) ? 251 : 247;
    if (s.line == bottom)
        s.vertBorder = true;
    else if (s.line == top && (r11 & 0x10))
        s.vertBorder = false;

    s.cycle = 1;
    s.rasterIrqDone = false;
    if (++s.line >= lines_) {
        s.line = 0;
        s.vcBase = 0;
        s.denSeen = false;
        last_.swap(work_);
        s.frames++;
    }
}

void Vic::cycle()
{
    const int c = s.cycle;
    if ((c == 1 && s.line != 0) || (c == 2 && s.line == 0))
        checkRasterIrq();
    if (s.line == 0x30 && (s.regs[0x11] & 0x10))
        s.denSeen = true;
    s.badline = s.line >= 0x30 && s.line <= 0xf7 && (s.line & 7) == (s.regs[0x11] & 7) && s.denSeen;
    if (s.badline)
        s.display = true;

    // The expansion flip-flop is held set while Y expansion is off.
    s.expFF |= static_cast<uint8_t>(~s.regs[0x17]);

    switch (c) {
    case 1:
        std::memset(gfx_, 0, sizeof(gfx_));
        break;
    case 11:
        // Sprite data for this line is complete (0-2 came at the end of the last).
        std::memcpy(s.lineData, s.fetched, sizeof(s.lineData));
        s.lineDisplay = s.spriteDisplay;
        break;
    case 14:
        s.vc = s.vcBase;
        s.vmli = 0;
        if (s.badline)
            s.rc = 0;
        break;
    case 15:
        for (int n = 0; n < 8; ++n)
            if ((s.dma & (1 << n)) && (s.expFF & (1 << n)))
                s.mcBase[n] = (s.mcBase[n] + 2) & 0x3f;
        break;
    case 16:
        for (int n = 0; n < 8; ++n) {
            const uint8_t bit = static_cast<uint8_t>(1 << n);
            if (!(s.dma & bit))
                continue;
            if (s.expFF & bit)
                s.mcBase[n] = (s.mcBase[n] + 1) & 0x3f;
            if (s.mcBase[n] == 63) {
                s.dma &= ~bit;
                s.spriteDisplay &= ~bit;
            }
        }
        break;
    case 55:
        s.expFF ^= s.regs[0x17];
        [[fallthrough]];
    case 56:
        for (int n = 0; n < 8; ++n) {
            const uint8_t bit = static_cast<uint8_t>(1 << n);
            if ((s.regs[0x15] & bit) && s.regs[1 + 2 * n] == (s.line & 0xff) && !(s.dma & bit)) {
                s.dma |= bit;
                s.mcBase[n] = 0;
                if (s.regs[0x17] & bit)
                    s.expFF &= ~bit;
            }
        }
        break;
    case 58:
        for (int n = 0; n < 8; ++n) {
            s.mc[n] = s.mcBase[n];
            if ((s.dma & (1 << n)) && s.regs[1 + 2 * n] == (s.line & 0xff))
                s.spriteDisplay |= static_cast<uint8_t>(1 << n);
        }
        if (s.rc == 7) {
            s.vcBase = s.vc;
            s.display = s.badline;
        }
        if (s.display)
            s.rc = (s.rc + 1) & 7;
        break;
    default:
        break;
    }

    // Phase 1: one memory access per cycle.
    bool fetched = false;
    for (int n = 0; n < 8; ++n) {
        if (spriteCycle_[n] == c) {
            spriteFetch(n);
            fetched = true;
        }
    }
    if (!fetched) {
        if (c >= 11 && c <= 15) {
            fetch(static_cast<uint16_t>(0x3f00 | s.refresh));
            s.refresh--;
        } else if (c >= 16 && c <= 55) {
            gAccess();
        } else {
            fetch(0x3fff);
        }
    }
    // Phase 2: the c-access on a bad line.
    if (s.badline && c >= 15 && c <= 54)
        cAccess();

    ba_ = (s.badline && c >= 12 && c <= 54) || (s.dma & baMask_[c]);

    if (c >= 12 && c <= 59)
        outputCycle();

    if (c >= cyclesPerLine_)
        endOfLine();
    else
        s.cycle = c + 1;
}

uint8_t Vic::peek(uint8_t reg) const
{
    reg &= 0x3f;
    switch (reg) {
    case 0x11: return static_cast<uint8_t>((s.regs[0x11] & 0x7f) | ((s.line & 0x100) >> 1));
    case 0x12: return static_cast<uint8_t>(s.line & 0xff);
    case 0x13: case 0x14: return 0;
    case 0x16: return s.regs[0x16] | 0xc0;
    case 0x18: return s.regs[0x18] | 0x01;
    case 0x19: return static_cast<uint8_t>(s.irqFlags | 0x70 | (irq() ? 0x80 : 0));
    case 0x1a: return s.regs[0x1a] | 0xf0;
    case 0x1e: return s.collSS;
    case 0x1f: return s.collSB;
    default:
        if (reg >= 0x20 && reg <= 0x2e)
            return s.regs[reg] | 0xf0;
        if (reg >= 0x2f)
            return 0xff;
        return s.regs[reg];
    }
}

uint8_t Vic::read(uint8_t reg)
{
    reg &= 0x3f;
    const uint8_t v = peek(reg);
    if (reg == 0x1e)
        s.collSS = 0;
    else if (reg == 0x1f)
        s.collSB = 0;
    return v;
}

void Vic::write(uint8_t reg, uint8_t v)
{
    reg &= 0x3f;
    switch (reg) {
    case 0x11:
    case 0x12:
        s.regs[reg] = v;
        if (s.cycle != 1 || s.line == 0) // the compare at cycle 1 has not happened yet otherwise
            checkRasterIrq();
        break;
    case 0x19:
        s.irqFlags &= static_cast<uint8_t>(~v & 0x0f);
        break;
    case 0x1a:
        s.regs[0x1a] = v & 0x0f;
        break;
    case 0x1e:
    case 0x1f:
        break;
    default:
        if (reg < 0x2f)
            s.regs[reg] = v;
        break;
    }
}

} // namespace sidmidi::emu
