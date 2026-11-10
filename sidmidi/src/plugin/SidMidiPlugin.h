#pragma once

// The CLAP plugin around the C64.
//
// Threads: the machine runs in process(). The GUI changes it in two ways:
// small input (keys, joystick, RESTORE, parameter moves) through lock-free
// rings, and everything structural (loading media, reset, the machine page)
// under machineLock_, which the audio thread takes with a short spin -- long
// enough to ride out a state save, short enough never to stall the host. A
// change that takes longer (building a new SID's filter tables) costs a few
// silent blocks, which is what a user who just changed the hardware expects.

#include <clap/clap.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/Engine.h"
#include "plugin/CcMap.h"
#include "plugin/MachineSettings.h"
#include "plugin/Params.h"
#include "plugin/SpscRing.h"

namespace sidmidi {

namespace gui {
class Editor;
}

// A plugin-specific extension: load a program as the Load button does, and
// count what reached the machine. For test harnesses and automation.
inline constexpr char kExtLoader[] = "de.ravetracer.sidmidi.loader/2";
struct ExtLoader
{
    bool (*load_file)(const clap_plugin_t* plugin, const char* path);
    // MIDI messages handed to the C64 so far (notes, CCs, CC slots, ...).
    uint32_t (*midi_in_count)(const clap_plugin_t* plugin);
};


class SidMidiPlugin
{
public:
    explicit SidMidiPlugin(const clap_host_t* host);
    ~SidMidiPlugin();

    static const clap_plugin_descriptor_t* descriptor();
    const clap_plugin_t* clapPlugin() { return &plugin_; }

    // ------------------------------------------------------------ CLAP
    bool init();
    void destroy();
    bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames);
    void deactivate();
    bool startProcessing() { return true; }
    void stopProcessing() {}
    void reset() {}
    clap_process_status process(const clap_process_t* p);
    const void* getExtension(const char* id);
    void onMainThread();

    uint32_t audioPortsCount(bool isInput) const;
    bool audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info_t* info) const;
    uint32_t notePortsCount(bool isInput) const { return 1; }
    bool notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info) const;

    uint32_t paramsCount() const { return static_cast<uint32_t>(params_.size()); }
    bool paramsInfo(uint32_t index, clap_param_info_t* info) const;
    bool paramsValue(clap_id id, double* value) const;
    bool paramsToText(clap_id id, double value, char* display, uint32_t size) const;
    bool paramsFromText(clap_id id, const char* display, double* value) const;
    void paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out);

    bool stateSave(const clap_ostream_t* stream);
    bool stateLoad(const clap_istream_t* stream);

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
    void onFd(int fd);

    // ------------------------------------------------------------ for the editor
    Engine& engine() { return engine_; }
    std::mutex& machineLock() { return machineLock_; }

    // Loads a .prg, .d64, .crt or .bin by its extension and starts it.
    bool loadFile(const std::string& path, std::string& error);
    bool loadData(const std::string& name, const std::vector<uint8_t>& data, std::string& error);
    void powerCycle();   // off and on: the attached media start again
    void resetButton();  // the reset button: memory survives
    void ejectDisk();
    void ejectCartridge();
    bool exportDisk(const std::string& path, std::string& error);
    void setWarp(bool on);
    bool warping() const { return engine_.warping(); }

    const MachineSettings& settings() const { return settings_; }
    void applySettings(const MachineSettings& s);

    void key(uint8_t matrixPos, bool down);
    void joystick(int port, uint8_t bits);
    void restoreKey();
    void releaseInput();
    void mouseMove(int port, int dx, int dy);
    void paddles(int port, uint8_t a, uint8_t b);
    void deviceButtons(int port, uint8_t bits);

    const std::vector<ParamInfo>& params() const { return params_; }
    double paramValue(uint32_t id) const;
    void guiBeginParam(uint32_t id);
    void guiSetParam(uint32_t id, double value);
    void guiEndParam(uint32_t id);

    // ------------------------------------------------------------ CC maps (main thread)
    const CcMap& ccMap() const { return ccMap_; }
    // Renames the slots and tells the host. Values stay; nothing is sent
    // until a slot moves.
    void setCcMap(const CcMap& map);
    // One slot changed in the map being edited.
    void setCcSlot(int slot, const CcSlot& s);
    // Counts map changes, so the editor knows when to rebuild.
    uint32_t ccMapSerial() const { return ccMapSerial_; }

    std::string mediaSummary();
    bool hasRoms() const { return hasRoms_.load(); }
    // Looks into the ROM folder again (main thread). Starts the machine when
    // the ROMs are complete now. Returns whether they are.
    bool rescanRoms();
    std::string romReport() const { return romReport_; }
    bool hasDriveRom() const { return hasDriveRom_.load(); }
    std::string lastError() const { return lastError_; }

