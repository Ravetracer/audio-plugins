#include "AudioFile.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

#include "util/Path.h"

namespace aurum {

namespace {

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// 80-bit IEEE extended (AIFF sample rate).
double extended(const uint8_t* p)
{
    const int exponent = ((p[0] & 0x7f) << 8) | p[1];
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i)
        mantissa = (mantissa << 8) | p[2 + i];
    if (exponent == 0 && mantissa == 0)
        return 0.0;
    const double v = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return (p[0] & 0x80) ? -v : v;
}

float sampleFrom(const uint8_t* p, int bits, bool isFloat, bool bigEndian)
{
    if (isFloat)
    {
        if (bits == 32)
        {
            uint32_t u = bigEndian ? be32(p) : le32(p);
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i)
            u |= static_cast<uint64_t>(p[bigEndian ? 7 - i : i]) << (8 * i);
        double d;
        std::memcpy(&d, &u, 8);
        return static_cast<float>(d);
    }
    const int bytes = bits / 8;
    int64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v |= static_cast<int64_t>(p[bigEndian ? bytes - 1 - i : i]) << (8 * i);
    if (bits == 8 && !bigEndian)
        return (static_cast<float>(v) - 128.0f) / 128.0f; // WAV 8-bit is unsigned
    const int64_t sign = int64_t(1) << (bits - 1);
    if (v & sign)
        v -= sign << 1;
    return static_cast<float>(static_cast<double>(v) / static_cast<double>(sign));
}

bool decode(const uint8_t* data, size_t bytes, int channels, int bits, bool isFloat, bool bigEndian, AudioData& out)
{
    const int frameBytes = channels * bits / 8;
    if (frameBytes <= 0)
        return false;
    const size_t frames = bytes / static_cast<size_t>(frameBytes);
    out.channels.assign(static_cast<size_t>(channels), std::vector<float>(frames));
    for (size_t f = 0; f < frames; ++f)
        for (int c = 0; c < channels; ++c)
            out.channels[static_cast<size_t>(c)][f] =
                sampleFrom(data + f * static_cast<size_t>(frameBytes) + static_cast<size_t>(c * bits / 8), bits, isFloat,
                           bigEndian);
    return frames > 0;
}

bool readWav(const std::vector<uint8_t>& b, AudioData& out, std::string* err)
{
    size_t pos = 12;
    int channels = 0, bits = 0, format = 0;
    while (pos + 8 <= b.size())
    {
        const uint32_t size = le32(&b[pos + 4]);
        const char* id = reinterpret_cast<const char*>(&b[pos]);
        const size_t body = pos + 8;
        if (body + size > b.size() && std::memcmp(id, "data", 4) != 0)
            break;
        if (!std::memcmp(id, "fmt ", 4) && size >= 16)
        {
            format = le16(&b[body]);
            channels = le16(&b[body + 2]);
            out.sampleRate = le32(&b[body + 4]);
            bits = le16(&b[body + 14]);
            if (format == 0xFFFE && size >= 40)
                format = le16(&b[body + 24]); // sub-format GUID starts with the format code
        }
        else if (!std::memcmp(id, "data", 4))
        {
            if (!channels)
                break;
            const size_t avail = std::min<size_t>(size, b.size() - body);
            if (format != 1 && format != 3)
            {
                if (err)
                    *err = "Unsupported WAV encoding";
                return false;
            }
            return decode(&b[body], avail, channels, bits, format == 3, false, out);
        }
        pos = body + size + (size & 1);
    }
    if (err)
        *err = "Invalid WAV file";
    return false;
}

bool readAiff(const std::vector<uint8_t>& b, AudioData& out, std::string* err)
{
    const bool aifc = !std::memcmp(&b[8], "AIFC", 4);
    size_t pos = 12;
    int channels = 0, bits = 0;
    bool isFloat = false, bigEndian = true;
    while (pos + 8 <= b.size())
    {
        const uint32_t size = be32(&b[pos + 4]);
        const char* id = reinterpret_cast<const char*>(&b[pos]);
        const size_t body = pos + 8;
        if (!std::memcmp(id, "COMM", 4) && size >= 18 && body + 18 <= b.size())
        {
            channels = be16(&b[body]);
            bits = be16(&b[body + 6]);
            out.sampleRate = extended(&b[body + 8]);
            if (aifc && size >= 22)
            {
                const char* comp = reinterpret_cast<const char*>(&b[body + 18]);
                if (!std::memcmp(comp, "sowt", 4))
                    bigEndian = false;
                else if (!std::memcmp(comp, "fl32", 4) || !std::memcmp(comp, "FL32", 4))
                {
                    isFloat = true;
                    bits = 32;
                }
                else if (!std::memcmp(comp, "fl64", 4) || !std::memcmp(comp, "FL64", 4))
                {
                    isFloat = true;
                    bits = 64;
                }
                else if (std::memcmp(comp, "NONE", 4) != 0)
                {
                    if (err)
                        *err = "Unsupported AIFF compression";
                    return false;
                }
            }
        }
        else if (!std::memcmp(id, "SSND", 4) && body + 8 <= b.size())
        {
            const uint32_t offset = be32(&b[body]);
            const size_t start = body + 8 + offset;
            const size_t avail = std::min<size_t>(size - 8 - offset, b.size() - start);
            return channels && decode(&b[start], avail, channels, bits, isFloat, bigEndian, out);
        }
        pos = body + size + (size & 1);
    }
    if (err)
        *err = "Invalid AIFF file";
    return false;
}

} // namespace

bool readAudioFile(const std::string& path, AudioData& out, std::string* error)
{
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in)
    {
        if (error)
            *error = "Cannot open file";
        return false;
    }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 12)
    {
        if (error)
            *error = "File too short";
        return false;
    }
    out = {};
    if (!std::memcmp(b.data(), "RIFF", 4) && !std::memcmp(&b[8], "WAVE", 4))
        return readWav(b, out, error);
    if (!std::memcmp(b.data(), "FORM", 4) && (!std::memcmp(&b[8], "AIFF", 4) || !std::memcmp(&b[8], "AIFC", 4)))
        return readAiff(b, out, error);
    if (error)
        *error = "Not a WAV or AIFF file";
    return false;
}

} // namespace aurum
