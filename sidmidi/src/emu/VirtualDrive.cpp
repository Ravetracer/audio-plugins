#include "VirtualDrive.h"

#include <cstdio>

namespace sidmidi::emu {

namespace {

constexpr int kActivity = 50000; // calls of tickActivity() the LED stays lit

// "0:NAME,P,W" -> name "NAME", type 'P', mode 'W'. A leading "@" means
// replace; a drive number before the colon is ignored.
struct ParsedName
{
    std::string name;
    char type = 0;
    char mode = 0;
    bool replace = false;
};

ParsedName parseName(std::string raw)
{
    ParsedName p;
    if (!raw.empty() && raw[0] == '@') {
        p.replace = true;
        raw.erase(0, 1);
    }
    const auto colon = raw.find(':');
    if (colon != std::string::npos && colon <= 2)
        raw.erase(0, colon + 1);
    const auto comma = raw.find(',');
    p.name = raw.substr(0, comma);
    if (comma != std::string::npos) {
        std::string rest = raw.substr(comma + 1);
        size_t at = 0;
        while (at < rest.size()) {
            const auto next = rest.find(',', at);
            const std::string tok = rest.substr(at, next == std::string::npos ? std::string::npos : next - at);
            if (!tok.empty()) {
                const char c = tok[0];
                if (c == 'P' || c == 'S' || c == 'U' || c == 'L')
                    p.type = c;
                else if (c == 'R' || c == 'W' || c == 'A' || c == 'M')
                    p.mode = c;
            }
            if (next == std::string::npos)
                break;
            at = next + 1;
        }
    }
    return p;
}

uint8_t typeCode(char t)
{
    switch (t) {
    case 'S': return 1;
    case 'P': return 2;
    case 'U': return 3;
    case 'L': return 4;
    default: return 0;
    }
}

} // namespace

void VirtualDrive::reset()
{
    for (auto& c : channels)
        c = Channel{};
    listening = talking = opening = false;
    channel = 0;
    openName.clear();
    command_.clear();
    status_ = "73,CBM DOS V2.6 1541,00,00";
    statusPos = 0;
}

void VirtualDrive::setStatus(int code, const char* text, int t, int s)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d,%s,%02d,%02d", code, text, t, s);
    status_ = buf;
    statusPos = 0;
}

uint8_t VirtualDrive::listen(uint8_t dev)
{
    if ((dev & 0x1f) != device)
        return ST_DEVICE_NOT_PRESENT;
    listening = true;
    talking = false;
    return 0;
}

uint8_t VirtualDrive::talk(uint8_t dev)
{
    if ((dev & 0x1f) != device)
        return ST_DEVICE_NOT_PRESENT;
    talking = true;
    listening = false;
    return 0;
}

uint8_t VirtualDrive::second(uint8_t sa)
{
    if (!listening)
        return 0;
    channel = sa & 0x0f;
    switch (sa & 0xf0) {
    case 0xf0:
        opening = true;
        openName.clear();
        break;
    case 0xe0:
        doClose(channel);
        break;
    default: // $60: data to the channel
        opening = false;
        if (channel == 15)
            command_.clear();
        break;
    }
    return 0;
}

uint8_t VirtualDrive::tksa(uint8_t sa)
{
    if (!talking)
        return 0;
    channel = sa & 0x0f;
    return 0;
}

uint8_t VirtualDrive::ciout(uint8_t byte)
{
    if (!listening)
        return ST_DEVICE_NOT_PRESENT;
    activity_ = kActivity;
    if (opening) {
        openName.push_back(static_cast<char>(byte));
        return 0;
    }
    if (channel == 15) {
        command_.push_back(static_cast<char>(byte));
        return 0;
    }
    auto& c = channels[channel];
    if (c.open && c.writing)
        c.data.push_back(byte);
    return 0;
}

uint8_t VirtualDrive::acptr(uint8_t& byte)
{
    activity_ = kActivity;
    if (channel == 15) {
        if (statusPos < status_.size()) {
            byte = static_cast<uint8_t>(status_[statusPos++]);
            return 0;
        }
        byte = 0x0d;
        setStatus(0, "OK");
        return ST_EOI;
    }
    auto& c = channels[channel];
    if (!c.open || c.writing || c.pos >= c.data.size()) {
        byte = 0x0d;
        return ST_EOI | ST_TIMEOUT_READ;
    }
    byte = c.data[c.pos++];
    return c.pos >= c.data.size() ? ST_EOI : 0;
}

uint8_t VirtualDrive::unlisten()
{
    if (listening) {
        if (opening) {
            opening = false;
            if (channel == 15)
                doCommand(openName);
            else
                doOpen(channel, openName);
        } else if (channel == 15 && !command_.empty()) {
            doCommand(command_);
            command_.clear();
        }
    }
    listening = false;
    return 0;
}

uint8_t VirtualDrive::untalk()
{
    talking = false;
    return 0;
}

