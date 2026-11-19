#pragma once

// The editor module's handle on the plugin instance it edits (ui.wasm only).
//
// The editor runs in the page and the plugin on the host's audio thread, so
// the window cannot call the plugin. It is given a second instance instead --
// the mirror -- which never processes audio: the window edits the mirror as it
// edits the plugin on the desktop, and the mirror sends what changed to the
// real plugin as page messages and takes in what the plugin reports. Every
// rule the plugin has about presets, patterns, the FX and the wavetable is
// therefore applied by the same code in both, and the messages carry only
// results. See src/web/protocol.h for the messages and plugin_web.inc for both
// ends.
//
// The extension id is private to this plugin's editor; no host asks for it.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace saeurekiste {

class WebCanvas;

constexpr char kWebMirrorExtension[] = "de.ravetracer.saeurekiste.web-mirror";

class WebMirror {
public:
   virtual ~WebMirror() = default;

   // Where the mirror's messages to the plugin go (wclap.send).
   virtual void webSetSender(std::function<void(const uint8_t *, size_t)> send) = 0;

   // Opens the editor's window on the mirror. Null if it could not be made.
   virtual WebCanvas *webOpenEditor() = 0;
   // The window's clock: once per animation frame.
   virtual void webTick() = 0;

   // Sends everything the editor changed since the last call. `final` is
   // false while a pointer button is held: the edit is still going on, and the
   // undo step it makes is closed by the first call with `final` set.
   virtual void webCollect(bool final) = 0;
   // A poll; also asks for the state when the plugin's has moved.
   virtual void webPoll() = 0;
   // A message from the plugin.
   virtual void webReceive(const uint8_t *data, size_t size) = 0;
   // Whether the plugin's state has arrived. Until it has, the editor shows a
   // fresh instance and must not send edits made to it.
   virtual bool webReady() const = 0;
};

} // namespace saeurekiste
