#include "Snapshot.h"

#include <cstring>
#include <memory>
#include <vector>
#include <string>
#include <type_traits>

#include "C64.h"

namespace sidmidi::emu {

namespace {

constexpr char kMagic[8] = {'S', 'M', 'S', 'N', 'A', 'P', '0', '1'};

class Writer
{
public:
    std::vector<uint8_t> out;
    template <class T> void pod(const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        u32(static_cast<uint32_t>(sizeof(T)));
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void u32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u64(uint64_t v)
    {
        u32(static_cast<uint32_t>(v));
        u32(static_cast<uint32_t>(v >> 32));
    }
    void blob(const void* data, size_t n)
    {
        u32(static_cast<uint32_t>(n));
        const auto* p = static_cast<const uint8_t*>(data);
        out.insert(out.end(), p, p + n);
    }
    void bytes(const std::vector<uint8_t>& v) { blob(v.data(), v.size()); }
    void str(const std::string& s) { blob(s.data(), s.size()); }
};

class Reader
{
public:
    Reader(const std::vector<uint8_t>& d) : d_(d) {}
    bool ok = true;
    template <class T> void pod(T& v)
    {
        if (u32() != sizeof(T) || !have(sizeof(T))) {
            ok = false;
            return;
        }
        std::memcpy(&v, &d_[at_], sizeof(T));
        at_ += sizeof(T);
    }
    uint32_t u32()
    {
        if (!have(4)) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(d_[at_ + i]) << (8 * i);
        at_ += 4;
        return v;
    }
    uint64_t u64()
    {
        const uint64_t lo = u32();
        return lo | (static_cast<uint64_t>(u32()) << 32);
    }
    std::vector<uint8_t> bytes()
    {
        const uint32_t n = u32();
        if (!have(n)) {
            ok = false;
            return {};
        }
        std::vector<uint8_t> v(d_.begin() + static_cast<long>(at_), d_.begin() + static_cast<long>(at_ + n));
        at_ += n;
        return v;
    }
    std::string str()
    {
        const auto v = bytes();
        return {v.begin(), v.end()};
    }
    bool magic()
    {
        if (!have(8) || std::memcmp(&d_[at_], kMagic, 8) != 0)
            return false;
        at_ += 8;
        return true;
    }

