#include "AurumPlugin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include <xmmintrin.h>

#include "aurum.h"
#include "gui/Editor.h"
#include "state/AudioFile.h"
#include "state/IrImport.h"
#include "state/PresetManager.h"
#include "state/Settings.h"
#include "state/StateIO.h"
#include "util/Path.h"

namespace aurum {

namespace {

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB,
                                 CLAP_PLUGIN_FEATURE_STEREO, CLAP_PLUGIN_FEATURE_MONO, nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    kPluginId,
    kPluginName,
    kPluginVendor,
    kPluginUrl,
    "",
    "",
    kPluginVersion,
    kPluginDescription,
    kFeatures,
};

AurumPlugin* self(const clap_plugin_t* p) { return static_cast<AurumPlugin*>(p->plugin_data); }

// The one window API each platform's editor is written against.
#if defined(_WIN32)
constexpr const char* kWindowApi = CLAP_WINDOW_API_WIN32;
#else
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;
#endif

struct PortsConfig
{
    const char* name;
    uint32_t in;
    uint32_t out;
};
constexpr PortsConfig kConfigs[] = {{"Stereo", 2, 2}, {"Mono", 1, 1}, {"Mono to Stereo", 1, 2}};

// Denormals flush-to-zero for the duration of a process call.
struct ScopedFtz
{
    unsigned int saved;
    ScopedFtz() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040); }
    ~ScopedFtz() { _mm_setcsr(saved); }
};

} // namespace

// Static C callbacks forwarding to the C++ object.
struct PluginGlue
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
    static uint32_t configCount(const clap_plugin_t* p) { return self(p)->portsConfigCount(); }
    static bool configGet(const clap_plugin_t* p, uint32_t i, clap_audio_ports_config_t* c)
    {
        return self(p)->portsConfigGet(i, c);
    }
    static bool configSelect(const clap_plugin_t* p, clap_id id) { return self(p)->portsConfigSelect(id); }

    static uint32_t paramsCount(const clap_plugin_t* p) { return static_cast<uint32_t>(self(p)->table_.count()); }
    static bool paramsInfo(const clap_plugin_t* p, uint32_t i, clap_param_info_t* info) { return self(p)->paramsGetInfo(i, info); }
    static bool paramsValue(const clap_plugin_t* p, clap_id id, double* v) { return self(p)->paramsGetValue(id, v); }
    static bool paramsToText(const clap_plugin_t* p, clap_id id, double v, char* d, uint32_t s)
    {
        return self(p)->paramsValueToText(id, v, d, s);
    }
    static bool paramsFromText(const clap_plugin_t* p, clap_id id, const char* d, double* v)
    {
        return self(p)->paramsTextToValue(id, d, v);
    }
    static void paramsFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
    {
        self(p)->paramsFlush(in, out);
    }
    static bool stateSave(const clap_plugin_t* p, const clap_ostream_t* s) { return self(p)->stateSave(s); }
    static bool stateLoad(const clap_plugin_t* p, const clap_istream_t* s) { return self(p)->stateLoad(s); }
    static uint32_t latencyGet(const clap_plugin_t*) { return 0; }
    static uint32_t tailGet(const clap_plugin_t* p) { return self(p)->tailGet(); }
    static bool renderHardRealtime(const clap_plugin_t*) { return false; }
    static bool renderSet(const clap_plugin_t* p, clap_plugin_render_mode mode)
    {
        AurumPlugin* s = self(p);
        s->offline_ = mode == CLAP_RENDER_OFFLINE;
        s->engine_.setOffline(s->offline_);
        return true;
    }

    static bool guiApiSupported(const clap_plugin_t*, const char* api, bool floating)
    {
        return !floating && !std::strcmp(api, kWindowApi);
    }
    static bool guiPreferredApi(const clap_plugin_t*, const char** api, bool* floating)
    {
        *api = kWindowApi;
        *floating = false;
        return true;
    }
    static bool guiCreate(const clap_plugin_t* p, const char* api, bool floating)
    {
        return guiApiSupported(p, api, floating) && self(p)->guiCreate();
    }
    static void guiDestroy(const clap_plugin_t* p) { self(p)->guiDestroy(); }
    static bool guiSetScale(const clap_plugin_t* p, double s) { return self(p)->guiSetScale(s); }
    static bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) { return self(p)->guiGetSize(w, h); }
    static bool guiCanResize(const clap_plugin_t*) { return true; }
    static bool guiResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints)
    {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        hints->aspect_ratio_width = 0;
        hints->aspect_ratio_height = 0;
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
    static void fdReady(const clap_plugin_t* p, int fd, clap_posix_fd_flags_t flags) { self(p)->onFd(fd, flags); }

    static uint32_t notePortsCount(const clap_plugin_t* p, bool in) { return self(p)->notePortsCount(in); }
    static bool notePortsGet(const clap_plugin_t* p, uint32_t i, bool in, clap_note_port_info_t* info)
    {
        return self(p)->notePortsGet(i, in, info);
    }
    static const clap_plugin_note_ports_t notePorts;
    static const clap_plugin_gui_t gui;
    static const clap_plugin_timer_support_t timer;
    static const clap_plugin_posix_fd_support_t posixFd;
    static const clap_plugin_audio_ports_t audioPorts;
    static const clap_plugin_audio_ports_config_t audioPortsConfig;
    static const clap_plugin_params_t params;
    static const clap_plugin_state_t state;
    static const clap_plugin_latency_t latency;
    static const clap_plugin_tail_t tail;
    static const clap_plugin_render_t render;
};

