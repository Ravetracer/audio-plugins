// sidmidi-cputest: the 6502 core against Klaus Dormann's 6502 functional test.
//
//   sidmidi-cputest 6502_functional_test.bin [success-address]
//
// The image is a full 64K memory dump with the code at $0400. The test ends
// in a "JMP *" loop: at the success address when everything passed, anywhere
// else at the first failure. The binary is GPL-3 test code from
// https://github.com/Klaus2m5/6502_65C02_functional_tests and is fetched into
// the build tree by tools/fetch-cpu-tests.sh, never committed.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "emu/Cpu6502.h"

namespace {

struct FlatBus
{
    uint8_t mem[65536] = {};
    uint64_t cycles = 0;
    uint8_t read(uint16_t a)
    {
        ++cycles;
        return mem[a];
    }
    void write(uint16_t a, uint8_t v)
    {
        ++cycles;
        mem[a] = v;
    }
    bool irqLine() const { return false; }
    bool nmiLine() const { return false; }
    bool trap(uint16_t) { return false; }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: sidmidi-cputest 6502_functional_test.bin [success-hex]\n");
        return 2;
    }
    const unsigned success = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 16)) : 0x3469;
    static FlatBus bus;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::perror(argv[1]);
        return 2;
    }
    const size_t n = std::fread(bus.mem, 1, sizeof(bus.mem), f);
    std::fclose(f);
    if (n < 0x4000) {
        std::fprintf(stderr, "%s: too short\n", argv[1]);
        return 2;
    }
    sidmidi::emu::Cpu6502<FlatBus> cpu(bus);
    cpu.s.pc = 0x0400;
    uint16_t last = 0xffff;
    for (uint64_t i = 0; i < 200000000ull; ++i) {
        const uint16_t pc = cpu.s.pc;
        cpu.step();
        if (cpu.s.pc == pc && pc == last) {
            if (pc == success) {
                std::printf("PASS at $%04X after %llu cycles\n", pc, static_cast<unsigned long long>(bus.cycles));
                return 0;
            }
            std::printf("FAIL: stuck at $%04X (A=%02X X=%02X Y=%02X P=%02X SP=%02X), test number $%02X\n", pc,
                        cpu.s.a, cpu.s.x, cpu.s.y, cpu.s.p, cpu.s.sp, bus.mem[0x0200]);
            return 1;
        }
        last = pc;
    }
    std::printf("FAIL: no end after 200M instructions (pc $%04X)\n", cpu.s.pc);
    return 1;
}
