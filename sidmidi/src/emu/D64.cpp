#include "D64.h"

#include <cstring>

namespace sidmidi::emu {

int D64::sectorsIn(int track)
{
    if (track <= 17)
        return 21;
    if (track <= 24)
        return 19;
    if (track <= 30)
        return 18;
    return 17;
}

size_t D64::offset(int track, int sec) const
{
    size_t off = 0;
    for (int t = 1; t < track; ++t)
        off += static_cast<size_t>(sectorsIn(t)) * 256;
    return off + static_cast<size_t>(sec) * 256;
}

uint8_t* D64::sector(int track, int sec)
{
    if (track < 1 || track > tracks_ || sec < 0 || sec >= sectorsIn(track))
        return nullptr;
    return data_.data() + offset(track, sec);
}

const uint8_t* D64::sector(int track, int sec) const
{
    return const_cast<D64*>(this)->sector(track, sec);
}

bool D64::load(const std::vector<uint8_t>& data, std::string& error)
{
    // Images with an error-info block (one byte per sector) are accepted;
    // the error bytes are dropped.
    if (data.size() == kSize35 || data.size() == kSize35 + 683) {
        tracks_ = 35;
        data_.assign(data.begin(), data.begin() + kSize35);
        return true;
    }
    if (data.size() == kSize40 || data.size() == kSize40 + 768) {
        tracks_ = 40;
        data_.assign(data.begin(), data.begin() + kSize40);
        return true;
    }
    error = "not a .d64 image (expected 174848 or 196608 bytes)";
    return false;
}

void D64::format(const std::string& name, const std::string& id)
{
    tracks_ = 35;
    data_.assign(kSize35, 0);
    uint8_t* bam = sector(18, 0);
    bam[0] = 18;
    bam[1] = 1;
    bam[2] = 0x41; // 'A'
    for (int t = 1; t <= 35; ++t)
        for (int s = 0; s < sectorsIn(t); ++s)
            setBam(t, s, true);
    std::memset(bam + 0x90, 0xa0, 0x1b);
    for (size_t i = 0; i < name.size() && i < 16; ++i)
        bam[0x90 + i] = static_cast<uint8_t>(name[i]);
    bam[0xa2] = id.size() > 0 ? static_cast<uint8_t>(id[0]) : '0';
    bam[0xa3] = id.size() > 1 ? static_cast<uint8_t>(id[1]) : '0';
    bam[0xa5] = '2';
    bam[0xa6] = 'A';
    setBam(18, 0, false);
    setBam(18, 1, false);
    uint8_t* dir = sector(18, 1);
    dir[0] = 0;
    dir[1] = 0xff;
}

std::string D64::diskName() const
{
    std::string s;
    const uint8_t* bam = sector(18, 0);
    if (!bam)
        return s;
    for (int i = 0; i < 16; ++i)
        s.push_back(static_cast<char>(bam[0x90 + i]));
    return s;
}

std::string D64::diskId() const
{
    const uint8_t* bam = sector(18, 0);
    if (!bam)
        return "  ";
    return {static_cast<char>(bam[0xa2]), static_cast<char>(bam[0xa3]), static_cast<char>(0xa0),
            static_cast<char>(bam[0xa5]), static_cast<char>(bam[0xa6])};
}

std::vector<DirEntry> D64::directory() const
{
    std::vector<DirEntry> out;
    int t = 18, s = 1;
    int guard = 0;
    while (t != 0 && guard++ < 40) {
        const uint8_t* sec = sector(t, s);
        if (!sec)
            break;
        for (int i = 0; i < 8; ++i) {
            const uint8_t* e = sec + i * 32;
            if (e[2] == 0)
                continue;
            DirEntry d;
            d.type = e[2];
            d.track = e[3];
            d.sector = e[4];
            for (int k = 0; k < 16 && e[5 + k] != 0xa0; ++k)
                d.name.push_back(static_cast<char>(e[5 + k]));
            d.blocks = static_cast<uint16_t>(e[0x1e] | (e[0x1f] << 8));
            d.slotTrack = t;
            d.slotSector = s;
            d.slotOffset = i * 32;
            out.push_back(d);
        }
        t = sec[0];
        s = sec[1];
    }
    return out;
}

int D64::blocksFree() const
{
    const uint8_t* bam = sector(18, 0);
    if (!bam)
        return 0;
    int n = 0;
    for (int t = 1; t <= 35; ++t)
        if (t != 18)
            n += bam[4 * t];
    return n;
}

bool D64::matches(const std::string& pattern, const std::string& name)
{
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '*')
            return true;
        if (i >= name.size())
            return false;
        if (pattern[i] != '?' && pattern[i] != name[i])
            return false;
    }
    return pattern.size() == name.size();
}