const clap_plugin_gui_t PluginGlue::gui = {guiApiSupported, guiPreferredApi, guiCreate,      guiDestroy,
                                           guiSetScale,     guiGetSize,      guiCanResize,   guiResizeHints,
                                           guiAdjustSize,   guiSetSize,      guiSetParent,   guiSetTransient,
                                           guiSuggestTitle, guiShow,         guiHide};
const clap_plugin_timer_support_t PluginGlue::timer = {timerTick};
const clap_plugin_note_ports_t PluginGlue::notePorts = {notePortsCount, notePortsGet};
const clap_plugin_posix_fd_support_t PluginGlue::posixFd = {fdReady};
const clap_plugin_audio_ports_t PluginGlue::audioPorts = {portsCount, portsGet};
const clap_plugin_audio_ports_config_t PluginGlue::audioPortsConfig = {configCount, configGet, configSelect};
const clap_plugin_params_t PluginGlue::params = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText,
                                                 paramsFlush};
const clap_plugin_state_t PluginGlue::state = {stateSave, stateLoad};
const clap_plugin_latency_t PluginGlue::latency = {latencyGet};
const clap_plugin_tail_t PluginGlue::tail = {tailGet};
const clap_plugin_render_t PluginGlue::render = {renderHardRealtime, renderSet};

const clap_plugin_descriptor_t* AurumPlugin::descriptor() { return &kDescriptor; }

AurumPlugin::AurumPlugin(const clap_host_t* host)
    : host_(host), table_(ParamTable::get()),
      session_([this](const std::vector<double>& v) { applyValues(v); }, [this] { return currentValues(); })
{
    plugin_.desc = &kDescriptor;
    plugin_.plugin_data = this;
    plugin_.init = PluginGlue::init;
    plugin_.destroy = PluginGlue::destroy;
    plugin_.activate = PluginGlue::activate;
    plugin_.deactivate = PluginGlue::deactivate;
    plugin_.start_processing = PluginGlue::startProcessing;
    plugin_.stop_processing = PluginGlue::stopProcessing;
    plugin_.reset = PluginGlue::reset;
    plugin_.process = PluginGlue::process;
    plugin_.get_extension = PluginGlue::getExtension;
    plugin_.on_main_thread = PluginGlue::onMainThread;

    const int n = table_.count();
    shared_ = std::make_unique<std::atomic<double>[]>(static_cast<size_t>(n));
    audio_.assign(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i)
    {
        shared_[static_cast<size_t>(i)].store(table_.def(i).def);
        audio_[static_cast<size_t>(i)] = table_.def(i).def;
    }
    for (auto& m : ccMap_)
        m.store(-1);
    revertMidiMappings();
    midiEnabled_.store(Settings::get().getDouble("midi_enabled", 1) > 0.5);
    // New instances start from the user's "Init" preset.
    StateDocument doc;
    const std::string def = PresetManager::get().defaultPresetPath();
    if (PresetManager::get().load(def, doc))
    {
        for (int i = 0; i < n; ++i)
        {
            shared_[static_cast<size_t>(i)].store(doc.values[static_cast<size_t>(i)]);
            audio_[static_cast<size_t>(i)] = doc.values[static_cast<size_t>(i)];
        }
        session_.restore("Init", def, doc.values);
    }
}

