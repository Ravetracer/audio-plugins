#pragma once

// What the editor needs from the plugin. Natively the editor runs beside the
// plugin and calls it directly (LocalLink). In the WCLAP the editor runs in
// the browser page and the plugin in the host's audio worklet, so the same
// calls become messages (RemoteLink in web/UiMain.cpp): values are mirrored
// from the plugin's answers, and an action's result arrives later, as a
// notice.

#include <cstdint>
#include <string>
#include <vector>

#include "plugin/CcMap.h"
#include "plugin/DiskEntry.h"
#include "plugin/MachineSettings.h"

namespace resident::gui {

// The ROMs, as bits of PluginLink::romsPresent().
enum RomBit : unsigned { RomKernal = 1, RomBasic = 2, RomChargen = 4, RomDrive = 8 };

class PluginLink
{
public:
    virtual ~PluginLink() = default;

    // True when the plugin is reached through messages (the WCLAP page).
    virtual bool remote() const { return false; }
    // Remote: a message from the plugin for the status line, once each.
    virtual bool takeNotice(std::string& text, bool& error) { return false; }

    // ------------------------------------------------------------ screen, indicators
    virtual uint64_t frameSerial() = 0;
    // Copies the newest frame (Vic::kWidth x kHeight palette indices).
    virtual uint64_t copyFrame(uint8_t* dst) = 0;
    // Whether the screen is shown (remote: the page then asks for frames).
    virtual void setScreenWanted(bool) {}
    virtual uint32_t midiInCount() = 0;
    virtual uint32_t midiOutCount() = 0;
    virtual bool driveLed() = 0;
    virtual bool warping() = 0;
    virtual void setWarp(bool on) = 0;

    // ------------------------------------------------------------ the machine page
    virtual const MachineSettings& settings() = 0;
    virtual void applySettings(const MachineSettings& s) = 0;

    // Parameters, by id.
    virtual double paramValue(uint32_t id) = 0;
    virtual void beginParam(uint32_t id) = 0;
    virtual void setParam(uint32_t id, double value) = 0;
    virtual void endParam(uint32_t id) = 0;

    // ------------------------------------------------------------ CC maps
    virtual const CcMap& ccMap() = 0;
    virtual void setCcMap(const CcMap& map) = 0;
    virtual void setCcSlot(int slot, const CcSlot& s) = 0;
    // Changes with every map change, so the CC page knows when to rebuild.
    virtual uint32_t ccMapSerial() = 0;

    // ------------------------------------------------------------ ROMs
    virtual bool hasRoms() = 0;
    virtual bool hasDriveRom() = 0;
    // Natively: looks into the ROM folder again. True when the C64 has its ROMs.
    virtual bool rescanRoms() = 0;
    virtual std::string romReport() = 0;
    // Remote: which ROMs the plugin has (RomBit), and an image to add; the
    // plugin recognises it by its content and keeps it in the project.
    virtual unsigned romsPresent() { return 0; }
    virtual void uploadRom(const std::string& name, const std::vector<uint8_t>& data) {}

    // ------------------------------------------------------------ input
    virtual void key(uint8_t matrixPos, bool down) = 0;
    virtual void joystick(int port, uint8_t bits) = 0;
    virtual void restoreKey() = 0;
    virtual void mouseMove(int port, int dx, int dy) = 0;
    virtual void paddles(int port, uint8_t a, uint8_t b) = 0;
    virtual void deviceButtons(int port, uint8_t bits) = 0;

    // ------------------------------------------------------------ buttons
    virtual void resetButton() = 0;
    virtual void powerCycle() = 0;
    virtual void freezeButton() = 0;
    virtual bool cartridgeFreezable() = 0;
    virtual void ejectDisk() = 0;
    virtual void ejectCartridge() = 0;

    // ------------------------------------------------------------ media
    // Paths in the editor's file system. Remote: true when the file was sent.
    virtual bool loadFile(const std::string& path, std::string& error) = 0;
    virtual std::string loadNotice() = 0;
    virtual bool attachDiskFile(const std::string& path, std::string& error) = 0;
    virtual bool diskPresent() = 0;
    // Changes when another disk goes in (remote: when the plugin reports it).
    virtual uint32_t diskSerial() { return 0; }
    virtual std::string diskTitle() = 0;
    virtual std::vector<DiskEntry> diskDirectory() = 0;
    virtual bool startFromDisk(const std::string& petsciiName, std::string& error) = 0;
    // Natively a path to write; remote: the page hands the image over as a
    // download named after `path`'s file name.
    virtual bool exportDisk(const std::string& path, std::string& error) = 0;
    virtual std::string mediaSummary() = 0;
};

} // namespace resident::gui
