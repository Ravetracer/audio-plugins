// FileDialog on macOS: an NSOpenPanel or NSSavePanel shown modeless with a
// completion handler, so the host's run loop goes on while it is open and
// the editor polls for the answer from its timer as on the other platforms.
// Compiled without ARC.

#include "FileDialog.h"

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cctype>
#include <set>

namespace resident::gui {

struct MacPanelState
{
    NSSavePanel* panel = nil; // retained
    bool done = false;
    std::string result;
};

namespace {

// "C64 programs | *.prg *.PRG *.d64" -> prg, d64 (the panel matches
// extensions without regard to case).
NSArray* extensionsOf(const std::string& filter)
{
    const size_t bar = filter.find('|');
    if (bar == std::string::npos)
        return nil;
    std::set<std::string> seen;
    NSMutableArray* out = [NSMutableArray array];
    std::string pats = filter.substr(bar + 1);
    size_t pos = 0;
    while (pos < pats.size()) {
        const size_t end = std::min(pats.find(' ', pos), pats.size());
        std::string pat = pats.substr(pos, end - pos);
        pos = end + 1;
        if (pat.rfind("*.", 0) != 0 || pat.size() < 3)
            continue;
        std::string ext = pat.substr(2);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == "*" || !seen.insert(ext).second)
            continue;
        [out addObject:[NSString stringWithUTF8String:ext.c_str()]];
    }
    return [out count] ? out : nil;
}

void closePanel(MacPanelState& st)
{
    if (st.panel) {
        [st.panel release];
        st.panel = nil;
    }
}

} // namespace

FileDialog::~FileDialog()
{
    if (!mac_)
        return;
    if (!mac_->done && mac_->panel)
        [mac_->panel cancel:nil]; // the handler holds its own reference to the state
    closePanel(*mac_);
}

bool FileDialog::available() { return true; }

bool FileDialog::running() const { return mac_ && !mac_->done; }

bool FileDialog::start(Mode mode, const std::string& title, const std::string& filter, const std::string& startDir)
{
    if (running())
        return false;
    if (mac_)
        closePanel(*mac_);

    NSSavePanel* panel = nil;
    if (mode == Mode::SaveFile) {
        panel = [NSSavePanel savePanel];
        [panel setCanCreateDirectories:YES];
    } else {
        NSOpenPanel* open = [NSOpenPanel openPanel];
        [open setCanChooseFiles:mode == Mode::OpenFile];
        [open setCanChooseDirectories:mode == Mode::OpenFolder];
        [open setAllowsMultipleSelection:NO];
        panel = open;
    }
    if (!panel)
        return false;
    NSString* text = [NSString stringWithUTF8String:title.c_str()];
    [panel setTitle:text];
    [panel setMessage:text]; // an open panel shows no title since macOS 11
    if (mode != Mode::OpenFolder) {
        if (NSArray* exts = extensionsOf(filter)) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            // allowedContentTypes needs macOS 11; this works on every version.
            [panel setAllowedFileTypes:exts];
#pragma clang diagnostic pop
        }
    }
    if (!startDir.empty())
        [panel setDirectoryURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:startDir.c_str()]
                                          isDirectory:YES]];

    auto st = std::make_shared<MacPanelState>();
    st->panel = [panel retain];
    mac_ = st;
    [panel beginWithCompletionHandler:^(NSModalResponse response) {
      if (response == NSModalResponseOK)
          if (NSURL* url = [st->panel URL])
              if (const char* path = [url fileSystemRepresentation])
                  st->result = path;
      st->done = true;
    }];
    return true;
}

bool FileDialog::poll(std::string& result)
{
    if (!mac_ || !mac_->done)
        return false;
    result = mac_->result;
    closePanel(*mac_);
    mac_.reset();
    return true;
}

} // namespace resident::gui
