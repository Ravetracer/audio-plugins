#include "SubstrikePlugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <xmmintrin.h>

#include "gui/Editor.h"
#include "state/PresetProvider.h"
#include "state/Presets.h"
#include "state/Settings.h"
#include "state/StateIO.h"
#include "substrike.h"
#include "util/Path.h"

namespace substrike {

namespace {

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_DRUM,
                                 CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

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

SubstrikePlugin* self(const clap_plugin_t* p) { return static_cast<SubstrikePlugin*>(p->plugin_data); }

// The one window API each platform's editor is written against.
#if defined(_WIN32)
constexpr const char* kWindowApi = CLAP_WINDOW_API_WIN32;
#else
constexpr const char* kWindowApi = CLAP_WINDOW_API_X11;
#endif

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
    static uint32_t notePortsCount(const clap_plugin_t* p, bool in) { return self(p)->notePortsCount(in); }
    static bool notePortsGet(const clap_plugin_t* p, uint32_t i, bool in, clap_note_port_info_t* info)
    {
        return self(p)->notePortsGet(i, in, info);
    }

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

    static const clap_plugin_audio_ports_t audioPorts;
    static const clap_plugin_note_ports_t notePorts;
    static const clap_plugin_params_t params;
    static const clap_plugin_state_t state;
    static bool presetLoad(const clap_plugin_t* p, uint32_t kind, const char* location, const char* key)
    {
        return self(p)->presetLoad(kind, location, key);
    }
    static const clap_plugin_preset_load_t presetLoadExt;
    static const clap_plugin_gui_t gui;
    static const clap_plugin_timer_support_t timer;
    static const clap_plugin_posix_fd_support_t posixFd;
};

const clap_plugin_audio_ports_t PluginGlue::audioPorts = {portsCount, portsGet};
const clap_plugin_note_ports_t PluginGlue::notePorts = {notePortsCount, notePortsGet};
const clap_plugin_params_t PluginGlue::params = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText,
                                                 paramsFlush};
const clap_plugin_state_t PluginGlue::state = {stateSave, stateLoad};
const clap_plugin_preset_load_t PluginGlue::presetLoadExt = {presetLoad};
const clap_plugin_gui_t PluginGlue::gui = {guiApiSupported, guiPreferredApi, guiCreate,      guiDestroy,
                                           guiSetScale,     guiGetSize,      guiCanResize,   guiResizeHints,
                                           guiAdjustSize,   guiSetSize,      guiSetParent,   guiSetTransient,
                                           guiSuggestTitle, guiShow,         guiHide};
const clap_plugin_timer_support_t PluginGlue::timer = {timerTick};
const clap_plugin_posix_fd_support_t PluginGlue::posixFd = {fdReady};

const clap_plugin_descriptor_t* SubstrikePlugin::descriptor() { return &kDescriptor; }

SubstrikePlugin::SubstrikePlugin(const clap_host_t* host) : host_(host), table_(ParamTable::get())
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
}

SubstrikePlugin::~SubstrikePlugin() = default;

bool SubstrikePlugin::init()
{
    hostParams_ = static_cast<const clap_host_params_t*>(host_->get_extension(host_, CLAP_EXT_PARAMS));
    hostState_ = static_cast<const clap_host_state_t*>(host_->get_extension(host_, CLAP_EXT_STATE));
    hostPresetLoad_ = static_cast<const clap_host_preset_load_t*>(host_->get_extension(host_, CLAP_EXT_PRESET_LOAD));
    if (!hostPresetLoad_)
        hostPresetLoad_ =
            static_cast<const clap_host_preset_load_t*>(host_->get_extension(host_, CLAP_EXT_PRESET_LOAD_COMPAT));
    hostGui_ = static_cast<const clap_host_gui_t*>(host_->get_extension(host_, CLAP_EXT_GUI));
    hostTimer_ = static_cast<const clap_host_timer_support_t*>(host_->get_extension(host_, CLAP_EXT_TIMER_SUPPORT));
    hostFd_ = static_cast<const clap_host_posix_fd_support_t*>(host_->get_extension(host_, CLAP_EXT_POSIX_FD_SUPPORT));
    return true;
}