    bool atEnd() const { return at_ >= d_.size(); }

private:
    bool have(size_t n) const { return ok && at_ + n <= d_.size(); }
    const std::vector<uint8_t>& d_;
    size_t at_ = 0;
};

} // namespace

std::vector<uint8_t> saveSnapshot(const C64& c)
{
    Writer w;
    w.out.insert(w.out.end(), kMagic, kMagic + 8);
    w.u32(static_cast<uint32_t>(c.standard()));
    w.pod(c.cpu.s);
    w.pod(c.m);
    w.pod(c.vic.s);
    w.pod(c.cia1.s);
    w.pod(c.cia2.s);
    w.u32(static_cast<uint32_t>(c.midi.type));
    w.pod(c.midi.s);
    w.u32(static_cast<uint32_t>(c.midi.rxQueue().size()));
    for (size_t i = 0; i < c.midi.rxQueue().size(); ++i) {
        const auto& b = c.midi.rxQueue()[i];
        w.u64(b.clk);
        w.u32(b.byte);
    }
    // Cartridge: the image and its live state.
    w.bytes(c.cart.image());
    w.str(c.cart.name());
    w.pod(c.cart.s);
    // Drive.
    const auto& d = c.drive;
    w.bytes(d.disk.data());
    w.u32(d.dirty ? 1 : 0);
    w.u32(d.listening | (d.talking << 1) | (d.opening << 2));
    w.u32(static_cast<uint32_t>(d.channel));
    w.str(d.openName);
    w.str(d.status_);
    w.u32(static_cast<uint32_t>(d.statusPos));
    w.str(d.command_);
    for (const auto& ch : d.channels) {
        w.u32(ch.open | (ch.writing << 1) | (ch.command << 2) | (ch.replace << 3));
        w.str(ch.name);
        w.u32(ch.type);
        w.bytes(ch.data);
        w.u32(static_cast<uint32_t>(ch.pos));
    }
    // SIDs: their registers (always usable) and reSIDfp's state (this build only).
    w.u32(static_cast<uint32_t>(c.sids.count()));
    for (int i = 0; i < c.sids.count(); ++i)
        for (int r = 0; r < 32; ++r)
            w.u32(c.sids.lastWritten(i, static_cast<uint8_t>(r)));
    const std::vector<char> sidState = c.sids.saveState();
    w.blob(sidState.data(), sidState.size());
    w.u32(static_cast<uint32_t>(c.autostart_));
    w.bytes(c.autostartPrg_);
    w.str(c.pendingTyping_);
    w.u32(c.booting_ ? 1 : 0);
    w.u64(c.bootStartClk_);
    // The 1541: mode, CPU, RAM and mechanics, VIAs, and its disk as GCR.
    w.u32(static_cast<uint32_t>(c.driveMode()));
    w.pod(c.drive1541.cpu.s);
    w.pod(c.drive1541.s);
    w.pod(c.drive1541.via1.s);
    w.pod(c.drive1541.via2.s);
    w.pod(c.driveDue);
    const bool gcr = c.driveMode() == DriveMode::True1541 && c.drive1541.disk.present;
    w.u32(gcr ? 1 : 0);
    if (gcr) {
        w.u32(c.drive1541.disk.dirty ? 1 : 0);
        for (int t = 1; t <= GcrDisk::kMaxTracks; ++t)
            w.bytes(c.drive1541.disk.tracks[static_cast<size_t>(t)]);
    }
    // HerMIDI: the PIC, the byte on the cable and the ones waiting.
    w.pod(c.herMidi.pic.s);
    w.pod(c.herMidi.st);
    w.pod(c.herDue);
    w.u32(static_cast<uint32_t>(c.herMidi.queue().size()));
    for (size_t i = 0; i < c.herMidi.queue().size(); ++i) {
        const auto& q = c.herMidi.queue()[i];
        w.u64(q.at);
        w.u32(q.byte);
    }
    // Appended in 0.5.0 (a snapshot without it reads with the defaults): the
    // freezer cartridges' register, RAM and NMI.
    w.pod(c.cart.x);
    w.bytes(c.cart.ram());
    return w.out;
}

bool loadSnapshot(C64& c, const std::vector<uint8_t>& data)
{
    Reader r(data);
    if (!r.magic())
        return false;
    const auto std = static_cast<VideoStandard>(r.u32());

    // Read everything into temporaries first: a snapshot that fails halfway
    // must leave the running machine untouched.
    CpuState cpu;
    auto m = std::make_unique<MachineState>();
    VicState vic;
    CiaState cia1, cia2;
    r.pod(cpu);
    r.pod(*m);
    r.pod(vic);
    r.pod(cia1);
    r.pod(cia2);
    const auto midiType = static_cast<MidiInterface>(r.u32());
    MidiCartState midi;
    r.pod(midi);
    std::vector<MidiByte> rx;
    const uint32_t nrx = r.u32();
    for (uint32_t i = 0; i < nrx && r.ok; ++i) {
        MidiByte b;
        b.clk = r.u64();
        b.byte = static_cast<uint8_t>(r.u32());
        if (rx.size() < MidiCart::RxQueue::capacity())
            rx.push_back(b);
    }
    const auto cartImage = r.bytes();
    const auto cartName = r.str();
    CartState cartState;
    r.pod(cartState);
    const auto disk = r.bytes();
    const bool dirty = r.u32() != 0;
    const uint32_t busFlags = r.u32();
    const int channel = static_cast<int>(r.u32());
    const auto openName = r.str();
    const auto status = r.str();
    const size_t statusPos = r.u32();
    const auto command = r.str();
    std::array<VirtualDrive::Channel, 16> channels;
    for (auto& ch : channels) {
        const uint32_t f = r.u32();
        ch.open = f & 1;
        ch.writing = f & 2;
        ch.command = f & 4;
        ch.replace = f & 8;
        ch.name = r.str();
        ch.type = static_cast<uint8_t>(r.u32());
        ch.data = r.bytes();
        ch.pos = r.u32();
    }
    const uint32_t nSids = r.u32();
    std::vector<std::array<uint8_t, 32>> regs(nSids);
    for (uint32_t i = 0; i < nSids && r.ok && i < 8; ++i)
        for (int k = 0; k < 32; ++k)
            regs[i][k] = static_cast<uint8_t>(r.u32());
    const auto sidBytes = r.bytes();
    const auto autostart = static_cast<C64::Autostart>(r.u32());
    const auto autostartPrg = r.bytes();
    const auto typing = r.str();
    const bool booting = r.u32() != 0;
    const uint64_t bootStart = r.u64();
    const auto driveMode = static_cast<DriveMode>(r.u32());
    CpuState driveCpu;
    auto driveState = std::make_unique<DriveState>();
    ViaState via1, via2;
    double driveDue = 0;
    r.pod(driveCpu);
    r.pod(*driveState);
    r.pod(via1);
    r.pod(via2);
    r.pod(driveDue);
    GcrDisk gcr;
    const bool hasGcr = r.u32() != 0;
    if (hasGcr) {
        gcr.present = true;
        gcr.dirty = r.u32() != 0;
        for (int t = 1; t <= GcrDisk::kMaxTracks; ++t)
            gcr.tracks[static_cast<size_t>(t)] = r.bytes();
    }
    PicState pic;
    HerMidiState herState;
    double herDue = 0;
    r.pod(pic);
    r.pod(herState);
    r.pod(herDue);
    std::vector<HerMidi::Queued> herQueue;
    const uint32_t nq = r.u32();
    for (uint32_t i = 0; i < nq && r.ok; ++i) {
        HerMidi::Queued q;
        q.at = r.u64();
        q.byte = static_cast<uint8_t>(r.u32());
        if (herQueue.size() < HerMidi::Queue::capacity())
            herQueue.push_back(q);
    }
    if (!r.ok)
        return false;
    CartFreezerState freezer;
    std::vector<uint8_t> cartRam;
    if (!r.atEnd()) {
        r.pod(freezer);
        cartRam = r.bytes();
        if (!r.ok)
            return false;
    }

    // Commit.
    if (c.standard() != std)
        c.setStandard(std);
    std::string err;
    if (!cartImage.empty()) {
        if (!c.cart.load(cartImage, cartName, err))
            return false;
    } else {
        c.cart.eject();
    }
    c.cart.s = cartState;
    c.cart.x = freezer;
    if (cartRam.size() == c.cart.ram().size())
        c.cart.ram() = cartRam;
    if (!disk.empty()) {
        if (!c.drive.disk.load(disk, err))
            return false;
    } else {
        c.drive.disk.eject();
    }
    c.drive.dirty = dirty;
    c.drive.listening = busFlags & 1;
    c.drive.talking = busFlags & 2;
    c.drive.opening = busFlags & 4;
    c.drive.channel = channel;
    c.drive.openName = openName;
    c.drive.status_ = status;
    c.drive.statusPos = statusPos;
    c.drive.command_ = command;
    c.drive.channels = channels;

    c.cpu.s = cpu;
    c.m = *m;
    c.vic.s = vic;
    c.cia1.s = cia1;
    c.cia2.s = cia2;
    c.midi.type = midiType;
    c.midi.s = midi;
    c.midi.rxQueue().clear();
    for (const auto& b : rx)
        c.midi.rxQueue().push_back(b);
    c.autostart_ = autostart;
    c.autostartPrg_ = autostartPrg;
    c.pendingTyping_ = typing;
    c.booting_ = booting;
    c.bootStartClk_ = bootStart;
    c.setDriveMode(driveMode); // builds the GCR from the image; replaced below when stored
    if (hasGcr)
        c.drive1541.disk = gcr;
    c.drive1541.cpu.s = driveCpu;
    c.drive1541.s = *driveState;
    c.drive1541.via1.s = via1;
    c.drive1541.via2.s = via2;
    c.driveDue = driveDue;
    c.herMidi.pic.s = pic;
    c.herMidi.st = herState;
    c.herMidi.queue().clear();
    for (const auto& q : herQueue)
        c.herMidi.queue().push_back(q);
    c.herDue = herDue;
    c.restoreDerivedState();

    // SIDs: the exact state when this build wrote it, else a reset with the
    // registers written back (envelopes restart; the sound settles at once).
    std::vector<char> sidState(sidBytes.begin(), sidBytes.end());
    if (static_cast<int>(nSids) != c.sids.count() || !c.sids.restoreState(sidState, c.clk())) {
        c.sids.reset(c.clk());
        for (int i = 0; i < c.sids.count() && i < static_cast<int>(nSids); ++i)
            for (int k = 0; k < 25; ++k)
                c.sids.write(i, static_cast<uint8_t>(k), regs[static_cast<size_t>(i)][static_cast<size_t>(k)], c.clk());
    } else {
        for (int i = 0; i < c.sids.count(); ++i)
            for (int k = 0; k < 32; ++k)
                c.sids.setLastWritten(i, static_cast<uint8_t>(k), regs[static_cast<size_t>(i)][static_cast<size_t>(k)]);
    }
    return true;
}

} // namespace sidmidi::emu