std::vector<double> AurumPlugin::currentValues() const
{
    std::vector<double> v(static_cast<size_t>(table_.count()));
    for (int i = 0; i < table_.count(); ++i)
        v[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    return v;
}

void AurumPlugin::applyValues(const std::vector<double>& values)
{
    // Before the values: the audio thread must not glide into them first.
    cutTail_.store(true);
    for (int i = 0; i < table_.count() && i < static_cast<int>(values.size()); ++i)
        setShared(i, std::clamp(values[static_cast<size_t>(i)], table_.minValue(i), table_.maxValue(i)));
    reloadFromShared_.store(true);
    if (hostParams_)
    {
        // Not recorded as automation by the host (CLAP_PARAM_RESCAN_VALUES).
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);
        if (!processing_)
            hostParams_->request_flush(host_);
    }
}

void AurumPlugin::importIr(const std::string& path)
{
    AudioData audio;
    std::string err;
    StateDocument doc;
    IrReport report;
    if (!readAudioFile(path, audio, &err) || !importImpulseResponse(audio, currentValues(), doc, &report, &err))
    {
        if (editor_)
            editor_->notify("IR import failed: " + err);
        return;
    }
    const std::string name = fromPath(toPath(path).stem());
    session_.applyDocument(doc, "IR: " + name, "");
    if (editor_)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Imported %s (T60 %.2f s)", name.c_str(), report.t60Mid);
        editor_->notify(buf);
    }
}

AurumPlugin::~AurumPlugin() = default;

bool AurumPlugin::init()
{
    hostParams_ = static_cast<const clap_host_params_t*>(host_->get_extension(host_, CLAP_EXT_PARAMS));
    hostState_ = static_cast<const clap_host_state_t*>(host_->get_extension(host_, CLAP_EXT_STATE));
    hostLatency_ = static_cast<const clap_host_latency_t*>(host_->get_extension(host_, CLAP_EXT_LATENCY));
    hostTail_ = static_cast<const clap_host_tail_t*>(host_->get_extension(host_, CLAP_EXT_TAIL));
    hostGui_ = static_cast<const clap_host_gui_t*>(host_->get_extension(host_, CLAP_EXT_GUI));
    hostTimer_ = static_cast<const clap_host_timer_support_t*>(host_->get_extension(host_, CLAP_EXT_TIMER_SUPPORT));
    hostFd_ = static_cast<const clap_host_posix_fd_support_t*>(host_->get_extension(host_, CLAP_EXT_POSIX_FD_SUPPORT));
    return true;
}

void AurumPlugin::destroy()
{
    guiDestroy();
    delete this;
}

bool AurumPlugin::activate(double sampleRate, uint32_t, uint32_t maxFrames)
{
    sampleRate_ = sampleRate;
    engine_.setOffline(offline_);
    engine_.prepare(sampleRate, static_cast<int>(maxFrames));
    scratchL_.assign(std::max<uint32_t>(maxFrames, 64), 0.0f);
    scratchR_.assign(std::max<uint32_t>(maxFrames, 64), 0.0f);
    reloadFromShared_.store(true);
    engineParamsDirty_ = true;
    active_ = true;
    return true;
}

void AurumPlugin::deactivate()
{
    engine_.release();
    active_ = false;
}

bool AurumPlugin::startProcessing()
{
    processing_ = true;
    return true;
}

void AurumPlugin::stopProcessing() { processing_ = false; }