void SubstrikePlugin::destroy()
{
    guiDestroy();
    delete this;
}

bool SubstrikePlugin::activate(double sampleRate, uint32_t, uint32_t)
{
    sampleRate_ = sampleRate;
    // The first values go in here, on the main thread, so the player's
    // buffers are sized before the audio thread uses them.
    player_.setValues(audio_.data());
    player_.prepare(sampleRate);
    reloadFromShared_.store(true);
    engineParamsDirty_ = true;
    active_ = true;
    return true;
}

void SubstrikePlugin::deactivate() { active_ = false; }

bool SubstrikePlugin::startProcessing()
{
    processing_ = true;
    return true;
}

void SubstrikePlugin::stopProcessing() { processing_ = false; }

void SubstrikePlugin::reset() { player_.reset(); }

void SubstrikePlugin::onMainThread()
{
    pushCurves();
    if (rescanInfo_.exchange(false) && hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
}

const void* SubstrikePlugin::getExtension(const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &PluginGlue::audioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &PluginGlue::notePorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &PluginGlue::params;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &PluginGlue::state;
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT))
        return &PluginGlue::presetLoadExt;
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &PluginGlue::gui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &PluginGlue::timer;
#if !defined(_WIN32)
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &PluginGlue::posixFd;
#endif
    return nullptr;
}

// ---------------------------------------------------------------- ports

// The main output, then one aux output per lane carrying that lane alone.
uint32_t SubstrikePlugin::audioPortsCount(bool isInput) const
{
    return isInput ? 0 : static_cast<uint32_t>(dsp::Engine::kNumBuses);
}

bool SubstrikePlugin::audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info_t* info) const
{
    if (isInput || index >= static_cast<uint32_t>(dsp::Engine::kNumBuses))
        return false;
    info->id = index;
    if (index == 0)
        std::snprintf(info->name, sizeof(info->name), "Main");
    else
        std::snprintf(info->name, sizeof(info->name), "Lane %u", index);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

bool SubstrikePlugin::notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info) const
{
    if (!isInput || index != 0)
        return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "Notes");
    return true;
}

// ---------------------------------------------------------------- params

bool SubstrikePlugin::paramsGetInfo(uint32_t index, clap_param_info_t* info) const
{
    if (index >= static_cast<uint32_t>(table_.count()))
        return false;
    const int i = static_cast<int>(index);
    const ParamDef& d = current(i);
    std::memset(info, 0, sizeof(*info));
    info->id = d.id;
    info->flags = d.automatable ? CLAP_PARAM_IS_AUTOMATABLE : 0;
    if (d.kind != Kind::Continuous)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (d.kind == Kind::Enum)
        info->flags |= CLAP_PARAM_IS_ENUM;
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof(info->name), "%s", d.name.c_str());
    std::snprintf(info->module, sizeof(info->module), "%s", d.module.c_str());
    info->min_value = table_.minValue(i);
    info->max_value = table_.maxValue(i);
    // The fixed default: a letter's own depends on the type, and a host may
    // not see that change.
    info->default_value = table_.def(i).def;
    return true;
}

bool SubstrikePlugin::paramsGetValue(clap_id id, double* value) const
{
    const int idx = table_.indexOf(id);
    if (idx < 0)
        return false;
    *value = shared_[static_cast<size_t>(idx)].load(std::memory_order_relaxed);
    return true;
}

bool SubstrikePlugin::paramsValueToText(clap_id id, double value, char* display, uint32_t size) const
{
    const int idx = table_.indexOf(id);
    if (idx < 0 || size == 0)
        return false;
    std::snprintf(display, size, "%s", ParamTable::toText(current(idx), value).c_str());
    return true;
}

bool SubstrikePlugin::paramsTextToValue(clap_id id, const char* display, double* value) const
{
    const int idx = table_.indexOf(id);
    if (idx < 0)
        return false;
    if (auto v = ParamTable::fromText(current(idx), display))
    {
        *value = *v;
        return true;
    }
    return false;
}

