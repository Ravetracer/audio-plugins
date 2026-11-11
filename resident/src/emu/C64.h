#pragma once

// The Commodore 64: the 6510 and its processor port, the PLA's memory map,
// 64K of RAM and the colour RAM, the VIC-II, two CIAs, the SID bank, the
// expansion port (a cartridge and a MIDI interface side by side) and drive 8
// as the virtual drive.
//
// The CPU drives time: every bus access is one cycle, and each cycle ticks the
// VIC (which may stop the CPU), the CIAs and the MIDI interface. run() goes
// instruction by instruction until a cycle count is reached, so it can
// overshoot by up to one instruction; the caller carries that over.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Cartridge.h"
#include "Cia6526.h"
#include "Cpu6502.h"
#include "Drive1541.h"
#include "HerMidi.h"
#include "MidiCart.h"
#include "SidBank.h"
#include "Vic.h"
#include "VirtualDrive.h"

namespace resident::emu {

struct Roms
{
    std::vector<uint8_t> kernal, basic, chargen, drive1541;
    bool complete() const { return kernal.size() == 8192 && basic.size() == 8192 && chargen.size() == 4096; }
};

// C64 keyboard matrix positions: column (CIA1 PA bit) * 8 + row (PB bit).
namespace keys {
enum : uint8_t {
    Del = 0x00, Return = 0x01, CrsrRight = 0x02, F7 = 0x03, F1 = 0x04, F3 = 0x05, F5 = 0x06, CrsrDown = 0x07,
    K3 = 0x08, W = 0x09, A = 0x0a, K4 = 0x0b, Z = 0x0c, S = 0x0d, E = 0x0e, LShift = 0x0f,
    K5 = 0x10, R = 0x11, D = 0x12, K6 = 0x13, C = 0x14, F = 0x15, T = 0x16, X = 0x17,
    K7 = 0x18, Y = 0x19, G = 0x1a, K8 = 0x1b, B = 0x1c, H = 0x1d, U = 0x1e, V = 0x1f,
    K9 = 0x20, I = 0x21, J = 0x22, K0 = 0x23, M = 0x24, K = 0x25, O = 0x26, N = 0x27,
    Plus = 0x28, P = 0x29, L = 0x2a, Minus = 0x2b, Period = 0x2c, Colon = 0x2d, At = 0x2e, Comma = 0x2f,
    Pound = 0x30, Asterisk = 0x31, Semicolon = 0x32, Home = 0x33, RShift = 0x34, Equal = 0x35, UpArrow = 0x36, Slash = 0x37,
    K1 = 0x38, LeftArrow = 0x39, Ctrl = 0x3a, K2 = 0x3b, Space = 0x3c, Commodore = 0x3d, Q = 0x3e, RunStop = 0x3f,
    None = 0xff
};
}

// Joystick bits as on the port (set = pressed here; the port is active low).
enum JoyBits : uint8_t { JoyUp = 0x01, JoyDown = 0x02, JoyLeft = 0x04, JoyRight = 0x08, JoyFire = 0x10 };

// Drive 8: the KERNAL-trap virtual drive, the emulated 1541, or nothing.
enum class DriveMode : uint8_t { Virtual = 0, True1541, None };

// What sits in a control port besides the keyboard joystick.
enum class PortDevice : uint8_t { Joystick = 0, Mouse1351, Paddles };

struct MachineState
{
    std::array<uint8_t, 65536> ram{};
    std::array<uint8_t, 1024> colorRam{};
    uint8_t portDdr = 0x2f, portData = 0x37;
    uint64_t clk = 0;
    std::array<uint8_t, 8> keyMatrix{}; // [column] = rows pressed
    uint8_t joy[2] = {0, 0};            // port 1, port 2: the keyboard joystick
    uint8_t devButtons[2] = {0, 0};     // the port device's buttons, as joystick bits
    uint8_t potX[2] = {0xff, 0xff}, potY[2] = {0xff, 0xff};
    uint16_t mouseX[2] = {0, 0}, mouseY[2] = {0, 0};
    PortDevice device[2] = {PortDevice::Joystick, PortDevice::Joystick};
    int restoreCycles = 0;
};

class C64
{
public:
    C64();
    ~C64();

    // ---------------------------------------------------------------- setup
    void setRoms(const Roms& roms);
    bool hasRoms() const { return roms_.complete(); }
    void setStandard(VideoStandard std);
    VideoStandard standard() const { return vic.standard(); }
    double clockHz() const;
    void setMidiInterface(MidiInterface m);
    // Switching keeps the disk: its image moves between the two drives.
    void setDriveMode(DriveMode mode);
    DriveMode driveMode() const { return driveMode_; }
    bool trueDriveActive() const { return driveMode_ == DriveMode::True1541 && drive1541.hasRom(); }
    // The disk image as it is now (what the true drive wrote decoded into it).
    const D64& diskImage();
    bool herMidiActive() const { return midi.type == MidiInterface::HerMIDI && herMidi.available(); }

