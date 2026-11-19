#include "LocalLink.h"

#include "plugin/ResidentPlugin.h"

namespace resident::gui {

uint64_t LocalLink::frameSerial() { return p_.engine().frameSerial(); }
uint64_t LocalLink::copyFrame(uint8_t* dst) { return p_.engine().copyFrame(dst); }
uint32_t LocalLink::midiInCount() { return p_.engine().midiInCount.load(std::memory_order_relaxed); }
uint32_t LocalLink::midiOutCount() { return p_.engine().midiOutCount.load(std::memory_order_relaxed); }
bool LocalLink::driveLed() { return p_.engine().driveLed.load(std::memory_order_relaxed); }
bool LocalLink::warping() { return p_.warping(); }
void LocalLink::setWarp(bool on) { p_.setWarp(on); }

const MachineSettings& LocalLink::settings() { return p_.settings(); }
void LocalLink::applySettings(const MachineSettings& s) { p_.applySettings(s); }
double LocalLink::paramValue(uint32_t id) { return p_.paramValue(id); }
void LocalLink::beginParam(uint32_t id) { p_.guiBeginParam(id); }
void LocalLink::setParam(uint32_t id, double value) { p_.guiSetParam(id, value); }
void LocalLink::endParam(uint32_t id) { p_.guiEndParam(id); }

const CcMap& LocalLink::ccMap() { return p_.ccMap(); }
void LocalLink::setCcMap(const CcMap& map) { p_.setCcMap(map); }
void LocalLink::setCcSlot(int slot, const CcSlot& s) { p_.setCcSlot(slot, s); }
uint32_t LocalLink::ccMapSerial() { return p_.ccMapSerial(); }

bool LocalLink::hasRoms() { return p_.hasRoms(); }
bool LocalLink::hasDriveRom() { return p_.hasDriveRom(); }
bool LocalLink::rescanRoms() { return p_.rescanRoms(); }
std::string LocalLink::romReport() { return p_.romReport(); }

void LocalLink::key(uint8_t matrixPos, bool down) { p_.key(matrixPos, down); }
void LocalLink::joystick(int port, uint8_t bits) { p_.joystick(port, bits); }
void LocalLink::restoreKey() { p_.restoreKey(); }
void LocalLink::mouseMove(int port, int dx, int dy) { p_.mouseMove(port, dx, dy); }
void LocalLink::paddles(int port, uint8_t a, uint8_t b) { p_.paddles(port, a, b); }
void LocalLink::deviceButtons(int port, uint8_t bits) { p_.deviceButtons(port, bits); }

void LocalLink::resetButton() { p_.resetButton(); }
void LocalLink::powerCycle() { p_.powerCycle(); }
void LocalLink::freezeButton() { p_.freezeButton(); }
bool LocalLink::cartridgeFreezable() { return p_.cartridgeFreezable(); }
void LocalLink::ejectDisk() { p_.ejectDisk(); }
void LocalLink::ejectCartridge() { p_.ejectCartridge(); }

bool LocalLink::loadFile(const std::string& path, std::string& error) { return p_.loadFile(path, error); }
std::string LocalLink::loadNotice() { return p_.loadNotice(); }
bool LocalLink::attachDiskFile(const std::string& path, std::string& error) { return p_.attachDiskFile(path, error); }
bool LocalLink::diskPresent() { return p_.diskPresent(); }
std::string LocalLink::diskTitle() { return p_.diskTitle(); }
std::vector<DiskEntry> LocalLink::diskDirectory() { return p_.diskDirectory(); }
bool LocalLink::startFromDisk(const std::string& petsciiName, std::string& error)
{
    return p_.startFromDisk(petsciiName, error);
}
bool LocalLink::exportDisk(const std::string& path, std::string& error) { return p_.exportDisk(path, error); }
std::string LocalLink::mediaSummary() { return p_.mediaSummary(); }

} // namespace resident::gui
