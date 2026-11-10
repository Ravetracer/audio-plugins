#include "MachineSettings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace sidmidi {

std::string MachineSettings::toText() const
{
    std::ostringstream o;
    o << "standard = " << (standard == emu::VideoStandard::PAL ? "PAL" : "NTSC") << "\n";
    o << "midi = " << static_cast<int>(midi) << "\n";
    o << "palette = " << palette << "\n";
    o << "joy_port = " << joyPort << "\n";
    o << "joy_keys = " << static_cast<int>(joyKeys) << "\n";
    o << "key_mapping = " << static_cast<int>(keyMapping) << "\n";
    o << "port1_device = " << static_cast<int>(portDevice[0]) << "\n";
    o << "port2_device = " << static_cast<int>(portDevice[1]) << "\n";
    o << "send_clock = " << (sendClock ? 1 : 0) << "\n";
    o << "drive_mode = " << static_cast<int>(driveMode) << "\n";
    o << "warp_boot = " << (warpOnBoot ? 1 : 0) << "\n";
    o << "warp_disk = " << (warpOnDiskAccess ? 1 : 0) << "\n";
    o << "screen_scale = " << screenScale << "\n";
    o << "cc_follow = " << (ccFollowProgram ? 1 : 0) << "\n";
    o << "sid_count = " << sids.count << "\n";
    o << "resampling = " << static_cast<int>(sids.resampling) << "\n";
    for (int i = 0; i < 8; ++i) {
        const auto& c = sids.chips[static_cast<size_t>(i)];
        char buf[256];
        std::snprintf(buf, sizeof(buf), "sid%d = %04X %d %.4f %d %.4f %.4f %d %.6f %d\n", i + 1, c.address,
                      static_cast<int>(c.variant), c.tuning.filterCurve, static_cast<int>(c.tuning.combined),
                      c.tuning.dacLeakage, c.tuning.digiOffset, c.tuning.filterEnabled ? 1 : 0,
                      c.tuning.filterRange, c.tuning.oldCaps ? 1 : 0);
        o << buf;
    }
    return o.str();
}

void MachineSettings::fromText(const std::string& text)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r'))
                s.pop_back();
            while (!s.empty() && s.front() == ' ')
                s.erase(0, 1);
            return s;
        };
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        const int iv = std::atoi(val.c_str());
        if (key == "standard")
            standard = val == "NTSC" ? emu::VideoStandard::NTSC : emu::VideoStandard::PAL;
        else if (key == "midi")
            midi = static_cast<emu::MidiInterface>(std::clamp(iv, 0, static_cast<int>(emu::MidiInterface::Count) - 1));
        else if (key == "palette")
            palette = val;
        else if (key == "joy_port")
            joyPort = iv == 1 ? 1 : 2;
        else if (key == "key_mapping")
            keyMapping = iv == 1 ? KeyMapping::Positional : KeyMapping::Symbolic;
        else if (key == "joy_keys")
            joyKeys = static_cast<JoyKeys>(std::clamp(iv, 0, 2));
        else if (key == "port1_device" || key == "port2_device")
            portDevice[key[4] - '1'] = static_cast<emu::PortDevice>(std::clamp(iv, 0, 2));
        else if (key == "send_clock")
            sendClock = iv != 0;
        else if (key == "drive_traps") // 0.1.0
            driveMode = iv ? emu::DriveMode::Virtual : emu::DriveMode::None;
        else if (key == "drive_mode")
            driveMode = static_cast<emu::DriveMode>(std::clamp(iv, 0, 2));
        else if (key == "warp_boot")
            warpOnBoot = iv != 0;
        else if (key == "warp_disk")
            warpOnDiskAccess = iv != 0;
        else if (key == "screen_scale")
            screenScale = std::clamp(iv, 1, 3);
        else if (key == "cc_follow")
            ccFollowProgram = iv != 0;
        else if (key == "sid_count")
            sids.count = std::clamp(iv, 1, 8);
        else if (key == "resampling")
            sids.resampling = iv == 1 ? emu::SidResampling::Fast : emu::SidResampling::Sinc;
        else if (key.size() == 4 && key.rfind("sid", 0) == 0 && key[3] >= '1' && key[3] <= '8') {
            auto& c = sids.chips[static_cast<size_t>(key[3] - '1')];
            unsigned addr = 0xd400;
            int variant = 0, combined = 0, filter = 1, caps = 0;
            float curve = 0.5f, leak = 1.0f, offset = 1.0f, range = 19.0f / 39.0f;
            if (std::sscanf(val.c_str(), "%x %d %f %d %f %f %d %f %d", &addr, &variant, &curve, &combined, &leak,
                            &offset, &filter, &range, &caps) >= 2) {
                c.address = static_cast<uint16_t>(addr & 0xffe0);
                c.variant = static_cast<emu::SidVariant>(std::clamp(variant, 0, static_cast<int>(emu::SidVariant::Count) - 1));
                c.tuning.filterCurve = std::clamp(curve, 0.0f, 1.0f);
                c.tuning.combined = static_cast<emu::CombinedWaves>(std::clamp(combined, 0, 2));
                c.tuning.dacLeakage = std::clamp(leak, 0.0f, 1.0f);
                c.tuning.digiOffset = std::clamp(offset, 0.0f, 1.0f);
                c.tuning.filterEnabled = filter != 0;
                c.tuning.filterRange = std::clamp(range, 0.0f, 1.0f);
                c.tuning.oldCaps = caps != 0;
            }
        }
    }
}

} // namespace sidmidi