private:
    struct InputEvent
    {
        enum Kind : uint8_t { Key, Joy, Restore, ReleaseAll, Mouse, Paddles, DevButtons } kind;
        uint8_t a = 0, b = 0;
        int16_t c = 0, d = 0;
    };
    struct GuiParamEvent
    {
        enum Kind : uint8_t { Begin, Value, End } kind;
        uint32_t id;
        double value;
    };

    int paramIndex(uint32_t id) const;
    // `send`: a CC slot that moved sends its controller (not when values are
    // only being restored).
    void applyParam(uint32_t id, double v, bool send = true);
    void publishCcRoutes();
    void followProgram(const std::string& fileName);
    // Audio thread: a CC slot's message, at `offset`, when its value changed.
    void emitCcSlot(int slot, uint32_t offset);
    void applyParamsToEngine();
    void handleInputEvents();
    void drainGuiParams(const clap_output_events_t* out);
    void collectEvents(const clap_process_t* p);
    void generateClock(const clap_process_t* p);
    void emitMidiOut(const clap_output_events_t* out);
    void configureMachine();

    clap_plugin_t plugin_{};
    const clap_host_t* host_;
    const clap_host_params_t* hostParams_ = nullptr;
    const clap_host_state_t* hostState_ = nullptr;
    const clap_host_gui_t* hostGui_ = nullptr;
    const clap_host_timer_support_t* hostTimer_ = nullptr;
    const clap_host_posix_fd_support_t* hostFd_ = nullptr;

    Engine engine_;
    std::mutex machineLock_;
    MachineSettings settings_;
    std::atomic<bool> hasRoms_{false};
    std::atomic<bool> hasDriveRom_{false};
    std::string romReport_;
    // A project loaded before the ROMs were there: applied once they are.
    std::vector<uint8_t> pendingSnapshot_, pendingAudio_;
    std::string lastError_;
    std::string lastFileName_;
    std::vector<uint8_t> lastFileData_;

    std::vector<ParamInfo> params_;
    std::unique_ptr<std::atomic<double>[]> values_;

    CcMap ccMap_;
    uint32_t ccMapSerial_ = 0;
    // For the audio thread: per slot (channel << 8) | cc, or -1.
    std::array<std::atomic<int32_t>, kCcSlots> ccRoute_{};
    std::atomic<uint64_t> ccDirty_{0};       // slots moved outside process(), one bit each
    static_assert(kCcSlots <= 64, "the dirty mask holds 64 slots");
    std::array<int16_t, kCcSlots> ccSent_{}; // audio thread: last value sent
    SpscRing<InputEvent, 1024> input_;
    SpscRing<GuiParamEvent, 512> guiParams_;

    // Audio thread scratch.
    std::vector<MidiEvent> events_, midiOut_;
    std::vector<uint8_t> sysexIn_;
    std::vector<uint8_t> txMessage_;
    uint8_t txRunning_ = 0;
    int txExpected_ = 0;
    bool txInSysex_ = false;
    std::vector<uint8_t> sysexOut_;
    bool wasPlaying_ = false;
    double nextClockTick_ = -1.0;
    std::vector<float> silence_;

    std::unique_ptr<gui::Editor> editor_;
    clap_id timerId_ = CLAP_INVALID_ID;
    int registeredFd_ = -1;
    std::atomic<bool> activated_{false};
};

bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace sidmidi
