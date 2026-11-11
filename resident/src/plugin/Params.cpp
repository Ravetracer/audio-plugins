#include "Params.h"

#include <cmath>
#include <cstdio>

namespace resident {

double defaultPan(int sid)
{
    // A short spread, so a second SID is audibly a second SID.
    return sid == 0 ? 0.0 : ((sid & 1) ? 0.5 : -0.5);
}

std::vector<ParamInfo> parameterTable()
{
    std::vector<ParamInfo> t;
    t.push_back({pid::Master, "Output", "Volume", -60.0, 12.0, 0.0});
    for (int i = 0; i < 8; ++i)
        t.push_back({pid::Level + static_cast<uint32_t>(i), "SIDs", "SID " + std::to_string(i + 1) + " Level", -60.0,
                     12.0, 0.0});
    for (int i = 0; i < 8; ++i)
        t.push_back({pid::Pan + static_cast<uint32_t>(i), "SIDs", "SID " + std::to_string(i + 1) + " Pan", -1.0, 1.0,
                     defaultPan(i)});
    const CcMap none;
    for (int i = 0; i < kCcSlots; ++i)
        t.push_back({pid::CcSlot + static_cast<uint32_t>(i), "CC", none.slotName(i), 0.0, 127.0, 0.0, true});
    return t;
}

bool paramValueText(uint32_t id, double v, std::string& out)
{
    char b[32];
    if (id >= pid::Pan && id < pid::Pan + 8) {
        if (std::fabs(v) < 0.005)
            std::snprintf(b, sizeof(b), "C");
        else
            std::snprintf(b, sizeof(b), "%d%% %s", static_cast<int>(std::lround(std::fabs(v) * 100)), v < 0 ? "L" : "R");
    } else if (id == pid::Master || (id >= pid::Level && id < pid::Level + 8)) {
        if (v <= -59.9)
            std::snprintf(b, sizeof(b), "-inf dB");
        else
            std::snprintf(b, sizeof(b), "%+.1f dB", v);
    } else {
        return false;
    }
    out = b;
    return true;
}

} // namespace resident
