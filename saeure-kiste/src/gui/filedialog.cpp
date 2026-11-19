#include "gui/filedialog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__wasm__)
#   include <vector>
#elif defined(_WIN32)
#   include <windows.h>
#   include <commdlg.h>
#   include <shlobj.h>
#   include <shellapi.h>
#else
#   include <unistd.h>
#endif

namespace saeurekiste {

#if defined(__wasm__)

// The page's file picker (wclap.file_dialog in shared/web/wclap-ui.js). It
// copies what the user picks into the editor's file system under /upload and
// answers through wclap_ui_dialog_done(), so a dialog here is never modal: the
// window hands over what to do with the path, and that runs when it comes.
// A save dialog asks for a name and answers with a path under /download; the
// window writes the file there and offers it with webOfferDownload().

extern "C" {
__attribute__((import_module("wclap"), import_name("file_dialog"))) void
wclapJsFileDialog(int id, int mode, const char *filter, int filterLength);
__attribute__((import_module("wclap"), import_name("download"))) void
wclapJsDownload(const char *name, int nameLength, const void *data, int dataLength);
}

namespace {

struct PendingDialog {
   int id = 0;
   std::function<void(const std::string &)> done;
};

// One dialog at a time, for the one editor this module holds.
PendingDialog &pending() {
   static PendingDialog p;
   return p;
}

void startDialog(int mode, const std::string &filter, std::function<void(const std::string &)> done) {
   static int next = 1;
   PendingDialog &p = pending();
   p.id = next++;
   p.done = std::move(done);
   wclapJsFileDialog(p.id, mode, filter.data(), static_cast<int>(filter.size()));
}

} // namespace

extern "C" __attribute__((export_name("wclap_ui_dialog_done"))) void
wclapUiDialogDone(int id, const char *path, int length) {
   PendingDialog &p = pending();
   if (id != p.id || !p.done)
      return;
   auto done = std::move(p.done);
   p = PendingDialog();
   done(std::string(path ? path : "", path && length > 0 ? static_cast<size_t>(length) : 0));
}

bool fileDialogAvailable() { return true; }

void openFileDialogAsync(const std::string &, const std::string &filterName,
                         const std::string &extension,
                         std::function<void(const std::string &)> done) {
   startDialog(0, filterName + " | *." + extension, std::move(done));
}

void saveFileDialogAsync(const std::string &, const std::string &,
                         const std::string &filterName, const std::string &extension,
                         std::function<void(const std::string &)> done) {
   startDialog(2, filterName + " | *." + extension, std::move(done));
}

void openFolderDialogAsync(const std::string &, std::function<void(const std::string &)> done) {
   startDialog(1, std::string(), std::move(done));
}

void cancelFileDialogs() { pending() = PendingDialog(); }

bool webOfferDownload(const std::string &path) {
   FILE *in = std::fopen(path.c_str(), "rb");
   if (!in)
      return false;
   std::vector<unsigned char> data;
   std::vector<unsigned char> buf(65536);
   size_t n;
   while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
      data.insert(data.end(), buf.data(), buf.data() + n);
   std::fclose(in);
   const size_t slash = path.find_last_of('/');
   const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
   wclapJsDownload(name.data(), static_cast<int>(name.size()), data.data(),
                   static_cast<int>(data.size()));
   return true;
}

// The blocking forms have nothing to block on in a page; the window uses the
// ones above instead.
std::string openFileDialog(const std::string &, const std::string &, const std::string &) {
   return std::string();
}
std::string saveFileDialog(const std::string &, const std::string &, const std::string &,
                           const std::string &) {
   return std::string();
}
std::string openFolderDialog(const std::string &) { return std::string(); }
void revealFolder(const std::string &) {}

#elif defined(_WIN32)

namespace {

// The common dialog wants a double-terminated list of "label\0pattern\0"
// pairs, which is not something std::string hands over conveniently.
std::wstring widen(const std::string &s) {
   if (s.empty())
      return std::wstring();
   const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
   std::wstring out(static_cast<size_t>(n), L'\0');
   MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
   return out;
}

std::string narrow(const wchar_t *s) {
   if (!s || !s[0])
      return std::string();
   const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
   std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
   if (n > 1)
      WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
   return out;
}

std::wstring filterBlock(const std::string &filterName, const std::string &extension) {
   const std::wstring label = widen(filterName + " (*." + extension + ")");
   const std::wstring pattern = widen("*." + extension);
   std::wstring block;
   block.append(label);
   block.push_back(L'\0');
   block.append(pattern);
   block.push_back(L'\0');
   block.append(widen("All files (*.*)"));
   block.push_back(L'\0');
   block.append(L"*.*");
   block.push_back(L'\0');
   block.push_back(L'\0');
   return block;
}

} // namespace

bool fileDialogAvailable() { return true; }

std::string openFileDialog(const std::string &title, const std::string &filterName,
                           const std::string &extension) {
   wchar_t file[MAX_PATH] = {0};
   const std::wstring filter = filterBlock(filterName, extension);
   const std::wstring wtitle = widen(title);
   OPENFILENAMEW ofn{};
   ofn.lStructSize = sizeof(ofn);
   ofn.lpstrFilter = filter.c_str();
   ofn.lpstrFile = file;
   ofn.nMaxFile = MAX_PATH;
   ofn.lpstrTitle = wtitle.c_str();
   ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
   return GetOpenFileNameW(&ofn) ? narrow(file) : std::string();
}

std::string saveFileDialog(const std::string &title, const std::string &suggestedPath,
                           const std::string &filterName, const std::string &extension) {
   wchar_t file[MAX_PATH] = {0};
   const std::wstring suggested = widen(suggestedPath);
   if (!suggested.empty() && suggested.size() < MAX_PATH)
      std::wmemcpy(file, suggested.c_str(), suggested.size() + 1);
   const std::wstring filter = filterBlock(filterName, extension);
   const std::wstring wtitle = widen(title);
   const std::wstring wext = widen(extension);
   OPENFILENAMEW ofn{};
   ofn.lStructSize = sizeof(ofn);
   ofn.lpstrFilter = filter.c_str();
   ofn.lpstrFile = file;
   ofn.nMaxFile = MAX_PATH;
   ofn.lpstrTitle = wtitle.c_str();
   ofn.lpstrDefExt = wext.c_str();
   ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
   return GetSaveFileNameW(&ofn) ? narrow(file) : std::string();
}

std::string openFolderDialog(const std::string &title) {
   // SHBrowseForFolder rather than IFileDialog: it needs no COM apartment of
   // its own, which matters in a plugin that does not own the host's thread.
   const std::wstring wtitle = widen(title);
   BROWSEINFOW bi{};
   bi.lpszTitle = wtitle.c_str();
   bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
   LPITEMIDLIST id = SHBrowseForFolderW(&bi);
   if (!id)
      return std::string();
   wchar_t path[MAX_PATH] = {0};
   const bool ok = SHGetPathFromIDListW(id, path) != FALSE;
   CoTaskMemFree(id);
   return ok ? narrow(path) : std::string();
}


void revealFolder(const std::string &path) {
   if (path.empty())
      return;
   ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

#else

namespace {

// Which chooser this desktop has, worked out once and remembered. Looked up in
// PATH by hand rather than by running a shell, because the answer is wanted
// before anything is run and running a shell to ask is the expensive way.
const char *chooser() {
   static const char *found = []() -> const char * {
      const char *path = std::getenv("PATH");
      if (!path)
         return nullptr;
      static const char *const kNames[] = {"zenity", "kdialog"};
      for (const char *name : kNames) {
         const char *start = path;
         while (*start) {
            const char *end = std::strchr(start, ':');
            const size_t len = end ? static_cast<size_t>(end - start) : std::strlen(start);
            if (len > 0) {
               std::string candidate(start, len);
               candidate += "/";
               candidate += name;
               if (access(candidate.c_str(), X_OK) == 0)
                  return name;
            }
            if (!end)
               break;
            start = end + 1;
         }
      }
      return nullptr;
   }();
   return found;
}

// One argument, quoted for /bin/sh. Everything that reaches here is a path or
// a title this plugin composed, but a preset folder is named by the user and
// ends up in the suggested filename, so it is quoted properly rather than
// hopefully.
std::string quote(const std::string &s) {
   std::string out = "'";
   for (const char c : s) {
      if (c == '\'')
         out += "'\\''";
      else
         out.push_back(c);
   }
   out += "'";
   return out;
}

std::string runChooser(const std::string &command) {
   FILE *pipe = popen(command.c_str(), "r");
   if (!pipe)
      return std::string();
   std::string out;
   char buf[512];
   while (std::fgets(buf, sizeof(buf), pipe))
      out += buf;
   pclose(pipe);
   while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
      out.pop_back();
   return out;
}

} // namespace

bool fileDialogAvailable() { return chooser() != nullptr; }

std::string openFileDialog(const std::string &title, const std::string &filterName,
                           const std::string &extension) {
   const char *tool = chooser();
   if (!tool)
      return std::string();
   if (std::strcmp(tool, "zenity") == 0)
      return runChooser("zenity --file-selection --title=" + quote(title) + " --file-filter=" +
                        quote(filterName + " | *." + extension) + " --file-filter=" +
                        quote("All files | *") + " 2>/dev/null");
   return runChooser("kdialog --title " + quote(title) + " --getopenfilename . " +
                     quote("*." + extension + "|" + filterName) + " 2>/dev/null");
}

std::string saveFileDialog(const std::string &title, const std::string &suggestedPath,
                           const std::string &filterName, const std::string &extension) {
   const char *tool = chooser();
   if (!tool)
      return std::string();
   if (std::strcmp(tool, "zenity") == 0)
      return runChooser("zenity --file-selection --save --confirm-overwrite --title=" +
                        quote(title) + " --filename=" + quote(suggestedPath) + " --file-filter=" +
                        quote(filterName + " | *." + extension) + " 2>/dev/null");
   return runChooser("kdialog --title " + quote(title) + " --getsavefilename " +
                     quote(suggestedPath) + " " + quote("*." + extension + "|" + filterName) +
                     " 2>/dev/null");
}

void revealFolder(const std::string &path) {
   if (path.empty())
      return;
   // xdg-open hands it to whatever file manager the desktop has, and the
   // trailing & keeps the editor from waiting for that to close.
   const std::string command = "xdg-open " + quote(path) + " >/dev/null 2>&1 &";
   const int ignored = std::system(command.c_str());
   (void)ignored;
}

std::string openFolderDialog(const std::string &title) {
   const char *tool = chooser();
   if (!tool)
      return std::string();
   if (std::strcmp(tool, "zenity") == 0)
      return runChooser("zenity --file-selection --directory --title=" + quote(title) +
                        " 2>/dev/null");
   return runChooser("kdialog --title " + quote(title) + " --getexistingdirectory . 2>/dev/null");
}

#endif

} // namespace saeurekiste
