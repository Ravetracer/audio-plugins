#include "FileDialog.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

extern char** environ;

namespace aurum::gui {

namespace {

bool inPath(const char* exe)
{
    const char* path = std::getenv("PATH");
    if (!path)
        return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':'))
        if (access((dir + "/" + exe).c_str(), X_OK) == 0)
            return true;
    return false;
}

} // namespace

FileDialog::~FileDialog()
{
    if (pid_ > 0)
    {
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
    }
    if (fd_ >= 0)
        close(fd_);
}

bool FileDialog::available() { return inPath("zenity") || inPath("kdialog"); }

bool FileDialog::start(Mode mode, const std::string& title, const std::string& filter, const std::string& startDir)
{
    if (pid_ > 0)
        return false;
    std::vector<std::string> args;
    if (inPath("zenity"))
    {
        args = {"zenity", "--file-selection", "--title=" + title};
        if (mode == Mode::OpenFolder)
            args.push_back("--directory");
        if (mode == Mode::SaveFile)
        {
            args.push_back("--save");
            args.push_back("--confirm-overwrite");
        }
        if (!filter.empty())
            args.push_back("--file-filter=" + filter);
        if (!startDir.empty())
            args.push_back("--filename=" + startDir + "/");
    }
    else if (inPath("kdialog"))
    {
        const char* op = mode == Mode::OpenFolder ? "--getexistingdirectory"
                                                  : (mode == Mode::SaveFile ? "--getsavefilename" : "--getopenfilename");
        args = {"kdialog", "--title", title, op, startDir.empty() ? "." : startDir};
        if (!filter.empty() && mode != Mode::OpenFolder)
        {
            // kdialog wants "*.wav *.aif|Description"
            const size_t bar = filter.find('|');
            const std::string desc = bar == std::string::npos ? filter : filter.substr(0, bar);
            const std::string pats = bar == std::string::npos ? "*" : filter.substr(bar + 1);
            args.push_back(pats + "|" + desc);
        }
    }
    else
        return false;

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    const int rc = posix_spawnp(&pid_, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0)
    {
        close(pipefd[0]);
        pid_ = -1;
        return false;
    }
    fd_ = pipefd[0];
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    output_.clear();
    return true;
}

bool FileDialog::poll(std::string& result)
{
    if (pid_ <= 0)
        return false;
    char buf[1024];
    ssize_t n;
    while ((n = read(fd_, buf, sizeof(buf))) > 0)
        output_.append(buf, static_cast<size_t>(n));
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) != pid_)
        return false;
    while ((n = read(fd_, buf, sizeof(buf))) > 0)
        output_.append(buf, static_cast<size_t>(n));
    close(fd_);
    fd_ = -1;
    pid_ = -1;
    while (!output_.empty() && (output_.back() == '\n' || output_.back() == '\r'))
        output_.pop_back();
    result = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? output_ : std::string();
    return true;
}

} // namespace aurum::gui
