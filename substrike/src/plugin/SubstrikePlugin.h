#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <clap/clap.h>

#include "Controller.h"
#include "Params.h"
#include "History.h"
#include "Player.h"
#include "dsp/Engine.h"
#include "util/SpscQueue.h"

namespace substrike {

namespace gui {
class Editor;
}

class SubstrikePlugin final : public Controller
{
public:
    static const clap_plugin_descriptor_t* descriptor();

    explicit SubstrikePlugin(const clap_host_t* host);
    ~SubstrikePlugin() override;

    const clap_plugin_t* clapPlugin() const { return &plugin_; }

    // Controller (main thread)
    double paramValue(int index) const override;
    void beginEdit(int index) override;
    void performEdit(int index, double value) override;
    void endEdit(int index) override;
    const dsp::Curve& curve(int index) const override { return curves_[static_cast<size_t>(index)]; }
    void setCurve(int index, const dsp::Curve& c) override;
    double sampleRate() const override { return sampleRate_; }
    void audition() override;
    void loadDocument(const StateDocument& doc, const std::string& name) override;
    StateDocument currentDocument() const override;
    std::string presetName() const override { return presetName_; }
    double modulatedValue(int index) const override
    {
        return modShared_[static_cast<size_t>(index)].load(std::memory_order_relaxed);
    }
    uint32_t modulationGeneration() const override { return modGeneration_.load(std::memory_order_relaxed); }
    void checkpoint() override;
    bool undo() override;
    bool redo() override;
    bool canUndo() const override { return history_.canUndo(); }
    bool canRedo() const override { return history_.canRedo(); }
    int readScope(float* lo, float* hi, int max) override;

private:
    struct GuiEvent
    {
        enum Type : uint8_t { Begin, Value, End } type;
        int index;
        double value;
    };
    struct CurveEvent
    {
        int index;
        dsp::Curve curve;
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
    uint32_t notePortsCount(bool isInput) const { return isInput ? 1 : 0; }
    bool notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info) const;
    bool paramsGetInfo(uint32_t index, clap_param_info_t* info) const;
    bool paramsGetValue(clap_id id, double* value) const;
    bool paramsValueToText(clap_id id, double value, char* display, uint32_t size) const;
    bool paramsTextToValue(clap_id id, const char* display, double* value) const;
    void paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out);
    bool stateSave(const clap_ostream_t* stream);
    bool stateLoad(const clap_istream_t* stream);
    bool presetLoad(uint32_t kind, const char* location, const char* loadKey);
    // Makes `doc` the plugin's state: values, curves, the preset's name; the
    // sounding hit stops and the host rescans (main thread).
    void applyDocument(const StateDocument& doc);

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

    void handleEvent(const clap_event_header_t* ev, const clap_output_events_t* out);
    // A new value for a parameter, from the host or the GUI (audio thread).
    void applyValue(int index, double value, const clap_output_events_t* out);
    void drainGuiEvents(const clap_output_events_t* out);
    // Curves the main thread changed, into the engine (audio thread).
    void drainCurves();
    // Hands every curve whose copy in the engine may be stale to the audio
    // thread (main thread).
    void pushCurves();
    void requestFlush();
    void syncFromShared();
    void setShared(int index, double value);
    // A parameter as it is now: a slot letter as its slot's type defines it.
    const ParamDef& current(int index) const;
    // A slot's type changed: its letters go to the new type's defaults, and
    // the host hears about it.
    void typeChanged(int typeIndex, const clap_output_events_t* out);

    clap_plugin_t plugin_;
    const clap_host_t* host_;
    const clap_host_params_t* hostParams_ = nullptr;
    const clap_host_state_t* hostState_ = nullptr;
    const clap_host_preset_load_t* hostPresetLoad_ = nullptr;
    // The preset the state came from, shown by the browser; saved with the
    // state.
    std::string presetName_ = "Init";
    const clap_host_gui_t* hostGui_ = nullptr;
    const clap_host_timer_support_t* hostTimer_ = nullptr;
    const clap_host_posix_fd_support_t* hostFd_ = nullptr;

    const ParamTable& table_;
    // Values visible to all threads (stored units, see ParamTable).
    std::unique_ptr<std::atomic<double>[]> shared_;
    // The audio thread's copy, used for DSP.
    std::vector<double> audio_;
    std::atomic<bool> reloadFromShared_{true};
    // The matrix's view of the parameters it moves, for the editor (written
    // by the audio thread after every block).
    void publishModulation();
    std::unique_ptr<std::atomic<double>[]> modShared_;
    std::atomic<uint32_t> modGeneration_{0};
    std::array<int, pid::kNumRoutes> modPublished_{};
    int modPublishedCount_ = 0;
    // A new state arrived: the audio thread stops the sounding hit.
    std::atomic<bool> freshStartRequested_{false};
    // The editor asked for a hit.
    std::atomic<bool> auditionRequested_{false};
    // A slot's type changed, so its letters have new names: the main thread
    // tells the host.
    std::atomic<bool> rescanInfo_{false};

    // Edits from the GUI, in order, for the audio thread.
    // Big enough for an undo that changes every parameter (three events each).
    SpscQueue<GuiEvent> guiEvents_{16384};

    // The oscilloscope's feed: the audio thread folds the main output into
    // bins and the editor reads them.
    struct ScopeBin
    {
        float lo, hi;
    };
    void feedScope(const float* l, const float* r, uint32_t n);
    SpscQueue<ScopeBin> scope_{8192};
    float scopeLo_ = 0.0f, scopeHi_ = 0.0f;
    int scopeCount_ = 0;

    History::Snapshot snapshot() const;
    // Sends a snapshot's values to the host and the engine as edits, the
    // slot types first, and its curves.
    void restore(const History::Snapshot& s);
    History history_;
    bool restoring_ = false;
    // The curves as the main thread knows them; the engine holds its own
    // copies, updated through curveEvents_.
    std::array<dsp::Curve, dsp::kNumCurves> curves_{};
    SpscQueue<CurveEvent> curveEvents_{64};
    std::array<bool, dsp::kNumCurves> curvePending_{};

    std::unique_ptr<gui::Editor> editor_;
    clap_id timerId_ = CLAP_INVALID_ID;
    int registeredFd_ = -1;

    Player player_;
    // Render target for ports the host did not connect, and for the right
    // channel of a mono one.
    static constexpr uint32_t kScratch = 256;
    std::array<std::array<std::array<float, kScratch>, 2>, dsp::Engine::kNumBuses> scratch_{};
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
