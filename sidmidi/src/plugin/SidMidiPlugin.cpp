#include "SidMidiPlugin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "emu/Snapshot.h"
#include "engine/Resources.h"
#include "gui/Editor.h"
#include "sidmidi.h"

namespace sidmidi {

namespace {

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                 CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT, kPluginId, kPluginName, kPluginVendor, kPluginUrl, kPluginUrl, kPluginUrl,
    kPluginVersion, kPluginDescription, kFeatures};

SidMidiPlugin* self(const clap_plugin_t* p) { return static_cast<SidMidiPlugin*>(p->plugin_data); }

double dbToGain(double db) { return db <= -59.9 ? 0.0 : std::pow(10.0, db / 20.0); }

// State container: magic, version, then length-prefixed sections.
constexpr char kStateMagic[8] = {'S', 'I', 'D', 'M', 'I', 'D', 'I', '1'};

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putBlob(std::vector<uint8_t>& out, const void* data, size_t n)
{
    putU32(out, static_cast<uint32_t>(n));
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}
bool getU32(const std::vector<uint8_t>& in, size_t& at, uint32_t& v)
{
    if (at + 4 > in.size())
        return false;
    v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(in[at + i]) << (8 * i);
    at += 4;
    return true;
}
bool getBlob(const std::vector<uint8_t>& in, size_t& at, std::vector<uint8_t>& out)
{
    uint32_t n;
    if (!getU32(in, at, n) || at + n > in.size())
        return false;
    out.assign(in.begin() + static_cast<long>(at), in.begin() + static_cast<long>(at + n));
    at += n;
    return true;
}

std::string lowerExt(const std::string& path)
{
    const auto dot = path.rfind('.');
    if (dot == std::string::npos)
        return {};
    std::string e = path.substr(dot + 1);
    for (auto& c : e)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string baseName(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

// ------------------------------------------------------------------ glue

struct Glue
{
    static bool init(const clap_plugin_t* p) { return self(p)->init(); }
    static void destroy(const clap_plugin_t* p) { self(p)->destroy(); }
    static bool activate(const clap_plugin_t* p, double sr, uint32_t mn, uint32_t mx) { return self(p)->activate(sr, mn, mx); }
    static void deactivate(const clap_plugin_t* p) { self(p)->deactivate(); }
    static bool startProcessing(const clap_plugin_t* p) { return self(p)->startProcessing(); }
    static void stopProcessing(const clap_plugin_t* p) { self(p)->stopProcessing(); }
    static void reset(const clap_plugin_t* p) { self(p)->reset(); }
    static clap_process_status process(const clap_plugin_t* p, const clap_process_t* pr) { return self(p)->process(pr); }
    static const void* getExtension(const clap_plugin_t* p, const char* id) { return self(p)->getExtension(id); }
    static void onMainThread(const clap_plugin_t* p) { self(p)->onMainThread(); }

    static uint32_t portsCount(const clap_plugin_t* p, bool in) { return self(p)->audioPortsCount(in); }
    static bool portsGet(const clap_plugin_t* p, uint32_t i, bool in, clap_audio_port_info_t* info)
    {
        return self(p)->audioPortsGet(i, in, info);
    }
    static uint32_t notePortsCount(const clap_plugin_t* p, bool in) { return self(p)->notePortsCount(in); }
    static bool notePortsGet(const clap_plugin_t* p, uint32_t i, bool in, clap_note_port_info_t* info)
    {
        return self(p)->notePortsGet(i, in, info);
    }
    static uint32_t paramsCount(const clap_plugin_t* p) { return self(p)->paramsCount(); }
    static bool paramsInfo(const clap_plugin_t* p, uint32_t i, clap_param_info_t* info) { return self(p)->paramsInfo(i, info); }
    static bool paramsValue(const clap_plugin_t* p, clap_id id, double* v) { return self(p)->paramsValue(id, v); }
    static bool paramsToText(const clap_plugin_t* p, clap_id id, double v, char* d, uint32_t s)
    {
        return self(p)->paramsToText(id, v, d, s);
    }
    static bool paramsFromText(const clap_plugin_t* p, clap_id id, const char* d, double* v)
    {
        return self(p)->paramsFromText(id, d, v);
    }
    static void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
    {
        self(p)->paramsFlush(in, out);
    }
    static bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) { return self(p)->stateSave(s); }
    static bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) { return self(p)->stateLoad(s); }

    static bool guiApiSupported(const clap_plugin_t*, const char* api, bool floating)
    {
#if defined(_WIN32)
        return !floating && !std::strcmp(api, CLAP_WINDOW_API_WIN32);
#else
        return !floating && !std::strcmp(api, CLAP_WINDOW_API_X11);
#endif
    }
    static bool guiPreferredApi(const clap_plugin_t*, const char** api, bool* floating)
    {
#if defined(_WIN32)
        *api = CLAP_WINDOW_API_WIN32;
#else
        *api = CLAP_WINDOW_API_X11;
#endif
        *floating = false;
        return true;
    }
    static bool guiCreate(const clap_plugin_t* p, const char* api, bool floating) { return !floating && self(p)->guiCreate(); }
    static void guiDestroy(const clap_plugin_t* p) { self(p)->guiDestroy(); }
    static bool guiSetScale(const clap_plugin_t* p, double s) { return self(p)->guiSetScale(s); }
    static bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { return self(p)->guiGetSize(w, h); }
    static bool guiCanResize(const clap_plugin_t*) { return true; }
    static bool guiResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints)
    {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        return true;
    }
    static bool guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { return self(p)->guiAdjustSize(w, h); }
    static bool guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) { return self(p)->guiSetSize(w, h); }
    static bool guiSetParent(const clap_plugin_t* p, const clap_window_t* w) { return self(p)->guiSetParent(w); }
    static bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
    static void guiSuggestTitle(const clap_plugin_t*, const char*) {}
    static bool guiShow(const clap_plugin_t* p) { return self(p)->guiShow(); }
    static bool guiHide(const clap_plugin_t* p) { return self(p)->guiHide(); }
    static void timerTick(const clap_plugin_t* p, clap_id id) { self(p)->onTimer(id); }
    static void fdReady(const clap_plugin_t* p, int fd, clap_posix_fd_flags_t) { self(p)->onFd(fd); }

    static const clap_plugin_audio_ports_t audioPorts;
    static const clap_plugin_note_ports_t notePorts;
    static const clap_plugin_params_t params;
    static const clap_plugin_state_t state;
    static const clap_plugin_gui_t gui;
    static const clap_plugin_timer_support_t timer;
    static const clap_plugin_posix_fd_support_t posixFd;
    static bool loaderLoad(const clap_plugin_t* p, const char* path)
    {
        std::string err;
        const bool ok = self(p)->loadFile(path, err);
        if (!ok)
            std::fprintf(stderr, "SidMidi: %s\n", err.c_str());
        return ok;
    }
    static uint32_t loaderMidiIn(const clap_plugin_t* p)
    {
        return self(p)->engine().midiInCount.load(std::memory_order_relaxed);
    }
    static const ExtLoader loader;
    // Offline bounces warp by a fixed amount, so they render the same every time.
    static bool renderHardRealtime(const clap_plugin_t*) { return false; }
    static bool renderSetMode(const clap_plugin_t* p, clap_plugin_render_mode mode)
    {
        self(p)->engine().realtime = mode == CLAP_RENDER_REALTIME;
        return true;
    }
    static const clap_plugin_render_t render;
};
const clap_plugin_render_t Glue::render = {renderHardRealtime, renderSetMode};
const ExtLoader Glue::loader = {loaderLoad, loaderMidiIn};

