#pragma once

// The desktop's own file chooser, for the two places this plugin needs one:
// writing a preset pack somewhere other than its own packs folder, reading one
// from wherever it was downloaded to, and picking the folder of foreign pattern
// files to import.
//
// There is no toolkit here -- the window is raw X11 and Cairo, which is what
// keeps the plugin a single file with no runtime dependencies -- so there is no
// file chooser to call. On Windows there is one in the operating system. On
// Linux there is whatever the desktop installed, so this asks zenity or
// kdialog and reports honestly when it finds neither: the browser then falls
// back to its own list of packs, which is where everything it writes goes and
// is enough on its own.

#include <string>

#if defined(__wasm__)
#include <functional>
#endif

namespace saeurekiste {

// Whether there is a chooser to open at all. The window hides its "Other..."
// entries when there is not, rather than offering a button that does nothing.
bool fileDialogAvailable();

// Both return the chosen path, or an empty string if the user cancelled or
// there was no chooser. Both block until the dialog closes, which on X11 means
// the plugin window stops repainting for as long as it is open -- the same deal
// a drag makes, and what every host-side file dialog does too.
std::string openFileDialog(const std::string &title, const std::string &filterName,
                           const std::string &extension);
std::string saveFileDialog(const std::string &title, const std::string &suggestedPath,
                           const std::string &filterName, const std::string &extension);

// A directory rather than a file, for importing a whole folder of patterns at
// once. Same deal: the chosen path, or empty if there was no chooser or the
// user cancelled.
std::string openFolderDialog(const std::string &title);

// Shows a folder in the desktop's file manager, without waiting for it. For
// the wavetable folder, so a user can see where the files go and drop some in.
void revealFolder(const std::string &path);

#if defined(__wasm__)
// The WebAssembly editor's dialogs are the page's, and they do not block: the
// result arrives later and is handed to `done` -- an empty path when the user
// cancelled. A save dialog answers with a path under /download; the caller
// writes the file there and hands it to the user with webOfferDownload().
void openFileDialogAsync(const std::string &title, const std::string &filterName,
                         const std::string &extension,
                         std::function<void(const std::string &)> done);
void saveFileDialogAsync(const std::string &title, const std::string &suggestedPath,
                         const std::string &filterName, const std::string &extension,
                         std::function<void(const std::string &)> done);
void openFolderDialogAsync(const std::string &title,
                           std::function<void(const std::string &)> done);
// Forgets a dialog that is still open, for a window that is going away.
void cancelFileDialogs();
// Offers a file in the editor's file system to the user as a download.
bool webOfferDownload(const std::string &path);
#endif

} // namespace saeurekiste
