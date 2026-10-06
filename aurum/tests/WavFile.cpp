#include "WavFile.h"

#include <cstdint>
#include <cstdio>

namespace {
void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
} // namespace

bool writeWav(const std::string& path, const std::vector<float>& left, const std::vector<float>& right, int rate)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const uint32_t frames = static_cast<uint32_t>(left.size());
    const uint32_t dataBytes = frames * 2 * 4;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + dataBytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 3); // IEEE float
    put16(f, 2);
    put32(f, static_cast<uint32_t>(rate));
    put32(f, static_cast<uint32_t>(rate) * 8);
    put16(f, 8);
    put16(f, 32);
    fwrite("data", 1, 4, f);
    put32(f, dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
    {
        fwrite(&left[i], 4, 1, f);
        fwrite(&right[i], 4, 1, f);
    }
    fclose(f);
    return true;
}