const clap_plugin_audio_ports_t Glue::audioPorts = {portsCount, portsGet};
const clap_plugin_note_ports_t Glue::notePorts = {notePortsCount, notePortsGet};
const clap_plugin_params_t Glue::params = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush};
const clap_plugin_state_t Glue::state = {stateSave, stateLoad};
const clap_plugin_gui_t Glue::gui = {guiApiSupported, guiPreferredApi, guiCreate, guiDestroy, guiSetScale, guiGetSize,
                                     guiCanResize, guiResizeHints, guiAdjustSize, guiSetSize, guiSetParent,
                                     guiSetTransient, guiSuggestTitle, guiShow, guiHide};
const clap_plugin_timer_support_t Glue::timer = {timerTick};
const clap_plugin_posix_fd_support_t Glue::posixFd = {fdReady};

const clap_plugin_descriptor_t* SidMidiPlugin::descriptor() { return &kDescriptor; }

// ------------------------------------------------------------------ lifetime

SidMidiPlugin::SidMidiPlugin(const clap_host_t* host) : host_(host)
{
    plugin_.desc = &kDescriptor;
    plugin_.plugin_data = this;
    plugin_.init = Glue::init;
    plugin_.destroy = Glue::destroy;
    plugin_.activate = Glue::activate;
    plugin_.deactivate = Glue::deactivate;
    plugin_.start_processing = Glue::startProcessing;
    plugin_.stop_processing = Glue::stopProcessing;
    plugin_.reset = Glue::reset;
    plugin_.process = Glue::process;
    plugin_.get_extension = Glue::getExtension;
    plugin_.on_main_thread = Glue::onMainThread;

    params_.push_back({pid::Master, "Output", "Volume", -60.0, 12.0, 0.0});
    for (int i = 0; i < 8; ++i)
        params_.push_back({pid::Level + i, "SIDs", "SID " + std::to_string(i + 1) + " Level", -60.0, 12.0, 0.0});
    for (int i = 0; i < 8; ++i)
        params_.push_back({pid::Pan + i, "SIDs", "SID " + std::to_string(i + 1) + " Pan", -1.0, 1.0,
                           static_cast<double>(engine_.mix[static_cast<size_t>(i)].pan)});
    for (int i = 0; i < kCcSlots; ++i)
        params_.push_back({pid::CcSlot + static_cast<uint32_t>(i), "CC", ccMap_.slotName(i), 0.0, 127.0, 0.0, true});
    for (auto& r : ccRoute_)
        r.store(-1);
    ccSent_.fill(-1);
    values_ = std::make_unique<std::atomic<double>[]>(params_.size());
    for (size_t i = 0; i < params_.size(); ++i)
        values_[i].store(params_[i].def);

    events_.reserve(4096);
    midiOut_.reserve(4096);
    sysexIn_.reserve(65536);
    sysexOut_.reserve(4096);
    txMessage_.reserve(16);
    silence_.assign(8192, 0.0f);

    engine_.videoWanted = false; // until a window opens
    configureMachine();
    applyParamsToEngine();
    prepareRomFolder();
    rescanRoms();
}

bool SidMidiPlugin::rescanRoms()
{
    if (hasRoms_)
        return true;
    emu::Roms roms;
    std::string report;
    const bool ok = romsFromFolder(romFolder(), roms, report);
    romReport_ = report;
    hasDriveRom_ = roms.drive1541.size() == 16384;
    if (!ok)
        return false;
    std::lock_guard<std::mutex> lock(machineLock_);
    auto& c64 = engine_.machine();
    c64.setRoms(roms);
    hasRoms_ = true;
    if (!pendingSnapshot_.empty() && emu::loadSnapshot(c64, pendingSnapshot_)) {
        if (!engine_.loadAudioState(pendingAudio_))
            engine_.resync();
    } else {
        std::string err;
        const std::string ext = lowerExt(lastFileName_);
        if ((ext == "prg" || ext == "p00") && !lastFileData_.empty())
            c64.attachPrg(lastFileData_, true, err);
        else if (ext == "d64" && !lastFileData_.empty())
            c64.attachDisk(lastFileData_, true, err);
        else if ((ext == "crt" || ext == "bin") && !lastFileData_.empty())
            c64.attachCartridge(lastFileData_, lastFileName_, err);
        else
            c64.powerOn();
        engine_.resync();
    }
    pendingSnapshot_.clear();
    pendingAudio_.clear();
    return true;
}

SidMidiPlugin::~SidMidiPlugin() = default;