void AurumPlugin::reset() { engine_.reset(); }

void AurumPlugin::onMainThread() {}

const void* AurumPlugin::getExtension(const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &PluginGlue::audioPorts;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG))
        return &PluginGlue::audioPortsConfig;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &PluginGlue::params;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &PluginGlue::state;
    if (!std::strcmp(id, CLAP_EXT_LATENCY))
        return &PluginGlue::latency;
    if (!std::strcmp(id, CLAP_EXT_TAIL))
        return &PluginGlue::tail;
    if (!std::strcmp(id, CLAP_EXT_RENDER))
        return &PluginGlue::render;
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &PluginGlue::gui;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &PluginGlue::notePorts;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &PluginGlue::timer;
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &PluginGlue::posixFd;
    return nullptr;
}

// ---------------------------------------------------------------- ports

uint32_t AurumPlugin::audioPortsCount(bool) const { return 1; }

bool AurumPlugin::audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info_t* info) const
{
    if (index != 0)
        return false;
    const PortsConfig& c = kConfigs[portsConfig_];
    const uint32_t ch = isInput ? c.in : c.out;
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = ch;
    info->port_type = ch == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

uint32_t AurumPlugin::portsConfigCount() const { return 3; }

bool AurumPlugin::portsConfigGet(uint32_t index, clap_audio_ports_config_t* config) const
{
    if (index >= 3)
        return false;
    const PortsConfig& c = kConfigs[index];
    config->id = index;
    std::snprintf(config->name, sizeof(config->name), "%s", c.name);
    config->input_port_count = 1;
    config->output_port_count = 1;
    config->has_main_input = true;
    config->main_input_channel_count = c.in;
    config->main_input_port_type = c.in == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    config->has_main_output = true;
    config->main_output_channel_count = c.out;
    config->main_output_port_type = c.out == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
    return true;
}

bool AurumPlugin::portsConfigSelect(clap_id configId)
{
    if (active_ || configId >= 3)
        return false;
    portsConfig_ = static_cast<int>(configId);
    return true;
}

// ---------------------------------------------------------------- params

bool AurumPlugin::paramsGetInfo(uint32_t index, clap_param_info_t* info) const
{
    if (index >= static_cast<uint32_t>(table_.count()))
        return false;
    const ParamDef& d = table_.def(static_cast<int>(index));
    std::memset(info, 0, sizeof(*info));
    info->id = d.id;
    info->flags = 0;
    if (d.automatable)
        info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    if (d.unit == Unit::Enum || d.unit == Unit::Bool)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (d.unit == Unit::Enum)
        info->flags |= CLAP_PARAM_IS_ENUM;
    if (d.isBypass)
        info->flags |= CLAP_PARAM_IS_BYPASS;
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof(info->name), "%s", d.name.c_str());
    std::snprintf(info->module, sizeof(info->module), "%s", d.module.c_str());
    info->min_value = table_.minValue(static_cast<int>(index));
    info->max_value = table_.maxValue(static_cast<int>(index));
    info->default_value = d.def;
    return true;
}

bool AurumPlugin::paramsGetValue(clap_id id, double* value) const
{
    const int idx = table_.indexOf(id);
    if (idx < 0)
        return false;
    *value = shared_[static_cast<size_t>(idx)].load(std::memory_order_relaxed);
    return true;
}

bool AurumPlugin::paramsValueToText(clap_id id, double value, char* display, uint32_t size) const
{
    const int idx = table_.indexOf(id);
    if (idx < 0 || size == 0)
        return false;
    std::snprintf(display, size, "%s", table_.toText(idx, value).c_str());
    return true;
}

bool AurumPlugin::paramsTextToValue(clap_id id, const char* display, double* value) const
{
    const int idx = table_.indexOf(id);
    if (idx < 0)
        return false;
    if (auto v = table_.fromText(idx, display))
    {
        *value = *v;
        return true;
    }
    return false;
}

void AurumPlugin::setShared(int index, double value)
{
    shared_[static_cast<size_t>(index)].store(value, std::memory_order_relaxed);
}

