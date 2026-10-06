#include "FileDialog.h"

#if defined(_WIN32)

#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>

#include <cwchar>
#include <vector>

namespace aurum::gui {

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const wchar_t* s)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// "Audio files | *.wav *.aif" -> "Audio files\0*.wav;*.aif\0All files\0*.*\0\0",
// the double-terminated list the common dialog wants.
std::wstring filterBlock(const std::string& filter)
{
    std::wstring block;
    if (!filter.empty())
    {
        const size_t bar = filter.find('|');
        std::string desc = bar == std::string::npos ? filter : filter.substr(0, bar);
        std::string pats = bar == std::string::npos ? "*.*" : filter.substr(bar + 1);
        auto trim = [](std::string& t) {
            const size_t a = t.find_first_not_of(' ');
            t = a == std::string::npos ? std::string() : t.substr(a, t.find_last_not_of(' ') - a + 1);
        };
        trim(desc);
        trim(pats);
        for (char& c : pats)
            if (c == ' ')
                c = ';';
        block += widen(desc);
        block.push_back(L'\0');
        block += widen(pats);
        block.push_back(L'\0');
    }
    block += L"All files";
    block.push_back(L'\0');
    block += L"*.*";
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

std::string runDialog(FileDialog::Mode mode, const std::wstring& title, const std::wstring& filter,
                      const std::wstring& startDir)
{
    if (mode == FileDialog::Mode::OpenFolder)
    {
        BROWSEINFOW bi{};
        bi.lpszTitle = title.c_str();
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
        if (!id)
            return {};
        wchar_t path[MAX_PATH] = {};
        const bool ok = SHGetPathFromIDListW(id, path) != FALSE;
        CoTaskMemFree(id);
        return ok ? narrow(path) : std::string();
    }
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrInitialDir = startDir.empty() ? nullptr : startDir.c_str();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (mode == FileDialog::Mode::SaveFile)
    {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
        return GetSaveFileNameW(&ofn) ? narrow(file.data()) : std::string();
    }
    ofn.Flags |= OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&ofn) ? narrow(file.data()) : std::string();
}

BOOL CALLBACK closeDialog(HWND hwnd, LPARAM)
{
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

} // namespace

FileDialog::~FileDialog()
{
    if (!thread_.joinable())
        return;
    // The dialog belongs to the worker thread. Closing it is the same as
    // pressing Cancel, after which the thread ends and can be joined; it must
    // not be detached, because its code lives in this module and the host may
    // unload the module once the editor is gone.
    while (!done_.load())
    {
        if (const DWORD id = threadId_.load())
            EnumThreadWindows(id, closeDialog, 0);
        Sleep(10);
    }
    thread_.join();
}

bool FileDialog::available() { return true; }

bool FileDialog::running() const { return thread_.joinable() && !done_.load(); }

bool FileDialog::start(Mode mode, const std::string& title, const std::string& filter, const std::string& startDir)
{
    if (thread_.joinable())
        return false;
    done_ = false;
    output_.clear();
    thread_ = std::thread([this, mode, t = widen(title), f = filterBlock(filter), d = widen(startDir)] {
        threadId_ = GetCurrentThreadId();
        // A thread of our own needs its own COM apartment for the shell
        // dialogs; it is not the host's to share.
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        output_ = runDialog(mode, t, f, d);
        if (SUCCEEDED(hr))
            CoUninitialize();
        done_ = true;
    });
    return true;
}

bool FileDialog::poll(std::string& result)
{
    if (!thread_.joinable() || !done_.load())
        return false;
    thread_.join();
    result = output_;
    return true;
}

} // namespace aurum::gui

#else

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

bool FileDialog::running() const { return pid_ > 0; }

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

#endif