bool SidMidiPlugin::init()
{
    hostParams_ = static_cast<const clap_host_params_t*>(host_->get_extension(host_, CLAP_EXT_PARAMS));
    hostState_ = static_cast<const clap_host_state_t*>(host_->get_extension(host_, CLAP_EXT_STATE));
    hostGui_ = static_cast<const clap_host_gui_t*>(host_->get_extension(host_, CLAP_EXT_GUI));
    hostTimer_ = static_cast<const clap_host_timer_support_t*>(host_->get_extension(host_, CLAP_EXT_TIMER_SUPPORT));
    hostFd_ = static_cast<const clap_host_posix_fd_support_t*>(host_->get_extension(host_, CLAP_EXT_POSIX_FD_SUPPORT));
    return true;
}

void SidMidiPlugin::destroy()
{
    guiDestroy();
    delete this;
}

bool SidMidiPlugin::activate(double sampleRate, uint32_t, uint32_t maxFrames)
{
    std::lock_guard<std::mutex> lock(machineLock_);
    engine_.setSampleRate(sampleRate);
    engine_.setMaxFrames(maxFrames);
    if (silence_.size() < maxFrames)
        silence_.assign(maxFrames, 0.0f);
    activated_ = true;
    return true;
}

void SidMidiPlugin::deactivate()
{
    activated_ = false;
}

void SidMidiPlugin::configureMachine()
{
    auto& c64 = engine_.machine();
    if (c64.standard() != settings_.standard)
        engine_.setStandard(settings_.standard);
    if (c64.midi.type != settings_.midi)
        c64.setMidiInterface(settings_.midi);
    c64.setDriveMode(settings_.driveMode);
    engine_.warpOnDiskAccess = settings_.warpOnDiskAccess;
    for (int p = 0; p < 2; ++p)
        if (c64.m.device[p] != settings_.portDevice[p])
            c64.setPortDevice(p, settings_.portDevice[p]);
    engine_.configureSids(settings_.sids);
}

void SidMidiPlugin::applySettings(const MachineSettings& s)
{
    std::lock_guard<std::mutex> lock(machineLock_);
    const bool standardChanged = s.standard != settings_.standard;
    settings_ = s;
    configureMachine();
    if (standardChanged && hasRoms_)
        engine_.machine().reset(); // the KERNAL measures PAL/NTSC at boot
}

const void* SidMidiPlugin::getExtension(const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &Glue::audioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &Glue::notePorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &Glue::params;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &Glue::state;
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &Glue::gui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &Glue::timer;
    if (!std::strcmp(id, kExtLoader))
        return &Glue::loader;
    if (!std::strcmp(id, CLAP_EXT_RENDER))
        return &Glue::render;
#if !defined(_WIN32)
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &Glue::posixFd;
#endif
    return nullptr;
}

void SidMidiPlugin::onMainThread() {}

// ------------------------------------------------------------------ ports

uint32_t SidMidiPlugin::audioPortsCount(bool isInput) const
{
    return isInput ? 0 : 1 + 8;
}

bool SidMidiPlugin::audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info_t* info) const
{
    if (isInput || index > 8)
        return false;
    std::memset(info, 0, sizeof(*info));
    info->in_place_pair = CLAP_INVALID_ID;
    if (index == 0) {
        info->id = 0;
        std::snprintf(info->name, sizeof(info->name), "Main");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
    } else {
        info->id = index;
        std::snprintf(info->name, sizeof(info->name), "SID %u", index);
        info->channel_count = 1;
        info->port_type = CLAP_PORT_MONO;
    }
    return true;
}

bool SidMidiPlugin::notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info) const
{
    if (index != 0)
        return false;
    std::memset(info, 0, sizeof(*info));
    info->id = isInput ? 0 : 1;
    if (isInput) {
        // The C64 program wants MIDI, so MIDI is what the host should send:
        // controllers, pitch bend and aftertouch come through untouched.
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        std::snprintf(info->name, sizeof(info->name), "MIDI In");
    } else {
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        std::snprintf(info->name, sizeof(info->name), "MIDI Out");
    }
    return true;
}

// ------------------------------------------------------------------ params

