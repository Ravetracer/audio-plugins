#pragma once

// Dragging a file out of the plugin window and into the host.
//
// The window writes a pattern to a .mid and offers that file to whatever is
// under the pointer, which is how a DAW's arranger takes MIDI from anything
// else: as a file drop. There is no CLAP extension for this and there does not
// need to be one -- it is the desktop's own drag protocol, and both of them are
// implemented here rather than in the window, because neither has anything to
// do with how the window draws.
//
// Both implementations run a nested event loop and return when the drag is
// over, which is what the platforms offer: Windows' DoDragDrop is modal by
// construction, and the X11 source side has to answer the target's messages
// while the button is down. The window is therefore not painting during a
// drag, which lasts as long as the user holds the button.

#include <cstdint>
#include <string>

namespace saeurekiste {

// `display` and `window` are the X11 Display* and Window on Linux, and nullptr
// and the HWND on Windows. Returns true if a target took the file.
bool dragFileOut(void *display, uintptr_t window, const std::string &path);

} // namespace saeurekiste