void SubstrikePlugin::setShared(int index, double value)
{
    shared_[static_cast<size_t>(index)].store(value, std::memory_order_relaxed);
}

const ParamDef& SubstrikePlugin::current(int index) const
{
    const int t = table_.def(index).typeParam;
    if (t < 0)
        return table_.def(index);
    return table_.effective(index, shared_[static_cast<size_t>(t)].load(std::memory_order_relaxed));
}

void SubstrikePlugin::typeChanged(int typeIndex, const clap_output_events_t* out)
{
    for (int i = 0; i < table_.count(); ++i)
    {
        if (table_.def(i).typeParam != typeIndex)
            continue;
        const double v = table_.effective(i, audio_[static_cast<size_t>(typeIndex)]).def;
        audio_[static_cast<size_t>(i)] = v;
        setShared(i, v);
        if (out)
        {
            clap_event_param_value_t ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.param_id = table_.def(i).id;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = v;
            out->try_push(out, &ev.header);
        }
    }
    rescanInfo_.store(true);
    host_->request_callback(host_);
}

void SubstrikePlugin::syncFromShared()
{
    if (!reloadFromShared_.exchange(false))
        return;
    for (int i = 0; i < table_.count(); ++i)
        audio_[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    engineParamsDirty_ = true;
}

void SubstrikePlugin::handleEvent(const clap_event_header_t* ev, const clap_output_events_t* out)
{
    if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    switch (ev->type)
    {
    case CLAP_EVENT_PARAM_VALUE:
    {
        const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(ev);
        const int idx = table_.indexOf(pv->param_id);
        if (idx >= 0)
            applyValue(idx, pv->value, out);
        return;
    }
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_CHOKE:
    case CLAP_EVENT_MIDI:
    {
        // A hit is played with the parameters as they stand at its sample.
        if (engineParamsDirty_)
        {
            player_.setValues(audio_.data());
            engineParamsDirty_ = false;
        }
        if (ev->type == CLAP_EVENT_NOTE_ON)
        {
            const auto* ne = reinterpret_cast<const clap_event_note_t*>(ev);
            player_.noteOn(ne->key >= 0 ? ne->key : player_.rootNote(), ne->velocity);
        }
        else if (ev->type == CLAP_EVENT_NOTE_CHOKE)
            player_.choke();
        else
        {
            const auto* me = reinterpret_cast<const clap_event_midi_t*>(ev);
            const uint8_t status = me->data[0] & 0xF0;
            // A kick is a one-shot: note-off does nothing, so only note-on
            // with a velocity counts.
            if (status == 0x90 && me->data[2] > 0)
                player_.noteOn(me->data[1] & 0x7F, (me->data[2] & 0x7F) / 127.0);
        }
        return;
    }
    default: return;
    }
}

void SubstrikePlugin::applyValue(int idx, double value, const clap_output_events_t* out)
{
    const double v = std::clamp(value, table_.minValue(idx), table_.maxValue(idx));
    const double before = audio_[static_cast<size_t>(idx)];
    audio_[static_cast<size_t>(idx)] = v;
    setShared(idx, v);
    engineParamsDirty_ = true;
    if (table_.isSlotType(idx) && std::lround(v) != std::lround(before))
        typeChanged(idx, out);
}

void SubstrikePlugin::drainGuiEvents(const clap_output_events_t* out)
{
    GuiEvent e;
    while (guiEvents_.pop(e))
    {
        const uint32_t id = table_.def(e.index).id;
        if (e.type == GuiEvent::Value)
        {
            // The host hears the value first, then any letters a type change
            // resets behind it.
            if (out)
            {
                clap_event_param_value_t v{};
                v.header.size = sizeof(v);
                v.header.time = 0;
                v.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                v.header.type = CLAP_EVENT_PARAM_VALUE;
                v.param_id = id;
                v.note_id = -1;
                v.port_index = -1;
                v.channel = -1;
                v.key = -1;
                v.value = e.value;
                out->try_push(out, &v.header);
            }
            applyValue(e.index, e.value, out);
            continue;
        }
        if (out)
        {
            clap_event_param_gesture_t g{};
            g.header.size = sizeof(g);
            g.header.time = 0;
            g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            g.header.type = e.type == GuiEvent::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
            g.param_id = id;
            out->try_push(out, &g.header);
        }
    }
}

void SubstrikePlugin::drainCurves()
{
    CurveEvent e;
    while (curveEvents_.pop(e))
        player_.setCurve(e.index, e.curve);
}

void SubstrikePlugin::pushCurves()
{
    bool any = false;
    for (int i = 0; i < dsp::kNumCurves; ++i)
    {
        if (!curvePending_[static_cast<size_t>(i)])
            continue;
        if (!curveEvents_.push({i, curves_[static_cast<size_t>(i)]}))
        {
            // Full: try again on the next main-thread callback.
            host_->request_callback(host_);
            break;
        }
        curvePending_[static_cast<size_t>(i)] = false;
        any = true;
    }
    if (any)
        requestFlush();
}

void SubstrikePlugin::requestFlush()
{
    // A plugin that has gone to sleep is not processed again until something
    // arrives; a flush (or a process call) drains what the GUI queued.
    if (hostParams_)
        hostParams_->request_flush(host_);
}

void SubstrikePlugin::paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out)
{
    syncFromShared();
    drainCurves();
    if (in)
    {
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; ++i)
        {
            const clap_event_header_t* ev = in->get(in, i);
            // Only parameter changes: a flush carries no time to play a note at.
            if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID && ev->type == CLAP_EVENT_PARAM_VALUE)
                handleEvent(ev, out);
        }
    }
    drainGuiEvents(out);
}

