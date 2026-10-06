#include "SubstrikePlugin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <xmmintrin.h>

#include "state/StateIO.h"
#include "substrike.h"

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

    static const clap_plugin_audio_ports_t audioPorts;
    static const clap_plugin_note_ports_t notePorts;
    static const clap_plugin_params_t params;
    static const clap_plugin_state_t state;
};

const clap_plugin_audio_ports_t PluginGlue::audioPorts = {portsCount, portsGet};
const clap_plugin_note_ports_t PluginGlue::notePorts = {notePortsCount, notePortsGet};
const clap_plugin_params_t PluginGlue::params = {paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText,
                                                 paramsFlush};
const clap_plugin_state_t PluginGlue::state = {stateSave, stateLoad};

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
    return true;
}

void SubstrikePlugin::destroy() { delete this; }

bool SubstrikePlugin::activate(double sampleRate, uint32_t, uint32_t)
{
    sampleRate_ = sampleRate;
    engine_.prepare(sampleRate);
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

void SubstrikePlugin::reset() { engine_.reset(); }

void SubstrikePlugin::onMainThread() {}

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
    return nullptr;
}

// ---------------------------------------------------------------- ports

uint32_t SubstrikePlugin::audioPortsCount(bool isInput) const { return isInput ? 0 : 1; }

bool SubstrikePlugin::audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info_t* info) const
{
    if (isInput || index != 0)
        return false;
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
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
    const ParamDef& d = table_.def(i);
    std::memset(info, 0, sizeof(*info));
    info->id = d.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (d.kind != Kind::Continuous)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (d.kind == Kind::Enum)
        info->flags |= CLAP_PARAM_IS_ENUM;
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof(info->name), "%s", d.name.c_str());
    std::snprintf(info->module, sizeof(info->module), "%s", d.module.c_str());
    info->min_value = table_.minValue(i);
    info->max_value = table_.maxValue(i);
    info->default_value = d.def;
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
    std::snprintf(display, size, "%s", table_.toText(idx, value).c_str());
    return true;
}

bool SubstrikePlugin::paramsTextToValue(clap_id id, const char* display, double* value) const
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

void SubstrikePlugin::setShared(int index, double value)
{
    shared_[static_cast<size_t>(index)].store(value, std::memory_order_relaxed);
}

void SubstrikePlugin::syncFromShared()
{
    if (!reloadFromShared_.exchange(false))
        return;
    for (int i = 0; i < table_.count(); ++i)
        audio_[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    engineParamsDirty_ = true;
}

void SubstrikePlugin::handleEvent(const clap_event_header_t* ev)
{
    if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    switch (ev->type)
    {
    case CLAP_EVENT_PARAM_VALUE:
    {
        const auto* pv = reinterpret_cast<const clap_event_param_value_t*>(ev);
        const int idx = table_.indexOf(pv->param_id);
        if (idx < 0)
            return;
        const double v = std::clamp(pv->value, table_.minValue(idx), table_.maxValue(idx));
        audio_[static_cast<size_t>(idx)] = v;
        setShared(idx, v);
        engineParamsDirty_ = true;
        return;
    }
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_CHOKE:
    case CLAP_EVENT_MIDI:
    {
        // A hit is played with the parameters as they stand at its sample.
        if (engineParamsDirty_)
        {
            engineParams_ = buildEngineParams(audio_.data());
            engineParamsDirty_ = false;
        }
        if (ev->type == CLAP_EVENT_NOTE_ON)
        {
            const auto* ne = reinterpret_cast<const clap_event_note_t*>(ev);
            engine_.noteOn(ne->key >= 0 ? ne->key : engineParams_.rootNote, ne->velocity, engineParams_);
        }
        else if (ev->type == CLAP_EVENT_NOTE_CHOKE)
            engine_.choke();
        else
        {
            const auto* me = reinterpret_cast<const clap_event_midi_t*>(ev);
            const uint8_t status = me->data[0] & 0xF0;
            // A kick is a one-shot: note-off does nothing, so only note-on
            // with a velocity counts.
            if (status == 0x90 && me->data[2] > 0)
                engine_.noteOn(me->data[1] & 0x7F, (me->data[2] & 0x7F) / 127.0, engineParams_);
        }
        return;
    }
    default: return;
    }
}

void SubstrikePlugin::paramsFlush(const clap_input_events_t* in, const clap_output_events_t*)
{
    syncFromShared();
    if (!in)
        return;
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i)
    {
        const clap_event_header_t* ev = in->get(in, i);
        // Only parameter changes: a flush carries no time to play a note at.
        if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID && ev->type == CLAP_EVENT_PARAM_VALUE)
            handleEvent(ev);
    }
}

// ---------------------------------------------------------------- state

bool SubstrikePlugin::stateSave(const clap_ostream_t* stream)
{
    StateDocument doc;
    doc.values.resize(static_cast<size_t>(table_.count()));
    for (int i = 0; i < table_.count(); ++i)
        doc.values[static_cast<size_t>(i)] = shared_[static_cast<size_t>(i)].load();
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
    chokeRequested_.store(true);
    for (int i = 0; i < table_.count(); ++i)
        setShared(i, doc.values[static_cast<size_t>(i)]);
    reloadFromShared_.store(true);
    if (hostParams_)
    {
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        if (!processing_)
            hostParams_->request_flush(host_);
    }
    return true;
}

// ---------------------------------------------------------------- process

clap_process_status SubstrikePlugin::process(const clap_process_t* process)
{
    ScopedFtz ftz;
    if (chokeRequested_.exchange(false))
        engine_.choke();
    syncFromShared();

    const uint32_t frames = process->frames_count;
    if (process->audio_outputs_count < 1)
        return CLAP_PROCESS_CONTINUE;
    clap_audio_buffer_t& outBuf = process->audio_outputs[0];
    if (!outBuf.data32 || outBuf.channel_count == 0)
        return CLAP_PROCESS_CONTINUE;
    float* outL = outBuf.data32[0];
    float* outR = outBuf.channel_count > 1 ? outBuf.data32[1] : nullptr;

    const clap_input_events_t* in = process->in_events;
    const uint32_t numEvents = in ? in->size(in) : 0;

    // Nothing sounding and nothing arriving: write silence and let the host
    // put the plugin to sleep until the next event.
    if (numEvents == 0 && engine_.idle())
    {
        std::fill(outL, outL + frames, 0.0f);
        if (outR)
            std::fill(outR, outR + frames, 0.0f);
        outBuf.constant_mask = outR ? 3 : 1;
        return CLAP_PROCESS_SLEEP;
    }
    outBuf.constant_mask = 0;

    // Mono output: the right channel goes to a scratch buffer and is folded in.
    float scratch[256];
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
            engineParams_ = buildEngineParams(audio_.data());
            engineParamsDirty_ = false;
        }
        if (outR)
            engine_.process(outL + pos, outR + pos, static_cast<int>(chunkEnd - pos), engineParams_);
        else
        {
            for (uint32_t p = pos; p < chunkEnd; p += 256)
            {
                const uint32_t n = std::min<uint32_t>(256, chunkEnd - p);
                engine_.process(outL + p, scratch, static_cast<int>(n), engineParams_);
                for (uint32_t i = 0; i < n; ++i)
                    outL[p + i] = 0.5f * (outL[p + i] + scratch[i]);
            }
        }
        pos = chunkEnd;
    }
    // Events stamped past the block's end are applied for the next one.
    while (nextEvent < numEvents)
        handleEvent(in->get(in, nextEvent++));

    return engine_.idle() ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
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
    return nullptr;
}

} // namespace substrike