std::vector<uint8_t> VirtualDrive::directoryListing(const std::string& pattern) const
{
    std::vector<uint8_t> out = {0x01, 0x04};
    auto line = [&](uint16_t number, const std::string& text) {
        out.push_back(0x01);
        out.push_back(0x01); // a dummy link; LOAD relinks the program
        out.push_back(static_cast<uint8_t>(number));
        out.push_back(static_cast<uint8_t>(number >> 8));
        for (char ch : text)
            out.push_back(static_cast<uint8_t>(ch));
        out.push_back(0);
    };
    std::string head = "\x12\"";
    std::string name = disk.diskName();
    for (auto& ch : name)
        if (static_cast<uint8_t>(ch) == 0xa0)
            ch = ' ';
    head += name + "\" ";
    std::string id = disk.diskId();
    for (auto& ch : id)
        if (static_cast<uint8_t>(ch) == 0xa0)
            ch = ' ';
    head += id;
    line(0, head);

    static const char* types[] = {"DEL", "SEQ", "PRG", "USR", "REL", "???", "???", "???"};
    for (const auto& e : disk.directory()) {
        if (!pattern.empty() && !D64::matches(pattern, e.name))
            continue;
        std::string t;
        const int pad = e.blocks < 10 ? 3 : e.blocks < 100 ? 2 : e.blocks < 1000 ? 1 : 0;
        t.append(static_cast<size_t>(pad), ' ');
        t += "\"" + e.name + "\"";
        t.append(16 - std::min<size_t>(16, e.name.size()), ' ');
        t += (e.type & 0x80) ? " " : "*";
        t += types[e.type & 0x07];
        t += (e.type & 0x40) ? "<" : " ";
        line(e.blocks, t);
    }
    line(static_cast<uint16_t>(disk.blocksFree()), "BLOCKS FREE.             ");
    out.push_back(0);
    out.push_back(0);
    return out;
}

void VirtualDrive::doOpen(int ch, const std::string& rawName)
{
    auto& c = channels[ch];
    c = Channel{};
    if (!disk.present()) {
        setStatus(74, "DRIVE NOT READY");
        return;
    }
    if (!rawName.empty() && rawName[0] == '$') {
        std::string pattern;
        const auto colon = rawName.find(':');
        if (colon != std::string::npos)
            pattern = rawName.substr(colon + 1);
        c.open = true;
        c.data = directoryListing(pattern);
        setStatus(0, "OK");
        return;
    }
    const ParsedName p = parseName(rawName);
    const bool write = ch == 1 || p.mode == 'W';
    if (p.name.empty() && !write) {
        // LOAD"",8 loads the last file accessed on a real drive; the first will do.
        c.name = "*";
    } else {
        c.name = p.name;
    }
    c.type = typeCode(p.type);
    if (write) {
        if (writeProtect) {
            setStatus(26, "WRITE PROTECT ON");
            return;
        }
        DirEntry e;
        if (!p.replace && disk.find(p.name, e)) {
            setStatus(63, "FILE EXISTS");
            return;
        }
        c.open = true;
        c.writing = true;
        c.replace = p.replace;
        if (!c.type)
            c.type = ch == 1 ? 2 : 1; // SAVE writes PRG, OPEN ...,W defaults to SEQ
        setStatus(0, "OK");
        return;
    }
    DirEntry e;
    if (!disk.find(c.name, e, ch <= 1 ? 0 : c.type)) {
        setStatus(62, "FILE NOT FOUND");
        return;
    }
    c.open = true;
    c.data = disk.readFile(e);
    setStatus(0, "OK");
}

void VirtualDrive::doClose(int ch)
{
    auto& c = channels[ch];
    if (c.open && c.writing) {
        const int err = disk.writeFile(c.name, c.type, c.data, c.replace);
        if (err == 0) {
            dirty = true;
            setStatus(0, "OK");
        } else if (err == 63) {
            setStatus(63, "FILE EXISTS");
        } else {
            setStatus(72, "DISK FULL");
        }
    }
    c = Channel{};
}

void VirtualDrive::doCommand(const std::string& raw)
{
    std::string cmd = raw;
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n'))
        cmd.pop_back();
    if (cmd.empty())
        return;
    const auto colon = cmd.find(':');
    const std::string arg = colon == std::string::npos ? std::string() : cmd.substr(colon + 1);
    switch (cmd[0]) {
    case 'S': { // scratch, comma-separated patterns
        if (!disk.present()) {
            setStatus(74, "DRIVE NOT READY");
            return;
        }
        int n = 0;
        size_t at = 0;
        while (at <= arg.size()) {
            const auto next = arg.find(',', at);
            const std::string pat = arg.substr(at, next == std::string::npos ? std::string::npos : next - at);
            if (!pat.empty())
                n += disk.scratch(pat);
            if (next == std::string::npos)
                break;
            at = next + 1;
        }
        if (n)
            dirty = true;
        setStatus(1, "FILES SCRATCHED", n, 0);
        return;
    }
    case 'R': { // R0:NEW=OLD
        const auto eq = arg.find('=');
        if (eq == std::string::npos) {
            setStatus(30, "SYNTAX ERROR");
            return;
        }
        std::string from = arg.substr(eq + 1);
        const auto c2 = from.find(':');
        if (c2 != std::string::npos)
            from.erase(0, c2 + 1);
        const int err = disk.rename(from, arg.substr(0, eq));
        if (err == 0) {
            dirty = true;
            setStatus(0, "OK");
        } else if (err == 63) {
            setStatus(63, "FILE EXISTS");
        } else {
            setStatus(62, "FILE NOT FOUND");
        }
        return;
    }
    case 'N': { // N0:NAME,ID
        const auto comma = arg.find(',');
        const std::string name = arg.substr(0, comma);
        const std::string id = comma == std::string::npos ? std::string("00") : arg.substr(comma + 1);
        disk.format(name, id);
        dirty = true;
        setStatus(0, "OK");
        return;
    }
    case 'U':
        if (cmd.size() > 1 && (cmd[1] == 'J' || cmd[1] == 'I' || cmd[1] == ':' || cmd[1] == '9')) {
            setStatus(73, "CBM DOS V2.6 1541");
            return;
        }
        setStatus(0, "OK");
        return;
    case 'I':
    case 'V':
    case 'M': // memory commands: nothing behind them in the virtual drive
    case 'B':
    default:
        setStatus(0, "OK");
        return;
    }
}

} // namespace sidmidi::emu
