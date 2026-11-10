#include "Resources.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#include "EmbeddedData.h"

namespace fs = std::filesystem;

namespace sidmidi {

namespace {

fs::path toPath(const std::string& utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string fromPath(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

#if defined(_WIN32)
std::string envUtf8(const wchar_t* name)
{
    const wchar_t* v = _wgetenv(name);
    if (!v || !*v)
        return {};
    return fromPath(fs::path(v));
}
#endif

bool contains(const std::vector<uint8_t>& d, const char* text)
{
    const size_t n = std::strlen(text);
    return std::search(d.begin(), d.end(), text, text + n) != d.end();
}

enum class RomKind { None, Kernal, Basic, Chargen, Drive };

// By content: the KERNAL holds BASIC's start-up banner (or at least its reset
// vector), BASIC has "CBMBASIC" after its start vectors, the character ROM
// begins with the "@" glyph, the 1541's DOS is known by its vectors.
RomKind identify(const std::vector<uint8_t>& d)
{
    if (d.size() == 8192) {
        if (d[4] == 'C' && d[5] == 'B' && d[6] == 'M' && d[7] == 'B')
            return RomKind::Basic;
        if (contains(d, "COMMODORE 64") || (d[0x1ffc] == 0xe2 && d[0x1ffd] == 0xfc))
            return RomKind::Kernal;
    } else if (d.size() == 4096) {
        static const uint8_t at[8] = {0x3c, 0x66, 0x6e, 0x6e, 0x60, 0x62, 0x3c, 0x00};
        if (std::memcmp(d.data(), at, 8) == 0)
            return RomKind::Chargen;
    } else if (d.size() == 16384) {
        // 1541 and 1541-II: reset at $EAA0, IRQ at $FE67 (its message table is
        // compressed, so the DOS name is not in it as text).
        if (d[0x3ffc] == 0xa0 && d[0x3ffd] == 0xea && d[0x3ffe] == 0x67 && d[0x3fff] == 0xfe)
            return RomKind::Drive;
    }
    return RomKind::None;
}

// VICE's names for the standard set win over anything else found.
int preference(const std::string& name, RomKind k)
{
    std::string n = name;
    for (auto& c : n)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    switch (k) {
    case RomKind::Kernal: return n.find("901227-03") != std::string::npos ? 3 : n.find("kernal") != std::string::npos ? 2 : 1;
    case RomKind::Basic: return n.find("901226-01") != std::string::npos ? 3 : n.find("basic") != std::string::npos ? 2 : 1;
    case RomKind::Chargen: return n.find("901225-01") != std::string::npos ? 3 : n.find("char") != std::string::npos ? 2 : 1;
    case RomKind::Drive:
        return n.find("1541ii") != std::string::npos ? 4 : n.find("1541") != std::string::npos ? 3 : 1;
    default: return 0;
    }
}

} // namespace

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(toPath(path), std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(toPath(path), std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::string dataFolder()
{
#if defined(_WIN32)
    std::string base = envUtf8(L"APPDATA");
    if (base.empty())
        base = envUtf8(L"USERPROFILE");
    return fromPath(toPath(base) / "SidMidi");
#else
    std::string base;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x)
        base = x;
    else if (const char* h = std::getenv("HOME"); h && *h)
        base = std::string(h) + "/.local/share";
    else
        base = "/tmp";
    return base + "/SidMidi";
#endif
}

std::string romFolder()
{
    if (const char* o = std::getenv("SIDMIDI_ROM_DIR"); o && *o)
        return o;
    return joinPath(dataFolder(), "ROMs");
}

bool makeFolder(const std::string& path)
{
    std::error_code ec;
    fs::create_directories(toPath(path), ec);
    return fs::is_directory(toPath(path), ec);
}

std::vector<std::string> listFiles(const std::string& folder, const std::string& ext)
{
    std::vector<std::string> out;
    std::error_code ec;
    auto lower = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string want = lower(ext);
    for (fs::directory_iterator it(toPath(folder), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        if (lower(fromPath(it->path().extension())) == want)
            out.push_back(fromPath(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string fileStem(const std::string& path)
{
    return fromPath(toPath(path).stem());
}

std::string joinPath(const std::string& folder, const std::string& name)
{
    return fromPath(toPath(folder) / toPath(name));
}

std::string romInstructions()
{
    return "SidMidi needs the Commodore 64's ROMs. They are copyrighted and do not come with SidMidi.\n"
           "\n"
           "Put these files into this folder:\n"
           "  - KERNAL            kernal-901227-03.bin\n"
           "  - BASIC             basic-901226-01.bin\n"
           "  - Character set     chargen-901225-01.bin\n"
           "  - 1541 drive DOS    dos1541ii-251968-03.bin (or dos1541-325302-01+901229-05.bin),\n"
           "                      needed only for the True 1541 drive\n"
           "\n"
           "The VICE emulator has them in its data folders C64 and DRIVES, or dump them from your own C64.\n"
           "Other file names work too: SidMidi recognises the files by their content.\n";
}

void prepareRomFolder()
{
    std::error_code ec;
    const fs::path dir = toPath(romFolder());
    fs::create_directories(dir, ec);
    const fs::path readme = dir / "README.txt";
    if (!fs::exists(readme, ec)) {
        std::ofstream f(readme);
        f << romInstructions();
    }
}

bool romsFromFolder(const std::string& folder, emu::Roms& out, std::string& report)
{
    report.clear();
    std::vector<fs::path> files;
    std::error_code ec;
    const fs::path dir = toPath(folder);
    for (const auto& sub : {fs::path(), fs::path("C64"), fs::path("DRIVES"), fs::path("c64"), fs::path("drives")}) {
        const fs::path d = sub.empty() ? dir : dir / sub;
        if (!fs::is_directory(d, ec))
            continue;
        for (const auto& e : fs::directory_iterator(d, ec)) {
            const auto size = e.is_regular_file(ec) ? e.file_size(ec) : 0;
            if (size == 4096 || size == 8192 || size == 16384)
                files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    int best[5] = {0, 0, 0, 0, 0};
    for (const auto& p : files) {
        std::vector<uint8_t> data;
        if (!readFile(fromPath(p), data))
            continue;
        const RomKind k = identify(data);
        if (k == RomKind::None)
            continue;
        const int pref = preference(fromPath(p.filename()), k);
        const int slot = static_cast<int>(k);
        if (pref <= best[slot])
            continue;
        best[slot] = pref;
        switch (k) {
        case RomKind::Kernal: out.kernal = data; break;
        case RomKind::Basic: out.basic = data; break;
        case RomKind::Chargen: out.chargen = data; break;
        case RomKind::Drive: out.drive1541 = data; break;
        default: break;
        }
    }
    if (out.kernal.size() != 8192)
        report += "KERNAL, ";
    if (out.basic.size() != 8192)
        report += "BASIC, ";
    if (out.chargen.size() != 4096)
        report += "character ROM, ";
    if (out.drive1541.size() != 16384)
        report += "1541 DOS (only for the True 1541), ";
    if (report.size() >= 2)
        report.erase(report.size() - 2);
    return out.complete();
}

void openFolder(const std::string& path)
{
#if defined(_WIN32)
    const std::wstring w = toPath(path).wstring();
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    pid_t pid;
    const char* argv[] = {"xdg-open", path.c_str(), nullptr};
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0) {
        // Reaped without blocking the GUI: xdg-open returns at once.
        int status;
        waitpid(pid, &status, WNOHANG);
    }
#endif
}

bool parseVpl(const std::string& text, Palette& out)
{
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line) && n < 16) {
        if (line.rfind("# NAME:", 0) == 0) {
            out.name = line.substr(7);
            while (!out.name.empty() && (out.name.back() == '\r' || out.name.back() == ' '))
                out.name.pop_back();
            continue;
        }
        if (line.empty() || line[0] == '#')
            continue;
        unsigned r, g, b;
        if (std::sscanf(line.c_str(), "%x %x %x", &r, &g, &b) == 3)
            out.rgb[n++] = ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
    }
    return n == 16;
}

const std::vector<Palette>& palettes()
{
    static const std::vector<Palette> list = [] {
        std::vector<Palette> v;
        for (int i = 0; i < embedded::kNumPalettes; ++i) {
            Palette p;
            p.key = embedded::kPalettes[i].key;
            if (parseVpl(embedded::kPalettes[i].text, p)) {
                if (p.name.empty())
                    p.name = p.key;
                v.push_back(p);
            }
        }
        if (v.empty()) {
            Palette p;
            p.key = "colodore";
            p.name = "Colodore (PAL)";
            p.rgb = {0x000000, 0xffffff, 0x96282e, 0x5bd6ce, 0x9f2dad, 0x41b936, 0x2724c4, 0xeff347,
                     0x9f4815, 0x5e3500, 0xda5f66, 0x474747, 0x787878, 0x91ff84, 0x6864ff, 0xaeaeae};
            v.push_back(p);
        }
        return v;
    }();
    return list;
}

const Palette& palette(const std::string& key)
{
    const auto& list = palettes();
    for (const auto& p : list)
        if (p.key == key)
            return p;
    for (const auto& p : list)
        if (p.key == "colodore")
            return p;
    return list.front();
}

} // namespace sidmidi