void AurumPlugin::handleEvent(const clap_event_header_t* ev)
{
    if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    if (ev->type == CLAP_EVENT_MIDI)
    {
        handleMidi(reinterpret_cast<const clap_event_midi_t*>(ev));
        return;
    }
    if (ev->type == CLAP_EVENT_PARAM_VALUE)
    {
        const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(ev);
        const int idx = table_.indexOf(pv->param_id);
        if (idx < 0)
            return;
        const double v = std::clamp(pv->value, table_.minValue(idx), table_.maxValue(idx));
        audio_[static_cast<size_t>(idx)] = v;
        setShared(idx, v);
        engineParamsDirty_ = true;
    }
}

void AurumPlugin::handleMidi(const clap_event_midi_t* ev)
{
    if (!midiEnabled_.load(std::memory_order_relaxed) || (ev->data[0] & 0xF0) != 0xB0)
        return;
    const int cc = ev->data[1] & 0x7F;
    const double norm = (ev->data[2] & 0x7F) / 127.0;
    const int target = learnTarget_.load(std::memory_order_relaxed);
    if (learnActiveAudio_.load(std::memory_order_relaxed) && target >= 0)
    {
        ccMap_[static_cast<size_t>(cc)].store(target, std::memory_order_relaxed);
        return;
    }
    const int idx = ccMap_[static_cast<size_t>(cc)].load(std::memory_order_relaxed);
    if (idx < 0 || idx >= table_.count())
        return;
    const ParamDef& d = table_.def(idx);
    double v = norm;
    if (d.unit == Unit::Enum || d.unit == Unit::Bool)
        v = std::round(norm * (d.steps - 1));
    audio_[static_cast<size_t>(idx)] = v;
    setShared(idx, v);
    engineParamsDirty_ = true;
    if (currentOut_)
    {
        // Tell the host about the plugin-internal change.
        clap_event_param_value_t pv{};
        pv.header.size = sizeof(pv);
        pv.header.time = ev->header.time;
        pv.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        pv.header.type = CLAP_EVENT_PARAM_VALUE;
        pv.param_id = d.id;
        pv.note_id = -1;
        pv.port_index = -1;
        pv.channel = -1;
        pv.key = -1;
        pv.value = v;
        currentOut_->try_push(currentOut_, &pv.header);
    }
}

bool AurumPlugin::notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info) const
{
    if (!isInput || index != 0)
        return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    std::snprintf(info->name, sizeof(info->name), "MIDI In");
    return true;
}

void AurumPlugin::setMidiLearn(bool on)
{
    learnActive_ = on;
    learnActiveAudio_.store(on);
    if (!on)
    {
        learnTarget_.store(-1);
        saveMidiMappings(); // mapping is saved automatically when leaving learn mode
    }
}

int AurumPlugin::ccForParam(int index) const
{
    for (int cc = 0; cc < 128; ++cc)
        if (ccMap_[static_cast<size_t>(cc)].load() == index)
            return cc;
    return -1;
}

std::vector<std::pair<int, int>> AurumPlugin::midiMappings() const
{
    std::vector<std::pair<int, int>> out;
    for (int cc = 0; cc < 128; ++cc)
    {
        const int p = ccMap_[static_cast<size_t>(cc)].load();
        if (p >= 0)
            out.emplace_back(cc, p);
    }
    return out;
}

void AurumPlugin::clearMidiMapping(int cc)
{
    if (cc >= 0 && cc < 128)
        ccMap_[static_cast<size_t>(cc)].store(-1);
}

void AurumPlugin::clearAllMidiMappings()
{
    for (auto& m : ccMap_)
        m.store(-1);
}

void AurumPlugin::revertMidiMappings()
{
    // Stored as "cc:paramkey|cc:paramkey".
    clearAllMidiMappings();
    for (const std::string& item : PresetManager::splitTags(Settings::get().getString("midi_map", "")))
    {
        const size_t colon = item.find(':');
        if (colon == std::string::npos)
            continue;
        const int cc = std::atoi(item.substr(0, colon).c_str());
        const int idx = table_.indexOfKey(item.substr(colon + 1));
        if (cc >= 0 && cc < 128 && idx >= 0)
            ccMap_[static_cast<size_t>(cc)].store(idx);
    }
}

