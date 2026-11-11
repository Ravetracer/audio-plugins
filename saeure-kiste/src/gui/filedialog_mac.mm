// filedialog.h on macOS: the system's own open and save panels. Compiled
// without ARC.
//
// Run modally, which is what the interface promises on every platform: the
// call blocks until the panel closes. The window is not painted meanwhile,
// the same deal as on the other two.

#include "gui/filedialog.h"

#import <Cocoa/Cocoa.h>

namespace saeurekiste {

namespace {

NSString *ns(const std::string &s) {
   NSString *out = [NSString stringWithUTF8String:s.c_str()];
   return out ? out : @"";
}

std::string pathOf(NSURL *url) {
   if (!url)
      return std::string();
   const char *p = [url fileSystemRepresentation];
   return p ? std::string(p) : std::string();
}

// allowedFileTypes is deprecated from macOS 12 in favour of UTType, which
// needs 11; this build runs on 10.15.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
void allowExtension(NSSavePanel *panel, const std::string &extension) {
   if (!extension.empty())
      [panel setAllowedFileTypes:@[ ns(extension) ]];
}
#pragma clang diagnostic pop

} // namespace

bool fileDialogAvailable() { return true; }

std::string openFileDialog(const std::string &title, const std::string &,
                           const std::string &extension) {
   @autoreleasepool {
      NSOpenPanel *panel = [NSOpenPanel openPanel];
      [panel setMessage:ns(title)];
      [panel setCanChooseFiles:YES];
      [panel setCanChooseDirectories:NO];
      [panel setAllowsMultipleSelection:NO];
      allowExtension(panel, extension);
      if ([panel runModal] != NSModalResponseOK)
         return std::string();
      return pathOf([[panel URLs] firstObject]);
   }
}

std::string saveFileDialog(const std::string &title, const std::string &suggestedPath,
                           const std::string &, const std::string &extension) {
   @autoreleasepool {
      NSSavePanel *panel = [NSSavePanel savePanel];
      [panel setMessage:ns(title)];
      [panel setCanCreateDirectories:YES];
      allowExtension(panel, extension);
      if (!suggestedPath.empty()) {
         NSString *suggested = ns(suggestedPath);
         NSString *dir = [suggested stringByDeletingLastPathComponent];
         if ([dir length] > 0)
            [panel setDirectoryURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
         [panel setNameFieldStringValue:[suggested lastPathComponent]];
      }
      if ([panel runModal] != NSModalResponseOK)
         return std::string();
      return pathOf([panel URL]);
   }
}

std::string openFolderDialog(const std::string &title) {
   @autoreleasepool {
      NSOpenPanel *panel = [NSOpenPanel openPanel];
      [panel setMessage:ns(title)];
      [panel setCanChooseFiles:NO];
      [panel setCanChooseDirectories:YES];
      [panel setCanCreateDirectories:NO];
      [panel setAllowsMultipleSelection:NO];
      if ([panel runModal] != NSModalResponseOK)
         return std::string();
      return pathOf([[panel URLs] firstObject]);
   }
}

void revealFolder(const std::string &path) {
   if (path.empty())
      return;
   @autoreleasepool {
      [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:ns(path) isDirectory:YES]];
   }
}

} // namespace saeurekiste
