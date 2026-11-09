#include "C64.h"

#include <algorithm>
#include <cstring>

namespace sidmidi::emu {

namespace {

constexpr double kPalClock = 985248.0;
constexpr double kNtscClock = 1022727.0;

// The serial routines as KERNAL 901227-03 has them (and -01/-02, which share
// these entry points). A trap only fires when the ROM bytes match, so a
// replacement KERNAL with its own serial code is left alone.
struct TrapSig
{
    uint16_t pc;
    uint8_t bytes[3];
};
constexpr TrapSig kTraps[] = {
    {0xed09, {0x09, 0x40, 0x2c}}, // TALK
    {0xed0c, {0x09, 0x20, 0x20}}, // LISTEN
    {0xedb9, {0x85, 0x95, 0x20}}, // SECOND
    {0xedc7, {0x85, 0x95, 0x20}}, // TKSA
    {0xeddd, {0x24, 0x94, 0x30}}, // CIOUT
    {0xedef, {0x78, 0x20, 0x8e}}, // UNTLK
    {0xedfe, {0xa9, 0x3f, 0x20}}, // UNLSN
    {0xee13, {0x78, 0xa9, 0x00}}, // ACPTR
};
constexpr uint16_t kKeyWait = 0xe5cd; // BASIC's input loop: LDA $C6 / STA $CC ...
constexpr uint8_t kKeyWaitSig[3] = {0xa5, 0xc6, 0x85};

} // namespace

C64::C64() : cpu(*this)
{
    vic.ram = m.ram.data();
    vic.colorRam = m.colorRam.data();
    static const uint8_t blank[4096] = {};
    vic.charRom = blank;

    cia1.readPortA = [this] {
        const uint8_t rows = static_cast<uint8_t>(cia1.portBOut() & ~portBits(0));
        return static_cast<uint8_t>(keyboardColumns(rows) & ~portBits(1));
    };
    cia1.readPortB = [this] {
        const uint8_t cols = static_cast<uint8_t>(cia1.portAOut() & ~portBits(1));
        return static_cast<uint8_t>(keyboardRows(cols) & ~portBits(0));
    };
    // CIA1 port A bits 6 and 7 switch the SID's POT lines between the ports.
    cia1.portsChanged = [this] { updatePots(); };
    cia2.readPortA = [this] {
        // The serial bus: a line is low when anybody pulls it. The C64's
        // outputs are inverted (1 = pull), its inputs read the line (1 = high).
        bool atn, clkLow, dataLow;
        iecLines(atn, clkLow, dataLow);
        return static_cast<uint8_t>(0x3f | (clkLow ? 0 : 0x40) | (dataLow ? 0 : 0x80));
    };
    cia2.portsChanged = [this] {
        updateVicBank();
        updateIec();
    };
    drive1541.readBus = [this](bool& atn, bool& clk, bool& data) { iecLines(atn, clk, data); };

    SidBankConfig cfg;
    sids.configure(cfg, kPalClock, 48000.0, 0);
    setStandard(VideoStandard::PAL);
    updateMap();
}

C64::~C64() = default;

double C64::clockHz() const
{
    return vic.standard() == VideoStandard::PAL ? kPalClock : kNtscClock;
}

void C64::setRoms(const Roms& roms)
{
    roms_ = roms;
    vic.charRom = roms_.chargen.size() == 4096 ? roms_.chargen.data() : vic.charRom;
    drive1541.setRom(roms_.drive1541);
}

void C64::iecLines(bool& atn, bool& clk, bool& data) const
{
    const uint8_t out = cia2.portAOut();
    atn = out & 0x08;
    clk = out & 0x10;
    data = out & 0x20;
    if (trueDriveActive()) {
        clk |= drive1541.pullsClk();
        data |= drive1541.pullsData(atn);
    }
    if (herMidiActive()) {
        clk |= herMidi.pullsClk();
        data |= herMidi.pullsData();
    }
}

void C64::midiReceive(uint8_t byte)
{
    if (herMidiActive())
        herMidi.receive(byte, herMidi.clk());
    else
        midi.receive(byte, m.clk);
}

void C64::midiFlush()
{
    midi.flush();
    herMidi.flush();
}

void C64::updateIec()
{
    const uint8_t out = cia2.portAOut();
    drive1541.setC64Lines(out & 0x08, out & 0x10, out & 0x20);
}

const D64& C64::diskImage()
{
    if (drive1541.disk.dirty && drive.disk.present()) {
        drive1541.disk.toD64(drive.disk);
        drive1541.disk.dirty = false;
        drive.dirty = true;
    }
    return drive.disk;
}

void C64::setDriveMode(DriveMode mode)
{
    if (mode == driveMode_)
        return;
    diskImage(); // what the 1541 wrote goes back into the image first
    driveMode_ = mode;
    if (mode == DriveMode::True1541) {
        drive1541.disk.fromD64(drive.disk);
        drive1541.reset();
        driveDue = static_cast<double>(drive1541.clk());
    }
    drive.reset();
}

void C64::setStandard(VideoStandard std)
{
    vic.setStandard(std);
    const double clock = clockHz();
    cia1.cyclesPerPowerTick = cia2.cyclesPerPowerTick =
        static_cast<uint32_t>(clock / (std == VideoStandard::PAL ? 50.0 : 60.0));
    midi.cyclesPerFrame = static_cast<uint32_t>(clock / 3125.0 + 0.5);
    driveRatio_ = Drive1541::kClockHz / clock;
}

void C64::setMidiInterface(MidiInterface type)
{
    midi.type = type;
    midi.reset();
    herMidi.reset();
    herDue = static_cast<double>(herMidi.clk());
}

void C64::powerOn()
{
    // VICE's default pattern: 64 bytes of $00, then 64 of $FF.
    for (size_t i = 0; i < m.ram.size(); ++i)
        m.ram[i] = (i & 0x40) ? 0xff : 0x00;
    m.colorRam.fill(0);
    reset();
    booting_ = !cart.present();
    bootStartClk_ = m.clk;
}

void C64::reset()
{
    m.portDdr = 0;
    m.portData = 0;
    m.restoreCycles = 0;
    cia1.reset();
    cia2.reset();
    vic.reset();
    sids.reset(m.clk);
    midi.reset();
    cart.reset();
    drive.reset();
    // The serial bus's RESET line takes the 1541 along.
    drive1541.reset();
    driveDue = static_cast<double>(drive1541.clk());
    updateMap();
    updateVicBank();
    updateIec();
    cpu.reset();
}

void C64::updateMap()
{
    const uint8_t lines = static_cast<uint8_t>((m.portData | ~m.portDdr) & 0x07);
    const bool lo = lines & 1, hi = lines & 2, ch = lines & 4;
    const bool exrom = cart.exrom(), game = cart.game();
    ultimax_ = game && !exrom;
    map_.fill(RAM);
    if (ultimax_) {
        for (int p = 1; p < 8; ++p)
            map_[p] = OPEN;
        map_[0x8] = map_[0x9] = ROML;
        map_[0xa] = map_[0xb] = OPEN;
        map_[0xc] = OPEN;
        map_[0xd] = IO;
        map_[0xe] = map_[0xf] = ROMH;
    } else {
        if (exrom && lo && hi)
            map_[0x8] = map_[0x9] = ROML;
        if (exrom && game)
            map_[0xa] = map_[0xb] = hi ? ROMH : RAM;
        else if (lo && hi)
            map_[0xa] = map_[0xb] = BASIC;
        if (lo || hi)
            map_[0xd] = ch ? IO : CHAR;
        if (hi)
            map_[0xe] = map_[0xf] = KERNAL;
    }
    vic.ultimaxRom = ultimax_ ? cart.romh() : nullptr;
}

void C64::updateVicBank()
{
    vic.bankBase = static_cast<uint16_t>(((~cia2.portAOut()) & 3) << 14);
}

uint8_t C64::keyboardRows(uint8_t columns) const
{
    uint8_t rows = 0xff;
    for (int c = 0; c < 8; ++c)
        if (!(columns & (1 << c)))
            rows &= static_cast<uint8_t>(~m.keyMatrix[c]);
    return rows;
}

uint8_t C64::keyboardColumns(uint8_t rows) const
{
    uint8_t cols = 0xff;
    for (int c = 0; c < 8; ++c)
        if (m.keyMatrix[c] & ~rows)
            cols &= static_cast<uint8_t>(~(1 << c));
    return cols;
}

void C64::endCycle()
{
    cia1.tick();
    cia2.tick();
    midi.tick(m.clk);
    if (driveMode_ == DriveMode::True1541) {
        driveDue += driveRatio_;
        drive1541.runUntil(static_cast<uint64_t>(driveDue));
    }
    if (midi.type == MidiInterface::HerMIDI) {
        bool atn, clk, data;
        iecLines(atn, clk, data);
        herMidi.lineAtn = atn;
        herMidi.lineClk = clk;
        herMidi.lineData = data;
        herDue += driveRatio_; // the PIC also runs at 1 MHz
        herMidi.runUntil(static_cast<uint64_t>(herDue));
    }
    if (m.restoreCycles)
        --m.restoreCycles;
    m.clk++;
}

bool C64::irqLine() const
{
    return vic.irq() || cia1.irq() || midi.irqLine();
}

bool C64::nmiLine() const
{
    return cia2.irq() || m.restoreCycles > 0 || midi.nmiLine();
}

void C64::run(uint64_t target)
{
    while (m.clk < target) {
        cpu.step();
        drive.tickActivity();
    }
}

// ------------------------------------------------------------------- the bus

uint8_t C64::read(uint16_t addr)
{
    for (;;) {
        beginCycle();
        if (!vic.baLow())
            break;
        endCycle(); // the CPU is held off the bus this cycle
    }
    uint8_t v;
    if (addr < 2) {
        if (addr == 0)
            v = m.portDdr;
        else
            v = static_cast<uint8_t>((m.portData & m.portDdr) | (~m.portDdr & 0x17));
    } else {
        switch (map_[addr >> 12]) {
        case RAM: v = m.ram[addr]; break;
        case BASIC: v = roms_.basic[addr & 0x1fff]; break;
        case KERNAL: v = roms_.kernal[addr & 0x1fff]; break;
        case CHAR: v = roms_.chargen[addr & 0x0fff]; break;
        case IO: v = readIo(addr); break;
        case ROML: {
            const uint8_t* p = cart.roml();
            v = p ? p[addr & 0x1fff] : (ultimax_ ? vic.busByte() : m.ram[addr]);
            break;
        }
        case ROMH: {
            const uint8_t* p = cart.romh();
            v = p ? p[addr & 0x1fff] : (ultimax_ ? vic.busByte() : m.ram[addr]);
            break;
        }
        default: v = vic.busByte(); break;
        }
    }
    endCycle();
    return v;
}

void C64::write(uint16_t addr, uint8_t v)
{
    beginCycle();
    if (addr < 2) {
        if (addr == 0)
            m.portDdr = v;
        else
            m.portData = v;
        updateMap();
    } else {
        switch (map_[addr >> 12]) {
        case IO: writeIo(addr, v); break;
        case OPEN: break;
        case ROML:
        case ROMH:
            if (!ultimax_)
                m.ram[addr] = v;
            break;
        default: m.ram[addr] = v; break;
        }
    }
    endCycle();
}

uint8_t C64::peek(uint16_t addr) const
{
    if (addr < 2)
        return addr == 0 ? m.portDdr : static_cast<uint8_t>((m.portData & m.portDdr) | (~m.portDdr & 0x17));
    switch (map_[addr >> 12]) {
    case RAM: return m.ram[addr];
    case BASIC: return roms_.basic[addr & 0x1fff];
    case KERNAL: return roms_.kernal[addr & 0x1fff];
    case CHAR: return roms_.chargen[addr & 0x0fff];
    case IO: return peekIo(addr);
    case ROML: {
        const uint8_t* p = cart.roml();
        return p ? p[addr & 0x1fff] : m.ram[addr];
    }
    case ROMH: {
        const uint8_t* p = cart.romh();
        return p ? p[addr & 0x1fff] : m.ram[addr];
    }
    default: return 0xff;
    }
}

uint8_t C64::readIo(uint16_t addr)
{
    if (addr < 0xd400)
        return vic.read(static_cast<uint8_t>(addr));
    if (addr < 0xd800) {
        const int c = sids.chipAt(addr);
        return c >= 0 ? sids.read(c, static_cast<uint8_t>(addr), m.clk) : vic.busByte();
    }
    if (addr < 0xdc00)
        return static_cast<uint8_t>((m.colorRam[addr & 0x3ff] & 0x0f) | (vic.busByte() & 0xf0));
    if (addr < 0xdd00)
        return cia1.read(static_cast<uint8_t>(addr));
    if (addr < 0xde00)
        return cia2.read(static_cast<uint8_t>(addr));
    if (midi.decodes(addr))
        return midi.read(addr, m.clk);
    const int c = sids.chipAt(addr);
    if (c >= 0)
        return sids.read(c, static_cast<uint8_t>(addr), m.clk);
    uint8_t v;
    if (cart.readIo(addr, v))
        return v;
    return vic.busByte();
}

uint8_t C64::peekIo(uint16_t addr) const
{
    if (addr < 0xd400)
        return vic.peek(static_cast<uint8_t>(addr));
    if (addr < 0xd800) {
        const int c = sids.chipAt(addr);
        return c >= 0 ? sids.peek(c, static_cast<uint8_t>(addr)) : 0xff;
    }
    if (addr < 0xdc00)
        return m.colorRam[addr & 0x3ff] & 0x0f;
    if (addr < 0xdd00)
        return cia1.peek(static_cast<uint8_t>(addr));
    if (addr < 0xde00)
        return cia2.peek(static_cast<uint8_t>(addr));
    if (midi.decodes(addr))
        return midi.peek(addr);
    return 0xff;
}

void C64::writeIo(uint16_t addr, uint8_t v)
{
    if (addr < 0xd400) {
        vic.write(static_cast<uint8_t>(addr), v);
        return;
    }
    if (addr < 0xd800) {
        const int c = sids.chipAt(addr);
        if (c >= 0)
            sids.write(c, static_cast<uint8_t>(addr), v, m.clk);
        return;
    }
    if (addr < 0xdc00) {
        m.colorRam[addr & 0x3ff] = v & 0x0f;
        return;
    }
    if (addr < 0xdd00) {
        cia1.write(static_cast<uint8_t>(addr), v);
        return;
    }
    if (addr < 0xde00) {
        cia2.write(static_cast<uint8_t>(addr), v);
        return;
    }
    if (midi.decodes(addr)) {
        midi.write(addr, v, m.clk);
        return;
    }
    const int c = sids.chipAt(addr);
    if (c >= 0)
        sids.write(c, static_cast<uint8_t>(addr), v, m.clk);
    if (cart.writeIo(addr, v))
        updateMap();
}

// ------------------------------------------------------------------- traps

void C64::returnFromTrap()
{
    // RTS from the trapped routine, with interrupts enabled as the KERNAL's
    // serial routines leave them.
    auto& r = cpu.s;
    const uint8_t lo = m.ram[0x100 | static_cast<uint8_t>(r.sp + 1)];
    const uint8_t hi = m.ram[0x100 | static_cast<uint8_t>(r.sp + 2)];
    r.sp = static_cast<uint8_t>(r.sp + 2);
    r.pc = static_cast<uint16_t>((lo | (hi << 8)) + 1);
    r.p &= static_cast<uint8_t>(~0x04);
    // A few cycles pass, so a long load still lets the machine breathe.
    for (int i = 0; i < 6; ++i)
        read(r.pc);
}

bool C64::trap(uint16_t pc)
{
    if (booting_ && m.clk - bootStartClk_ > 4000000)
        booting_ = false;
    if (pc == kKeyWait && kernalVisible() && std::memcmp(&roms_.kernal[pc & 0x1fff], kKeyWaitSig, 3) == 0) {
        booting_ = false;
        if (autostart_ == Autostart::Prg) {
            autostart_ = Autostart::None;
            injectPrg(autostartPrg_);
            autostartPrg_.clear();
        } else if (autostart_ == Autostart::Disk) {
            autostart_ = Autostart::None;
            pendingTyping_ += "LOAD\"*\",8,1\rRUN\r";
        }
        serviceKeyboardBuffer();
        return false;
    }
    if (driveMode_ != DriveMode::Virtual || pc < 0xed09 || pc > 0xee13 || !kernalVisible())
        return false;
    const TrapSig* sig = nullptr;
    for (const auto& t : kTraps)
        if (t.pc == pc)
            sig = &t;
    if (!sig || std::memcmp(&roms_.kernal[pc & 0x1fff], sig->bytes, 3) != 0)
        return false;

    auto& r = cpu.s;
    uint8_t st = 0;
    switch (pc) {
    case 0xed09: st = drive.talk(r.a); break;
    case 0xed0c: st = drive.listen(r.a); break;
    case 0xedb9: st = drive.second(r.a); break;
    case 0xedc7: st = drive.tksa(r.a); break;
    case 0xeddd: st = drive.ciout(r.a); break;
    case 0xedef: st = drive.untalk(); break;
    case 0xedfe: st = drive.unlisten(); break;
    case 0xee13: {
        uint8_t b = 0;
        st = drive.acptr(b);
        r.a = b;
        r.p = static_cast<uint8_t>((r.p & ~0x82) | (b & 0x80) | (b ? 0 : 0x02));
        break;
    }
    default: return false;
    }
    m.ram[0x90] |= st;
    m.ram[0x94] = 0; // no byte buffered in CIOUT
    r.p &= static_cast<uint8_t>(~0x01);
    returnFromTrap();
    return true;
}

bool C64::serviceKeyboardBuffer()
{
    if (pendingTyping_.empty())
        return false;
    // Only into an empty buffer, and never past a RETURN: what follows waits
    // until BASIC has executed the line and asks again, so nothing is left
    // in the buffer for a program that starts itself.
    uint8_t count = m.ram[0xc6];
    if (count != 0)
        return false;
    while (count < 10 && !pendingTyping_.empty()) {
        m.ram[0x277 + count] = static_cast<uint8_t>(pendingTyping_.front());
        pendingTyping_.erase(0, 1);
        ++count;
        // A RETURN goes alone: BASIC must execute the line before the next is typed.
        if (m.ram[0x277 + count - 1] == 0x0d)
            break;
    }
    m.ram[0xc6] = count;
    return true;
}

void C64::typeText(const std::string& petscii)
{
    pendingTyping_ += petscii;
}

void C64::injectPrg(const std::vector<uint8_t>& prg)
{
    if (prg.size() < 3)
        return;
    const uint16_t load = static_cast<uint16_t>(prg[0] | (prg[1] << 8));
    size_t n = prg.size() - 2;
    if (load + n > 0x10000)
        n = 0x10000 - load;
    std::memcpy(&m.ram[load], &prg[2], n);
    const uint16_t end = static_cast<uint16_t>(load + n);
    for (uint16_t p : {0x2d, 0x2f, 0x31, 0xae}) {
        m.ram[p] = static_cast<uint8_t>(end);
        m.ram[p + 1] = static_cast<uint8_t>(end >> 8);
    }
    if (load == 0x0801)
        pendingTyping_ += "RUN\r";
    else
        pendingTyping_ += "SYS" + std::to_string(load) + "\r";
}

// ------------------------------------------------------------------- media

bool C64::attachPrg(const std::vector<uint8_t>& prg, bool autostart, std::string& error)
{
    if (prg.size() < 3) {
        error = "the .prg file is empty";
        return false;
    }
    if (autostart) {
        autostart_ = Autostart::Prg;
        autostartPrg_ = prg;
        powerOn();
    } else {
        injectPrg(prg);
        pendingTyping_.clear();
    }
    return true;
}

bool C64::attachDisk(const std::vector<uint8_t>& d64, bool autostart, std::string& error)
{
    if (!drive.disk.load(d64, error))
        return false;
    drive.dirty = false;
    drive.reset();
    drive1541.disk.fromD64(drive.disk);
    if (autostart) {
        autostart_ = Autostart::Disk;
        powerOn();
    }
    return true;
}

void C64::ejectDisk()
{
    drive.disk.eject();
    drive.dirty = false;
    drive1541.disk.clear();
}

bool C64::attachCartridge(const std::vector<uint8_t>& data, const std::string& name, std::string& error)
{
    if (!cart.load(data, name, error))
        return false;
    autostart_ = Autostart::None;
    powerOn();
    return true;
}

void C64::ejectCartridge()
{
    cart.eject();
    powerOn();
}

// ------------------------------------------------------------------- input

void C64::setKey(uint8_t pos, bool down)
{
    if (pos == keys::None)
        return;
    const uint8_t bit = static_cast<uint8_t>(1 << (pos & 7));
    if (down)
        m.keyMatrix[pos >> 3] |= bit;
    else
        m.keyMatrix[pos >> 3] &= static_cast<uint8_t>(~bit);
}

void C64::releaseAllKeys()
{
    m.keyMatrix.fill(0);
}

void C64::pressRestore()
{
    m.restoreCycles = 2000;
}

void C64::setJoystick(int port, uint8_t bits)
{
    m.joy[port & 1] = bits & 0x1f;
}

void C64::updatePots()
{
    // PA6 selects port 1, PA7 port 2 (the 4066 switches). Both closed: the
    // lines are connected together and the lower resistance wins -- roughly
    // the larger value; none: the lines float high.
    const uint8_t sel = cia1.portAOut() & 0xc0;
    uint8_t x = 0xff, y = 0xff;
    if (sel == 0x40) {
        x = m.potX[0];
        y = m.potY[0];
    } else if (sel == 0x80) {
        x = m.potX[1];
        y = m.potY[1];
    } else if (sel == 0xc0) {
        x = std::max(m.potX[0], m.potX[1]);
        y = std::max(m.potY[0], m.potY[1]);
    }
    sids.setPots(x, y);
}

void C64::setPortDevice(int port, PortDevice d)
{
    port &= 1;
    m.device[port] = d;
    m.devButtons[port] = 0;
    m.potX[port] = m.potY[port] = 0xff;
    if (d == PortDevice::Mouse1351)
        m.potX[port] = m.potY[port] = 0;
    updatePots();
}

void C64::moveMouse(int port, int dx, int dy)
{
    port &= 1;
    if (m.device[port] != PortDevice::Mouse1351)
        return;
    // The 1351 reports its position modulo 64 in bits 1-6 of each POT
    // register; up is positive on its Y axis.
    m.mouseX[port] = static_cast<uint16_t>(m.mouseX[port] + dx);
    m.mouseY[port] = static_cast<uint16_t>(m.mouseY[port] - dy);
    m.potX[port] = static_cast<uint8_t>((m.mouseX[port] & 0x3f) << 1);
    m.potY[port] = static_cast<uint8_t>((m.mouseY[port] & 0x3f) << 1);
    updatePots();
}

void C64::setPaddles(int port, uint8_t a, uint8_t b)
{
    port &= 1;
    if (m.device[port] != PortDevice::Paddles)
        return;
    m.potX[port] = a;
    m.potY[port] = b;
    updatePots();
}

void C64::setDeviceButtons(int port, uint8_t bits)
{
    m.devButtons[port & 1] = bits & 0x1f;
}

} // namespace sidmidi::emu
