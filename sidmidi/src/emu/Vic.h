#pragma once

// The VIC-II: 6569 (PAL, 63 cycles x 312 lines) and 6567R8 (NTSC, 65 x 263).
//
// Cycle-stepped after Christian Bauer's description of the chip: bad lines and
// the c-accesses that stop the CPU, the g-accesses, the video counter and row
// counter, display and idle state, sprite DMA with its MCBASE logic and Y
// expansion, and the two border flip-flops. Each cycle outputs its 8 pixels
// with the registers as they are in that cycle, so a colour written mid-line
// shows where the CPU wrote it.
//
// What is simplified: sprite data is latched per line rather than shifted
// pixel by pixel (no sprite crunch, no mid-line sprite X tricks), the light
// pen is absent, and the open-bus value is the last phase-1 fetch.

#include <cstdint>
#include <vector>

namespace sidmidi::emu {

enum class VideoStandard : uint8_t { PAL = 0, NTSC = 1 };

struct VicState
{
    uint8_t regs[0x40] = {};
    int cycle = 1;   // 1-based, as in Bauer's tables
    int line = 0;
    uint16_t vc = 0, vcBase = 0;
    uint8_t rc = 0, vmli = 0;
    bool display = false;
    bool badline = false;
    bool denSeen = false;   // DEN was set in line $30
    bool rasterIrqDone = false;
    uint8_t irqFlags = 0;
    uint8_t lineChar[40] = {}, lineColor[40] = {};
    // Sprites
    uint8_t mc[8] = {}, mcBase[8] = {};
    uint8_t dma = 0, spriteDisplay = 0, expFF = 0xff;
    uint32_t fetched[8] = {};
    uint32_t lineData[8] = {};
    uint8_t lineDisplay = 0;
    uint8_t collSS = 0, collSB = 0;
    // Border
    bool mainBorder = true, vertBorder = true;
    uint8_t refresh = 0xff;
    uint8_t busByte = 0xff;
    uint64_t frames = 0;
};

class Vic
{
public:
    static constexpr int kWidth = 384;   // VICE's "normal" borders
    static constexpr int kHeight = 272;

    // Memory the VIC sees. Set by the machine.
    const uint8_t* ram = nullptr;
    const uint8_t* charRom = nullptr;
    const uint8_t* colorRam = nullptr;
    const uint8_t* ultimaxRom = nullptr; // ROMH in Ultimax mode, else null
    uint16_t bankBase = 0;               // from CIA2 port A
    // Off when nobody looks at the screen: lines without sprites then skip
    // the pixels (sprite lines are still drawn, for the collisions).
    bool renderVideo = true;

    VicState s;

    Vic();
    void setStandard(VideoStandard std);
    VideoStandard standard() const { return std_; }
    int cyclesPerLine() const { return cyclesPerLine_; }
    int linesPerFrame() const { return lines_; }

    void reset();
    void cycle();
    bool baLow() const { return ba_; }
    bool irq() const { return (s.irqFlags & s.regs[0x1a] & 0x0f) != 0; }
    uint8_t read(uint8_t reg);
    uint8_t peek(uint8_t reg) const;
    void write(uint8_t reg, uint8_t v);
    uint8_t busByte() const { return s.busByte; }

    // The finished frame (palette indices, kWidth x kHeight) and its number.
    const uint8_t* frame() const { return last_.data(); }
    uint64_t frameCount() const { return s.frames; }

private:
    uint8_t fetch(uint16_t a14);
    void gAccess();
    void cAccess();
    void spriteFetch(int n);
    void checkRasterIrq();
    void outputCycle();
    void endOfLine();
    uint8_t resolve(uint8_t code) const;

    VideoStandard std_ = VideoStandard::PAL;
    int cyclesPerLine_ = 63, lines_ = 312, firstVisibleLine_ = 16;
    int spriteCycle_[8] = {};
    uint8_t baMask_[66] = {};   // sprites whose DMA holds BA low in a cycle
    bool ba_ = false;

    // Graphics pixels of the current line, indexed by X - 16: bit 7 marks a
    // foreground pixel; bit 6 an explicit colour in bits 0-3, otherwise bits
    // 0-1 select background register $D021+n.
    uint8_t gfx_[400] = {};
    std::vector<uint8_t> work_, last_;
};

} // namespace sidmidi::emu
