// The WCLAP's editor (ui.wasm): RumpelKiste's own window, running in the
// plugin's page and drawn into its canvas (shared/web/wclap-ui.js). The plugin
// itself runs on the host's audio thread as module.wasm and is reached only
// through the page's messages (src/web/protocol.h).
//
// The window is opened on a mirror: a second instance of the plugin, made
// here through clap_entry and never activated. The window edits it exactly as
// it edits the plugin on the desktop, and the mirror sends what changed and
// takes in what the plugin reports (src/web/plugin_web.inc). Presets, user
// wavetables and settings live in the editor's own file system, which the
// page keeps under /home/web.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <clap/clap.h>

#include "gui/webcanvas.h"
#include "rumpelkiste.h"
#include "web/mirror.h"
#include "web/protocol.h"

extern "C" {
extern const clap_plugin_entry_t clap_entry;
// Posts bytes to the plugin (window.parent.postMessage in the page).
__attribute__((import_module("wclap"), import_name("send"))) void wclapJsSend(const void *data,
                                                                            int size);
}

namespace rumpelkiste {

namespace {

struct Ui {
   const clap_plugin_t *plugin = nullptr;
   WebMirror *mirror = nullptr;
   WebCanvas *canvas = nullptr;
   double lastPoll = 0.0;
};

Ui *ui = nullptr;
std::vector<uint8_t> scratch;

double nowSeconds() {
   return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sendToPlugin(const uint8_t *data, size_t size) { wclapJsSend(data, static_cast<int>(size)); }

// The window asks its host for a size when the collapsible section or the FX
// page changes its height; the plugin asks the real host for it.
bool hostRequestResize(const clap_host_t *, uint32_t width, uint32_t height) {
   if (!ui || !ui->canvas)
      return false;
   uint32_t dw = 0, dh = 0;
   ui->canvas->webDesignSize(dw, dh);
   web::Writer w(web::kPageResize);
   w.put(width);
   w.put(height);
   w.put(dw);
   w.put(dh);
   sendToPlugin(w.bytes.data(), w.bytes.size());
   return true;
}
void hostResizeHintsChanged(const clap_host_t *) {}
bool hostRequestShow(const clap_host_t *) { return false; }
bool hostRequestHide(const clap_host_t *) { return false; }
void hostClosed(const clap_host_t *, bool) {}
const clap_host_gui_t kHostGui = {hostResizeHintsChanged, hostRequestResize, hostRequestShow,
                                  hostRequestHide, hostClosed};

// The mirror's parameter changes are collected from it, not flushed.
void hostParamsRescan(const clap_host_t *, clap_param_rescan_flags) {}
void hostParamsClear(const clap_host_t *, clap_id, clap_param_clear_flags) {}
void hostParamsRequestFlush(const clap_host_t *) {}
const clap_host_params_t kHostParams = {hostParamsRescan, hostParamsClear, hostParamsRequestFlush};

const void *hostGetExtension(const clap_host_t *, const char *id) {
   if (std::strcmp(id, CLAP_EXT_GUI) == 0)
      return &kHostGui;
   if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
      return &kHostParams;
   return nullptr;
}
void hostNoop(const clap_host_t *) {}

const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "RumpelKiste editor", "Ravetracer", "",
                           kPluginVersion,    hostGetExtension, hostNoop, hostNoop, hostNoop};

// After every input event and every frame: what the window changed goes to
// the plugin. While a button is held the edit is still going on.
void collect() {
   if (ui && ui->mirror && ui->canvas)
      ui->mirror->webCollect(!ui->canvas->webButtonHeld());
}

// Input before the plugin's state has arrived would edit a fresh instance.
bool accepting() { return ui && ui->canvas && ui->mirror->webReady(); }

} // namespace

} // namespace rumpelkiste

using rumpelkiste::ui;
namespace sk = rumpelkiste;

