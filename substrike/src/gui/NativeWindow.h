#pragma once

// The window-system backend for this platform. All backends have the same
// interface; the editor only ever names NativeWindow.
#if defined(_WIN32)
#include "Win32Window.h"
namespace substrike::gui {
using NativeWindow = Win32Window;
}
#elif defined(__wasm__)
#include "WebWindow.h"
namespace substrike::gui {
using NativeWindow = WebWindow;
}
#elif defined(__APPLE__)
#include "CocoaWindow.h"
namespace substrike::gui {
using NativeWindow = CocoaWindow;
}
#else
#include "X11Window.h"
namespace substrike::gui {
using NativeWindow = X11Window;
}
#endif
