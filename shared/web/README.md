# WCLAP (WebCLAP) builds

A plugin here can be built a second time for WebAssembly as a **WCLAP**, the
CLAP format for browser hosts (openDAW first). Aurum was the first; its
`cmake/wclap.cmake` is the reference.

A WCLAP bundle is two modules and a page:

```
<name>.wclap/
    module.wasm    the plugin: DSP, parameters, state, clap.gui (webview API
                   only) and clap.webview/3 with the page /index.html
    ui.wasm        the plugin's own Cairo editor, compiled for the page
    index.html     calls start() from wclap-ui.js
    wclap-ui.js    this folder: canvas, input, messages (shared)
    wasi.js        this folder: an in-memory WASI file system (shared)
    fonts/         the editor's fonts and their licence
    LICENSE
```

packed by `shared/tools/pack-wclap.py` into `<name>.wclap.tar.gz`, which is
what openDAW imports.

**Why two modules.** openDAW runs a WCLAP's module.wasm, the page's messages
included, on its audio worklet thread. The editor cannot draw there. So the
editor runs in the page as ui.wasm and talks to the plugin only through the
webview messages, with a stand-in for the plugin (Aurum's `RemoteController`)
that mirrors parameter values and sends every edit. Each plugin defines its own
messages (`aurum/src/plugin/WebProtocol.h`): the page polls, the plugin
answers, never unasked.

## The editor's contract (ui.wasm)

A reactor module (`-mexec-model=reactor`) importing WASI preview 1 and:

| import | |
|---|---|
| `wclap.send(ptr, len)` | bytes to the plugin |
| `wclap.file_dialog(id, mode, filterPtr, filterLen)` | 0 open file, 1 open folder, 2 save; answered with `wclap_ui_dialog_done` |
| `wclap.download(namePtr, nameLen, dataPtr, dataLen)` | hands the user a file |

and exporting:

| export | |
|---|---|
| `wclap_ui_buffer(size) -> ptr` | scratch the page writes into before a call |
| `wclap_ui_start(cssW, cssH, dpr)` / `wclap_ui_resize(...)` | |
| `wclap_ui_tick() -> serial` | once per animation frame; the serial of the newest frame |
| `wclap_ui_frame() -> ptr`, `_frame_width()`, `_frame_height()` | RGBA |
| `wclap_ui_pointer_down(x, y, button, mods, clicks)`, `_up(x, y, button, mods)`, `_move(x, y, mods)` | canvas pixels |
| `wclap_ui_wheel(x, y, notches, mods)`, `wclap_ui_leave()` | |
| `wclap_ui_key(keysym, textLen, mods) -> used` | X11 keysyms, text in the buffer |
| `wclap_ui_key_up(keysym, mods) -> used` | optional |
| `wclap_ui_message(len)` | a message from the plugin, in the buffer |
| `wclap_ui_dialog_done(id, ptr, len)` | a picked path, empty when cancelled |
| `wclap_ui_files_dropped(len, x, y)` | optional; paths one per line, in the buffer |

Modifiers: 1 shift, 2 ctrl/cmd, 4 alt. Picked and dropped files are copied
into `/upload/...`; fonts listed in `start()` are at `/fonts/<name>`; `HOME` is
`/home/web`, which is kept in localStorage when the browser allows it.

## Building

- wasi-sdk with `wasi-sdk-p1.cmake` (no threads). Anything that starts a
  thread needs a synchronous path in a WASI build.
- Cairo, pixman and FreeType for the editor: `shared/cmake/build-wasm-cairo.sh
  <wasi-sdk> wasmbuild/cairo-wasi`. The editor needs `-mexception-handling
  -mllvm -wasm-enable-sjlj` (FreeType's setjmp), `-lsetjmp
  -lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-process-clocks`
  and a 1 MB stack (`-Wl,-z,stack-size=1048576`): wasm's default 64 KB
  overflows silently into libc's data.
- module.wasm: `-mexec-model=reactor
  -Wl,--max-memory=2147483648,--export-table,--growable-table,--export=malloc,--export=clap_entry`,
  `-fno-exceptions`, no setjmp. Check its imports are WASI only.
- `shared/cmake/wclap.cmake` has helpers for all of this.

## Testing

- `node shared/tools/run-wasi.mjs <selftest>.wasm`: a plugin self-test built
  as a WASI command.
- `node shared/tools/wclap-ui-snapshot.mjs <bundle dir> out.png`: the editor
  offscreen, one frame as PNG.
- In a browser **only headless on a private Xvfb**, never on the user's
  desktop: a browser window there takes their focus and their keystrokes.
