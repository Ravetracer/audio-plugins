#pragma once

// The editor beside the plugin, in the same process: every call goes straight
// to ResidentPlugin (the CLAP, VST3 and AU builds, and the snapshot tool).

#include "PluginLink.h"

namespace resident {
class ResidentPlugin;
}

namespace resident::gui {

class LocalLink final : public PluginLink
{
public:
    explicit LocalLink(ResidentPlugin& plugin) : p_(plugin) {}

    uint64_t frameSerial() override;
    uint64_t copyFrame(uint8_t* dst) override;
    uint32_t midiInCount() override;
    uint32_t midiOutCount() override;
    bool driveLed() override;
    bool warping() override;
    void setWarp(bool on) override;

    const MachineSettings& settings() override;
    void applySettings(const MachineSettings& s) override;
    double paramValue(uint32_t id) override;
    void beginParam(uint32_t id) override;
    void setParam(uint32_t id, double value) override;
    void endParam(uint32_t id) override;

    const CcMap& ccMap() override;
    void setCcMap(const CcMap& map) override;
    void setCcSlot(int slot, const CcSlot& s) override;
    uint32_t ccMapSerial() override;

    bool hasRoms() override;
    bool hasDriveRom() override;
    bool rescanRoms() override;
    std::string romReport() override;

    void key(uint8_t matrixPos, bool down) override;
    void joystick(int port, uint8_t bits) override;
    void restoreKey() override;
    void mouseMove(int port, int dx, int dy) override;
    void paddles(int port, uint8_t a, uint8_t b) override;
    void deviceButtons(int port, uint8_t bits) override;

    void resetButton() override;
    void powerCycle() override;
    void freezeButton() override;
    bool cartridgeFreezable() override;
    void ejectDisk() override;
    void ejectCartridge() override;

    bool loadFile(const std::string& path, std::string& error) override;
    std::string loadNotice() override;
    bool attachDiskFile(const std::string& path, std::string& error) override;
    bool diskPresent() override;
    std::string diskTitle() override;
    std::vector<DiskEntry> diskDirectory() override;
    bool startFromDisk(const std::string& petsciiName, std::string& error) override;
    bool exportDisk(const std::string& path, std::string& error) override;
    std::string mediaSummary() override;

private:
    ResidentPlugin& p_;
};

} // namespace resident::gui
