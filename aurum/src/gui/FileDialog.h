#pragma once

#include <string>
#include <sys/types.h>

namespace aurum::gui {

// Native file chooser via zenity or kdialog, run as a child process so the
// host's GUI thread is never blocked. Poll from the GUI timer.
class FileDialog
{
public:
    enum class Mode { OpenFile, OpenFolder, SaveFile };

    ~FileDialog();
    // filter example: "Audio files | *.wav *.aif *.aiff"
    bool start(Mode mode, const std::string& title, const std::string& filter = {}, const std::string& start = {});
    bool running() const { return pid_ > 0; }
    // Returns true once the dialog finished; `result` is empty when cancelled.
    bool poll(std::string& result);
    static bool available();

private:
    pid_t pid_ = -1;
    int fd_ = -1;
    std::string output_;
};

} // namespace aurum::gui