int SidMidiPlugin::paramIndex(uint32_t id) const
{
    for (size_t i = 0; i < params_.size(); ++i)
        if (params_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

bool SidMidiPlugin::paramsInfo(uint32_t index, clap_param_info_t* info) const
{
    if (index >= params_.size())
        return false;
    const auto& p = params_[index];
    std::memset(info, 0, sizeof(*info));
    info->id = p.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | (p.stepped ? CLAP_PARAM_IS_STEPPED : 0);
    std::snprintf(info->name, sizeof(info->name), "%s", p.name.c_str());
    std::snprintf(info->module, sizeof(info->module), "%s", p.module);
    info->min_value = p.lo;
    info->max_value = p.hi;
    info->default_value = p.def;
    return true;
}

bool SidMidiPlugin::paramsValue(clap_id id, double* value) const
{
    const int i = paramIndex(id);
    if (i < 0)
        return false;
    *value = values_[static_cast<size_t>(i)].load();
    return true;
}

double SidMidiPlugin::paramValue(uint32_t id) const
{
    double v = 0.0;
    paramsValue(id, &v);
    return v;
}

bool SidMidiPlugin::paramsToText(clap_id id, double v, char* display, uint32_t size) const
{
    if (pid::isCcSlot(id)) {
        std::snprintf(display, size, "%s",
                      ccMap_.valueText(static_cast<int>(id - pid::CcSlot), static_cast<int>(std::lround(v))).c_str());
        return true;
    }
    if (id >= pid::Pan && id < pid::Pan + 8) {
        if (std::fabs(v) < 0.005)
            std::snprintf(display, size, "C");
        else
            std::snprintf(display, size, "%d%% %s", static_cast<int>(std::lround(std::fabs(v) * 100)), v < 0 ? "L" : "R");
        return true;
    }
    if (paramIndex(id) < 0)
        return false;
    if (v <= -59.9)
        std::snprintf(display, size, "-inf dB");
    else
        std::snprintf(display, size, "%+.1f dB", v);
    return true;
}

bool SidMidiPlugin::paramsFromText(clap_id id, const char* display, double* value) const
{
    const int i = paramIndex(id);
    if (i < 0)
        return false;
    if (pid::isCcSlot(id)) {
        int cv;
        if (!ccMap_.valueFromText(static_cast<int>(id - pid::CcSlot), display, cv))
            return false;
        *value = cv;
        return true;
    }
    double v = std::atof(display);
    if (id >= pid::Pan && id < pid::Pan + 8) {
        if (std::strchr(display, 'L') || std::strchr(display, 'l'))
            v = -std::fabs(v);
        v /= 100.0;
    }
    *value = std::clamp(v, params_[static_cast<size_t>(i)].lo, params_[static_cast<size_t>(i)].hi);
    return true;
}

void SidMidiPlugin::applyParam(uint32_t id, double v, bool send)
{
    const int i = paramIndex(id);
    if (i < 0)
        return;
    if (pid::isCcSlot(id))
        v = std::clamp(std::round(v), 0.0, 127.0); // stepped: whole controller values
    values_[static_cast<size_t>(i)].store(v);
    if (pid::isCcSlot(id)) {
        if (send)
            ccDirty_.fetch_or(uint64_t{1} << (id - pid::CcSlot), std::memory_order_acq_rel);
    } else if (id == pid::Master)
        engine_.masterGain = static_cast<float>(dbToGain(v));
    else if (id >= pid::Level && id < pid::Level + 8)
        engine_.mix[id - pid::Level].level = static_cast<float>(dbToGain(v));
    else if (id >= pid::Pan && id < pid::Pan + 8)
        engine_.mix[id - pid::Pan].pan = static_cast<float>(v);
}

void SidMidiPlugin::applyParamsToEngine()
{
    for (size_t i = 0; i < params_.size(); ++i)
        applyParam(params_[i].id, values_[i].load(), false);
}

// ------------------------------------------------------------------ CC maps

void SidMidiPlugin::publishCcRoutes()
{
    for (int i = 0; i < kCcSlots; ++i) {
        const auto& s = ccMap_.slots[i];
        ccRoute_[static_cast<size_t>(i)].store(s.used() ? (ccMap_.channelOf(i) << 8) | s.cc : -1);
        params_[static_cast<size_t>(paramIndex(pid::CcSlot + static_cast<uint32_t>(i)))].name = ccMap_.slotName(i);
    }
    ++ccMapSerial_;
    if (hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT);
}

void SidMidiPlugin::setCcMap(const CcMap& map)
{
    ccMap_ = map;
    publishCcRoutes();
}

void SidMidiPlugin::setCcSlot(int slot, const CcSlot& s)
{
    if (slot < 0 || slot >= kCcSlots)
        return;
    ccMap_.slots[slot] = s;
    publishCcRoutes();
}

void SidMidiPlugin::followProgram(const std::string& fileName)
{
    if (!settings_.ccFollowProgram)
        return;
    // The user's own maps first: one made for a program replaces the factory's.
    const auto maps = availableCcMaps();
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& e : maps)
            if (e.factory == (pass == 1) && e.map.matchesProgram(fileName)) {
                setCcMap(e.map);
                return;
            }
}

void SidMidiPlugin::emitCcSlot(int slot, uint32_t offset)
{
    const int32_t route = ccRoute_[static_cast<size_t>(slot)].load(std::memory_order_relaxed);
    const int v = std::clamp(static_cast<int>(std::lround(values_[static_cast<size_t>(pid::kCount - kCcSlots + slot)].load())), 0, 127);
    if (route < 0 || v == ccSent_[static_cast<size_t>(slot)] || events_.size() >= events_.capacity())
        return;
    ccSent_[static_cast<size_t>(slot)] = static_cast<int16_t>(v);
    MidiEvent e;
    e.offset = offset;
    e.size = 3;
    e.data[0] = static_cast<uint8_t>(0xb0 | ((route >> 8) & 15));
    e.data[1] = static_cast<uint8_t>(route & 127);
    e.data[2] = static_cast<uint8_t>(v);
    events_.push_back(e);
}

void SidMidiPlugin::guiBeginParam(uint32_t id)
{
    guiParams_.push({GuiParamEvent::Begin, id, 0.0});
}

void SidMidiPlugin::guiSetParam(uint32_t id, double value)
{
    const int i = paramIndex(id);
    if (i < 0)
        return;
    value = std::clamp(value, params_[static_cast<size_t>(i)].lo, params_[static_cast<size_t>(i)].hi);
    values_[static_cast<size_t>(i)].store(value);
    guiParams_.push({GuiParamEvent::Value, id, value});
    if (hostParams_ && !activated_)
        hostParams_->request_flush(host_);
}

void SidMidiPlugin::guiEndParam(uint32_t id)
{
    guiParams_.push({GuiParamEvent::End, id, 0.0});
    if (hostParams_)
        hostParams_->request_flush(host_);
}

void SidMidiPlugin::drainGuiParams(const clap_output_events_t* out)
{
    GuiParamEvent e;
    while (guiParams_.pop(e)) {
        if (e.kind == GuiParamEvent::Value) {
            applyParam(e.id, e.value);
            if (out) {
                clap_event_param_value_t ev{};
                ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
                ev.param_id = e.id;
                ev.note_id = -1;
                ev.port_index = -1;
                ev.channel = -1;
                ev.key = -1;
                ev.value = e.value;
                out->try_push(out, &ev.header);
            }
        } else if (out) {
            clap_event_param_gesture_t ev{};
            ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID,
                         static_cast<uint16_t>(e.kind == GuiParamEvent::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                                              : CLAP_EVENT_PARAM_GESTURE_END),
                         0};
            ev.param_id = e.id;
            out->try_push(out, &ev.header);
        }
    }
}

void SidMidiPlugin::paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out)
{
    if (in) {
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_event_header_t* h = in->get(in, i);
            if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
                const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(h);
                applyParam(pv->param_id, pv->value);
            }
        }
    }
    drainGuiParams(out);
}

// ------------------------------------------------------------------ input from the GUI

void SidMidiPlugin::key(uint8_t matrixPos, bool down)
{
    input_.push({InputEvent::Key, matrixPos, static_cast<uint8_t>(down ? 1 : 0)});
}

