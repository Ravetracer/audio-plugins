#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <clap/clap.h>

#include "Params.h"
#include "PresetSession.h"
#include "dsp/ReverbEngine.h"
#include "util/SpscQueue.h"

namespace aurum {

namespace gui {
class Editor;
}

// Interface the editor uses to talk to the plugin (main thread only).
class Controller
{
public:
    virtual ~Controller() = default;
    virtual double paramValue(int index) const = 0;
    virtual void beginEdit(int index) = 0;
    virtual void performEdit(int index, double value) = 0;
    virtual void endEdit(int index) = 0;
    virtual double sampleRate() const = 0;
    virtual double tempo() const = 0;

    // MIDI learn (defaults for controllers without MIDI support).
    virtual bool midiLearnActive() const { return false; }
    virtual void setMidiLearn(bool) {}
    virtual void setLearnTarget(int) {}
    virtual int learnTarget() const { return -1; }
    virtual int ccForParam(int) const { return -1; }
    virtual std::vector<std::pair<int, int>> midiMappings() const { return {}; } // (cc, param index)
    virtual void clearMidiMapping(int) {}
    virtual void clearAllMidiMappings() {}
    virtual void revertMidiMappings() {}
    virtual void saveMidiMappings() {}
    virtual bool midiEnabled() const { return false; }
    virtual void setMidiEnabled(bool) {}
};

class AurumPlugin final : public Controller
{
public:
    static const clap_plugin_descriptor_t* descriptor();

    explicit AurumPlugin(const clap_host_t* host);
    ~AurumPlugin() override;

    const clap_plugin_t* clapPlugin() const { return &plugin_; }

    // Controller
    double paramValue(int index) const override;
    void beginEdit(int index) override;
    void performEdit(int index, double value) override;
    void endEdit(int index) override;
    double sampleRate() const override { return sampleRate_; }
    double tempo() const override { return tempo_; }
    bool midiLearnActive() const override { return learnActive_; }
    void setMidiLearn(bool on) override;
    void setLearnTarget(int index) override { learnTarget_.store(index); }
    int learnTarget() const override { return learnTarget_.load(); }
    int ccForParam(int index) const override;
    std::vector<std::pair<int, int>> midiMappings() const override;
    void clearMidiMapping(int cc) override;
    void clearAllMidiMappings() override;
    void revertMidiMappings() override;
    void saveMidiMappings() override;
    bool midiEnabled() const override { return midiEnabled_.load(); }
    void setMidiEnabled(bool on) override;

private:
    struct GuiEvent
    {
        enum Type : uint8_t { Begin, Value, End } type;
        int index;
        double value;
    };

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
    uint32_t portsConfigCount() const;
    bool portsConfigGet(uint32_t index, clap_audio_ports_config_t* config) const;
    bool portsConfigSelect(clap_id configId);
    bool paramsGetInfo(uint32_t index, clap_param_info_t* info) const;
    bool paramsGetValue(clap_id id, double* value) const;
    bool paramsValueToText(clap_id id, double value, char* display, uint32_t size) const;
    bool paramsTextToValue(clap_id id, const char* display, double* value) const;
    void paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out);
    bool stateSave(const clap_ostream_t* stream);
    bool stateLoad(const clap_istream_t* stream);
    uint32_t tailGet() const;

    // GUI
    bool guiCreate();
    void guiDestroy();
    bool guiSetScale(double scale);
    bool guiGetSize(uint32_t* w, uint32_t* h);
    bool guiAdjustSize(uint32_t* w, uint32_t* h);
    bool guiSetSize(uint32_t w, uint32_t h);
    bool guiSetParent(const clap_window_t* window);
    bool guiShow();
    bool guiHide();
    void onTimer(clap_id id);
    void onFd(int fd, clap_posix_fd_flags_t flags);

    uint32_t notePortsCount(bool isInput) const { return isInput ? 1 : 0; }
    bool notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info) const;
    void handleMidi(const clap_event_midi_t* ev);

    void applyValues(const std::vector<double>& values); // main thread, e.g. presets
    std::vector<double> currentValues() const;
    void importIr(const std::string& path);

    void handleEvent(const clap_event_header_t* ev);
    void drainGuiEvents(const clap_output_events_t* out);
    void syncFromShared();
    void setShared(int index, double value);

    clap_plugin_t plugin_;
    const clap_host_t* host_;
    const clap_host_params_t* hostParams_ = nullptr;
    const clap_host_state_t* hostState_ = nullptr;
    const clap_host_latency_t* hostLatency_ = nullptr;
    const clap_host_tail_t* hostTail_ = nullptr;
    const clap_host_gui_t* hostGui_ = nullptr;
    const clap_host_timer_support_t* hostTimer_ = nullptr;
    const clap_host_posix_fd_support_t* hostFd_ = nullptr;
    std::unique_ptr<gui::Editor> editor_;
    clap_id timerId_ = CLAP_INVALID_ID;
    int registeredFd_ = -1;

    const ParamTable& table_;
    // Values visible to all threads (stored units, see ParamTable).
    std::unique_ptr<std::atomic<double>[]> shared_;
    // Audio thread copy used for DSP.
    std::vector<double> audio_;
    std::atomic<bool> reloadFromShared_{true};
    SpscQueue<GuiEvent> guiEvents_{1024};
    // MIDI learn: CC number -> parameter index (-1 = none).
    std::array<std::atomic<int>, 128> ccMap_{};
    std::atomic<int> learnTarget_{-1};
    std::atomic<bool> midiEnabled_{true};
    bool learnActive_ = false;
    std::atomic<bool> learnActiveAudio_{false};
    const clap_output_events_t* currentOut_ = nullptr;
    PresetSession session_;

    dsp::ReverbEngine engine_;
    dsp::EngineParams engineParams_{};
    bool engineParamsDirty_ = true;
    double sampleRate_ = 48000.0;
    double tempo_ = 120.0;
    bool offline_ = false;
    bool active_ = false;
    std::atomic<bool> processing_{false};
    int portsConfig_ = 0; // 0 stereo, 1 mono, 2 mono -> stereo
    std::vector<float> scratchL_, scratchR_;
    uint32_t lastTail_ = 0;

    friend struct PluginGlue;
};

// Factory/entry implementation (exported by the format-specific entry file).
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace aurum
