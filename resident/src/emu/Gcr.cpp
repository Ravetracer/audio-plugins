#include "Gcr.h"

#include <cstring>

namespace resident::emu {

namespace {

constexpr uint8_t kToGcr[16] = {0x0a, 0x0b, 0x12, 0x13, 0x0e, 0x0f, 0x16, 0x17,
                                0x09, 0x19, 0x1a, 0x1b, 0x0d, 0x1d, 0x1e, 0x15};

struct FromGcr
{
    int8_t t[32];
    FromGcr()
    {
        for (auto& v : t)
            v = -1;
        for (int i = 0; i < 16; ++i)
            t[kToGcr[i]] = static_cast<int8_t>(i);
    }
};
const FromGcr kFromGcr;

// Four bytes to five GCR bytes.
void encode4(const uint8_t* in, uint8_t* out)
{
    uint64_t bits = 0;
    for (int i = 0; i < 4; ++i)
        bits = (bits << 10) | (static_cast<uint64_t>(kToGcr[in[i] >> 4]) << 5) | kToGcr[in[i] & 0x0f];
    for (int i = 0; i < 5; ++i)
        out[i] = static_cast<uint8_t>(bits >> (32 - 8 * i));
}

// Five GCR bytes to four. False on an invalid code.
bool decode5(const uint8_t* in, uint8_t* out)
{
    uint64_t bits = 0;
    for (int i = 0; i < 5; ++i)
        bits = (bits << 8) | in[i];
    for (int i = 0; i < 4; ++i) {
        const int hi = kFromGcr.t[(bits >> (35 - 10 * i)) & 0x1f];
        const int lo = kFromGcr.t[(bits >> (30 - 10 * i)) & 0x1f];
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void appendGcr(std::vector<uint8_t>& t, const uint8_t* raw, size_t n)
{
    uint8_t out[5];
    for (size_t i = 0; i < n; i += 4) {
        encode4(raw + i, out);
        t.insert(t.end(), out, out + 5);
    }
}

} // namespace

void GcrDisk::fromD64(const D64& d64)
{
    clear();
    if (!d64.present())
        return;
    const uint8_t* bam = d64.sector(18, 0);
    const uint8_t id1 = bam[0xa2], id2 = bam[0xa3];
    for (int t = 1; t <= d64.tracks(); ++t) {
        const int n = D64::sectorsIn(t);
        const size_t size = trackBytes(t);
        constexpr size_t perSector = 5 + 10 + 9 + 5 + 325;
        const size_t gap = (size - n * perSector) / n;
        auto& tr = tracks[t];
        tr.reserve(size);
        for (int s = 0; s < n; ++s) {
            tr.insert(tr.end(), 5, 0xff);
            const uint8_t header[8] = {0x08, static_cast<uint8_t>(s ^ t ^ id2 ^ id1), static_cast<uint8_t>(s),
                                       static_cast<uint8_t>(t), id2, id1, 0x0f, 0x0f};
            appendGcr(tr, header, 8);
            tr.insert(tr.end(), 9, 0x55);
            tr.insert(tr.end(), 5, 0xff);
            uint8_t block[260];
            block[0] = 0x07;
            const uint8_t* data = d64.sector(t, s);
            std::memcpy(block + 1, data, 256);
            uint8_t sum = 0;
            for (int i = 0; i < 256; ++i)
                sum ^= data[i];
            block[257] = sum;
            block[258] = block[259] = 0;
            appendGcr(tr, block, 260);
            tr.insert(tr.end(), gap, 0x55);
        }
        tr.resize(size, 0x55);
    }
    present = true;
    dirty = false;
}

int GcrDisk::toD64(D64& d64) const
{
    if (!present || !d64.present())
        return 0;
    int found = 0;
    for (int t = 1; t <= d64.tracks(); ++t) {
        const auto& tr = tracks[t];
        const size_t n = tr.size();
        if (n < 400)
            continue;
        auto at = [&](size_t i) { return tr[i % n]; };
        auto copy = [&](size_t from, uint8_t* dst, size_t len) {
            for (size_t i = 0; i < len; ++i)
                dst[i] = at(from + i);
        };
        // One lap is enough; a header found near the end wraps around.
        int pendingSector = -1;
        for (size_t i = 1; i < n + 1; ++i) {
            if (at(i - 1) != 0xff || at(i) == 0xff)
                continue; // the first byte after a sync
            uint8_t gcr[325];
            copy(i, gcr, 10);
            uint8_t raw[8];
            if (decode5(gcr, raw) && decode5(gcr + 5, raw + 4) && raw[0] == 0x08) {
                if (raw[3] == t && raw[2] < D64::sectorsIn(t))
                    pendingSector = raw[2];
                continue;
            }
            if (pendingSector < 0)
                continue;
            copy(i, gcr, 325);
            uint8_t block[260];
            bool ok = true;
            for (int k = 0; k < 65 && ok; ++k)
                ok = decode5(gcr + 5 * k, block + 4 * k);
            if (ok && block[0] == 0x07) {
                uint8_t sum = 0;
                for (int k = 1; k <= 256; ++k)
                    sum ^= block[k];
                if (sum == block[257]) {
                    std::memcpy(d64.sector(t, pendingSector), block + 1, 256);
                    ++found;
                }
            }
            pendingSector = -1;
        }
    }
    return found;
}

} // namespace resident::emu