bool D64::find(const std::string& pattern, DirEntry& out, uint8_t typeWanted) const
{
    for (const auto& e : directory()) {
        if (!(e.type & 0x80))
            continue; // a file left open (splat) is not loadable
        if (typeWanted && (e.type & 0x07) != typeWanted)
            continue;
        if (matches(pattern, e.name)) {
            out = e;
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> D64::readFile(const DirEntry& e) const
{
    std::vector<uint8_t> out;
    int t = e.track, s = e.sector;
    int guard = 0;
    while (t != 0 && guard++ < 800) {
        const uint8_t* sec = sector(t, s);
        if (!sec)
            break;
        if (sec[0] == 0) {
            const int last = sec[1];
            for (int i = 2; i <= last && i < 256; ++i)
                out.push_back(sec[i]);
            break;
        }
        out.insert(out.end(), sec + 2, sec + 256);
        t = sec[0];
        s = sec[1];
    }
    return out;
}

bool D64::isFree(int track, int sec) const
{
    const uint8_t* bam = sector(18, 0);
    return (bam[4 * track + 1 + sec / 8] >> (sec % 8)) & 1;
}

void D64::setBam(int track, int sec, bool free)
{
    if (track > 35)
        return; // the extra tracks of a 40-track image have no standard BAM
    uint8_t* bam = sector(18, 0);
    uint8_t& byte = bam[4 * track + 1 + sec / 8];
    const uint8_t bit = static_cast<uint8_t>(1 << (sec % 8));
    const bool was = byte & bit;
    if (free && !was) {
        byte |= bit;
        bam[4 * track]++;
    } else if (!free && was) {
        byte &= ~bit;
        bam[4 * track]--;
    }
}

void D64::freeBlock(int track, int sec)
{
    setBam(track, sec, true);
}

// The 1541's strategy: tracks outward from the directory track, sectors with
// an interleave of 10 within a track.
bool D64::allocate(int& track, int& sec, int nearTrack, int nearSector)
{
    auto tryTrack = [&](int t, int start) {
        const int n = sectorsIn(t);
        for (int k = 0; k < n; ++k) {
            const int s = (start + k) % n;
            if (isFree(t, s)) {
                setBam(t, s, false);
                track = t;
                sec = s;
                return true;
            }
        }
        return false;
    };
    if (nearTrack > 0 && nearTrack != 18 && tryTrack(nearTrack, (nearSector + 10) % sectorsIn(nearTrack)))
        return true;
    for (int d = 1; d < 18; ++d) {
        if (18 - d >= 1 && tryTrack(18 - d, 0))
            return true;
        if (18 + d <= 35 && tryTrack(18 + d, 0))
            return true;
    }
    return false;
}

bool D64::freeDirSlot(int& t, int& s, int& off)
{
    int ct = 18, cs = 1;
    int guard = 0;
    while (guard++ < 40) {
        uint8_t* sec = sector(ct, cs);
        for (int i = 0; i < 8; ++i) {
            if (sec[i * 32 + 2] == 0) {
                t = ct;
                s = cs;
                off = i * 32;
                return true;
            }
        }
        if (sec[0] == 0) {
            // Extend the directory on track 18.
            for (int ns = 1; ns < sectorsIn(18); ++ns) {
                if (isFree(18, ns)) {
                    setBam(18, ns, false);
                    sec[0] = 18;
                    sec[1] = static_cast<uint8_t>(ns);
                    uint8_t* fresh = sector(18, ns);
                    std::memset(fresh, 0, 256);
                    fresh[1] = 0xff;
                    t = 18;
                    s = ns;
                    off = 0;
                    return true;
                }
            }
            return false;
        }
        ct = sec[0];
        cs = sec[1];
    }
    return false;
}

int D64::writeFile(const std::string& name, uint8_t type, const std::vector<uint8_t>& bytes, bool replace)
{
    DirEntry existing;
    if (find(name, existing)) {
        if (!replace)
            return 63;
        scratch(name);
    }
    const size_t blocks = bytes.empty() ? 1 : (bytes.size() + 253) / 254;
    if (static_cast<size_t>(blocksFree()) < blocks)
        return 72;
    int dt, ds, doff;
    if (!freeDirSlot(dt, ds, doff))
        return 72;

    std::vector<std::pair<int, int>> chain;
    int t = 0, s = 0;
    for (size_t b = 0; b < blocks; ++b) {
        if (!allocate(t, s, t, s)) {
            for (auto& c : chain)
                freeBlock(c.first, c.second);
            return 72;
        }
        chain.emplace_back(t, s);
    }
    for (size_t b = 0; b < blocks; ++b) {
        uint8_t* sec = sector(chain[b].first, chain[b].second);
        std::memset(sec, 0, 256);
        const size_t from = b * 254;
        const size_t n = std::min<size_t>(254, bytes.size() - std::min(bytes.size(), from));
        if (n)
            std::memcpy(sec + 2, bytes.data() + from, n);
        if (b + 1 < blocks) {
            sec[0] = static_cast<uint8_t>(chain[b + 1].first);
            sec[1] = static_cast<uint8_t>(chain[b + 1].second);
        } else {
            sec[0] = 0;
            sec[1] = static_cast<uint8_t>(n + 1);
        }
    }
    uint8_t* e = sector(dt, ds) + doff;
    e[2] = static_cast<uint8_t>(0x80 | (type & 0x07));
    e[3] = static_cast<uint8_t>(chain[0].first);
    e[4] = static_cast<uint8_t>(chain[0].second);
    std::memset(e + 5, 0xa0, 16);
    for (size_t i = 0; i < name.size() && i < 16; ++i)
        e[5 + i] = static_cast<uint8_t>(name[i]);
    std::memset(e + 0x15, 0, 9);
    e[0x1e] = static_cast<uint8_t>(blocks & 0xff);
    e[0x1f] = static_cast<uint8_t>(blocks >> 8);
    return 0;
}

int D64::scratch(const std::string& pattern)
{
    int n = 0;
    for (const auto& e : directory()) {
        if (!matches(pattern, e.name) || (e.type & 0x40))
            continue; // locked files stay
        int t = e.track, s = e.sector;
        int guard = 0;
        while (t != 0 && guard++ < 800) {
            const uint8_t* sec = sector(t, s);
            if (!sec)
                break;
            freeBlock(t, s);
            t = sec[0];
            s = sec[1];
        }
        sector(e.slotTrack, e.slotSector)[e.slotOffset + 2] = 0;
        ++n;
    }
    return n;
}

int D64::rename(const std::string& from, const std::string& to)
{
    DirEntry e, clash;
    if (find(to, clash))
        return 63;
    if (!find(from, e))
        return 62;
    uint8_t* slot = sector(e.slotTrack, e.slotSector) + e.slotOffset;
    std::memset(slot + 5, 0xa0, 16);
    for (size_t i = 0; i < to.size() && i < 16; ++i)
        slot[5 + i] = static_cast<uint8_t>(to[i]);
    return 0;
}

} // namespace sidmidi::emu