void AurumPlugin::saveMidiMappings()
{
    std::vector<std::string> items;
    for (const auto& [cc, idx] : midiMappings())
        items.push_back(std::to_string(cc) + ":" + table_.def(idx).key);
    Settings::get().set("midi_map", PresetManager::joinTags(items));
    Settings::get().save();
}

void AurumPlugin::setMidiEnabled(bool on)
{
    midiEnabled_.store(on);
    Settings::get().set("midi_enabled", on ? 1.0 : 0.0);
    Settings::get().save();
}

void AurumPlugin::drainGuiEvents(const clap_output_events_t* out)
{
    GuiEvent e;
    while (guiEvents_.pop(e))
    {
        const ParamDef& d = table_.def(e.index);
        switch (e.type)
        {
        case GuiEvent::Begin:
        case GuiEvent::End:
        {
            clap_event_param_gesture_t g{};
            g.header.size = sizeof(g);
            g.header.time = 0;
            g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            g.header.type = e.type == GuiEvent::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
            g.header.flags = 0;
            g.param_id = d.id;
            if (out)
                out->try_push(out, &g.header);
            break;
        }
        case GuiEvent::Value:
        {
            audio_[static_cast<size_t>(e.index)] = e.value;
            engineParamsDirty_ = true;
            clap_event_param_value_t v{};
            v.header.size = sizeof(v);
            v.header.time = 0;
            v.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            v.header.type = CLAP_EVENT_PARAM_VALUE;
            v.header.flags = 0;
            v.param_id = d.id;
            v.cookie = nullptr;
            v.note_id = -1;
            v.port_index = -1;
            v.channel = -1;
            v.key = -1;
            v.value = e.value;
            if (out)
                out->try_push(out, &v.header);
            break;
        }
        }
    }
}

void AurumPlugin::syncFromShared()
{
    if (!reloadFromShared_.exchange(false))
        return;
    for (int i = 0; i < table_.count(); ++i)
        audio_[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    engineParamsDirty_ = true;
}

void AurumPlugin::paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out)
{
    currentOut_ = out;
    syncFromShared();
    if (in)
    {
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; ++i)
            handleEvent(in->get(in, i));
    }
    drainGuiEvents(out);
    currentOut_ = nullptr;
}

// Controller (main thread) ------------------------------------------------

double AurumPlugin::paramValue(int index) const { return shared_[static_cast<size_t>(index)].load(std::memory_order_relaxed); }

void AurumPlugin::beginEdit(int index)
{
    guiEvents_.push({GuiEvent::Begin, index, 0.0});
    if (hostParams_ && !processing_)
        hostParams_->request_flush(host_);
}

void AurumPlugin::performEdit(int index, double value)
{
    value = std::clamp(value, table_.minValue(index), table_.maxValue(index));
    setShared(index, value);
    guiEvents_.push({GuiEvent::Value, index, value});
    if (hostParams_ && !processing_)
        hostParams_->request_flush(host_);
}

void AurumPlugin::endEdit(int index)
{
    guiEvents_.push({GuiEvent::End, index, 0.0});
    if (hostParams_ && !processing_)
        hostParams_->request_flush(host_);
}

// ---------------------------------------------------------------- state

