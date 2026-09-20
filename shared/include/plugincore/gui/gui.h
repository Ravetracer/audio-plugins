#pragma once

// The plugin window, shared by the suite. Deliberately toolkit-free: raw X11
// for the window and Cairo for the drawing, so a plugin stays a single .clap
// file with no runtime dependencies a Linux audio machine does not already
// have.
//
// Every window built on it is the same window: the same layout engine, the same
// widgets, the same interactions. A plugin supplies a WindowSpec (see
// window.h) describing its panels, its palette and its header ornament.
//
// The GUI never touches the plugin's atomics directly. It reads and writes
// parameters through GuiDelegate, which turns writes into proper CLAP events
// so host automation recording sees them.

#include <cstdint>
#include <string>
#include <vector>

namespace plugincore {

// One entry in the preset browser. Exactly one of loadKey / path is set:
// built-in presets live in the binary, user presets are files on disk.
struct GuiPreset {
   std::string name;
   std::string description;
   std::string loadKey;
   std::string path;
   bool userContent = false;
   // Which shelf of the library this preset is on: the name of the directory
   // it was found in, one level under the user preset directory, or whatever
   // the plugin calls its built-in set. Empty means the library's root, which
   // is where a preset saved with no folder in its name lands.
   //
   // A plugin that does not group its presets leaves this empty on every entry
   // and the browser draws one flat list, exactly as it did before folders
   // existed.
   std::string folder;
};

// Everything the window needs from the plugin.
class GuiDelegate {
public:
   virtual ~GuiDelegate() = default;

   virtual double guiParamValue(uint32_t id) const = 0;
   virtual void guiBeginEdit(uint32_t id) = 0;
   virtual void guiSetParam(uint32_t id, double value) = 0;
   virtual void guiEndEdit(uint32_t id) = 0;

   // Decaying output peak per channel, 0..1 linear, published by the audio
   // thread.
   virtual void guiOutputPeaks(float &left, float &right) const = 0;

   // Voices currently sounding, and the pool they come from, published by the
   // audio thread. "Voice" is whatever the plugin's activity meter counts:
   // droplets for rain, shock waves for thunder.
   virtual uint32_t guiVoiceCount() const = 0;
   virtual uint32_t guiVoiceLimit() const = 0;

   // A monotonic count of discrete events -- lightning flashes, say. The header
   // ornament is given it, and the window repaints when it changes. Plugins
   // with no such thing leave it at zero.
   virtual uint32_t guiEventCounter() const { return 0; }

   virtual const std::vector<GuiPreset> &guiPresets() const = 0;
   virtual int guiCurrentPreset() const = 0;
   virtual bool guiPresetEdited() const = 0;
   virtual void guiLoadPreset(int index) = 0;

   // The name to offer when the save field opens.
   virtual std::string guiSuggestedPresetName() const = 0;
   // Writes the current parameter values into the user preset directory and
   // rescans it. Returns false and fills `error` if that did not work.
   virtual bool guiSavePreset(const std::string &name, std::string &error) = 0;

   // ------------------------------------------------------- folders and packs
   //
   // All of these default to "this plugin does not do that", so a plugin that
   // keeps a flat preset directory implements none of them and its browser is
   // the list it always was.

   // Whether the browser should offer folders and the pack buttons at all.
   virtual bool guiPresetFoldersSupported() const { return false; }

   // The pack files the plugin can see, as full paths. What the IMPORT list
   // shows before it offers a file dialog.
   virtual std::vector<std::string> guiPresetPacks() const { return {}; }

   // Where a pack of `folder` would be written by default, full path. Empty
   // when there is nowhere to write one.
   virtual std::string guiPackPathFor(const std::string &folder) const {
      (void)folder;
      return {};
   }

   // Writes every preset in `folder` to one pack file at `path`.
   virtual bool guiExportPack(const std::string &folder, const std::string &path,
                              std::string &error) {
      (void)folder;
      (void)path;
      error = "this plugin has no preset packs";
      return false;
   }

   // Reads a pack in and adds its presets to the library as a folder, whose
   // name comes back in `folder` so the browser can select it. Nothing is
   // overwritten: a preset whose file is already there is saved beside it.
   virtual bool guiImportPack(const std::string &path, std::string &folder, std::string &error) {
      (void)path;
      (void)folder;
      error = "this plugin has no preset packs";
      return false;
   }

   // The version label in the header was clicked. Most plugins have nothing to
   // do with that, which is why it defaults to doing nothing.
   virtual void guiVersionClicked() {}

   // The window has just taken the keyboard for its save field. Whatever the
   // host had routed to the plugin, the key releases for it now go to the
   // window instead: on X11 because of the grab, on Windows because the focus
   // moved. A plugin that keeps its own record of what is held has to assume
   // that record is about to go stale -- a note played from the host's computer
   // keyboard gets its note-on and never its note-off.
   //
   // Plugins that hold nothing of their own do not need it, which is why it
   // defaults to doing nothing.
   virtual void guiKeyboardTaken() {}
};

class Gui {
public:
   virtual ~Gui() = default;

   // Creates the window, unmapped and unparented.
   virtual bool open() = 0;
   // A native window handle. Not `unsigned long`: that is 32 bits on 64-bit
   // Windows, which would quietly truncate an HWND.
   virtual bool embed(uintptr_t parentWindow) = 0;
   virtual bool setTransientFor(uintptr_t parentWindow) = 0;
   virtual void setTitle(const char *title) = 0;
   virtual void setScale(double scale) = 0;
   virtual void size(uint32_t *width, uint32_t *height) const = 0;
   // The layout's own size at scale 1, which is the aspect ratio a host has to
   // keep when it resizes the window.
   virtual void designSize(uint32_t *width, uint32_t *height) const = 0;
   // Snaps a requested size to one the window can take: the largest scale that
   // fits inside it, within the scale range, at the design aspect ratio.
   virtual void fitSize(uint32_t *width, uint32_t *height) const = 0;
   // Resizes to the requested size by choosing the scale from it. The window
   // ends up at fitSize() of the request.
   virtual bool resize(uint32_t width, uint32_t height) = 0;
   virtual void show() = 0;
   virtual void hide() = 0;

   // Drains pending X11 events and repaints. Called from a host timer.
   virtual void tick() = 0;
};



} // namespace plugincore
