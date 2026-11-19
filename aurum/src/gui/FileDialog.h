#pragma once

#include <string>

#if defined(_WIN32)
#include <atomic>
#include <thread>
#elif defined(__wasm__)
#else
#include <sys/types.h>
#endif

namespace aurum::gui {

// Native file chooser that never blocks the host's GUI thread. On Linux it is
// zenity or kdialog run as a child process; on Windows the system's own
// dialog, run on a worker thread; in the browser the page's file picker, whose
// files are copied into the editor's file system. Poll from the GUI timer.
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
#else
    pid_t pid_ = -1;
    int fd_ = -1;
#endif
    std::string output_;
};

} // namespace aurum::gui
