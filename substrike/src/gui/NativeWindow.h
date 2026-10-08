#pragma once

// The window-system backend for this platform. Both backends have the same
// interface; the editor only ever names NativeWindow.
#if defined(_WIN32)
#include "Win32Window.h"
namespace substrike::gui {
using NativeWindow = Win32Window;
}
#else
#include "X11Window.h"
namespace substrike::gui {
using NativeWindow = X11Window;
}
#endif
