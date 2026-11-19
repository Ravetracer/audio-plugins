// The WCLAP's side of the page (clap.webview): what the editor in the page
// asks for and sends, see WebProtocol.h. Only in the WebAssembly build, where
// the host calls this on the same thread as process() -- openDAW runs both on
// its audio worklet -- so nothing here races the machine. Answers are built
// into one reused buffer.

#include "ResidentPlugin.h"

#if RESIDENT_WEBVIEW

#include <algorithm>
#include <cstdio>

#include "WebProtocol.h"
#include "emu/Vic.h"
#include "engine/Resources.h"

namespace resident {

namespace {

void beginSection(std::vector<uint8_t>& out, uint8_t id, uint32_t serial, size_t& sizeAt)
{
    out.push_back(id);
    out.push_back(0);
    web::put<uint16_t>(out, 0);
    web::put<uint32_t>(out, serial);
    sizeAt = out.size();
    web::put<uint32_t>(out, 0);
}

void endSection(std::vector<uint8_t>& out, size_t sizeAt)
{
    const uint32_t n = static_cast<uint32_t>(out.size() - sizeAt - 4);
    std::memcpy(out.data() + sizeAt, &n, 4);
}

std::string fileNameOf(const std::string& path) { return path.substr(path.find_last_of("/\\") + 1); }

} // namespace

bool ResidentPlugin::webReceive(const void* data, uint32_t size)
{
    const auto* b = static_cast<const uint8_t*>(data);
    if (!b || size < 4)
        return false;
    switch (b[0]) {
    case web::PagePoll:
        webPoll(b, size);
        return true;
    case web::PageParam: {
        if (size < 16)
            return false;
        const uint32_t id = web::get<uint32_t>(b, 4);
        if (paramIndex(id) < 0)
            return false;
        if (b[1] == 0)
            guiBeginParam(id);
        else if (b[1] == 1)
            guiSetParam(id, web::get<double>(b, 8));
        else {
            guiEndParam(id);
            webMarkDirty(); // one undo step per drag
        }
        return true;
    }
    case web::PageSettings: {
        MachineSettings s;
        s.fromText(std::string(reinterpret_cast<const char*>(b + 4), size - 4));
        applySettings(s);
        webMarkDirty();
        return true;
    }
    case web::PageCcMap:
        setCcMap(CcMap::fromText(std::string(reinterpret_cast<const char*>(b + 4), size - 4)));
        webMarkDirty();
        return true;
    case web::PageInput: {
        if (size < 8)
            return false;
        const uint8_t a = b[2], v = b[3];
        const int16_t c = web::get<int16_t>(b, 4), d = web::get<int16_t>(b, 6);
        switch (b[1]) {
        case web::InputKey: key(a, v != 0); break;
        case web::InputJoystick: joystick(a, v); break;
        case web::InputRestore: restoreKey(); break;
        case web::InputMouse: mouseMove(a, c, d); break;
        case web::InputPaddles: paddles(a, static_cast<uint8_t>(c), static_cast<uint8_t>(d)); break;
        case web::InputButtons: deviceButtons(a, v); break;
        default: return false;
        }
        return true;
    }
    case web::PageCommand:
        webCommand(b, size);
        return true;
    case web::PageUpload:
        webUpload(b, size);
        return true;
    case web::PageResize: {
        if (size < 12)
            return false;
        uint32_t w = web::get<uint32_t>(b, 4), h = web::get<uint32_t>(b, 8);
        if (!w || !h)
            return false;
        guiAdjustSize(&w, &h);
        if (hostGui_ && hostGui_->request_resize(host_, w, h))
            setWebSize(w, h);
        return true;
    }
    default:
        return false;
    }
}

void ResidentPlugin::webMarkDirty()
{
    if (hostState_)
        hostState_->mark_dirty(host_);
}

void ResidentPlugin::webNotice(const std::string& text, bool error)
{
    webNotice_ = text;
    webNoticeError_ = error;
    if (++webNoticeSerial_ == 0)
        webNoticeSerial_ = 1;
}

void ResidentPlugin::webCommand(const uint8_t* b, uint32_t size)
{
    size_t pos = 4;
    std::string arg;
    web::getString(b, size, pos, arg);
    std::string err;
    switch (b[1]) {
    case web::CommandReset: resetButton(); break;
    case web::CommandPower: powerCycle(); break;
    case web::CommandWarpOn: setWarp(true); break;
    case web::CommandWarpOff: setWarp(false); break;
    case web::CommandEjectDisk:
        ejectDisk();
        webMarkDirty();
        break;
    case web::CommandEjectCartridge:
        ejectCartridge();
        webMarkDirty();
        break;
    case web::CommandFreeze: freezeButton(); break;
    case web::CommandStartFromDisk:
        if (startFromDisk(arg, err)) {
            webNotice("Starting " + (arg == "*" ? std::string("the first file") : "\"" + arg + "\""), false);
            webMarkDirty();
        } else {
            webNotice(err, true);
        }
        break;
    case web::CommandExportDisk: {
        std::vector<uint8_t> image;
        {
            std::lock_guard<std::mutex> lock(machineLock_);
            image = engine_.machine().diskImage().data();
        }
        if (image.empty()) {
            webNotice("No disk in the drive", true);
            break;
        }
        if (!hostWebview_)
            break;
        webOut_.clear();
        webOut_.push_back(web::PluginDiskImage);
        webOut_.insert(webOut_.end(), 3, 0);
        web::putString(webOut_, arg.empty() ? std::string("disk.d64") : arg);
        webOut_.insert(webOut_.end(), image.begin(), image.end());
        hostWebview_->send(host_, webOut_.data(), static_cast<uint32_t>(webOut_.size()));
        break;
    }
    default: break;
    }
}

void ResidentPlugin::webUpload(const uint8_t* b, uint32_t size)
{
    if (size < 12)
        return;
    const uint32_t total = web::get<uint32_t>(b, 4), offset = web::get<uint32_t>(b, 8);
    size_t pos = 12;
    std::string name;
    if (!web::getString(b, size, pos, name) || total > (64u << 20))
        return;
    if (offset == 0) {
        webUpload_.clear();
        webUpload_.reserve(total);
        webUploadName_ = name;
        webUploadKind_ = b[1];
    }
    if (offset != webUpload_.size() || name != webUploadName_)
        return; // a chunk out of order: the file is dropped
    webUpload_.insert(webUpload_.end(), b + pos, b + size);
    if (webUpload_.size() < total)
        return;
    std::vector<uint8_t> data;
    data.swap(webUpload_);
    std::string err;
    switch (webUploadKind_) {
    case web::UploadRom: {
        const bool had = hasRoms_;
        if (!installRom(data, err)) {
            webNotice(fileNameOf(name) + ": " + err, true);
            break;
        }
        if (!had && hasRoms_)
            webNotice("The ROMs are complete: the C64 starts", false);
        else if (!hasRoms_)
            webNotice(fileNameOf(name) + " is in. Still missing: " + romReport_, false);
        else
            webNotice(fileNameOf(name) + " is in", false);
        webMarkDirty();
        break;
    }
    case web::UploadDisk:
        if (attachDisk(fileNameOf(name), data, err)) {
            ++webDiskInserts_;
            webNotice("Disk inserted: " + fileNameOf(name), false);
            webMarkDirty();
        } else {
            webNotice(err, true);
        }
        break;
    default:
        if (loadData(fileNameOf(name), data, err)) {
            if (!diskFileName_.empty() && diskFileName_ == fileNameOf(name))
                ++webDiskInserts_; // a .d64 that was loaded is in the drive
            webNotice(notice_.empty() ? "Loaded " + fileNameOf(name) : notice_, false);
            webMarkDirty();
        } else {
            webNotice(err, true);
        }
        break;
    }
}

// The screen section: the lines that differ from the frame the page has, or
// every line for a page that has none (or another).
void ResidentPlugin::webScreen(uint32_t pageSerial, std::vector<uint8_t>& out)
{
    constexpr int W = emu::Vic::kWidth, H = emu::Vic::kHeight;
    const size_t n = static_cast<size_t>(W) * H;
    if (webFrame_.size() != n) {
        webFrame_.assign(n, 0);
        webFrameNew_.assign(n, 0);
    }
    const bool fresh = engine_.frameSerial() != webEngineFrame_;
    if (!fresh && (pageSerial == webFrameSerial_ || webFrameSerial_ == 0))
        return; // nothing new, and the page has the newest (or there is none yet)
    uint32_t base = 0;
    if (fresh) {
        webEngineFrame_ = engine_.copyFrame(webFrameNew_.data());
        if (pageSerial == webFrameSerial_ && webFrameSerial_ != 0)
            base = webFrameSerial_;
        if (++webFrameSerial_ == 0)
            webFrameSerial_ = 1;
    } else {
        webFrameNew_ = webFrame_; // a page that is behind: the newest frame, whole
    }
    size_t sizeAt;
    beginSection(out, web::SectionScreen, webFrameSerial_, sizeAt);
    web::put<uint32_t>(out, base);
    const size_t countAt = out.size();
    web::put<uint16_t>(out, 0);
    web::put<uint16_t>(out, 0);
    uint16_t lines = 0;
    for (int y = 0; y < H; ++y) {
        const uint8_t* row = &webFrameNew_[static_cast<size_t>(y) * W];
        if (base && std::memcmp(row, &webFrame_[static_cast<size_t>(y) * W], W) == 0)
            continue;
        web::put<uint16_t>(out, static_cast<uint16_t>(y));
        const size_t modeAt = out.size();
        out.push_back(0);
        out.push_back(0);
        const size_t lenAt = out.size();
        web::put<uint16_t>(out, 0);
        uint8_t mode = 0;
        web::encodeLine(row, W, out, mode, base ? &webFrame_[static_cast<size_t>(y) * W] : nullptr);
        out[modeAt] = mode;
        const uint16_t len = static_cast<uint16_t>(out.size() - lenAt - 2);
        std::memcpy(out.data() + lenAt, &len, 2);
        ++lines;
    }
    std::memcpy(out.data() + countAt, &lines, 2);
    endSection(out, sizeAt);
    webFrame_.swap(webFrameNew_);
}

void ResidentPlugin::webPoll(const uint8_t* b, uint32_t size)
{
    if (!hostWebview_ || size < web::kPollSize)
        return;
    const bool wantsScreen = b[1] & 1;
    uint32_t serial[6];
    for (int i = 0; i < 6; ++i)
        serial[i] = web::get<uint32_t>(b, 4 + 4 * static_cast<size_t>(i));
    if (wantsScreen) {
        webScreenAsked_ = true;
        webScreenAskedAt_ = samplesRun_;
        engine_.videoWanted = true;
    }

    // What changed since the last answer gets a new serial.
    const size_t np = params_.size();
    bool valuesChanged = webValues_.size() != np;
    webValues_.resize(np);
    for (size_t i = 0; i < np; ++i) {
        const double v = values_[i].load();
        if (v != webValues_[i]) {
            webValues_[i] = v;
            valuesChanged = true;
        }
    }
    if (valuesChanged && ++webValuesSerial_ == 0)
        webValuesSerial_ = 1;
    const std::string settingsText = settings_.toText();
    if (settingsText != webSettings_) {
        webSettings_ = settingsText;
        if (++webSettingsSerial_ == 0)
            webSettingsSerial_ = 1;
    }
    std::vector<uint8_t> media;
    web::put<uint32_t>(media, webDiskInserts_);
    web::putString(media, mediaSummary());
    web::putString(media, diskTitle());
    web::putString(media, romReport_);
    const auto files = diskDirectory();
    web::put<uint16_t>(media, static_cast<uint16_t>(std::min<size_t>(files.size(), 0xffff)));
    for (const auto& f : files) {
        web::putString(media, f.petscii);
        web::putString(media, f.display);
        web::putString(media, f.type);
        web::put<uint16_t>(media, static_cast<uint16_t>(f.blocks));
    }
    if (media != webMedia_) {
        webMedia_.swap(media);
        if (++webMediaSerial_ == 0)
            webMediaSerial_ = 1;
    }
    const uint32_t ccSerial = ccMapSerial_ + 1; // never 0, which asks for everything

    auto& out = webOut_;
    out.assign(web::kStatusHeader, 0);
    uint8_t flags = 0;
    if (hasRoms_)
        flags |= web::StatusHasRoms;
    if (hasDriveRom_)
        flags |= web::StatusHasDriveRom;
    {
        std::lock_guard<std::mutex> lock(machineLock_);
        auto& c64 = engine_.machine();
        if (engine_.warping())
            flags |= web::StatusWarping;
        if (c64.cart.freezable())
            flags |= web::StatusFreezable;
        if (c64.drive.disk.present())
            flags |= web::StatusDisk;
    }
    if (engine_.driveLed.load(std::memory_order_relaxed))
        flags |= web::StatusDriveLed;
    out[0] = web::PluginStatus;
    out[1] = flags;
    out[2] = static_cast<uint8_t>(romsInUse());
    out[3] = static_cast<uint8_t>(romsInState());
    const uint32_t in = engine_.midiInCount.load(std::memory_order_relaxed);
    const uint32_t outCount = engine_.midiOutCount.load(std::memory_order_relaxed);
    std::memcpy(out.data() + 4, &in, 4);
    std::memcpy(out.data() + 8, &outCount, 4);
    uint16_t sections = 0;
    size_t sizeAt;
    if (serial[0] != webValuesSerial_) {
        beginSection(out, web::SectionValues, webValuesSerial_, sizeAt);
        for (double v : webValues_)
            web::put<double>(out, v);
        endSection(out, sizeAt);
        ++sections;
    }
    if (serial[1] != webSettingsSerial_) {
        beginSection(out, web::SectionSettings, webSettingsSerial_, sizeAt);
        out.insert(out.end(), webSettings_.begin(), webSettings_.end());
        endSection(out, sizeAt);
        ++sections;
    }
    if (serial[2] != ccSerial) {
        beginSection(out, web::SectionCcMap, ccSerial, sizeAt);
        const std::string text = ccMap_.toText();
        out.insert(out.end(), text.begin(), text.end());
        endSection(out, sizeAt);
        ++sections;
    }
    if (serial[3] != webMediaSerial_) {
        beginSection(out, web::SectionMedia, webMediaSerial_, sizeAt);
        out.insert(out.end(), webMedia_.begin(), webMedia_.end());
        endSection(out, sizeAt);
        ++sections;
    }
    if (webNoticeSerial_ != 0 && serial[4] != webNoticeSerial_) {
        beginSection(out, web::SectionNotice, webNoticeSerial_, sizeAt);
        out.push_back(webNoticeError_ ? 1 : 0);
        out.insert(out.end(), webNotice_.begin(), webNotice_.end());
        endSection(out, sizeAt);
        ++sections;
    }
    if (wantsScreen && hasRoms_) {
        const size_t before = out.size();
        webScreen(serial[5], out);
        if (out.size() != before)
            ++sections;
    }
    std::memcpy(out.data() + 12, &sections, 2);
    hostWebview_->send(host_, out.data(), static_cast<uint32_t>(out.size()));
}

} // namespace resident

#endif