void SidMidiPlugin::joystick(int port, uint8_t bits)
{
    input_.push({InputEvent::Joy, static_cast<uint8_t>(port), bits});
}

void SidMidiPlugin::restoreKey()
{
    input_.push({InputEvent::Restore, 0, 0});
}

void SidMidiPlugin::releaseInput()
{
    input_.push({InputEvent::ReleaseAll, 0, 0});
}

void SidMidiPlugin::mouseMove(int port, int dx, int dy)
{
    input_.push({InputEvent::Mouse, static_cast<uint8_t>(port), 0, static_cast<int16_t>(dx), static_cast<int16_t>(dy)});
}

void SidMidiPlugin::paddles(int port, uint8_t a, uint8_t b)
{
    input_.push({InputEvent::Paddles, static_cast<uint8_t>(port), 0, a, b});
}

void SidMidiPlugin::deviceButtons(int port, uint8_t bits)
{
    input_.push({InputEvent::DevButtons, static_cast<uint8_t>(port), bits});
}

void SidMidiPlugin::handleInputEvents()
{
    auto& c64 = engine_.machine();
    InputEvent e;
    while (input_.pop(e)) {
        switch (e.kind) {
        case InputEvent::Key: c64.setKey(e.a, e.b != 0); break;
        case InputEvent::Joy: c64.setJoystick(e.a, e.b); break;
        case InputEvent::Restore: c64.pressRestore(); break;
        case InputEvent::ReleaseAll:
            c64.releaseAllKeys();
            c64.setJoystick(0, 0);
            c64.setJoystick(1, 0);
            c64.setDeviceButtons(0, 0);
            c64.setDeviceButtons(1, 0);
            break;
        case InputEvent::Mouse: c64.moveMouse(e.a, e.c, e.d); break;
        case InputEvent::Paddles: c64.setPaddles(e.a, static_cast<uint8_t>(e.c), static_cast<uint8_t>(e.d)); break;
        case InputEvent::DevButtons: c64.setDeviceButtons(e.a, e.b); break;
        }
    }
}

// ------------------------------------------------------------------ media

bool SidMidiPlugin::loadFile(const std::string& path, std::string& error)
{
    std::vector<uint8_t> data;
    if (!readFile(path, data)) {
        error = "cannot read " + path;
        return false;
    }
    return loadData(baseName(path), data, error);
}

bool SidMidiPlugin::loadData(const std::string& name, const std::vector<uint8_t>& data, std::string& error)
{
    if (!hasRoms_) {
        error = "The C64 ROMs are missing: put them into " + romFolder();
        return false;
    }
    const std::string ext = lowerExt(name);
    std::unique_lock<std::mutex> lock(machineLock_);
    auto& c64 = engine_.machine();
    bool ok = false;
    if (ext == "prg" || ext == "p00") {
        std::vector<uint8_t> prg = data;
        if (ext == "p00" && prg.size() > 26)
            prg.erase(prg.begin(), prg.begin() + 26); // the PC64 header
        ok = c64.attachPrg(prg, true, error);
    } else if (ext == "d64") {
        ok = c64.attachDisk(data, true, error);
    } else if (ext == "crt" || ext == "bin") {
        ok = c64.attachCartridge(data, name, error);
    } else {
        error = "unknown file type ." + ext + " (expected .prg, .d64, .crt or .bin)";
    }
    if (ok) {
        lastFileName_ = name;
        lastFileData_ = data;
        engine_.resync();
    }
    lastError_ = ok ? std::string() : error;
    lock.unlock();
    if (ok)
        followProgram(name); // reads the map folder: not under the machine lock
    return ok;
}

void SidMidiPlugin::powerCycle()
{
    std::lock_guard<std::mutex> lock(machineLock_);
    auto& c64 = engine_.machine();
    // Power on with what is attached: a disk or a .prg autostarts again.
    std::string err;
    const std::string ext = lowerExt(lastFileName_);
    if ((ext == "prg" || ext == "p00") && !lastFileData_.empty()) {
        std::vector<uint8_t> prg = lastFileData_;
        if (ext == "p00" && prg.size() > 26)
            prg.erase(prg.begin(), prg.begin() + 26);
        c64.attachPrg(prg, true, err);
    } else if (c64.drive.disk.present() && !c64.cart.present()) {
        c64.autostart_ = emu::C64::Autostart::Disk;
        c64.powerOn();
    } else {
        c64.powerOn();
    }
    engine_.resync();
}

void SidMidiPlugin::resetButton()
{
    std::lock_guard<std::mutex> lock(machineLock_);
    engine_.machine().reset();
    engine_.resync();
}

void SidMidiPlugin::ejectDisk()
{
    std::lock_guard<std::mutex> lock(machineLock_);
    engine_.machine().ejectDisk();
}

void SidMidiPlugin::ejectCartridge()
{
    std::lock_guard<std::mutex> lock(machineLock_);
    engine_.machine().ejectCartridge();
    engine_.resync();
}

bool SidMidiPlugin::exportDisk(const std::string& path, std::string& error)
{
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(machineLock_);
        data = engine_.machine().diskImage().data();
    }
    if (data.empty()) {
        error = "no disk attached";
        return false;
    }
    if (!writeFile(path, data)) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

void SidMidiPlugin::setWarp(bool on)
{
    std::lock_guard<std::mutex> lock(machineLock_);
    engine_.setWarp(on);
}

std::string SidMidiPlugin::mediaSummary()
{
    std::lock_guard<std::mutex> lock(machineLock_);
    auto& c64 = engine_.machine();
    std::string s;
    if (c64.cart.present())
        s += "Cartridge: " + baseName(c64.cart.name());
    if (c64.drive.disk.present()) {
        if (!s.empty())
            s += "   ";
        std::string name = c64.drive.disk.diskName();
        while (!name.empty() && static_cast<uint8_t>(name.back()) == 0xa0)
            name.pop_back();
        for (auto& ch : name)
            if (static_cast<uint8_t>(ch) < 0x20 || static_cast<uint8_t>(ch) > 0x7e)
                ch = ' ';
        s += "Disk: " + name + (c64.drive.dirty ? " (changed)" : "");
    }
    if (s.empty() && !lastFileName_.empty())
        s = lastFileName_;
    return s;
}