extern "C" {

// A buffer the page writes into before a call (messages, key text, paths).
__attribute__((export_name("wclap_ui_buffer"))) uint8_t *wclapUiBuffer(int size) {
   sk::scratch.resize(static_cast<size_t>(std::max(size, 1)));
   return sk::scratch.data();
}

__attribute__((export_name("wclap_ui_start"))) void wclapUiStart(int cssW, int cssH, double dpr) {
   if (ui)
      return;
   ui = new sk::Ui;
   // The page's file system starts with nothing but the fonts and /home/web;
   // a pattern dragged out as MIDI is written under /tmp first.
   mkdir("/tmp", 0700);
   clap_entry.init("");
   const auto *factory =
      static_cast<const clap_plugin_factory_t *>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
   ui->plugin = factory ? factory->create_plugin(factory, &sk::kHost, sk::kPluginId) : nullptr;
   if (!ui->plugin || !ui->plugin->init(ui->plugin))
      return;
   ui->mirror = const_cast<sk::WebMirror *>(
      static_cast<const sk::WebMirror *>(ui->plugin->get_extension(ui->plugin, sk::kWebMirrorExtension)));
   if (!ui->mirror)
      return;
   ui->mirror->webSetSender(&sk::sendToPlugin);
   ui->canvas = ui->mirror->webOpenEditor();
   if (!ui->canvas)
      return;
   ui->canvas->webSetPage(cssW, cssH, dpr);
   ui->mirror->webPoll();
   ui->lastPoll = sk::nowSeconds();
}

__attribute__((export_name("wclap_ui_resize"))) void wclapUiResize(int cssW, int cssH, double dpr) {
   if (ui && ui->canvas)
      ui->canvas->webSetPage(cssW, cssH, dpr);
}

// Once per animation frame. Returns the serial of the newest rendered frame.
__attribute__((export_name("wclap_ui_tick"))) uint32_t wclapUiTick() {
   if (!ui || !ui->canvas)
      return 0;
   const double now = sk::nowSeconds();
   if (now - ui->lastPoll > 0.033) {
      ui->lastPoll = now;
      ui->mirror->webPoll();
   }
   const uint32_t serial = ui->canvas->webTick();
   sk::collect();
   return serial;
}

__attribute__((export_name("wclap_ui_frame"))) const uint8_t *wclapUiFrame() {
   int w = 0, h = 0;
   return ui && ui->canvas ? ui->canvas->webFrame(w, h) : nullptr;
}
__attribute__((export_name("wclap_ui_frame_width"))) int wclapUiFrameWidth() {
   int w = 0, h = 0;
   if (ui && ui->canvas)
      ui->canvas->webFrame(w, h);
   return w;
}
__attribute__((export_name("wclap_ui_frame_height"))) int wclapUiFrameHeight() {
   int w = 0, h = 0;
   if (ui && ui->canvas)
      ui->canvas->webFrame(w, h);
   return h;
}

// Pointer input in canvas pixels; buttons 1 left, 2 middle, 3 right. A double
// click is the window's own business: it times the clicks itself.
__attribute__((export_name("wclap_ui_pointer_down"))) void
wclapUiPointerDown(float x, float y, int button, unsigned mods, int) {
   if (!sk::accepting())
      return;
   ui->canvas->webPointerDown(x, y, button, mods);
   sk::collect();
}
__attribute__((export_name("wclap_ui_pointer_up"))) void wclapUiPointerUp(float x, float y, int button,
                                                                          unsigned mods) {
   if (!sk::accepting())
      return;
   ui->canvas->webPointerUp(x, y, button, mods);
   sk::collect();
}
__attribute__((export_name("wclap_ui_pointer_move"))) void wclapUiPointerMove(float x, float y,
                                                                              unsigned mods) {
   if (!sk::accepting())
      return;
   ui->canvas->webPointerMove(x, y, mods);
   sk::collect();
}
__attribute__((export_name("wclap_ui_wheel"))) void wclapUiWheel(float x, float y, float notches,
                                                                 unsigned mods) {
   if (!sk::accepting())
      return;
   ui->canvas->webWheel(x, y, notches, mods);
   sk::collect();
}
__attribute__((export_name("wclap_ui_leave"))) void wclapUiLeave() {
   if (sk::accepting())
      ui->canvas->webLeave();
}

// The key's text (UTF-8) is in the buffer, `textLen` bytes. Returns 1 when used.
__attribute__((export_name("wclap_ui_key"))) int wclapUiKey(unsigned keysym, int textLen,
                                                           unsigned mods) {
   if (!sk::accepting())
      return 0;
   const int n = std::clamp(textLen, 0, static_cast<int>(sk::scratch.size()));
   const std::string text(reinterpret_cast<const char *>(sk::scratch.data()), static_cast<size_t>(n));
   const bool used = ui->canvas->webKey(keysym, text.c_str(), static_cast<int>(text.size()), mods);
   sk::collect();
   return used ? 1 : 0;
}

// A message from the plugin, `size` bytes in the buffer.
__attribute__((export_name("wclap_ui_message"))) void wclapUiMessage(int size) {
   if (!ui || !ui->mirror || size <= 0 || static_cast<size_t>(size) > sk::scratch.size())
      return;
   // The buffer is reused by the next call into the module; take a copy.
   const std::vector<uint8_t> message(sk::scratch.begin(), sk::scratch.begin() + size);
   ui->mirror->webReceive(message.data(), message.size());
}

} // extern "C"