    // MIDI from the host, to whichever interface is fitted.
    void midiReceive(uint8_t byte);
    // Bytes the cable queue of the active interface can still take. A
    // message that does not fit is dropped whole: a cut one would leave a
    // stray status byte that some programs' parsers never recover from.
    size_t midiRoom() const
    {
        return herMidiActive() ? herMidi.queue().capacity() - herMidi.queue().size()
                               : midi.rxQueue().capacity() - midi.rxQueue().size();
    }
    void midiFlush();
    // The serial bus as everybody drives it (true = line low).
    void iecLines(bool& atn, bool& clk, bool& data) const;
    // The same with the C64's outputs as `pa` (the drive's view lags a cycle).
    void iecLines(bool& atn, bool& clk, bool& data, uint8_t pa) const;
    void powerOn();          // cold start: RAM pattern, everything reset
    void reset();            // the reset button: RAM survives

    // ---------------------------------------------------------------- running
    // Runs until clk >= target (finishing the current instruction).
    void run(uint64_t target);
    uint64_t clk() const { return m.clk; }

    // ---------------------------------------------------------------- media
    bool attachPrg(const std::vector<uint8_t>& prg, bool autostart, std::string& error);
    // Inserts a disk. With `autostart`, switches the machine on and loads and
    // runs its first file; without, nothing else changes (a running program
    // or cartridge goes on, and the disk is there for LOAD"$",8 and the like).
    bool attachDisk(const std::vector<uint8_t>& d64, bool autostart, std::string& error);
    // Switches on and loads and runs a file from the inserted disk by its
    // PETSCII name ("*" for the first); LOAD"name",8,1 and RUN, typed.
    void autostartDisk(const std::string& petsciiName);
    void ejectDisk();
    bool attachCartridge(const std::vector<uint8_t>& data, const std::string& name, std::string& error);
    void ejectCartridge();
    // The freeze button of an Action Replay or a Final Cartridge.
    void freezeButton();
    bool autostartPending() const { return autostart_ != Autostart::None; }
    // True from power-on until BASIC first waits for input (the KERNAL's RAM
    // test and screen setup), so the host can run it in warp. Never longer
    // than about four seconds of machine time: a cartridge does not go there.
    bool booting() const { return booting_; }
    // Typing: queued into the KERNAL keyboard buffer as BASIC wants it.
    void typeText(const std::string& petscii);

    // ---------------------------------------------------------------- input
    void setKey(uint8_t matrixPos, bool down);
    void releaseAllKeys();
    void pressRestore();
    void setJoystick(int port, uint8_t bits); // port 0 = port 1, 1 = port 2
    void setPortDevice(int port, PortDevice d);
    // 1351 mouse: movement in mouse units (right and down positive), buttons
    // as JoyFire (left) and JoyUp (right).
    void moveMouse(int port, int dx, int dy);
    // Paddles: positions 0-255 for the pair, buttons as JoyLeft/JoyRight.
    void setPaddles(int port, uint8_t a, uint8_t b);
    void setDeviceButtons(int port, uint8_t bits);

    // ---------------------------------------------------------------- bus (for the CPU)
    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);
    bool irqLine() const;
    bool nmiLine() const;
    bool trap(uint16_t pc);

    // Recomputes what follows from the stored state (memory map, VIC bank)
    // after a snapshot was loaded.
    void restoreDerivedState()
    {
        updateMap();
        updateVicBank();
        updatePots();
    }

    // Reads memory as the CPU would see it, without side effects.
    uint8_t peek(uint16_t addr) const;

    // ---------------------------------------------------------------- parts
    Cpu6502<C64> cpu;
    Vic vic;
    Cia6526 cia1, cia2;
    SidBank sids;
    MidiCart midi;
    Cartridge cart;
    VirtualDrive drive;
    Drive1541 drive1541;
    HerMidi herMidi;
    MachineState m;
    double driveDue = 0.0; // the 1541's clock target, in its cycles
    double herDue = 0.0;   // HerMIDI's PIC, likewise

    const Roms& roms() const { return roms_; }

    enum class Autostart : uint8_t { None, Prg, Disk };
    Autostart autostart_ = Autostart::None;
    // Prg: the program. Disk: the file's PETSCII name, empty for the first.
    std::vector<uint8_t> autostartPrg_;
    std::string pendingTyping_;
    bool booting_ = false;
    uint64_t bootStartClk_ = 0;

private:
    enum Src : uint8_t { RAM, BASIC, KERNAL, CHAR, IO, ROML, ROMH, OPEN };
    void updateMap();
    void updateVicBank();
    void updateIec();
    uint8_t readIo(uint16_t addr);
    uint8_t peekIo(uint16_t addr) const;
    void writeIo(uint16_t addr, uint8_t v);
    bool freezerWriting() const;
    void beginCycle() { vic.cycle(); }
    void endCycle();
    uint8_t keyboardRows(uint8_t columns) const;
    uint8_t portBits(int port) const { return static_cast<uint8_t>(m.joy[port] | m.devButtons[port]); }
    void updatePots();
    uint8_t keyboardColumns(uint8_t rows) const;
    bool kernalVisible() const { return map_[0xe] == KERNAL; }
    void injectPrg(const std::vector<uint8_t>& prg);
    bool serviceKeyboardBuffer();
    void returnFromTrap();

    Roms roms_;
    DriveMode driveMode_ = DriveMode::Virtual;
    double driveRatio_ = 1000000.0 / 985248.0;
    uint8_t drivePa_ = 0x3f;   // CIA 2 port A as the drive sees it
    bool iecChanged_ = false;
    std::array<Src, 16> map_{};
    bool ultimax_ = false;
};

} // namespace resident::emu
