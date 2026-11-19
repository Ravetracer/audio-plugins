#pragma once

#include <string>

#if defined(_WIN32)
#include <atomic>
#include <thread>
#elif defined(__wasm__)
#elif defined(__APPLE__)
#include <memory>
#else
#include <sys/types.h>
#endif

namespace substrike::gui {

#if defined(__APPLE__)
struct MacPanelState;
#endif

// Native file chooser that never blocks the host's GUI thread. On Linux it is
// zenity or kdialog run as a child process; on Windows the system's own
// dialog, run on a worker thread; on macOS an open or save panel run modeless
// on the main thread; in the browser the page's file picker, whose files are
// copied into the editor's file system. Poll from the GUI timer.
class FileDialog
{
public:
    enum class Mode { OpenFile, OpenFolder, SaveFile };

    ~FileDialog();
    // filter example: "Audio files | *.wav *.aif *.aiff"
    bool start(Mode mode, const std::string& title, const std::string& filter = {}, const std::string& start = {});
    bool running() const;
    // Returns true once the dialog finished; `result` is empty when cancelled.
    bool poll(std::string& result);
    static bool available();

private:
#if defined(_WIN32)
    std::thread thread_;
    std::atomic<bool> done_{false};
    std::atomic<unsigned long> threadId_{0};
#elif defined(__wasm__)
    int id_ = 0;
#elif defined(__APPLE__)
    // Shared with the panel's completion handler, which may outlive this.
    std::shared_ptr<MacPanelState> mac_;
#else
    pid_t pid_ = -1;
    int fd_ = -1;
#endif
    std::string output_;
};

} // namespace substrike::gui