// ------------------------------------------------------------------ process

void SidMidiPlugin::collectEvents(const clap_process_t* p)
{
    events_.clear();
    sysexIn_.clear();
    const clap_input_events_t* in = p->in_events;
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        MidiEvent e;
        e.offset = h->time;
        switch (h->type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF: {
            const auto* ne = reinterpret_cast<const clap_event_note_t*>(h);
            if (ne->key < 0 || ne->key > 127)
                break;
            const int ch = ne->channel >= 0 ? (ne->channel & 15) : 0;
            const int vel = std::clamp(static_cast<int>(std::lround(ne->velocity * 127.0)), 0, 127);
            if (h->type == CLAP_EVENT_NOTE_ON) {
                e.data[0] = static_cast<uint8_t>(0x90 | ch);
                e.data[2] = static_cast<uint8_t>(std::max(1, vel));
            } else {
                e.data[0] = static_cast<uint8_t>(0x80 | ch);
                e.data[2] = static_cast<uint8_t>(vel);
            }
            e.data[1] = static_cast<uint8_t>(ne->key);
            e.size = 3;
            if (events_.size() < events_.capacity()) // reserved: never grow in process()
                events_.push_back(e);
            break;
        }
        case CLAP_EVENT_MIDI: {
            const auto* me = reinterpret_cast<const clap_event_midi_t*>(h);
            const uint8_t st = me->data[0];
            if (st < 0x80)
                break;
            e.data[0] = st;
            e.data[1] = me->data[1];
            e.data[2] = me->data[2];
            if (st >= 0xf8)
                e.size = 1;
            else if ((st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0 || st == 0xf1 || st == 0xf3)
                e.size = 2;
            else if (st == 0xf6)
                e.size = 1;
            else
                e.size = 3;
            if (events_.size() < events_.capacity()) // reserved: never grow in process()
                events_.push_back(e);
            break;
        }
        case CLAP_EVENT_MIDI_SYSEX: {
            const auto* se = reinterpret_cast<const clap_event_midi_sysex_t*>(h);
            if (!se->buffer || !se->size || sysexIn_.size() + se->size > sysexIn_.capacity()
                || events_.size() >= events_.capacity())
                break;
            const size_t at = sysexIn_.size();
            sysexIn_.insert(sysexIn_.end(), se->buffer, se->buffer + se->size);
            e.ext = sysexIn_.data() + at; // capacity is reserved, so this stays valid
            e.extSize = se->size;
            if (events_.size() < events_.capacity()) // reserved: never grow in process()
                events_.push_back(e);
            break;
        }
        case CLAP_EVENT_PARAM_VALUE: {
            const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(h);
            applyParam(pv->param_id, pv->value, false);
            if (pid::isCcSlot(pv->param_id))
                emitCcSlot(static_cast<int>(pv->param_id - pid::CcSlot), h->time); // at its sample
            break;
        }
        default:
            break;
        }
    }
    // Slots moved from the window or in a flush since the last block.
    for (uint64_t dirty = ccDirty_.exchange(0, std::memory_order_acq_rel); dirty; dirty &= dirty - 1)
        emitCcSlot(__builtin_ctzll(dirty), 0);
    if (settings_.sendClock)
        generateClock(p);
    // Stable and in place: std::stable_sort takes a temporary buffer from the
    // heap. The host's events arrive sorted and the clock's are sorted among
    // themselves, so this is close to linear.
    for (size_t i = 1; i < events_.size(); ++i) {
        const MidiEvent e = events_[i];
        size_t j = i;
        for (; j > 0 && events_[j - 1].offset > e.offset; --j)
            events_[j] = events_[j - 1];
        events_[j] = e;
    }
}

// MIDI clock (24 per quarter note), start/continue/stop and the song position
// from the host transport, for programs that follow a sequencer.
void SidMidiPlugin::generateClock(const clap_process_t* p)
{
    const clap_event_transport_t* t = p->transport;
    const bool playing = t && (t->flags & CLAP_TRANSPORT_IS_PLAYING);
    auto push1 = [&](uint32_t off, uint8_t b) {
        MidiEvent e;
        e.offset = off;
        e.size = 1;
        e.data[0] = b;
        if (events_.size() < events_.capacity())
            events_.push_back(e);
    };
    if (!playing) {
        if (wasPlaying_)
            push1(0, 0xfc);
        wasPlaying_ = false;
        nextClockTick_ = -1.0;
        return;
    }
    if (!(t->flags & CLAP_TRANSPORT_HAS_TEMPO) || !(t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) || t->tempo <= 0.0)
        return;
    const double pos = static_cast<double>(t->song_pos_beats) / CLAP_BEATTIME_FACTOR;
    const double beatsPerSample = t->tempo / 60.0 / engine_.sampleRate();
    if (!wasPlaying_) {
        // Song position in sixteenths, then start (from zero) or continue.
        const int spp = std::max(0, static_cast<int>(std::floor(pos * 4.0)));
        MidiEvent e;
        e.offset = 0;
        e.size = 3;
        e.data[0] = 0xf2;
        e.data[1] = static_cast<uint8_t>(spp & 0x7f);
        e.data[2] = static_cast<uint8_t>((spp >> 7) & 0x7f);
        if (events_.size() < events_.capacity())
            events_.push_back(e);
        push1(0, pos < 1e-6 ? 0xfa : 0xfb);
        nextClockTick_ = std::ceil(pos * 24.0 - 1e-9);
    }
    wasPlaying_ = true;
    const double end = (pos + p->frames_count * beatsPerSample) * 24.0;
    if (nextClockTick_ < pos * 24.0 - 48.0 || nextClockTick_ > end + 48.0)
        nextClockTick_ = std::ceil(pos * 24.0 - 1e-9); // the host jumped
    while (nextClockTick_ < end) {
        const double off = (nextClockTick_ / 24.0 - pos) / beatsPerSample;
        push1(static_cast<uint32_t>(std::clamp(off, 0.0, static_cast<double>(p->frames_count - 1))), 0xf8);
        nextClockTick_ += 1.0;
    }
}