bool AurumPlugin::stateSave(const clap_ostream_t* stream)
{
    StateDocument doc;
    doc.values.resize(static_cast<size_t>(table_.count()));
    for (int i = 0; i < table_.count(); ++i)
        doc.values[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load();
    doc.meta["preset_name"] = session_.name();
    doc.meta["preset_path"] = session_.path();
    const std::string text = serializeState(doc);
    size_t written = 0;
    while (written < text.size())
    {
        const int64_t r = stream->write(stream, text.data() + written, text.size() - written);
        if (r <= 0)
            return false;
        written += static_cast<size_t>(r);
    }
    return true;
}

bool AurumPlugin::stateLoad(const clap_istream_t* stream)
{
    std::string text;
    char buf[4096];
    for (;;)
    {
        const int64_t r = stream->read(stream, buf, sizeof(buf));
        if (r < 0)
            return false;
        if (r == 0)
            break;
        text.append(buf, static_cast<size_t>(r));
    }
    StateDocument doc;
    if (!parseState(text, doc))
        return false;
    cutTail_.store(true);
    for (int i = 0; i < table_.count(); ++i)
        setShared(i, doc.values[static_cast<size_t>(i)]);
    reloadFromShared_.store(true);
    session_.restore(doc.meta["preset_name"], doc.meta["preset_path"], doc.values);
    if (editor_)
        editor_->resetHistory();
    if (hostParams_)
    {
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        if (!processing_)
            hostParams_->request_flush(host_);
    }
    return true;
}

uint32_t AurumPlugin::tailGet() const
{
    std::vector<double> v(static_cast<size_t>(table_.count()));
    for (int i = 0; i < table_.count(); ++i)
        v[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    const double secs = dsp::ReverbEngine::tailSeconds(buildEngineParams(v.data(), tempo_));
    if (secs < 0.0)
        return UINT32_MAX;
    return static_cast<uint32_t>(std::min(secs * sampleRate_, 4.0e9));
}

// ---------------------------------------------------------------- process

clap_process_status AurumPlugin::process(const clap_process_t* process)
{
    ScopedFtz ftz;
    currentOut_ = process->out_events;
    if (cutTail_.exchange(false))
        engine_.cut();
    syncFromShared();
    drainGuiEvents(process->out_events);

    if (process->transport && (process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) &&
        process->transport->tempo != tempo_)
    {
        tempo_ = process->transport->tempo;
        engineParamsDirty_ = true;
    }

    const uint32_t frames = process->frames_count;
    if (process->audio_inputs_count < 1 || process->audio_outputs_count < 1)
        return CLAP_PROCESS_CONTINUE;
    const clap_audio_buffer_t& inBuf = process->audio_inputs[0];
    const clap_audio_buffer_t& outBuf = process->audio_outputs[0];
    if (!inBuf.data32 || !outBuf.data32 || outBuf.channel_count == 0)
        return CLAP_PROCESS_CONTINUE;
    const float* inL = inBuf.data32[0];
    const float* inR = inBuf.channel_count > 1 ? inBuf.data32[1] : inBuf.data32[0];
    float* outL = outBuf.data32[0];
    float* outR = outBuf.channel_count > 1 ? outBuf.data32[1] : nullptr;
    if (frames > scratchL_.size())
        return CLAP_PROCESS_ERROR;

    const clap_input_events_t* in = process->in_events;
    const uint32_t numEvents = in ? in->size(in) : 0;
    uint32_t nextEvent = 0;
    uint32_t pos = 0;
    while (pos < frames)
    {
        // Apply all events at or before `pos`, then render up to the next one.
        uint32_t chunkEnd = frames;
        while (nextEvent < numEvents)
        {
            const clap_event_header_t* ev = in->get(in, nextEvent);
            if (ev->time > pos)
            {
                chunkEnd = std::min(frames, ev->time);
                break;
            }
            handleEvent(ev);
            ++nextEvent;
        }
        if (engineParamsDirty_)
        {
            engineParams_ = buildEngineParams(audio_.data(), tempo_);
            engineParamsDirty_ = false;
            const double secs = dsp::ReverbEngine::tailSeconds(engineParams_);
            const uint32_t tail = secs < 0 ? UINT32_MAX : static_cast<uint32_t>(std::min(secs * sampleRate_, 4.0e9));
            if (tail != lastTail_)
            {
                lastTail_ = tail;
                if (hostTail_)
                    hostTail_->changed(host_); // [audio-thread]
            }
        }
        const uint32_t n = chunkEnd - pos;
        // Work on scratch buffers so in-place host buffers are safe.
        std::copy(inL + pos, inL + pos + n, scratchL_.data() + pos);
        std::copy(inR + pos, inR + pos + n, scratchR_.data() + pos);
        engine_.process(scratchL_.data() + pos, scratchR_.data() + pos, scratchL_.data() + pos, scratchR_.data() + pos,
                        static_cast<int>(n), engineParams_);
        if (outR)
        {
            std::copy(scratchL_.data() + pos, scratchL_.data() + pos + n, outL + pos);
            std::copy(scratchR_.data() + pos, scratchR_.data() + pos + n, outR + pos);
        }
        else
        {
            for (uint32_t i = pos; i < pos + n; ++i)
                outL[i] = 0.5f * (scratchL_[i] + scratchR_[i]);
        }
        pos = chunkEnd;
    }
    currentOut_ = nullptr;
    // Remaining events (time >= frames) are applied for the next block.
    while (nextEvent < numEvents)
        handleEvent(in->get(in, nextEvent++));

    return CLAP_PROCESS_CONTINUE;
}

// ---------------------------------------------------------------- GUI

bool AurumPlugin::guiCreate()
{
    if (editor_)
        return true;
    editor_ = std::make_unique<gui::Editor>(*this);
    editor_->setPresetSession(&session_);
    editor_->onImportIr = [this](const std::string& path) { importIr(path); };
    session_.onLoaded = [this] {
        if (editor_)
            editor_->pushHistory();
    };
    editor_->requestResize = [this](int w, int h) {
        return hostGui_ && hostGui_->request_resize(host_, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    };
    if (hostTimer_)
        hostTimer_->register_timer(host_, 40, &timerId_); // 25 fps
    return true;
}

void AurumPlugin::guiDestroy()
{
    if (!editor_)
        return;
    if (learnActive_)
        setMidiLearn(false);
    if (hostTimer_ && timerId_ != CLAP_INVALID_ID)
        hostTimer_->unregister_timer(host_, timerId_);
    timerId_ = CLAP_INVALID_ID;
    if (hostFd_ && registeredFd_ >= 0)
        hostFd_->unregister_fd(host_, registeredFd_);
    registeredFd_ = -1;
    session_.onLoaded = nullptr;
    editor_.reset();
}

bool AurumPlugin::guiSetScale(double scale)
{
    if (!editor_)
        return false;
    // The host scale replaces the desktop guess; the user scaling still applies.
    editor_->setScale(scale * Settings::get().getDouble("gui_scaling", 1.0));
    return true;
}

bool AurumPlugin::guiGetSize(uint32_t* w, uint32_t* h)
{
    if (!editor_)
        return false;
    int pw, ph;
    editor_->physicalSize(pw, ph);
    *w = static_cast<uint32_t>(pw);
    *h = static_cast<uint32_t>(ph);
    return true;
}

bool AurumPlugin::guiAdjustSize(uint32_t* w, uint32_t* h)
{
    const double s = editor_ ? editor_->scale() : 1.0;
    *w = std::max(*w, static_cast<uint32_t>(gui::Editor::kMinW * s));
    *h = std::max(*h, static_cast<uint32_t>(gui::Editor::kMinH * s));
    return true;
}

bool AurumPlugin::guiSetSize(uint32_t w, uint32_t h)
{
    if (!editor_)
        return false;
    editor_->setPhysicalSize(static_cast<int>(w), static_cast<int>(h));
    return true;
}

bool AurumPlugin::guiSetParent(const clap_window_t* window)
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

bool AurumPlugin::guiShow()
{
    if (editor_)
        editor_->show();
    return editor_ != nullptr;
}

bool AurumPlugin::guiHide()
{
    if (editor_)
        editor_->hide();
    return editor_ != nullptr;
}

void AurumPlugin::onTimer(clap_id id)
{
    if (editor_ && id == timerId_)
        editor_->onTimer();
}

void AurumPlugin::onFd(int fd, clap_posix_fd_flags_t)
{
    if (editor_ && fd == registeredFd_)
        editor_->onFd();
}

// ---------------------------------------------------------------- factory

namespace {

uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? AurumPlugin::descriptor() : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, kDescriptor.id) != 0)
        return nullptr;
    auto* p = new AurumPlugin(host);
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

} // namespace aurum
