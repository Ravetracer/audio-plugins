#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <clap/clap.h>

#include "Params.h"
#include "dsp/Engine.h"

namespace substrike {

class SubstrikePlugin
{
public:
    static const clap_plugin_descriptor_t* descriptor();

    explicit SubstrikePlugin(const clap_host_t* host);
    ~SubstrikePlugin();

    const clap_plugin_t* clapPlugin() const { return &plugin_; }

private:
    // clap_plugin
    bool init();
    void destroy();
    bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames);
    void deactivate();
    bool startProcessing();
    void stopProcessing();
    void reset();
    clap_process_status process(const clap_process_t* process);
    const void* getExtension(const char* id);
    void onMainThread();

    // extensions
    uint32_t audioPortsCount(bool isInput) const;
    bool audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info_t* info) const;
    uint32_t notePortsCount(bool isInput) const { return isInput ? 1 : 0; }
    bool notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info) const;
    bool paramsGetInfo(uint32_t index, clap_param_info_t* info) const;
    bool paramsGetValue(clap_id id, double* value) const;
    bool paramsValueToText(clap_id id, double value, char* display, uint32_t size) const;
    bool paramsTextToValue(clap_id id, const char* display, double* value) const;
    void paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out);
    bool stateSave(const clap_ostream_t* stream);
    bool stateLoad(const clap_istream_t* stream);

    void handleEvent(const clap_event_header_t* ev);
    void syncFromShared();
    void setShared(int index, double value);

    clap_plugin_t plugin_;
    const clap_host_t* host_;
    const clap_host_params_t* hostParams_ = nullptr;

    const ParamTable& table_;
    // Values visible to all threads (stored units, see ParamTable).
    std::unique_ptr<std::atomic<double>[]> shared_;
    // The audio thread's copy, used for DSP.
    std::vector<double> audio_;
    std::atomic<bool> reloadFromShared_{true};
    // A new state arrived: the audio thread stops the sounding hit.
    std::atomic<bool> chokeRequested_{false};

    dsp::Engine engine_;
    // Render target for ports the host did not connect, and for the right
    // channel of a mono one.
    static constexpr uint32_t kScratch = 256;
    std::array<std::array<std::array<float, kScratch>, 2>, dsp::Engine::kNumBuses> scratch_{};
    dsp::EngineParams engineParams_{};
    bool engineParamsDirty_ = true;
    double sampleRate_ = 48000.0;
    bool active_ = false;
    std::atomic<bool> processing_{false};

    friend struct PluginGlue;
};

// Factory/entry implementation (exported by the format-specific entry file).
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace substrike