// ---------------------------------------------------------------- Controller

double SubstrikePlugin::paramValue(int index) const
{
    return shared_[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

void SubstrikePlugin::beginEdit(int index)
{
    guiEvents_.push({GuiEvent::Begin, index, 0.0});
    requestFlush();
}

void SubstrikePlugin::performEdit(int index, double value)
{
    value = std::clamp(value, table_.minValue(index), table_.maxValue(index));
    setShared(index, value);
    guiEvents_.push({GuiEvent::Value, index, value});
    requestFlush();
}

void SubstrikePlugin::endEdit(int index)
{
    guiEvents_.push({GuiEvent::End, index, 0.0});
    requestFlush();
}

void SubstrikePlugin::audition()
{
    auditionRequested_.store(true);
    // The plugin may be asleep; this wakes it.
    host_->request_process(host_);
}

void SubstrikePlugin::setCurve(int index, const dsp::Curve& c)
{
    if (curves_[static_cast<size_t>(index)] == c)
        return;
    curves_[static_cast<size_t>(index)] = c;
    curvePending_[static_cast<size_t>(index)] = true;
    pushCurves();
    // A curve is not a parameter, so the host does not know the song changed.
    if (hostState_)
        hostState_->mark_dirty(host_);
}

// ---------------------------------------------------------------- state

bool SubstrikePlugin::stateSave(const clap_ostream_t* stream)
{
    StateDocument doc;
    doc.values.resize(static_cast<size_t>(table_.count()));
    for (int i = 0; i < table_.count(); ++i)
        doc.values[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load();
    doc.curves = curves_;
    doc.meta["preset"] = presetName_;
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

bool SubstrikePlugin::stateLoad(const clap_istream_t* stream)
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
    applyDocument(doc);
    return true;
}

void SubstrikePlugin::applyDocument(const StateDocument& doc)
{
    freshStartRequested_.store(true);
    for (int i = 0; i < table_.count(); ++i)
        setShared(i, doc.values[static_cast<size_t>(i)]);
    reloadFromShared_.store(true);
    curves_ = doc.curves;
    curvePending_.fill(true);
    pushCurves();
    const auto preset = doc.meta.find("preset");
    presetName_ = preset != doc.meta.end() ? preset->second : std::string("Init");
    if (hostParams_)
    {
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO);
        if (!processing_)
            hostParams_->request_flush(host_);
    }
}

void SubstrikePlugin::loadDocument(const StateDocument& doc, const std::string& name)
{
    StateDocument d = doc;
    d.meta["preset"] = name;
    applyDocument(d);
    // Not through a parameter change the host saw, so the song is dirty here.
    if (hostState_)
        hostState_->mark_dirty(host_);
}

StateDocument SubstrikePlugin::currentDocument() const
{
    StateDocument doc;
    doc.values.resize(static_cast<size_t>(table_.count()));
    for (int i = 0; i < table_.count(); ++i)
        doc.values[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load();
    doc.curves = curves_;
    doc.meta["preset"] = presetName_;
    return doc;
}

bool SubstrikePlugin::presetLoad(uint32_t kind, const char* location, const char* loadKey)
{
    StateDocument doc;
    std::string error;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
    {
        const char* text = loadKey ? factoryPresetText(loadKey) : nullptr;
        if (!text || !parseState(text, doc))
            error = std::string("no factory preset '") + (loadKey ? loadKey : "") + "'";
    }
    else if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE)
    {
        if (!location || !readPresetFile(location, doc))
            error = std::string("cannot read the preset ") + (location ? location : "");
    }
    else
        error = "unsupported preset location";
    if (!error.empty())
    {
        if (hostPresetLoad_)
            hostPresetLoad_->on_error(host_, kind, location, loadKey, 0, error.c_str());
        return false;
    }
    PresetInfo info = presetInfo(doc);
    if (info.name.empty() && location)
        info.name = fromPath(toPath(location).stem());
    doc.meta["preset"] = info.name;
    applyDocument(doc);
    if (hostPresetLoad_)
        hostPresetLoad_->loaded(host_, kind, location, loadKey);
    return true;
}

// ---------------------------------------------------------------- process

clap_process_status SubstrikePlugin::process(const clap_process_t* process)
{
    ScopedFtz ftz;
    // A new state or preset: the old sound fades out and the engine starts
    // clean, rather than gliding from the old settings into the new.
    if (freshStartRequested_.exchange(false))
        player_.freshStart();
    syncFromShared();
    drainCurves();
    drainGuiEvents(process->out_events);
    // The synced delay times follow the host's tempo; without a transport
    // they keep the last one (120 to begin with).
    if (process->steady_time >= 0)
        player_.setClock(static_cast<uint64_t>(process->steady_time));
    if (const clap_event_transport_t* t = process->transport)
    {
        const bool beats = (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
        player_.setTransport((t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? t->tempo : 0.0, beats,
                             beats ? static_cast<double>(t->song_pos_beats) / CLAP_BEATTIME_FACTOR : 0.0);
    }

    const uint32_t frames = process->frames_count;
    const clap_input_events_t* in = process->in_events;
    const uint32_t numEvents = in ? in->size(in) : 0;

    // The ports the host connected; a missing one renders into scratch and is
    // dropped, a mono one gets the right channel folded in.
    const int ports = static_cast<int>(std::min<uint32_t>(process->audio_outputs_count, dsp::Engine::kNumBuses));
    auto portChannels = [&](int b) -> uint32_t {
        if (b >= ports)
            return 0;
        const clap_audio_buffer_t& o = process->audio_outputs[b];
        return o.data32 ? o.channel_count : 0;
    };

    if (auditionRequested_.exchange(false))
    {
        if (engineParamsDirty_)
        {
            player_.setValues(audio_.data());
            engineParamsDirty_ = false;
        }
        player_.noteOn(player_.rootNote(), 1.0);
    }

    // Nothing sounding and nothing arriving: write silence and let the host
    // put the plugin to sleep until the next event.
    if (numEvents == 0 && player_.idle())
    {
        for (int b = 0; b < ports; ++b)
        {
            clap_audio_buffer_t& o = process->audio_outputs[b];
            for (uint32_t c = 0; c < portChannels(b); ++c)
                std::fill(o.data32[c], o.data32[c] + frames, 0.0f);
            o.constant_mask = (1ull << o.channel_count) - 1;
        }
        if (process->steady_time < 0)
            player_.skip(static_cast<int>(frames));
        return CLAP_PROCESS_SLEEP;
    }
    for (int b = 0; b < ports; ++b)
        process->audio_outputs[b].constant_mask = 0;

    auto renderSpan = [&](uint32_t from, uint32_t to) {
        for (uint32_t p = from; p < to; p += kScratch)
        {
            const uint32_t n = std::min<uint32_t>(kScratch, to - p);
            dsp::Bus buses[dsp::Engine::kNumBuses];
            for (int b = 0; b < dsp::Engine::kNumBuses; ++b)
            {
                const uint32_t ch = portChannels(b);
                float* const* data = ch ? process->audio_outputs[b].data32 : nullptr;
                buses[b].l = ch >= 1 ? data[0] + p : scratch_[b][0].data();
                buses[b].r = ch >= 2 ? data[1] + p : scratch_[b][1].data();
            }
            player_.process(buses, static_cast<int>(n));
            for (int b = 0; b < ports; ++b)
                if (portChannels(b) == 1)
                    for (uint32_t i = 0; i < n; ++i)
                        buses[b].l[i] = 0.5f * (buses[b].l[i] + buses[b].r[i]);
        }
    };

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
            handleEvent(ev, process->out_events);
            ++nextEvent;
        }
        if (engineParamsDirty_)
        {
            player_.setValues(audio_.data());
            engineParamsDirty_ = false;
        }
        renderSpan(pos, chunkEnd);
        pos = chunkEnd;
    }
    // Events stamped past the block's end are applied for the next one.
    while (nextEvent < numEvents)
        handleEvent(in->get(in, nextEvent++), process->out_events);

    return player_.idle() ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
}

// ---------------------------------------------------------------- GUI

bool SubstrikePlugin::guiCreate()
{
    if (editor_)
        return true;
    editor_ = std::make_unique<gui::Editor>(*this);
    editor_->requestResize = [this](int w, int h) {
        return hostGui_ && hostGui_->request_resize(host_, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    };
    if (hostTimer_)
        hostTimer_->register_timer(host_, 33, &timerId_); // 30 fps
    return true;
}

void SubstrikePlugin::guiDestroy()
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
}

bool SubstrikePlugin::guiSetScale(double scale)
{
    if (!editor_)
        return false;
    // The host scale replaces the desktop guess; the user scaling still applies.
    editor_->setScale(scale * Settings::get().getDouble("gui_scaling", 1.0));
    return true;
}

bool SubstrikePlugin::guiGetSize(uint32_t* w, uint32_t* h)
{
    if (!editor_)
        return false;
    int pw, ph;
    editor_->physicalSize(pw, ph);
    *w = static_cast<uint32_t>(pw);
    *h = static_cast<uint32_t>(ph);
    return true;
}

bool SubstrikePlugin::guiAdjustSize(uint32_t* w, uint32_t* h)
{
    const double s = editor_ ? editor_->scale() : 1.0;
    *w = std::max(*w, static_cast<uint32_t>(gui::Editor::kMinW * s));
    *h = std::max(*h, static_cast<uint32_t>(gui::Editor::kMinH * s));
    return true;
}

bool SubstrikePlugin::guiSetSize(uint32_t w, uint32_t h)
{
    if (!editor_)
        return false;
    editor_->setPhysicalSize(static_cast<int>(w), static_cast<int>(h));
    return true;
}

bool SubstrikePlugin::guiSetParent(const clap_window_t* window)
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

bool SubstrikePlugin::guiShow()
{
    if (editor_)
        editor_->show();
    return editor_ != nullptr;
}

bool SubstrikePlugin::guiHide()
{
    if (editor_)
        editor_->hide();
    return editor_ != nullptr;
}

void SubstrikePlugin::onTimer(clap_id id)
{
    if (editor_ && id == timerId_)
        editor_->onTimer();
}

void SubstrikePlugin::onFd(int fd, clap_posix_fd_flags_t)
{
    if (editor_ && fd == registeredFd_)
        editor_->onFd();
}

// ---------------------------------------------------------------- factory

namespace {

uint32_t factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? SubstrikePlugin::descriptor() : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, kDescriptor.id) != 0)
        return nullptr;
    auto* p = new SubstrikePlugin(host);
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
    if (!std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) ||
        !std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT))
        return presetDiscoveryFactory();
    return nullptr;
}

} // namespace substrike