// The ACIA's byte stream back into messages (running status included), for
// the host's MIDI output.
void SidMidiPlugin::emitMidiOut(const clap_output_events_t* out)
{
    if (!out)
        return;
    sysexOut_.clear();
    for (const auto& m : midiOut_) {
        const uint8_t b = m.data[0];
        auto sendShort = [&](const uint8_t* d, size_t n) {
            clap_event_midi_t ev{};
            ev.header = {sizeof(ev), m.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            ev.port_index = 0;
            for (size_t k = 0; k < 3; ++k)
                ev.data[k] = k < n ? d[k] : 0;
            out->try_push(out, &ev.header);
        };
        if (b >= 0xf8) { // real time, between any two bytes
            sendShort(&b, 1);
            continue;
        }
        if (b == 0xf0) {
            txInSysex_ = true;
            txMessage_.clear();
            sysexOut_.clear();
            sysexOut_.push_back(b);
            continue;
        }
        if (txInSysex_) {
            if (b == 0xf7 || b >= 0x80) {
                sysexOut_.push_back(0xf7);
                clap_event_midi_sysex_t ev{};
                ev.header = {sizeof(ev), m.offset, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
                ev.port_index = 0;
                ev.buffer = sysexOut_.data();
                ev.size = static_cast<uint32_t>(sysexOut_.size());
                out->try_push(out, &ev.header);
                txInSysex_ = false;
                if (b == 0xf7)
                    continue;
            } else {
                if (sysexOut_.size() + 1 < sysexOut_.capacity()) // room for the closing F7
                    sysexOut_.push_back(b);
                continue;
            }
        }
        if (b >= 0x80) {
            txMessage_.clear();
            txMessage_.push_back(b);
            if (b < 0xf0)
                txRunning_ = b;
            const uint8_t hi = b & 0xf0;
            txExpected_ = (hi == 0xc0 || hi == 0xd0 || b == 0xf1 || b == 0xf3) ? 2 : (b == 0xf6 ? 1 : 3);
        } else {
            if (txMessage_.empty()) {
                if (!txRunning_)
                    continue;
                txMessage_.push_back(txRunning_);
                const uint8_t hi = txRunning_ & 0xf0;
                txExpected_ = (hi == 0xc0 || hi == 0xd0) ? 2 : 3;
            }
            txMessage_.push_back(b);
        }
        if (static_cast<int>(txMessage_.size()) >= txExpected_) {
            sendShort(txMessage_.data(), txMessage_.size());
            txMessage_.clear();
        }
    }
}

clap_process_status SidMidiPlugin::process(const clap_process_t* p)
{
    const uint32_t frames = p->frames_count;
    // The lock, with a short spin: a GUI action or a state save holds it for
    // well under a millisecond.
    std::unique_lock<std::mutex> lock(machineLock_, std::defer_lock);
    const auto t0 = std::chrono::steady_clock::now();
    while (!lock.try_lock()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(1500))
            break;
        std::this_thread::yield();
    }

    Engine::Outputs outs;
    auto port = [&](uint32_t i, uint32_t ch) -> float* {
        if (i >= p->audio_outputs_count)
            return nullptr;
        const auto& b = p->audio_outputs[i];
        if (!b.data32 || ch >= b.channel_count)
            return nullptr;
        return b.data32[ch];
    };
    outs.left = port(0, 0);
    outs.right = port(0, 1);
    for (int i = 0; i < 8; ++i)
        outs.sid[i] = port(static_cast<uint32_t>(i + 1), 0);

    if (!lock.owns_lock() || !hasRoms_) {
        for (uint32_t i = 0; i < p->audio_outputs_count; ++i)
            for (uint32_t c = 0; c < p->audio_outputs[i].channel_count; ++c)
                if (p->audio_outputs[i].data32 && p->audio_outputs[i].data32[c])
                    std::fill(p->audio_outputs[i].data32[c], p->audio_outputs[i].data32[c] + frames, 0.0f);
        drainGuiParams(p->out_events);
        return CLAP_PROCESS_CONTINUE;
    }

    handleInputEvents();
    drainGuiParams(p->out_events);
    collectEvents(p);
    midiOut_.clear();
    engine_.process(frames, events_.data(), events_.size(), outs, midiOut_);
    emitMidiOut(p->out_events);
    return CLAP_PROCESS_CONTINUE;
}

// ------------------------------------------------------------------ state

bool SidMidiPlugin::stateSave(const clap_ostream_t* stream)
{
    std::vector<uint8_t> out(kStateMagic, kStateMagic + 8);
    putU32(out, 1);
    const std::string settings = settings_.toText();
    putBlob(out, settings.data(), settings.size());
    std::string params;
    for (size_t i = 0; i < params_.size(); ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%u = %.6f\n", params_[i].id, values_[i].load());
        params += buf;
    }
    putBlob(out, params.data(), params.size());
    std::vector<uint8_t> snap, audio;
    {
        std::lock_guard<std::mutex> lock(machineLock_);
        if (!hasRoms_ && !pendingSnapshot_.empty()) {
            // Still waiting for the ROMs: the project's machine stays as it was.
            snap = pendingSnapshot_;
            audio = pendingAudio_;
        } else {
            snap = emu::saveSnapshot(engine_.machine());
            audio = engine_.saveAudioState();
        }
    }
    putBlob(out, snap.data(), snap.size());
    putBlob(out, lastFileName_.data(), lastFileName_.size());
    putBlob(out, lastFileData_.data(), lastFileData_.size());
    putBlob(out, audio.data(), audio.size());
    // The CC map in full, so the project keeps its slot names without the file.
    const std::string map = ccMap_.toText();
    putBlob(out, map.data(), map.size());

    size_t at = 0;
    while (at < out.size()) {
        const int64_t n = stream->write(stream, out.data() + at, out.size() - at);
        if (n <= 0)
            return false;
        at += static_cast<size_t>(n);
    }
    return true;
}

bool SidMidiPlugin::stateLoad(const clap_istream_t* stream)
{
    std::vector<uint8_t> in;
    uint8_t buf[65536];
    for (;;) {
        const int64_t n = stream->read(stream, buf, sizeof(buf));
        if (n < 0)
            return false;
        if (n == 0)
            break;
        in.insert(in.end(), buf, buf + n);
    }
    if (in.size() < 12 || std::memcmp(in.data(), kStateMagic, 8) != 0)
        return false;
    size_t at = 8;
    uint32_t version;
    std::vector<uint8_t> settings, params, snap, fileName, fileData, audio, ccMapText;
    if (!getU32(in, at, version) || !getBlob(in, at, settings) || !getBlob(in, at, params) || !getBlob(in, at, snap))
        return false;
    getBlob(in, at, fileName);
    getBlob(in, at, fileData);
    getBlob(in, at, audio);
    getBlob(in, at, ccMapText); // absent before 0.3.0: no map
    setCcMap(CcMap::fromText(std::string(ccMapText.begin(), ccMapText.end())));

    MachineSettings s;
    s.fromText(std::string(settings.begin(), settings.end()));
    {
        // Parameters.
        const std::string text(params.begin(), params.end());
        size_t pos = 0;
        while (pos < text.size()) {
            const auto nl = text.find('\n', pos);
            const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            unsigned id;
            double v;
            if (std::sscanf(line.c_str(), "%u = %lf", &id, &v) == 2) {
                const int i = paramIndex(id);
                if (i >= 0)
                    values_[static_cast<size_t>(i)].store(std::clamp(v, params_[static_cast<size_t>(i)].lo, params_[static_cast<size_t>(i)].hi));
            }
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
    }

    std::lock_guard<std::mutex> lock(machineLock_);
    settings_ = s;
    configureMachine();
    applyParamsToEngine();
    lastFileName_.assign(fileName.begin(), fileName.end());
    lastFileData_ = fileData;
    auto& c64 = engine_.machine();
    if (!hasRoms_) {
        pendingSnapshot_ = snap;
        pendingAudio_ = audio;
        return true;
    }
    if (emu::loadSnapshot(c64, snap)) {
        if (!engine_.loadAudioState(audio))
            engine_.resync();
    } else {
        // A snapshot from another build: start the attached media again.
        std::string err;
        const std::string ext = lowerExt(lastFileName_);
        if ((ext == "prg" || ext == "p00") && !lastFileData_.empty())
            c64.attachPrg(lastFileData_, true, err);
        else if (ext == "d64" && !lastFileData_.empty())
            c64.attachDisk(lastFileData_, true, err);
        else if ((ext == "crt" || ext == "bin") && !lastFileData_.empty())
            c64.attachCartridge(lastFileData_, lastFileName_, err);
        else
            c64.powerOn();
        engine_.resync();
    }
    if (hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);
    return true;
}

// ------------------------------------------------------------------ GUI

bool SidMidiPlugin::guiCreate()
{
    if (editor_)
        return true;
    editor_ = std::make_unique<gui::Editor>(*this);
    engine_.videoWanted = true;
    editor_->requestResize = [this](int w, int h) {
        return hostGui_ && hostGui_->request_resize(host_, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    };
    if (hostTimer_)
        hostTimer_->register_timer(host_, 20, &timerId_); // the C64's 50 frames a second
    return true;
}

void SidMidiPlugin::guiDestroy()
{
    if (!editor_)
        return;
    if (hostTimer_ && timerId_ != CLAP_INVALID_ID)
        hostTimer_->unregister_timer(host_, timerId_);
    timerId_ = CLAP_INVALID_ID;
    if (hostFd_ && registeredFd_ >= 0)
        hostFd_->unregister_fd(host_, registeredFd_);
    registeredFd_ = -1;
    editor_.reset();
    engine_.videoWanted = false;
    releaseInput();
}

bool SidMidiPlugin::guiSetScale(double scale)
{
    if (!editor_)
        return false;
    editor_->setScale(scale);
    return true;
}

bool SidMidiPlugin::guiGetSize(uint32_t* w, uint32_t* h)
{
    if (!editor_)
        return false;
    int pw, ph;
    editor_->physicalSize(pw, ph);
    *w = static_cast<uint32_t>(pw);
    *h = static_cast<uint32_t>(ph);
    return true;
}

bool SidMidiPlugin::guiAdjustSize(uint32_t* w, uint32_t* h)
{
    const double s = editor_ ? editor_->scale() : 1.0;
    *w = std::max(*w, static_cast<uint32_t>(gui::Editor::kMinW * s));
    *h = std::max(*h, static_cast<uint32_t>(gui::Editor::kMinH * s));
    return true;
}

bool SidMidiPlugin::guiSetSize(uint32_t w, uint32_t h)
{
    if (!editor_)
        return false;
    editor_->setPhysicalSize(static_cast<int>(w), static_cast<int>(h));
    return true;
}

bool SidMidiPlugin::guiSetParent(const clap_window_t* window)
{
    if (!editor_ || !window)
        return false;
#if defined(_WIN32)
    const auto parent = reinterpret_cast<uintptr_t>(window->win32);
#else
    const auto parent = static_cast<uintptr_t>(window->x11);
#endif
    if (!editor_->attach(parent))
        return false;
    const int fd = editor_->fd();
    if (hostFd_ && fd >= 0 && hostFd_->register_fd(host_, fd, CLAP_POSIX_FD_READ))
        registeredFd_ = fd;
    return true;
}

bool SidMidiPlugin::guiShow()
{
    if (editor_)
        editor_->show();
    return editor_ != nullptr;
}

bool SidMidiPlugin::guiHide()
{
    if (editor_)
        editor_->hide();
    return editor_ != nullptr;
}

void SidMidiPlugin::onTimer(clap_id id)
{
    if (editor_ && id == timerId_)
        editor_->onTimer();
}

void SidMidiPlugin::onFd(int fd)
{
    if (editor_ && fd == registeredFd_)
        editor_->onFd();
}

// ------------------------------------------------------------------ factory

namespace {

uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? SidMidiPlugin::descriptor() : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, kDescriptor.id) != 0)
        return nullptr;
    auto* p = new SidMidiPlugin(host);
    return p->clapPlugin();
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

} // namespace

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* factoryId)
{
    if (!std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID))
        return &kFactory;
    return nullptr;
}

} // namespace sidmidi
