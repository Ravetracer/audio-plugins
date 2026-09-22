// SaeureKiste's window. A verbatim fork of shared/src/gui/window.cpp with the
// namespace changed and one thing added: the step grid, at the end of the file
// and hooked into buildLayout(), paint() and onPointerDown(). Everything else
// is the shared window, and diffing the two shows exactly that.
//
// The original comment follows.
//
// The shared plugin window.
//
// Cairo for everything drawn inside it, and the platform's own windowing under
// that: X11 on Linux, Win32 on Windows. No toolkit, so the plugin stays one
// .clap file and pulls in nothing a machine that can run a DAW does not already
// have. Everything between the window and the drawing -- the layout, the hit
// testing, the overlays -- is shared, and the two platforms differ only in how
// a window is made, how events arrive and where the finished frame is blitted.
//
// The whole layout is generated from the parameter table in params.cpp: the
// panels are the modules, the cells are the parameters, and the help line is
// the parameter's own tip. Adding a parameter there puts it on screen here.

#include "seqwindow.h"

#include "dragfile.h"
#include "filedialog.h"

#include "params.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <cairo/cairo.h>

#if defined(_WIN32)
#   include <cairo-win32.h>
#   include <windows.h>
#   include <windowsx.h> // GET_X_LPARAM
// MinGW's <cmath> hides M_PI unless this is asked for, and the knob arcs need it.
#   ifndef M_PI
#      define M_PI 3.14159265358979323846
#   endif
#else
#   include <X11/Xlib.h>
#   include <X11/Xutil.h>
#   include <X11/keysym.h>
#   include <cairo/cairo-xlib.h>
#endif

namespace saeurekiste {

using namespace plugincore;

namespace {

// --------------------------------------------------------------- the window

class PluginWindow final : public Gui {
public:
   PluginWindow(GuiDelegate &delegate, const WindowSpec &spec)
      : mDelegate(delegate), mSpec(spec),
        mWindowW(spec.contentW + 2 * kMargin) {
      mShown.assign(mSpec.paramCount, -1.0e9);
      // The section remembers whether it was open, so reopening the editor or
      // reloading the project finds the window the way it was left.
      if (mSpec.host)
         mAdvancedOpen = mSpec.host->windowAdvancedOpen();
      const size_t strips = mSpec.mixer ? static_cast<size_t>(mSpec.mixerCount) : 0;
      mMuted.assign(strips, 0);
      mSoloed.assign(strips, 0);
      mHeld.assign(strips, 0);
      mHeldLevel.assign(strips, 0.0);
      buildLayout();
   }

   ~PluginWindow() override {
      closeWindow();
      // The ornament animates per window, so a plugin with more than one editor
      // open hands each one its own and asks the window to keep it.
      if (mSpec.ownsOrnament)
         delete mSpec.ornament;
   }

#if defined(_WIN32)
   static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

   // The module this code is linked into -- the plugin's own .clap or .vst3,
   // never the host .exe.
   //
   // A window class is keyed on (HINSTANCE, name), and registering one under
   // GetModuleHandle(nullptr) keys it on the host instead, so every binary
   // built on this window competes for one name. The second one to register
   // loses silently with ERROR_CLASS_ALREADY_EXISTS and then creates its
   // windows against the first one's class -- which means the first binary's
   // wndProc, and therefore the first binary's statically linked Cairo, driving
   // surfaces the second binary's Cairo allocated. Two copies of Cairo with
   // separate global state handing each other objects corrupts both of them.
   // X11 has no class registry, so this can only ever bite on Windows.
   //
   // This is the shared window's fix (Verdalis issue #1), ported by hand,
   // because this file is a fork and nothing ports a fix into a fork by itself.
   // The copy predates the fix and carried the bug until the first Windows
   // build was attempted.
   static HINSTANCE moduleInstance() {
      HMODULE mod = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&PluginWindow::wndProc), &mod);
      return reinterpret_cast<HINSTANCE>(mod);
   }

   // Per-module too, for the same reason and so that a stale class left by an
   // unloaded build cannot be picked up by a new one.
   static const wchar_t *windowClassName() {
      static wchar_t name[64] = {};
      if (!name[0])
         swprintf(name, 64, L"SaeureKisteWindow_%p", static_cast<void *>(moduleInstance()));
      return name;
   }

   // Registered on the first window this module opens and dropped with the
   // last, so the class never outlives the code its wndProc points into: a host
   // that unloads the plugin would otherwise leave a dangling procedure behind
   // under a name a later load would find.
   static int &windowCount() {
      static int count = 0;
      return count;
   }

   bool open() override {
      if (mWindow)
         return true;
      if (windowCount() == 0) {
         WNDCLASSEXW wc{};
         wc.cbSize = sizeof(wc);
         wc.style = CS_OWNDC;
         wc.lpfnWndProc = &PluginWindow::wndProc;
         wc.hInstance = moduleInstance();
         wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
         wc.hbrBackground = nullptr; // every pixel is painted, so never erase
         wc.lpszClassName = windowClassName();
         if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
      }
      // Created as an unowned popup, not as a child: WS_CHILD demands a parent
      // at creation and CLAP does not supply one until set_parent. embed()
      // turns it into a child once the host says where it goes.
      mWindow = CreateWindowExW(0, windowClassName(), L"SaeureKiste", WS_POPUP | WS_CLIPCHILDREN,
                                0, 0, static_cast<int>(pixelW()), static_cast<int>(pixelH()),
                                nullptr, nullptr, moduleInstance(), this);
      if (!mWindow) {
         if (windowCount() == 0)
            UnregisterClassW(windowClassName(), moduleInstance());
         return false;
      }
      ++windowCount();
      // No target surface is made here. A cached DC belongs to the window as it
      // was when the DC was taken, and this window is reparented into the
      // host's afterwards; the surface to draw on is the one BeginPaint hands
      // over, which is correct by construction.
      allocateBuffer();
      return true;
   }

   bool embed(uintptr_t parentWindow) override {
      if (!mWindow)
         return false;
      SetParent(mWindow, reinterpret_cast<HWND>(parentWindow));
      SetWindowLongPtrW(mWindow, GWL_STYLE, WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE);
      SetWindowPos(mWindow, nullptr, 0, 0, static_cast<int>(pixelW()),
                   static_cast<int>(pixelH()), SWP_NOZORDER | SWP_FRAMECHANGED);
      return true;
   }

   bool setTransientFor(uintptr_t) override {
      // Windows has no separate transient hint for an embedded child.
      return mWindow != nullptr;
   }

   void setTitle(const char *title) override {
      if (mWindow && title)
         SetWindowTextA(mWindow, title);
   }

#else
   bool open() override {
      if (mWindow)
         return true;
      XInitThreads();
      mDisplay = XOpenDisplay(nullptr);
      if (!mDisplay)
         return false;

      const int screen = DefaultScreen(mDisplay);
      mVisual = DefaultVisual(mDisplay, screen);

      XSetWindowAttributes attrs;
      std::memset(&attrs, 0, sizeof(attrs));
      attrs.background_pixel = BlackPixel(mDisplay, screen);
      attrs.border_pixel = 0;
      attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                         LeaveWindowMask | KeyPressMask | StructureNotifyMask;

      mWindow = XCreateWindow(mDisplay, RootWindow(mDisplay, screen), 0, 0, pixelW(), pixelH(), 0,
                              DefaultDepth(mDisplay, screen), InputOutput, mVisual,
                              CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
      if (!mWindow) {
         XCloseDisplay(mDisplay);
         mDisplay = nullptr;
         return false;
      }

      // Politely handle the close button when the window is floating.
      mDeleteAtom = XInternAtom(mDisplay, "WM_DELETE_WINDOW", False);
      XSetWMProtocols(mDisplay, mWindow, &mDeleteAtom, 1);

      mTarget = cairo_xlib_surface_create(mDisplay, mWindow, mVisual, pixelW(), pixelH());
      mTargetCr = cairo_create(mTarget);
      allocateBuffer();
      XFlush(mDisplay);
      return true;
   }

   bool embed(uintptr_t parentWindow) override {
      if (!mWindow)
         return false;
      XReparentWindow(mDisplay, mWindow, static_cast<Window>(parentWindow), 0, 0);
      XFlush(mDisplay);
      return true;
   }

   bool setTransientFor(uintptr_t parentWindow) override {
      if (!mWindow)
         return false;
      XSetTransientForHint(mDisplay, mWindow, static_cast<Window>(parentWindow));
      XFlush(mDisplay);
      return true;
   }

   void setTitle(const char *title) override {
      if (mWindow && title)
         XStoreName(mDisplay, mWindow, title);
   }
#endif

   // Resizing is zooming: every coordinate in this file is a design pixel and
   // cairo applies one scale to all of it, so a host that asks for a bigger
   // window gets the same layout drawn larger. Nothing reflows, and nothing has
   // to. The scale range is generous at the top for large displays and stops at
   // half size at the bottom, where the help line becomes unreadable.
   static constexpr double kMinScale = 0.5;
   static constexpr double kMaxScale = 4.0;

   double scaleFor(uint32_t width, uint32_t height) const {
      const double s = std::min(width / static_cast<double>(mWindowW),
                                height / static_cast<double>(currentH()));
      return std::min(kMaxScale, std::max(kMinScale, s));
   }

   void designSize(uint32_t *width, uint32_t *height) const override {
      *width = mWindowW;
      *height = currentH();
   }

   void fitSize(uint32_t *width, uint32_t *height) const override {
      const double s = scaleFor(*width, *height);
      *width = static_cast<uint32_t>(mWindowW * s + 0.5);
      *height = static_cast<uint32_t>(currentH() * s + 0.5);
   }

   bool resize(uint32_t width, uint32_t height) override {
      setScale(scaleFor(width, height));
      return true;
   }

   void setScale(double scale) override {
      if (scale < kMinScale || scale > kMaxScale || std::fabs(scale - mScale) < 0.001)
         return;
      mScale = scale;
      applySize();
   }

   // Takes the window to whatever pixelW() x pixelH() currently is. Called for
   // a scale change and for the collapsible section, which changes the height
   // at a fixed scale; both are the same job once the numbers have moved.
   void applySize() {
      if (!mWindow)
         return;
#if defined(_WIN32)
      SetWindowPos(mWindow, nullptr, 0, 0, static_cast<int>(pixelW()),
                   static_cast<int>(pixelH()), SWP_NOMOVE | SWP_NOZORDER);
#else
      XResizeWindow(mDisplay, mWindow, pixelW(), pixelH());
      cairo_xlib_surface_set_size(mTarget, pixelW(), pixelH());
#endif
      allocateBuffer();
      mDirty = true;
   }

   void size(uint32_t *width, uint32_t *height) const override {
      *width = pixelW();
      *height = pixelH();
   }

   void show() override {
      if (!mWindow)
         return;
#if defined(_WIN32)
      ShowWindow(mWindow, SW_SHOWNA);
#else
      XMapWindow(mDisplay, mWindow);
      XFlush(mDisplay);
#endif
      mDirty = true;
   }

   void hide() override {
      if (!mWindow)
         return;
      if (mSaveOpen)
         closeSaveDialog();
      closeEntry();
#if defined(_WIN32)
      ShowWindow(mWindow, SW_HIDE);
#else
      XUnmapWindow(mDisplay, mWindow);
      XFlush(mDisplay);
#endif
   }

   void tick() override {
      syncAdvanced();
#if defined(_WIN32)
      if (!mWindow)
         return;
      pumpEvents();
      // Painting happens in WM_PAINT, not here. GDI belongs to the thread that
      // owns the window, and tick() is called from whatever clock the host
      // offers -- in a host with no timer that is a thread of the plugin's own,
      // which would be drawing on someone else's DC. Marking the window dirty
      // is safe from any thread and the owning thread does the work.
      if (needsRepaint())
         InvalidateRect(mWindow, nullptr, FALSE);
#else
      if (!mDisplay)
         return;
      pumpEvents();
      if (!mWindow)
         return;
      if (needsRepaint())
         paint();
#endif
   }

private:
   uint32_t pixelW() const { return static_cast<uint32_t>(mWindowW * mScale + 0.5); }
   uint32_t pixelH() const { return static_cast<uint32_t>(currentH() * mScale + 0.5); }

   void allocateBuffer() {
      if (mBufferCr) {
         cairo_destroy(mBufferCr);
         mBufferCr = nullptr;
      }
      if (mBuffer) {
         cairo_surface_destroy(mBuffer);
         mBuffer = nullptr;
      }
      mBuffer = cairo_image_surface_create(CAIRO_FORMAT_RGB24, static_cast<int>(pixelW()),
                                           static_cast<int>(pixelH()));
      mBufferCr = cairo_create(mBuffer);
   }

   void closeWindow() {
      if (mBufferCr)
         cairo_destroy(mBufferCr);
      if (mBuffer)
         cairo_surface_destroy(mBuffer);
      if (mTargetCr)
         cairo_destroy(mTargetCr);
      if (mTarget)
         cairo_surface_destroy(mTarget);
      mBufferCr = nullptr;
      mBuffer = nullptr;
      mTargetCr = nullptr;
      mTarget = nullptr;
#if defined(_WIN32)
      if (mWindow) {
         releaseKeyboard();
         DestroyWindow(mWindow);
         if (--windowCount() == 0)
            UnregisterClassW(windowClassName(), moduleInstance());
      }
      mWindow = nullptr;
#else
      if (mDisplay) {
         // Closing the display would drop the grab anyway, but say so plainly
         // rather than depending on it: a keyboard nobody can type on is a very
         // expensive thing to leave behind.
         releaseKeyboard();
         if (mWindow)
            XDestroyWindow(mDisplay, mWindow);
         XCloseDisplay(mDisplay);
      }
      mWindow = 0;
      mDisplay = nullptr;
#endif
   }

   // ------------------------------------------------------------------ layout

   struct Panel {
      const PanelSpec *spec;
      Rect rect;
   };

   // The window is two stacks of panel rows with the sequencer and the preset
   // bar between them: rows 0 to advancedRow-1 always, then the step grid and
   // its bank, then the bar, then -- only while the section is open -- the
   // rest of the rows. The collapsible half holds the ten mods and the three
   // panels nobody reaches for twice in a session, which is what makes the
   // window that opens with the plugin a page of controls rather than two.
   bool hasAdvanced() const {
      return mSpec.host && mSpec.advancedRow > 0 && mSpec.advancedRow < mSpec.rowCount;
   }

   int visibleRowCount() const { return hasAdvanced() ? mSpec.advancedRow : mSpec.rowCount; }

   int currentH() const {
      return mAdvancedOpen && mSpec.windowExpandedH > 0 ? mSpec.windowExpandedH : mSpec.windowH;
   }

   // Lays one row of panels out at `y` and returns where the next row starts.
   //
   // A panel is exactly as wide as the cells it holds. The shared window
   // stretches the last panel of a row out to the right-hand edge so the rows
   // end flush; this one does not, because the collapsible section's rows are
   // nowhere near full and a panel stretched over a few hundred pixels of
   // nothing reads as a panel with a control missing from it. What is left
   // over stays background.
   int layoutRow(int row, int y) {
      int x = kMargin;
      int rowH = 0;
      for (int i = 0; i < mSpec.rowLength[row]; ++i) {
         const PanelSpec &spec = mSpec.panels[mSpec.rowStart[row] + i];
         Panel p;
         p.spec = &spec;
         p.rect.x = x;
         p.rect.y = y;
         p.rect.w = spec.cols * kCellW + 2 * kPanelPad;
         p.rect.h = kPanelTitleH + spec.rows * kCellH + kPanelPad;
         rowH = std::max(rowH, static_cast<int>(p.rect.h));

         for (const Cell &cell : flowCells(spec)) {
            mCells.push_back(cell);
            mCellRects.push_back(cellRect(p, cell));
         }
         mPanels.push_back(p);
         x += static_cast<int>(p.rect.w) + kGap;
      }
      return y + rowH + kGap;
   }

   void buildLayout() {
      mPanels.clear();
      mCells.clear();
      mCellRects.clear();

      int y = kHeaderH + kGap;
      const int visible = visibleRowCount();
      for (int row = 0; row < visible; ++row)
         y = layoutRow(row, y);

      // The step grid and, beside it, the bank of sixty-four patterns.
      mSeqRect = {0, 0, 0, 0};
      mBankRect = {0, 0, 0, 0};
      mSeqClearRect = {0, 0, 0, 0};
      mSeqGenRect = {0, 0, 0, 0};
      mSeqSeedDownRect = {0, 0, 0, 0};
      mSeqSeedUpRect = {0, 0, 0, 0};
      mSeqSeedLabelRect = {0, 0, 0, 0};
      mSeqLeftRect = {0, 0, 0, 0};
      mSeqRightRect = {0, 0, 0, 0};
      mSeqMidiRect = {0, 0, 0, 0};
      mSeqMapRect = {0, 0, 0, 0};
      mSeqPrevPatRect = {0, 0, 0, 0};
      mSeqNextPatRect = {0, 0, 0, 0};
      mSeqOctDownRect = {0, 0, 0, 0};
      mSeqOctUpRect = {0, 0, 0, 0};
      mSeqOctLabelRect = {0, 0, 0, 0};
      if (hasPattern()) {
         const double bankW = hasBank() ? kBankW + kGap : 0.0;
         mSeqRect.x = kMargin;
         mSeqRect.y = y;
         mSeqRect.w = mSpec.contentW - bankW;
         mSeqRect.h = seqPaneHeight();
         if (hasBank()) {
            mBankRect.x = mSeqRect.x + mSeqRect.w + kGap;
            mBankRect.y = y;
            mBankRect.w = kBankW;
            mBankRect.h = mSeqRect.h;
         }
         // Right to left: GEN, + , the seed, -, then the two shifts and CLEAR.
         const double bh = 14.0;
         const double by = y + 4.0;
         double bx = mSeqRect.x + mSeqRect.w - kSeqPad - 38.0;
         mSeqGenRect = {bx, by, 38.0, bh};
         bx -= 20.0;
         mSeqSeedUpRect = {bx, by, 18.0, bh};
         // Wide enough for the whole of a 32-bit seed. Ten digits is what a
         // Unix timestamp takes and the range exists so that one can be used.
         bx -= 90.0;
         mSeqSeedLabelRect = {bx, by, 88.0, bh};
         bx -= 20.0;
         mSeqSeedDownRect = {bx, by, 18.0, bh};
         bx -= 30.0;
         mSeqRightRect = {bx, by, 22.0, bh};
         bx -= 24.0;
         mSeqLeftRect = {bx, by, 22.0, bh};
         bx -= 54.0;
         mSeqClearRect = {bx, by, 46.0, bh};
         bx -= 48.0;
         mSeqMidiRect = {bx, by, 42.0, bh};
         // Live mode's group: the map switch, the two pattern steps, and the
         // pattern's own octave. Left of everything that edits the pattern,
         // because none of these does -- they are for playing it.
         bx -= 50.0;
         mSeqMapRect = {bx, by, 44.0, bh};
         bx -= 44.0;
         mSeqNextPatRect = {bx, by, 38.0, bh};
         bx -= 42.0;
         mSeqPrevPatRect = {bx, by, 38.0, bh};
         bx -= 26.0;
         mSeqOctUpRect = {bx, by, 18.0, bh};
         bx -= 58.0;
         mSeqOctLabelRect = {bx, by, 56.0, bh};
         bx -= 20.0;
         mSeqOctDownRect = {bx, by, 18.0, bh};
         y += static_cast<int>(mSeqRect.h) + kGap;
      }

      const int barY = y + 2;
      mPrevRect = {static_cast<double>(kMargin) + 62, static_cast<double>(barY), 26, kBarH};
      mNameRect = {mPrevRect.x + mPrevRect.w + 4, static_cast<double>(barY), 300, kBarH};
      mNextRect = {mNameRect.x + mNameRect.w + 4, static_cast<double>(barY), 26, kBarH};
      mSaveRect = {mNextRect.x + mNextRect.w + 14, static_cast<double>(barY), 58, kBarH};
      mMixerRect = {mSaveRect.x + mSaveRect.w + 8, static_cast<double>(barY), 62, kBarH};
      // The switch for the collapsible section, on the bar because that is the
      // one row of the window that is neither a panel nor the grid.
      mAdvancedRect = {mSaveRect.x + mSaveRect.w + (hasMixer() ? 78.0 : 8.0),
                       static_cast<double>(barY), 104, kBarH};
      // Where the window says that a mute or a solo is holding a level down.
      // Only drawn while one is, and drawn in the accent so it cannot be
      // mistaken for part of the furniture: a forced-down layer that looks
      // like a saved one is the whole trap this chip exists to close.
      // Back to the machine it started as. On the bar rather than on a panel
      // because it is not one module's control -- it undoes all of them.
      mResetRect = {mAdvancedRect.x + mAdvancedRect.w + 8, static_cast<double>(barY), 62, kBarH};
      mHoldRect = {mResetRect.x + mResetRect.w + 10, static_cast<double>(barY), 104, kBarH};
      // The version label, right-aligned in the header at baseline 44. The box
      // is a fixed size anchored to the right edge rather than measured from
      // the text, because the layout runs without a cairo context to measure
      // with. Nothing else is drawn there, so it cannot catch a stray click.
      mVersionRect = {static_cast<double>(kMargin + mSpec.contentW) - 56.0, 33.0, 56.0, 14.0};
      mBarY = barY;

      y = barY + kBarH + 6;
      if (hasAdvanced() && mAdvancedOpen)
         for (int row = visible; row < mSpec.rowCount; ++row)
            y = layoutRow(row, y);
      mHelpY = y - 2;
   }

   Rect cellRect(const Panel &p, const Cell &cell) const {
      Rect r;
      r.x = p.rect.x + kPanelPad + cell.col * kCellW;
      r.y = p.rect.y + kPanelTitleH + cell.row * kCellH;
      r.w = kCellW * cell.span;
      r.h = kCellH;
      if (cell.half) {
         r.h = kCellH * 0.5;
         if (cell.half == 2)
            r.y += kCellH * 0.5;
      }
      return r;
   }

   // Lays a panel's parameters out left to right, wrapping to the next row when
   // a cell no longer fits. Kept in one place so drawing and hit testing can
   // never disagree about where a control is.
   //
   // A pair of parameters marked kStacked shares one column, one chip above the
   // other. Only enum parameters are ever marked, and they are marked in pairs;
   // an unpaired one simply takes the top half and leaves the bottom empty.
   std::vector<Cell> flowCells(const PanelSpec &spec) const {
      std::vector<Cell> cells;
      int col = 0, row = 0;
      bool topTaken = false;
      for (int c = 0; c < spec.count; ++c) {
         const uint32_t raw = spec.params[c];
         const uint32_t id = raw & kParamMask;
         const bool stack = (raw & kStacked) != 0;
         if (stack && topTaken) {
            cells.push_back({id, col, row, 1, 2});
            topTaken = false;
            if (++col >= spec.cols) {
               col = 0;
               ++row;
            }
            continue;
         }
         topTaken = false;
         const int span = cellSpan(id);
         if (col + span > spec.cols) {
            col = 0;
            ++row;
         }
         if (stack) {
            cells.push_back({id, col, row, 1, 1});
            topTaken = true;
            continue;
         }
         cells.push_back({id, col, row, span, 0});
         col += span;
         if (col >= spec.cols) {
            col = 0;
            ++row;
         }
      }
      return cells;
   }

   // ------------------------------------------------------------------- paint

   bool needsRepaint() {
      // Leaving Live mode leaves the map behind with it: the bank has to go
      // back to selecting patterns, or the next click on it does nothing and
      // looks broken.
      if (mMapMode && hasPattern() && !mSpec.pattern->seqLive()) {
         mMapMode = false;
         mSpec.pattern->seqSetLearnTarget(kNoteNone);
         return true;
      }
      // While the map is open the window repaints every frame, because the
      // thing it is waiting for -- a MIDI note -- arrives on the audio thread
      // and there is nothing here to notice it otherwise. It is a mode you are
      // in for a few seconds at a time.
      if (mMapMode)
         return true;
      if (mDirty)
         return true;
      for (uint32_t i = 0; i < mSpec.paramCount; ++i) {
         const double v = mDelegate.guiParamValue(i);
         if (std::fabs(v - mShown[i]) > 1.0e-9)
            return true;
      }
      if (hasPattern() && (mSpec.pattern->seqPlayhead() != mLastPlayhead ||
                           mSpec.pattern->seqPlayingPattern() != mLastPlayingPattern ||
                           mSpec.pattern->seqPendingPattern() != mLastPending ||
                           (mLastPending >= 0 && pendingBlinkOn() != mLastBlinkOn)))
         return true;
      return mSpec.ornament && mSpec.ornament->animating(mDelegate.guiEventCounter());
   }

#if defined(_WIN32)
   // Lends paint() a target for the duration of one WM_PAINT. Everything the
   // frame is built from is the shared code; only where it lands differs.
   void paintToDC(HDC dc) {
      cairo_surface_t *surface = cairo_win32_surface_create(dc);
      cairo_t *cr = cairo_create(surface);
      cairo_surface_t *const keptTarget = mTarget;
      cairo_t *const keptCr = mTargetCr;
      mTarget = surface;
      mTargetCr = cr;
      paint();
      mTarget = keptTarget;
      mTargetCr = keptCr;
      cairo_destroy(cr);
      cairo_surface_destroy(surface);
   }
#endif

   void paint() {
      cairo_t *cr = mBufferCr;
      if (!cr)
         return;
      mDirty = false;
      for (uint32_t i = 0; i < mSpec.paramCount; ++i)
         mShown[i] = mDelegate.guiParamValue(i);

      cairo_save(cr);
      cairo_scale(cr, mScale, mScale);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

      drawBackground(cr);
      drawHeader(cr);
      for (const Panel &p : mPanels)
         drawPanel(cr, p);
      drawSequencer(cr);
      drawBank(cr);
      drawPresetBar(cr);
      drawHelpLine(cr);
      if (mBrowserOpen)
         drawBrowser(cr);
      if (mMixerOpen)
         drawMixer(cr);
      if (mMenuParam >= 0)
         drawMenu(cr);
      if (mSaveOpen)
         drawSaveDialog(cr);

      cairo_restore(cr);

      cairo_set_source_surface(mTargetCr, mBuffer, 0, 0);
      cairo_paint(mTargetCr);
      cairo_surface_flush(mTarget);
#if !defined(_WIN32)
      XFlush(mDisplay);
#endif
   }

   void drawBackground(cairo_t *cr) {
      cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, currentH());
      cairo_pattern_add_color_stop_rgb(grad, 0.0, mSpec.theme.bgTop.r, mSpec.theme.bgTop.g, mSpec.theme.bgTop.b);
      cairo_pattern_add_color_stop_rgb(grad, 1.0, mSpec.theme.bgBottom.r, mSpec.theme.bgBottom.g, mSpec.theme.bgBottom.b);
      cairo_set_source(cr, grad);
      cairo_rectangle(cr, 0, 0, mWindowW, currentH());
      cairo_fill(cr);
      cairo_pattern_destroy(grad);
   }

   // The header carries the wordmark and a few falling streaks whose density
   // follows the engine, so the window shows what it is doing even at a glance.
   void drawHeader(cairo_t *cr) {
      if (mSpec.ornament) {
         const uint32_t limit = std::max<uint32_t>(1, mDelegate.guiVoiceLimit());
         const OrnamentContext ctx{static_cast<double>(mWindowW),
                                   static_cast<double>(kHeaderH),
                                   &mSpec.theme,
                                   std::min(1.0, static_cast<double>(mDelegate.guiVoiceCount()) /
                                                    static_cast<double>(limit)),
                                   mDelegate.guiEventCounter()};
         cairo_save(cr);
         cairo_rectangle(cr, 0, 0, mWindowW, kHeaderH);
         cairo_clip(cr);
         mSpec.ornament->draw(cr, ctx);
         cairo_restore(cr);
      }

      const double baseline = 46;
      cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 30);
      setColor(cr, mSpec.theme.text);
      cairo_move_to(cr, kMargin, baseline);
      cairo_show_text(cr, mSpec.wordmarkFirst);
      double advance = textWidth(cr, mSpec.wordmarkFirst, 30, true);
      setColor(cr, mSpec.theme.accent);
      cairo_move_to(cr, kMargin + advance + 2, baseline);
      cairo_show_text(cr, mSpec.wordmarkSecond);

      setColor(cr, mSpec.theme.textMute);
      drawText(cr, kMargin + mSpec.contentW, 30, mSpec.subtitle, 9, true, Align::Right);
      char ver[64];
      std::snprintf(ver, sizeof(ver), "v%s", mSpec.version);
      drawText(cr, kMargin + mSpec.contentW, 44, ver, 9, false, Align::Right);

      setColor(cr, mSpec.theme.panelEdge);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, kMargin, kHeaderH - 0.5);
      cairo_line_to(cr, kMargin + mSpec.contentW, kHeaderH - 0.5);
      cairo_stroke(cr);
   }

   void drawPanel(cairo_t *cr, const Panel &p) {
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, p.rect.x, p.rect.y, p.rect.w, p.rect.h, 5);
      cairo_fill_preserve(cr);
      setColor(cr, mSpec.theme.panelEdge);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      setColor(cr, mSpec.theme.accent, 0.85);
      drawText(cr, p.rect.x + kPanelPad + 2, p.rect.y + 16, p.spec->title, 10, true, Align::Left);

      for (const Cell &cell : flowCells(*p.spec))
         drawCell(cr, cellRect(p, cell), cell.param);
   }

   // Fits a caps label into one cell: full size if it fits, a little smaller if
   // that is enough, otherwise broken across two lines at its middle space.
   void drawLabel(cairo_t *cr, const Rect &r, const char *label) {
      const double avail = r.w - 6;
      const double cx = r.x + r.w * 0.5;
      if (textWidth(cr, label, 8.5, true) <= avail) {
         drawText(cr, cx, r.y + 14, label, 8.5, true, Align::Center);
         return;
      }
      const char *space = nullptr;
      const size_t len = std::strlen(label);
      for (size_t i = 0; i < len; ++i)
         if (label[i] == ' ' &&
             (!space || std::abs(static_cast<int>(i) - static_cast<int>(len / 2)) <
                           std::abs(static_cast<int>(space - label) - static_cast<int>(len / 2))))
            space = label + i;
      if (!space) {
         drawText(cr, cx, r.y + 14, label, 7.0, true, Align::Center);
         return;
      }
      (void)avail;
      char first[64];
      const size_t firstLen = std::min(sizeof(first) - 1, static_cast<size_t>(space - label));
      std::memcpy(first, label, firstLen);
      first[firstLen] = 0;
      drawText(cr, cx, r.y + 10, first, 8.0, true, Align::Center);
      drawText(cr, cx, r.y + 19, space + 1, 8.0, true, Align::Center);
   }

   // The value under a knob is also a text field. Click it and type: whatever
   // the display prints is accepted back, including units and multipliers
   // ("2.2k", "500 ms", "-12 dB"), because paramTextToValue is the same parser
   // the host's text entry goes through. Like the save field it grabs the
   // keyboard for as long as it is open, because an embedded window is not
   // given the focus by every host; it is a modal field and lives for one
   // value, which is the case where a grab is defensible.
   // Both window systems reduce a keystroke to the same few commands plus some
   // text, so the fields do not need to know which one they are on.
   enum class KeyCommand { NoCommand, Escape, Accept, Backspace, Up, Down };

   Rect valueRect(const Rect &cell) const { return {cell.x + 6, cell.y + 74, cell.w - 12, 22}; }

   // Where an enum chip sits inside its cell. A stacked pair gets half the
   // height each, so the chip moves up and the dropdown has to follow it.
   Rect chipRect(const Rect &cell) const {
      const bool half = cell.h < kCellH - 1.0;
      return {cell.x + 5, cell.y + (half ? 17.0 : 34.0), cell.w - 10, 22.0};
   }

   void openEntry(uint32_t id) { openEntryAt(id, valueRect(cellRectFor(id))); }

   // The same field, told where to draw itself. Everything on a panel goes
   // through openEntry(); the seed is not on a panel, and a control whose
   // range spans a 32-bit word has to be typeable or it cannot be reached.
   void openEntryAt(uint32_t id, const Rect &where) {
      const ParamDesc &d = mSpec.params[id];
      if (isChip(d))
         return;
      mEntryRect = where;
      mBrowserOpen = false;
      closeMenu();
      char text[128];
      if (!paramValueToText(d, mDelegate.guiParamValue(id), text, sizeof(text)))
         text[0] = 0;
      mEntryParam = static_cast<int>(id);
      mEntryText = text;
      mEntryFailed = false;
      mEntryFresh = true;
      grabKeyboard();
      mDirty = true;
   }

   // Every path that leaves the field comes through here, for the same reason
   // as closeSaveDialog: a grab that outlives its field is a dead keyboard.
   void closeEntry() {
      if (mEntryParam < 0)
         return;
      mEntryParam = -1;
      releaseKeyboard();
      mDirty = true;
   }

   void commitEntry() {
      if (mEntryParam < 0)
         return;
      const uint32_t id = static_cast<uint32_t>(mEntryParam);
      double raw = 0.0;
      if (!paramTextToValue(mSpec.params[id], mEntryText.c_str(), &raw)) {
         mEntryFailed = true;
         mDirty = true;
         return;
      }
      mDelegate.guiBeginEdit(id);
      mDelegate.guiSetParam(id, raw);
      mDelegate.guiEndEdit(id);
      closeEntry();
   }

   void onEntryKey(KeyCommand cmd, const char *text, int textLen) {
      if (cmd == KeyCommand::Escape) {
         closeEntry();
         return;
      }
      if (cmd == KeyCommand::Accept) {
         commitEntry();
         return;
      }
      // The field opens showing the current value as selected text: the first
      // keystroke replaces it, and only then does typing append.
      if (cmd == KeyCommand::Backspace) {
         if (mEntryFresh)
            mEntryText.clear();
         else if (!mEntryText.empty())
            mEntryText.pop_back();
         mEntryFresh = false;
      } else {
         bool typed = false;
         for (int i = 0; i < textLen; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 0x20 || c == 0x7F)
               continue;
            if (mEntryFresh) {
               mEntryText.clear();
               mEntryFresh = false;
            }
            if (mEntryText.size() < 24)
               mEntryText.push_back(static_cast<char>(c));
            typed = true;
         }
         if (!typed && cmd == KeyCommand::NoCommand)
            return; // a modifier or dead key: nothing to show
      }
      mEntryFailed = false;
      mDirty = true;
   }

   void drawEntryField(cairo_t *cr, const Rect &cell) {
      const Rect f = valueRect(cell);
      setColor(cr, mSpec.theme.knobFace);
      roundedRect(cr, f.x, f.y, f.w, f.h, 3);
      cairo_fill_preserve(cr);
      if (mEntryFailed)
         cairo_set_source_rgb(cr, 0.85, 0.30, 0.30);
      else
         setColor(cr, mSpec.theme.accent, 0.9);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      if (mEntryFresh && !mEntryText.empty()) {
         // Selected: the text sits on an accent bar, as a text field shows a
         // selection, so it is clear that typing replaces it.
         setColor(cr, mSpec.theme.accent, 0.35);
         roundedRect(cr, f.x + 4, f.y + 4, f.w - 8, f.h - 8, 2);
         cairo_fill(cr);
      }
      setColor(cr, mSpec.theme.text);
      const std::string shown = mEntryFresh ? mEntryText : mEntryText + "|";
      drawText(cr, f.x + f.w * 0.5, f.y + f.h * 0.5 + 4, shown.c_str(), 9.5, false, Align::Center);
   }

   void drawCell(cairo_t *cr, const Rect &r, uint32_t id) {
      const ParamDesc &d = mSpec.params[id];
      const double raw = mDelegate.guiParamValue(id);
      const bool hot = (mHover == static_cast<int>(id)) || (mDrag == static_cast<int>(id));

      char label[64];
      upperCase(d.name, label, sizeof(label));
      setColor(cr, hot ? mSpec.theme.text : mSpec.theme.textDim);
      drawLabel(cr, r, label);

      char text[128];
      if (!paramValueToText(d, raw, text, sizeof(text)))
         std::snprintf(text, sizeof(text), "--");

      if (isChip(d)) {
         const Rect chip = chipRect(r);
         const double cw = chip.w;
         const double ch = chip.h;
         const double cx = chip.x;
         const double cy = chip.y;
         setColor(cr, mSpec.theme.knobFace);
         roundedRect(cr, cx, cy, cw, ch, 3);
         cairo_fill_preserve(cr);
         setColor(cr, hot ? mSpec.theme.accent : mSpec.theme.panelEdge, hot ? 0.7 : 1.0);
         cairo_set_line_width(cr, 1.0);
         cairo_stroke(cr);

         setColor(cr, mSpec.theme.textMute);
         drawTriangle(cr, cx + 9, cy + ch * 0.5, 7, -1);
         drawTriangle(cr, cx + cw - 9, cy + ch * 0.5, 7, 1);
         setColor(cr, mSpec.theme.text);
         drawText(cr, cx + cw * 0.5, cy + 15, text, 10, false, Align::Center);
         return;
      }

      const double cx = r.x + r.w * 0.5;
      const double cy = r.y + 46;
      const double t = normalised(d, raw);

      setColor(cr, mSpec.theme.track);
      cairo_set_line_width(cr, 3.0);
      cairo_arc(cr, cx, cy, kKnobR, kArcStart, kArcStart + kArcSweep);
      cairo_stroke(cr);

      const double from = isBipolar(d) ? kArcStart + kArcSweep * 0.5 : kArcStart;
      const double to = kArcStart + kArcSweep * t;
      setColor(cr, mSpec.theme.accent);
      cairo_set_line_width(cr, 3.0);
      if (to >= from)
         cairo_arc(cr, cx, cy, kKnobR, from, to);
      else
         cairo_arc_negative(cr, cx, cy, kKnobR, from, to);
      cairo_stroke(cr);

      setColor(cr, mSpec.theme.knobFace);
      cairo_arc(cr, cx, cy, kKnobR - 5, 0, 2 * M_PI);
      cairo_fill_preserve(cr);
      setColor(cr, hot ? mSpec.theme.accent : mSpec.theme.panelEdge, hot ? 0.6 : 1.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      const double ang = kArcStart + kArcSweep * t;
      setColor(cr, mSpec.theme.text);
      cairo_set_line_width(cr, 2.0);
      cairo_move_to(cr, cx + std::cos(ang) * (kKnobR - 15), cy + std::sin(ang) * (kKnobR - 15));
      cairo_line_to(cr, cx + std::cos(ang) * (kKnobR - 7), cy + std::sin(ang) * (kKnobR - 7));
      cairo_stroke(cr);

      if (mEntryParam == static_cast<int>(id)) {
         drawEntryField(cr, r);
         return;
      }
      setColor(cr, hot ? mSpec.theme.accent : mSpec.theme.text, hot ? 1.0 : 0.85);
      drawText(cr, cx, r.y + 86, text, 9.5, false, Align::Center);
   }

   const char *presetLabel(char *buf, size_t size) const {
      const auto &list = mDelegate.guiPresets();
      const int cur = mDelegate.guiCurrentPreset();
      if (cur < 0 || cur >= static_cast<int>(list.size())) {
         std::snprintf(buf, size, "Init");
         return buf;
      }
      std::snprintf(buf, size, "%s%s", list[static_cast<size_t>(cur)].name.c_str(),
                    mDelegate.guiPresetEdited() ? " *" : "");
      return buf;
   }

   void drawPresetBar(cairo_t *cr) {
      setColor(cr, mSpec.theme.textMute);
      drawText(cr, kMargin, mBarY + 21, "PRESET", 9, true, Align::Left);

      auto button = [&](const Rect &r, int dir, bool hot) {
         setColor(cr, mSpec.theme.panelFill);
         roundedRect(cr, r.x, r.y, r.w, r.h, 4);
         cairo_fill_preserve(cr);
         setColor(cr, hot ? mSpec.theme.accent : mSpec.theme.panelEdge, hot ? 0.7 : 1.0);
         cairo_set_line_width(cr, 1.0);
         cairo_stroke(cr);
         setColor(cr, hot ? mSpec.theme.accent : mSpec.theme.textDim);
         drawTriangle(cr, r.x + r.w * 0.5, r.y + r.h * 0.5, 9, dir);
      };

      button(mPrevRect, -1, mHoverWidget == Widget::Prev);
      button(mNextRect, 1, mHoverWidget == Widget::Next);

      const bool hot = mHoverWidget == Widget::Name || mBrowserOpen;
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, mNameRect.x, mNameRect.y, mNameRect.w, mNameRect.h, 4);
      cairo_fill_preserve(cr);
      setColor(cr, hot ? mSpec.theme.accent : mSpec.theme.panelEdge, hot ? 0.7 : 1.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      char name[160];
      presetLabel(name, sizeof(name));
      setColor(cr, mSpec.theme.accent);
      drawText(cr, mNameRect.x + mNameRect.w * 0.5, mNameRect.y + 21, name, 13, true,
               Align::Center);
      setColor(cr, mSpec.theme.textMute);
      drawTriangle(cr, mNameRect.x + mNameRect.w - 12, mNameRect.y + mNameRect.h * 0.5, 8, 0);

      const bool saveHot = mHoverWidget == Widget::Save || mSaveOpen;
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, mSaveRect.x, mSaveRect.y, mSaveRect.w, mSaveRect.h, 4);
      cairo_fill_preserve(cr);
      setColor(cr, saveHot ? mSpec.theme.accent : mSpec.theme.panelEdge, saveHot ? 0.7 : 1.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      setColor(cr, saveHot ? mSpec.theme.accent : mSpec.theme.textDim);
      drawText(cr, mSaveRect.x + mSaveRect.w * 0.5, mSaveRect.y + 20, "SAVE", 10, true,
               Align::Center);

      if (hasAdvanced()) {
         const bool advHot = mHoverWidget == Widget::Advanced;
         const bool on = mAdvancedOpen;
         setColor(cr, on ? mSpec.theme.accent : mSpec.theme.panelFill, on ? 0.16 : 1.0);
         roundedRect(cr, mAdvancedRect.x, mAdvancedRect.y, mAdvancedRect.w, mAdvancedRect.h, 4);
         cairo_fill_preserve(cr);
         setColor(cr, (advHot || on) ? mSpec.theme.accent : mSpec.theme.panelEdge,
                  advHot ? 0.7 : 1.0);
         cairo_set_line_width(cr, 1.0);
         cairo_stroke(cr);
         setColor(cr, (advHot || on) ? mSpec.theme.accent : mSpec.theme.textDim);
         drawText(cr, mAdvancedRect.x + mAdvancedRect.w * 0.5 - 7, mAdvancedRect.y + 20,
                  mSpec.advancedLabel ? mSpec.advancedLabel : "ADVANCED", 10, true, Align::Center);
         // The triangle points the way the window is about to move, which is
         // the only part of a disclosure control anybody actually reads. The
         // toolkit draws left, right and down; up is this, because a chevron
         // that never turns round is worse than none.
         const double tx = mAdvancedRect.x + mAdvancedRect.w - 14;
         const double ty = mAdvancedRect.y + mAdvancedRect.h * 0.5;
         cairo_new_path(cr);
         if (on) {
            cairo_move_to(cr, tx - 4, ty + 2.8);
            cairo_line_to(cr, tx + 4, ty + 2.8);
            cairo_line_to(cr, tx, ty - 2.8);
         } else {
            cairo_move_to(cr, tx - 4, ty - 2.8);
            cairo_line_to(cr, tx + 4, ty - 2.8);
            cairo_line_to(cr, tx, ty + 2.8);
         }
         cairo_close_path(cr);
         cairo_fill(cr);
      }

      // RESET, and the second click that means it. An armed button says SURE?
      // in the accent: this throws away every knob position in the window and
      // there is no undo for it inside the plugin.
      {
         const bool resetHot = mHoverWidget == Widget::Reset;
         setColor(cr, mResetArmed ? mSpec.theme.accent : mSpec.theme.panelFill,
                  mResetArmed ? 0.16 : 1.0);
         roundedRect(cr, mResetRect.x, mResetRect.y, mResetRect.w, mResetRect.h, 4);
         cairo_fill_preserve(cr);
         setColor(cr, (resetHot || mResetArmed) ? mSpec.theme.accent : mSpec.theme.panelEdge,
                  resetHot ? 0.7 : 1.0);
         cairo_set_line_width(cr, 1.0);
         cairo_stroke(cr);
         setColor(cr, (resetHot || mResetArmed) ? mSpec.theme.accent : mSpec.theme.textDim);
         drawText(cr, mResetRect.x + mResetRect.w * 0.5, mResetRect.y + 20,
                  mResetArmed ? "SURE?" : "RESET", 10, true, Align::Center);
      }

      if (!hasMixer())
         return;

      const bool mixHot = mHoverWidget == Widget::Mixer || mMixerOpen;
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, mMixerRect.x, mMixerRect.y, mMixerRect.w, mMixerRect.h, 4);
      cairo_fill_preserve(cr);
      setColor(cr, mixHot ? mSpec.theme.accent : mSpec.theme.panelEdge, mixHot ? 0.7 : 1.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      setColor(cr, mixHot ? mSpec.theme.accent : mSpec.theme.textDim);
      drawText(cr, mMixerRect.x + mMixerRect.w * 0.5, mMixerRect.y + 20, "MIXER", 10, true,
               Align::Center);

      if (!holdsActive())
         return;

      const bool holdHot = mHoverWidget == Widget::Hold;
      setColor(cr, mSpec.theme.accent, 0.16);
      roundedRect(cr, mHoldRect.x, mHoldRect.y, mHoldRect.w, mHoldRect.h, 4);
      cairo_fill_preserve(cr);
      setColor(cr, mSpec.theme.accent, holdHot ? 1.0 : 0.75);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      setColor(cr, mSpec.theme.accent);
      drawText(cr, mHoldRect.x + mHoldRect.w * 0.5 - 7, mHoldRect.y + 20,
               anySolo() ? "SOLO ON" : "MUTE ON", 10, true, Align::Center);
      // The dismiss cross is drawn rather than typed, for the same reason the
      // preset arrows are: a missing glyph here would read as a bug.
      const double cx = mHoldRect.x + mHoldRect.w - 14;
      const double cy = mHoldRect.y + mHoldRect.h * 0.5;
      cairo_set_line_width(cr, 1.6);
      cairo_move_to(cr, cx - 4, cy - 4);
      cairo_line_to(cr, cx + 4, cy + 4);
      cairo_move_to(cr, cx + 4, cy - 4);
      cairo_line_to(cr, cx - 4, cy + 4);
      cairo_stroke(cr);
   }

   // ------------------------------------------------------------ save dialog

   Rect savePanel() const {
      Rect r;
      r.w = 420;
      r.h = 156;
      r.x = (mWindowW - r.w) * 0.5;
      r.y = kHeaderH + 130;
      return r;
   }

   Rect saveFieldRect() const {
      const Rect p = savePanel();
      return {p.x + 20, p.y + 54, p.w - 40, 30};
   }

   Rect saveOkRect() const {
      const Rect p = savePanel();
      return {p.x + p.w - 20 - 92, p.y + p.h - 20 - 28, 92, 28};
   }

   Rect saveCancelRect() const {
      const Rect p = savePanel();
      return {p.x + p.w - 20 - 92 - 10 - 92, p.y + p.h - 20 - 28, 92, 28};
   }

   void openSaveDialog() {
      mBrowserOpen = false;
      mMixerOpen = false;
      closeMenu();
      // A preset is the parameter values, and a muted layer's value is zero
      // only because the mixer is holding it there. Saving that would bake a
      // silent layer into the file, so every hold is released first and what
      // goes out is what the preset really is.
      clearHolds();
      mSaveOpen = true;
      mSaveName = mDelegate.guiSuggestedPresetName();
      mSaveStatus.clear();
      mSaveFailed = false;
      grabKeyboard();
      if (!mKeyboardGrabbed)
         mSaveStatus = "Cannot reach the keyboard. SAVE stores it under this name.";
      mDirty = true;
   }

   // An embedded plugin window is not given the input focus by every host, and
   // there is no way in CLAP to ask for it. Bitwig does not hand it over, so
   // XSetInputFocus alone leaves the field unable to see a single keystroke.
   //
   // A grab does not depend on the host at all, and a modal dialog is the one
   // situation that genuinely warrants one: it lasts only while the field is
   // open, and taking the keyboard is exactly what being modal means. The focus
   // request is still made first, because where it does work it is the better
   // behaved of the two and leaves the host's own shortcuts alone.
   //
   // Failing to get the grab is not an error worth propagating: another client
   // may hold one. The dialog says so and saving under the offered name still
   // works with the mouse.
   void grabKeyboard() {
      if (!mWindow || mKeyboardGrabbed)
         return;
      // Said before the keyboard actually moves, because from here on the key
      // releases for anything the host is playing from its own computer
      // keyboard go to this window instead of to the host.
      mDelegate.guiKeyboardTaken();
#if defined(_WIN32)
      // Windows routes keys to whichever window holds the focus, and a child
      // window is allowed to take it, so there is nothing here to grab. That is
      // why this is not the ugly compromise it has to be on X11: the host keeps
      // its shortcuts, and focus returns on its own when the dialog closes.
      mPrevFocus = SetFocus(mWindow);
      mKeyboardGrabbed = GetFocus() == mWindow;
#else
      if (!mDisplay)
         return;
      XErrorHandler previous = XSetErrorHandler(&ignoreXError);
      XSetInputFocus(mDisplay, mWindow, RevertToParent, CurrentTime);
      mKeyboardGrabbed = XGrabKeyboard(mDisplay, mWindow, True, GrabModeAsync, GrabModeAsync,
                                       CurrentTime) == GrabSuccess;
      XSync(mDisplay, False);
      XSetErrorHandler(previous);
#endif
   }

   void releaseKeyboard() {
      if (!mKeyboardGrabbed)
         return;
#if defined(_WIN32)
      if (mPrevFocus && IsWindow(mPrevFocus))
         SetFocus(mPrevFocus);
      mPrevFocus = nullptr;
#else
      if (mDisplay) {
         XErrorHandler previous = XSetErrorHandler(&ignoreXError);
         XUngrabKeyboard(mDisplay, CurrentTime);
         XSync(mDisplay, False);
         XSetErrorHandler(previous);
      }
#endif
      mKeyboardGrabbed = false;
   }

   // Every path that leaves the dialog has to come through here. A keyboard
   // grab that outlives its dialog would leave the whole desktop unable to type
   // until the plugin is unloaded.
   void closeSaveDialog() {
      mSaveOpen = false;
      releaseKeyboard();
      mDirty = true;
   }

   void commitSave() {
      std::string name = mSaveName;
      while (!name.empty() && name.front() == ' ')
         name.erase(name.begin());
      while (!name.empty() && name.back() == ' ')
         name.pop_back();
      if (name.empty()) {
         mSaveStatus = "Give the preset a name.";
         mSaveFailed = true;
         mDirty = true;
         return;
      }
      std::string error;
      if (!mDelegate.guiSavePreset(name, error)) {
         mSaveStatus = error.empty() ? std::string("Could not save the preset.") : error;
         mSaveFailed = true;
         mDirty = true;
         return;
      }
      closeSaveDialog();
   }

   void drawSaveDialog(cairo_t *cr) {
      setColor(cr, mSpec.theme.bgBottom, 0.88);
      cairo_rectangle(cr, 0, 0, mWindowW, currentH());
      cairo_fill(cr);

      const Rect p = savePanel();
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, p.x, p.y, p.w, p.h, 6);
      cairo_fill_preserve(cr);
      setColor(cr, mSpec.theme.accent, 0.5);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      setColor(cr, mSpec.theme.accent);
      drawText(cr, p.x + 20, p.y + 26, "SAVE PRESET", 11, true, Align::Left);
      setColor(cr, mSpec.theme.textMute);
      drawText(cr, p.x + p.w - 20, p.y + 26,
               hasFolders() ? "to your own preset library" : "to your own preset folder", 9,
               false, Align::Right);

      const Rect f = saveFieldRect();
      setColor(cr, mSpec.theme.knobFace);
      roundedRect(cr, f.x, f.y, f.w, f.h, 3);
      cairo_fill_preserve(cr);
      setColor(cr, mSaveFailed ? mSpec.theme.accent : mSpec.theme.panelEdge, 1.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      std::string shown = mSaveName;
      shown += "_"; // a plain caret; the window has no blinking anywhere else
      setColor(cr, mSpec.theme.text);
      drawText(cr, f.x + 9, f.y + 20, shown.c_str(), 12, false, Align::Left);

      setColor(cr, mSpec.theme.textMute);
      // Typing the folder is the whole of the "new folder" gesture: there is
      // no second dialog, and a folder cannot be made empty, which is right
      // for something whose only job is to hold presets.
      drawText(cr, p.x + 20, f.y + f.h + 20,
               !mSaveStatus.empty() ? mSaveStatus.c_str()
               : hasFolders()
                  ? "Type a name, then Enter. \"Folder/Name\" saves into a folder and makes "
                    "it if it is new. Esc cancels."
                  : "Type a name, then Enter. Esc cancels.",
               9, false, Align::Left);

      auto dialogButton = [&](const Rect &r, const char *label, bool accent) {
         setColor(cr, mSpec.theme.panelFill);
         roundedRect(cr, r.x, r.y, r.w, r.h, 4);
         cairo_fill_preserve(cr);
         setColor(cr, accent ? mSpec.theme.accent : mSpec.theme.panelEdge, accent ? 0.7 : 1.0);
         cairo_set_line_width(cr, 1.0);
         cairo_stroke(cr);
         setColor(cr, accent ? mSpec.theme.accent : mSpec.theme.textDim);
         drawText(cr, r.x + r.w * 0.5, r.y + 18, label, 10, true, Align::Center);
      };
      dialogButton(saveCancelRect(), "CANCEL", false);
      dialogButton(saveOkRect(), "SAVE", true);
   }

   // `None` is taken: X11 defines it as a macro, the same trap as Widget.

   void onSaveKey(KeyCommand cmd, const char *text, int textLen) {
      if (cmd == KeyCommand::Escape) {
         closeSaveDialog();
         return;
      }
      if (cmd == KeyCommand::Accept) {
         commitSave();
         return;
      }
      if (cmd == KeyCommand::Backspace) {
         if (!mSaveName.empty())
            mSaveName.pop_back();
         mSaveStatus.clear();
         mSaveFailed = false;
         mDirty = true;
         return;
      }
      for (int i = 0; i < textLen; ++i) {
         const unsigned char c = static_cast<unsigned char>(text[i]);
         if (c >= 0x20 && c != 0x7F && mSaveName.size() < 48)
            mSaveName.push_back(static_cast<char>(c));
      }
      mSaveStatus.clear();
      mSaveFailed = false;
      mDirty = true;
   }

   // A keystroke while a list is open. Up and down move through a selector
   // list or scroll the browser, Return and Escape close them, and anything
   // else dismisses whatever is open, which is what a key did here before the
   // lists learnt to navigate. Whether keys arrive at all is up to the host:
   // an embedded window is not given the focus by every one of them.
   void onOverlayKey(KeyCommand cmd) {
      if (mMenuParam >= 0) {
         if (cmd == KeyCommand::Up || cmd == KeyCommand::Down) {
            stepMenu(cmd == KeyCommand::Up ? -1 : 1);
            return;
         }
         closeMenu();
         mDirty = true;
         return;
      }
      if (mMixerOpen) {
         closeMixer();
         return;
      }
      if (mBrowserOpen) {
         if (cmd == KeyCommand::Up || cmd == KeyCommand::Down) {
            scrollBrowser(cmd == KeyCommand::Up ? -1 : 1);
            return;
         }
         mBrowserOpen = false;
         mDirty = true;
      }
   }

   // Moves the open selector list's value by one entry, clamped at the ends.
   void stepMenu(int direction) {
      if (mMenuParam < 0)
         return;
      const uint32_t id = static_cast<uint32_t>(mMenuParam);
      const ParamDesc &d = mSpec.params[id];
      const int cur = static_cast<int>(std::floor(mDelegate.guiParamValue(id) + 0.5));
      const int next = std::min(static_cast<int>(d.enumCount) - 1, std::max(0, cur + direction));
      if (next == cur)
         return;
      mDelegate.guiBeginEdit(id);
      mDelegate.guiSetParam(id, static_cast<double>(next));
      mDelegate.guiEndEdit(id);
      mMenuHover = next;
      mDirty = true;
   }

   void drawHelpLine(cairo_t *cr) {
      const char *msg = nullptr;
      if (mHover >= 0 && mHover < static_cast<int>(mSpec.paramCount))
         msg = mSpec.params[mHover].tip;
      if (!msg && mPaneHover >= 0 && mPaneHover < static_cast<int>(mSpec.paramCount))
         msg = mSpec.params[mPaneHover].tip;
      if (!msg && mHoverWidget == Widget::Reset)
         msg = mResetArmed ? "Click again to put every control back to the stock machine. The "
                             "pattern bank, the play mode and the clock are left alone."
                           : "RESET: every control back to the stock machine -- no mods, no "
                             "modifications, no drive and no delay. Click twice.";
      if (!msg) {
         const auto &list = mDelegate.guiPresets();
         const int cur = mDelegate.guiCurrentPreset();
         if (cur >= 0 && cur < static_cast<int>(list.size()) &&
             !list[static_cast<size_t>(cur)].description.empty())
            msg = list[static_cast<size_t>(cur)].description.c_str();
      }
      if (!msg)
         msg = hasMixer()
                  ? "Drag a knob to edit, double-click to reset, shift-drag for fine "
                    "control. Click a value to type one. MIXER balances the layers."
                  : "Drag a knob to edit, double-click to reset, shift-drag for fine "
                    "control. Click a menu to pick from the list. Drag MIDI into the host to "
                    "take the pattern with you.";

      setColor(cr, mSpec.theme.textMute);
      drawText(cr, kMargin, mHelpY + kHelpH - 8, msg, 10, false, Align::Left);
   }

   // ---------------------------------------------------------------- browser
   //
   // The library, grouped. A plugin that says nothing about folders gets the
   // flat grid this always was; one that does gets a column of shelves down
   // the left, the selected shelf's presets beside it, and a footer that can
   // write a whole shelf out as one file or read one back in.

   bool hasFolders() const { return mDelegate.guiPresetFoldersSupported(); }

   // Every folder in the library, in the order the browser lists them:
   // whatever the plugin calls its built-in set first, then the user's own
   // alphabetically, then the library's root. Rebuilt whenever the browser
   // opens or the library changes under it, which is the only time it can
   // move.
   void rebuildFolders() {
      mFolders.clear();
      if (!hasFolders())
         return;
      bool root = false;
      for (const auto &preset : mDelegate.guiPresets()) {
         if (preset.folder.empty()) {
            root = true;
            continue;
         }
         if (std::find(mFolders.begin(), mFolders.end(), preset.folder) == mFolders.end())
            mFolders.push_back(preset.folder);
      }
      if (root)
         mFolders.push_back(std::string()); // the root, drawn as "Unfiled"
      if (mBrowserFolder >= static_cast<int>(mFolders.size()))
         mBrowserFolder = -1;
   }

   // -1 is "All", which is the flat list this browser has always shown and is
   // what it opens on.
   std::string selectedFolder() const {
      return mBrowserFolder >= 0 && mBrowserFolder < static_cast<int>(mFolders.size())
                ? mFolders[static_cast<size_t>(mBrowserFolder)]
                : std::string();
   }

   // The presets on the selected shelf, as indices into the plugin's list --
   // loading still goes through the same index the plugin handed out, so
   // filtering cannot make the browser load the wrong preset.
   std::vector<int> browserItems() const {
      std::vector<int> out;
      const auto &list = mDelegate.guiPresets();
      for (size_t i = 0; i < list.size(); ++i)
         if (mBrowserFolder < 0 || list[i].folder == selectedFolder())
            out.push_back(static_cast<int>(i));
      return out;
   }

   int browserCols() const { return hasFolders() ? kBrowserCols - 1 : kBrowserCols; }

   int browserRows() const {
      const int n = static_cast<int>(browserItems().size());
      return (n + browserCols() - 1) / browserCols();
   }

   double browserFolderW() const { return hasFolders() ? kBrowserFolderW : 0.0; }
   double browserFooterH() const { return hasFolders() ? kBrowserFooterH : 0.0; }

   Rect browserPanel() const {
      const double maxH = currentH() - kHeaderH - 90;
      const int folderRows = static_cast<int>(mFolders.size()) + 1; // + "All"
      // Tall enough for whichever side needs more room, so a library with many
      // folders and few presets in the one on screen still shows its shelves.
      const int rows = std::max(browserRows(), hasFolders() ? folderRows : 0);
      Rect r;
      r.x = kMargin + 40;
      r.w = mSpec.contentW - 80;
      r.h = std::min(maxH, 40.0 + rows * kBrowserRowH + kBrowserPad + browserFooterH());
      r.y = kHeaderH + (maxH - r.h) * 0.5 + 20;
      return r;
   }

   // Rows that fit in the panel. A library larger than that scrolls, one row
   // per wheel step; the factory set fits without scrolling.
   int browserVisibleRows() const {
      const Rect p = browserPanel();
      return std::max(
         1, static_cast<int>((p.h - 40 - kBrowserPad - browserFooterH()) / kBrowserRowH));
   }

   int browserMaxScroll() const { return std::max(0, browserRows() - browserVisibleRows()); }

   void scrollBrowser(int rows) {
      const int next = std::min(browserMaxScroll(), std::max(0, mBrowserScroll + rows));
      if (next != mBrowserScroll) {
         mBrowserScroll = next;
         mDirty = true;
      }
   }

   void openBrowser() {
      mBrowserOpen = true;
      mBrowserHover = -1;
      mImportOpen = false;
      mBrowserStatus.clear();
      rebuildFolders();
      // Open with the current preset in view, on its own shelf rather than in
      // the flat list: the folder a preset is in is part of where it is.
      const int cur = mDelegate.guiCurrentPreset();
      const auto &list = mDelegate.guiPresets();
      if (hasFolders() && cur >= 0 && cur < static_cast<int>(list.size())) {
         const auto it =
            std::find(mFolders.begin(), mFolders.end(), list[static_cast<size_t>(cur)].folder);
         mBrowserFolder = it == mFolders.end() ? -1 : static_cast<int>(it - mFolders.begin());
      }
      const std::vector<int> items = browserItems();
      const auto at = std::find(items.begin(), items.end(), cur);
      const int position = at == items.end() ? 0 : static_cast<int>(at - items.begin());
      mBrowserScroll = std::min(browserMaxScroll(),
                                std::max(0, position / browserCols() - browserVisibleRows() / 2));
      closeMenu();
      mDirty = true;
   }

   // Row-major, so scrolling by rows keeps every column moving together. The
   // position is the preset's place on the shelf on screen, not its index in
   // the plugin's list; a negative or too-large row is out of view and
   // browserItemVisible() says so.
   Rect browserItemRect(int position) const {
      const Rect p = browserPanel();
      const int cols = browserCols();
      const int col = position % cols;
      const int row = position / cols - mBrowserScroll;
      Rect r;
      r.w = (p.w - 2 * kBrowserPad - kBrowserScrollW - browserFolderW()) / cols;
      r.h = kBrowserRowH;
      r.x = p.x + kBrowserPad + browserFolderW() + col * r.w;
      r.y = p.y + 40 + row * r.h;
      return r;
   }

   bool browserItemVisible(int position) const {
      const int row = position / browserCols() - mBrowserScroll;
      return row >= 0 && row < browserVisibleRows();
   }

   // The shelves. Row 0 is "All"; the rest are mFolders in order.
   Rect browserFolderRect(int row) const {
      const Rect p = browserPanel();
      Rect r;
      r.x = p.x + kBrowserPad;
      r.w = kBrowserFolderW - kBrowserPad;
      r.h = kBrowserRowH;
      r.y = p.y + 40 + row * kBrowserRowH;
      return r;
   }

   int browserFolderAt(double x, double y) const {
      if (!hasFolders())
         return -2;
      for (int row = 0; row <= static_cast<int>(mFolders.size()); ++row)
         if (browserFolderRect(row).contains(x, y))
            return row - 1; // row 0 is "All", which is folder -1
      return -2;
   }

   // The footer: export this shelf, export it somewhere else, import a pack.
   Rect browserFooterRect(int which) const {
      const Rect p = browserPanel();
      const double w = 108.0;
      Rect r;
      r.h = 22.0;
      r.y = p.y + p.h - kBrowserFooterH + 6.0;
      r.x = p.x + kBrowserPad + which * (w + 8.0);
      r.w = w;
      return r;
   }

   // The list of packs the plugin can see, shown under IMPORT. The last entry
   // is the desktop's own file chooser, when there is one.
   Rect browserImportRect(int row) const {
      const Rect anchor = browserFooterRect(2);
      Rect r;
      r.x = anchor.x;
      r.w = 260.0;
      r.h = kMenuRowH;
      r.y = anchor.y - 4.0 - (browserImportCount() - row) * kMenuRowH;
      return r;
   }

   // The packs, then "Other file...", then "Pattern folder...". The last two
   // need a desktop chooser and are left out where there is none.
   int browserImportCount() const {
      return static_cast<int>(mImportPacks.size()) + (fileDialogAvailable() ? 3 : 0);
   }

   Rect browserScrollbar() const {
      const Rect p = browserPanel();
      Rect r;
      r.w = kBrowserScrollW - 6;
      r.x = p.x + p.w - kBrowserPad - r.w;
      r.y = p.y + 40;
      r.h = browserVisibleRows() * kBrowserRowH;
      return r;
   }

   // ------------------------------------------------------------- the packs

   // Where a pack of the selected folder goes by default, and what the status
   // line says afterwards. Writing it is the plugin's job; the window only
   // knows which shelf is on screen.
   void exportPack(bool chooseWhere) {
      if (!hasFolders())
         return;
      const std::string folder = selectedFolder();
      if (mBrowserFolder < 0) {
         mBrowserStatus = "Pick a folder to export -- All is the whole library.";
         mDirty = true;
         return;
      }
      std::string path = mDelegate.guiPackPathFor(folder);
      if (chooseWhere) {
         mDelegate.guiKeyboardTaken(); // a modal dialog takes the keyboard too
         path = saveFileDialog("Export preset pack", path, "Preset pack",
                               packExtensionFromPath(path));
         if (path.empty()) {
            mDirty = true;
            return; // cancelled
         }
      }
      if (path.empty()) {
         mBrowserStatus = "Nowhere to write a pack: no user config directory.";
         mDirty = true;
         return;
      }
      std::string error;
      mBrowserStatus = mDelegate.guiExportPack(folder, path, error)
                          ? "Wrote " + path
                          : (error.empty() ? std::string("Could not write the pack.") : error);
      mImportPacks = mDelegate.guiPresetPacks();
      mDirty = true;
   }

   void importPack(const std::string &path) {
      mImportOpen = false;
      if (path.empty()) {
         mDirty = true;
         return;
      }
      std::string folder;
      std::string error;
      if (!mDelegate.guiImportPack(path, folder, error)) {
         mBrowserStatus = error.empty() ? std::string("Could not read the pack.") : error;
         mDirty = true;
         return;
      }
      rebuildFolders();
      const auto it = std::find(mFolders.begin(), mFolders.end(), folder);
      mBrowserFolder = it == mFolders.end() ? -1 : static_cast<int>(it - mFolders.begin());
      mBrowserScroll = 0;
      mBrowserStatus = "Imported as \"" + folder + "\"";
      mDirty = true;
   }

   // The extension a pack path ends in, so the file chooser can filter on it
   // without the window having to know what the plugin calls its presets.
   static std::string packExtensionFromPath(const std::string &path) {
      const size_t dot = path.find_last_of('.');
      return dot == std::string::npos ? std::string("pack") : path.substr(dot + 1);
   }

   // An enum chip opens a list anchored to itself. Stepping one value per click
   // never reaches the far end of a seven-way enum without a lot of clicking,
   // and the value the user wants is always visible this way.
   Rect menuPanel() const {
      const ParamDesc &d = mSpec.params[mMenuParam];
      const Rect &c = cellRectFor(static_cast<uint32_t>(mMenuParam));
      Rect r;
      r.w = std::max(c.w - 10.0, 96.0);
      r.h = d.enumCount * kMenuRowH + 2 * kMenuPad;
      const Rect chip = chipRect(c);
      r.x = chip.x;
      r.y = chip.y + chip.h + 2; // just under the chip
      // Flip above the chip rather than run off the bottom of the window.
      if (r.y + r.h > currentH() - 8)
         r.y = chip.y - 2 - r.h;
      if (r.y < kHeaderH + 4)
         r.y = kHeaderH + 4;
      return r;
   }

   Rect menuItemRect(int index) const {
      const Rect p = menuPanel();
      Rect r;
      r.x = p.x + kMenuPad;
      r.w = p.w - 2 * kMenuPad;
      r.h = kMenuRowH;
      r.y = p.y + kMenuPad + index * kMenuRowH;
      return r;
   }

   int menuItemAt(double x, double y) const {
      if (mMenuParam < 0)
         return -1;
      const ParamDesc &d = mSpec.params[mMenuParam];
      for (int i = 0; i < static_cast<int>(d.enumCount); ++i)
         if (menuItemRect(i).contains(x, y))
            return i;
      return -1;
   }

   void closeMenu() {
      mMenuParam = -1;
      mMenuHover = -1;
   }

   void drawMenu(cairo_t *cr) {
      const ParamDesc &d = mSpec.params[mMenuParam];
      const Rect p = menuPanel();
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, p.x, p.y, p.w, p.h, 5);
      cairo_fill_preserve(cr);
      setColor(cr, mSpec.theme.accent, 0.5);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      const int cur = static_cast<int>(
         std::floor(mDelegate.guiParamValue(static_cast<uint32_t>(mMenuParam)) + 0.5));
      for (int i = 0; i < static_cast<int>(d.enumCount); ++i) {
         const Rect r = menuItemRect(i);
         const bool hot = mMenuHover == i;
         const bool sel = cur == i;
         if (hot || sel) {
            setColor(cr, mSpec.theme.accent, hot ? 0.20 : 0.10);
            roundedRect(cr, r.x, r.y, r.w, r.h, 3);
            cairo_fill(cr);
         }
         setColor(cr, sel ? mSpec.theme.accent : mSpec.theme.text, hot ? 1.0 : 0.85);
         drawText(cr, r.x + 8, r.y + r.h * 0.5 + 4, d.enumNames[i], 10, sel, Align::Left);
      }
   }

   void drawBrowser(cairo_t *cr) {
      setColor(cr, mSpec.theme.bgBottom, 0.88);
      cairo_rectangle(cr, 0, 0, mWindowW, currentH());
      cairo_fill(cr);

      const Rect p = browserPanel();
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, p.x, p.y, p.w, p.h, 6);
      cairo_fill_preserve(cr);
      setColor(cr, mSpec.theme.accent, 0.5);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      setColor(cr, mSpec.theme.accent);
      drawText(cr, p.x + kBrowserPad, p.y + 24, "PRESETS", 11, true, Align::Left);
      setColor(cr, mSpec.theme.textMute);
      drawText(cr, p.x + p.w - kBrowserPad, p.y + 24,
               browserMaxScroll() > 0 ? "click to load, wheel to scroll, click outside to close"
                                      : "click to load, or click outside to close",
               9, false, Align::Right);

      const auto &list = mDelegate.guiPresets();
      const int cur = mDelegate.guiCurrentPreset();

      // ---- the shelves
      if (hasFolders()) {
         const double top = p.y + 40;
         const double bottom = top + browserVisibleRows() * kBrowserRowH;
         setColor(cr, mSpec.theme.panelEdge, 0.8);
         cairo_set_line_width(cr, 1.0);
         cairo_move_to(cr, p.x + browserFolderW() + 2.0, top);
         cairo_line_to(cr, p.x + browserFolderW() + 2.0, bottom);
         cairo_stroke(cr);

         for (int row = 0; row <= static_cast<int>(mFolders.size()); ++row) {
            const Rect r = browserFolderRect(row);
            if (r.y + r.h > bottom + 1.0)
               break;
            const int folder = row - 1;
            const bool sel = folder == mBrowserFolder;
            const bool hot = mFolderHover == folder;
            if (sel || hot) {
               setColor(cr, mSpec.theme.accent, sel ? 0.18 : 0.10);
               roundedRect(cr, r.x, r.y + 1, r.w, r.h - 2, 3);
               cairo_fill(cr);
            }
            const char *label = row == 0 ? "All"
                               : mFolders[static_cast<size_t>(folder)].empty()
                                  ? "Unfiled"
                                  : mFolders[static_cast<size_t>(folder)].c_str();
            setColor(cr, sel ? mSpec.theme.accent : mSpec.theme.text, hot ? 1.0 : 0.8);
            drawText(cr, r.x + 8, r.y + r.h * 0.5 + 4, label, 10.5, sel, Align::Left);
            // How many presets are on it, which is the one thing a shelf can
            // say about itself without being opened.
            int count = 0;
            for (const auto &preset : list)
               if (row == 0 || preset.folder == mFolders[static_cast<size_t>(folder)])
                  ++count;
            char num[12];
            std::snprintf(num, sizeof(num), "%d", count);
            setColor(cr, mSpec.theme.textMute);
            drawText(cr, r.x + r.w - 8, r.y + r.h * 0.5 + 4, num, 9, false, Align::Right);
         }
      }

      // ---- the presets on the shelf
      const std::vector<int> items = browserItems();
      for (size_t k = 0; k < items.size(); ++k) {
         if (!browserItemVisible(static_cast<int>(k)))
            continue;
         const int index = items[k];
         const Rect r = browserItemRect(static_cast<int>(k));
         const bool hot = mBrowserHover == index;
         const bool sel = cur == index;
         if (hot || sel) {
            setColor(cr, mSpec.theme.accent, hot ? 0.20 : 0.10);
            roundedRect(cr, r.x + 2, r.y + 1, r.w - 4, r.h - 2, 3);
            cairo_fill(cr);
         }
         setColor(cr, sel ? mSpec.theme.accent : mSpec.theme.text, hot ? 1.0 : 0.85);
         drawText(cr, r.x + 10, r.y + r.h * 0.5 + 4, list[static_cast<size_t>(index)].name.c_str(),
                  11, sel, Align::Left);
         if (list[static_cast<size_t>(index)].userContent) {
            setColor(cr, mSpec.theme.textMute);
            drawText(cr, r.x + r.w - 10, r.y + r.h * 0.5 + 4, "USER", 8, true, Align::Right);
         }
      }

      // ---- the footer: a folder in, a folder out, and whatever the last one
      // of those had to say about itself.
      if (hasFolders()) {
         drawSeqButton(cr, browserFooterRect(0), "EXPORT", mBrowserFolder >= 0);
         if (fileDialogAvailable())
            drawSeqButton(cr, browserFooterRect(1), "EXPORT AS...", mBrowserFolder >= 0);
         drawSeqButton(cr, browserFooterRect(2), "IMPORT...");

         const Rect last = browserFooterRect(2);
         setColor(cr, mSpec.theme.textMute);
         drawText(cr, last.x + last.w + 12.0, last.y + last.h - 6.0,
                  mBrowserStatus.empty()
                     ? "A pack is one file holding a whole folder; IMPORT also reads "
                       ".pat and .mid patterns, one or a folder at a time."
                     : mBrowserStatus.c_str(),
                  9, false, Align::Left);

         if (mImportOpen && browserImportCount() > 0) {
            const int rows = browserImportCount();
            const Rect first = browserImportRect(0);
            setColor(cr, mSpec.theme.bgTop, 0.98);
            roundedRect(cr, first.x - 4.0, first.y - 4.0, first.w + 8.0,
                        rows * kMenuRowH + 8.0, 4.0);
            cairo_fill_preserve(cr);
            setColor(cr, mSpec.theme.accent, 0.6);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            for (int row = 0; row < rows; ++row) {
               const Rect r = browserImportRect(row);
               const bool hot = mImportHover == row;
               if (hot) {
                  setColor(cr, mSpec.theme.accent, 0.2);
                  roundedRect(cr, r.x, r.y, r.w, r.h, 3.0);
                  cairo_fill(cr);
               }
               const int extra = row - static_cast<int>(mImportPacks.size());
               std::string label = extra < 0    ? fileNameOf(mImportPacks[row])
                                   : extra == 0 ? "Other file..."
                                   : extra == 1 ? "Pattern file..."
                                                : "Pattern folder...";
               setColor(cr, mSpec.theme.text, hot ? 1.0 : 0.85);
               drawText(cr, r.x + 8, r.y + r.h - 6.0, label.c_str(), 10, extra >= 0, Align::Left);
            }
         }
      }

      // A scrollbar only when there is something to scroll: a track with a
      // thumb whose length is the visible fraction of the list.
      if (browserMaxScroll() > 0) {
         const Rect sb = browserScrollbar();
         setColor(cr, mSpec.theme.text, 0.08);
         roundedRect(cr, sb.x, sb.y, sb.w, sb.h, 3);
         cairo_fill(cr);
         const double frac = browserVisibleRows() / static_cast<double>(browserRows());
         const double thumbH = std::max(18.0, sb.h * frac);
         const double thumbY =
            sb.y + (sb.h - thumbH) * (mBrowserScroll / static_cast<double>(browserMaxScroll()));
         setColor(cr, mSpec.theme.accent, 0.55);
         roundedRect(cr, sb.x, thumbY, sb.w, thumbH, 3);
         cairo_fill(cr);
      }
   }

   // ------------------------------------------------------------------ mixer
   //
   // One strip per layer: the layer's level as a fader, its pan and its width
   // as slim sliders under it, and a mute and a solo button. The parameters are
   // the plugin's own and also live on the panels -- the mixer is a second view
   // of them, not a second set. What it adds is that they are next to each
   // other, which is what balancing layers needs and what a panel layout
   // organised by layer cannot give.
   //
   // Mute and solo are not parameters. They cannot be: a preset is a set of
   // parameter values, and a mute that saved itself would be a layer that
   // comes back silent. So they work by holding a level down and remembering
   // what it was -- see applyHolds() -- with the bar chip saying so for as long
   // as one is active, and openSaveDialog() releasing them all before a preset
   // is written.

   enum MixerHitKind { HitNone, HitFader, HitPan, HitWidth, HitMute, HitSolo };

   struct MixerHit {
      int strip = -1;
      MixerHitKind kind = HitNone;
   };

   bool hasMixer() const { return mSpec.mixer && mSpec.mixerCount > 0; }
   int stripCount() const { return hasMixer() ? mSpec.mixerCount : 0; }

   // kNoParam, and anything else that is not an index into the plugin's table.
   bool hasParam(uint32_t id) const { return id < mSpec.paramCount; }

   // The fader is what a strip is, so a strip whose level is not a real
   // parameter is not drawn and cannot be hit. Not every layer in the suite
   // has a level of its own -- RainyDay's close droplets are loudness
   // compensated and have none -- and a plugin that lists one anyway would
   // otherwise read past the end of its own parameter table.
   bool stripValid(int i) const { return hasParam(mSpec.mixer[i].level); }

   // The master strip, drawn after a gap because it is the whole instrument
   // rather than one of the layers. -1 when the plugin does not list one.
   int masterStrip() const {
      for (int i = 0; i < stripCount(); ++i)
         if (mSpec.mixer[i].master)
            return i;
      return -1;
   }

   // Strips are the same width in every plugin until there are too many of
   // them for the window, which no plugin has yet but a later one might.
   double stripW() const {
      const double avail = mSpec.contentW - 40.0 - 2 * kMixerPad - kMixerGap;
      return std::min(kStripW, avail / std::max(1, stripCount()));
   }

   Rect mixerPanel() const {
      Rect r;
      r.w = stripCount() * stripW() + 2 * kMixerPad + (masterStrip() > 0 ? kMixerGap : 0.0);
      r.h = kMixerTitleH + kMixerStripH + kMixerPad;
      r.x = kMargin + (mSpec.contentW - r.w) * 0.5;
      // Centred in what is left between the header and the preset bar, so it
      // sits over the panels rather than over the bar it was opened from.
      r.y = kHeaderH + std::max(0.0, (mBarY - kHeaderH - r.h) * 0.5);
      return r;
   }

   Rect stripRect(int i) const {
      const Rect p = mixerPanel();
      const int master = masterStrip();
      Rect r;
      r.w = stripW();
      r.x = p.x + kMixerPad + i * r.w + (master > 0 && i >= master ? kMixerGap : 0.0);
      r.y = p.y + kMixerTitleH;
      r.h = kMixerStripH;
      return r;
   }

   // The fader's track. The handle travels inside it, which is why the value
   // maps onto h - kHandleH rather than onto h.
   Rect faderRect(int i) const {
      const Rect s = stripRect(i);
      Rect r;
      r.w = 12.0;
      r.x = s.x + (s.w - r.w) * 0.5;
      r.y = s.y + kLabelH;
      r.h = kFaderH;
      return r;
   }

   // The pan and width rows sit at a fixed offset in every strip, so they line
   // up across the mixer whether or not a given layer has them.
   Rect sliderRect(int i, int row) const {
      const Rect s = stripRect(i);
      Rect r;
      r.x = s.x + 16.0;
      r.w = s.w - 22.0;
      r.y = s.y + kLabelH + kFaderH + kValueH + row * (kSliderH + 14.0);
      r.h = kSliderH;
      return r;
   }

   Rect mixerButtonRect(int i, bool solo) const {
      const Rect s = stripRect(i);
      Rect r;
      r.w = 26.0;
      r.h = kButtonH;
      r.x = s.x + s.w * 0.5 + (solo ? 3.0 : -3.0 - r.w);
      r.y = s.y + kLabelH + kFaderH + kValueH + 2 * (kSliderH + 14.0) + 2.0;
      return r;
   }

   MixerHit mixerHitAt(double x, double y) const {
      for (int i = 0; i < stripCount(); ++i) {
         if (!stripValid(i))
            continue;
         const MixerStrip &st = mSpec.mixer[i];
         // The fader is grabbed by its handle as well as its track, and the
         // handle is wider than the track.
         Rect grab = faderRect(i);
         grab.x -= 6.0;
         grab.w += 12.0;
         if (grab.contains(x, y))
            return {i, HitFader};
         if (hasParam(st.pan) && sliderRect(i, 0).contains(x, y))
            return {i, HitPan};
         if (hasParam(st.width) && sliderRect(i, 1).contains(x, y))
            return {i, HitWidth};
         if (!st.master) {
            if (mixerButtonRect(i, false).contains(x, y))
               return {i, HitMute};
            if (mixerButtonRect(i, true).contains(x, y))
               return {i, HitSolo};
         }
      }
      return {};
   }

   uint32_t hitParam(const MixerHit &h) const {
      if (h.strip < 0)
         return kNoParam;
      const MixerStrip &st = mSpec.mixer[h.strip];
      if (h.kind == HitFader)
         return st.level;
      if (h.kind == HitPan)
         return st.pan;
      if (h.kind == HitWidth)
         return st.width;
      return kNoParam;
   }

   void openMixer() {
      closeEntry();
      closeMenu();
      mBrowserOpen = false;
      mMixerOpen = true;
      mMixerHit = {};
      mDirty = true;
   }

   // Closing the mixer deliberately leaves the holds in place: soloing a layer
   // and then going to its knobs on the panels is the reason the mixer exists.
   // The bar chip is what keeps that visible.
   void closeMixer() {
      mMixerOpen = false;
      mDirty = true;
   }

   // ------------------------------------------------------------ mute / solo

   bool anySolo() const {
      for (int i = 0; i < stripCount(); ++i)
         if (mSoloed[static_cast<size_t>(i)])
            return true;
      return false;
   }

   bool holdsActive() const {
      for (int i = 0; i < stripCount(); ++i)
         if (mMuted[static_cast<size_t>(i)] || mSoloed[static_cast<size_t>(i)])
            return true;
      return false;
   }

   void setParamNow(uint32_t id, double value) {
      mDelegate.guiBeginEdit(id);
      mDelegate.guiSetParam(id, value);
      mDelegate.guiEndEdit(id);
   }

   // Brings every layer's level into line with the mute and solo buttons. A
   // layer that should not be heard has its level driven to the bottom of its
   // own range and its real value remembered; one that should be heard again
   // gets that value back. Nothing else touches those levels, so the memory
   // cannot go stale -- except across a preset load, which is why the loader
   // releases the holds first.
   void applyHolds() {
      const bool solo = anySolo();
      for (int i = 0; i < stripCount(); ++i) {
         const size_t k = static_cast<size_t>(i);
         const MixerStrip &st = mSpec.mixer[i];
         if (st.master || !stripValid(i))
            continue;
         const bool audible = solo ? mSoloed[k] != 0 : mMuted[k] == 0;
         if (!audible) {
            if (!mHeld[k]) {
               mHeldLevel[k] = mDelegate.guiParamValue(st.level);
               mHeld[k] = 1;
            }
            setParamNow(st.level, mSpec.params[st.level].min);
         } else if (mHeld[k]) {
            setParamNow(st.level, mHeldLevel[k]);
            mHeld[k] = 0;
         }
      }
      mDirty = true;
   }

   // Restores every held level and clears the buttons. Called by the chip, by
   // the save dialog, by a preset load, and by editing a held fader -- that
   // last one because a fader that fights the hand on it is worse than one
   // that simply lets go.
   void clearHolds() {
      for (int i = 0; i < stripCount(); ++i) {
         const size_t k = static_cast<size_t>(i);
         if (mHeld[k] && stripValid(i)) {
            setParamNow(mSpec.mixer[i].level, mHeldLevel[k]);
            mHeld[k] = 0;
         }
         mMuted[k] = 0;
         mSoloed[k] = 0;
      }
      mDirty = true;
   }

   // ------------------------------------------------------------ mixer paint

   void drawFader(cairo_t *cr, const Rect &t, double v, bool hot, bool live) {
      setColor(cr, mSpec.theme.knobFace);
      roundedRect(cr, t.x, t.y, t.w, t.h, 4);
      cairo_fill_preserve(cr);
      setColor(cr, mSpec.theme.panelEdge);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      const Rgb &fill = live ? mSpec.theme.accent : mSpec.theme.textMute;
      const double top = t.y + (1.0 - v) * (t.h - kHandleH) + kHandleH * 0.5;
      setColor(cr, fill, live ? 0.8 : 0.4);
      roundedRect(cr, t.x + 2.5, top, t.w - 5.0, t.y + t.h - top - 2.5, 2);
      cairo_fill(cr);

      const Rect h = {t.x - 6.0, top - kHandleH * 0.5, t.w + 12.0, kHandleH};
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, h.x, h.y, h.w, h.h, 3);
      cairo_fill_preserve(cr);
      setColor(cr, hot ? mSpec.theme.accent : mSpec.theme.panelEdge, hot ? 0.9 : 1.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      setColor(cr, fill, live ? 1.0 : 0.6);
      cairo_set_line_width(cr, 1.6);
      cairo_move_to(cr, h.x + 4.0, h.y + h.h * 0.5);
      cairo_line_to(cr, h.x + h.w - 4.0, h.y + h.h * 0.5);
      cairo_stroke(cr);
   }

   // Pan and width. Bipolar parameters fill from the centre out, the way the
   // knobs' arcs do, so a pan of dead centre reads as nothing rather than as
   // half of something.
   void drawSlider(cairo_t *cr, const Rect &r, const char *tag, const ParamDesc &d, double raw,
                   bool hot, bool live) {
      setColor(cr, live ? mSpec.theme.textDim : mSpec.theme.textMute, live ? 1.0 : 0.6);
      drawText(cr, r.x - 11.0, r.y + r.h - 1.0, tag, 8.0, true, Align::Left);

      setColor(cr, mSpec.theme.knobFace);
      roundedRect(cr, r.x, r.y, r.w, r.h, 3);
      cairo_fill_preserve(cr);
      setColor(cr, mSpec.theme.panelEdge);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      const double t = normalised(d, raw);
      const double from = isBipolar(d) ? 0.5 : 0.0;
      const double x0 = r.x + std::min(from, t) * r.w;
      const double x1 = r.x + std::max(from, t) * r.w;
      setColor(cr, live ? mSpec.theme.accent : mSpec.theme.textMute, live ? 0.75 : 0.4);
      cairo_rectangle(cr, x0, r.y + 2.5, std::max(1.0, x1 - x0), r.h - 5.0);
      cairo_fill(cr);

      setColor(cr, hot ? mSpec.theme.accent : mSpec.theme.text, hot ? 1.0 : 0.85);
      cairo_rectangle(cr, r.x + t * r.w - 1.5, r.y - 2.0, 3.0, r.h + 4.0);
      cairo_fill(cr);
   }

   void drawMixerButton(cairo_t *cr, const Rect &r, const char *label, bool on, bool hot,
                        const Rgb &lit) {
      setColor(cr, on ? lit : mSpec.theme.knobFace, on ? 0.9 : 1.0);
      roundedRect(cr, r.x, r.y, r.w, r.h, 3);
      cairo_fill_preserve(cr);
      setColor(cr, on || hot ? lit : mSpec.theme.panelEdge, hot && !on ? 0.8 : 1.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      if (on)
         setColor(cr, mSpec.theme.bgBottom);
      else
         setColor(cr, hot ? lit : mSpec.theme.textMute);
      drawText(cr, r.x + r.w * 0.5, r.y + r.h - 5.0, label, 9.5, true, Align::Center);
   }

   void drawMixerStrip(cairo_t *cr, int i) {
      const MixerStrip &st = mSpec.mixer[i];
      const size_t k = static_cast<size_t>(i);
      const Rect s = stripRect(i);
      const bool held = mHeld[k] != 0;
      const bool hovered = mMixerHit.strip == i;

      char label[64];
      upperCase(st.label, label, sizeof(label));
      setColor(cr, held ? mSpec.theme.textMute
                        : (hovered ? mSpec.theme.text : mSpec.theme.textDim));
      drawText(cr, s.x + s.w * 0.5, s.y + 12.0, label, 9.0, true, Align::Center);

      const ParamDesc &level = mSpec.params[st.level];
      const double raw = mDelegate.guiParamValue(st.level);
      drawFader(cr, faderRect(i), normalised(level, raw),
                hovered && mMixerHit.kind == HitFader, !held);

      char text[128];
      if (!paramValueToText(level, raw, text, sizeof(text)))
         std::snprintf(text, sizeof(text), "--");
      setColor(cr, held ? mSpec.theme.textMute : mSpec.theme.text, held ? 1.0 : 0.9);
      drawText(cr, s.x + s.w * 0.5, s.y + kLabelH + kFaderH + 14.0, text, 9.5, false,
               Align::Center);
      // A held layer shows what it will come back to. Without it the fader is
      // just sitting at the bottom and there is nothing to say the value it
      // used to have still exists.
      if (held) {
         char was[128];
         if (paramValueToText(level, mHeldLevel[k], was, sizeof(was))) {
            char line[160];
            std::snprintf(line, sizeof(line), "was %s", was);
            setColor(cr, mSpec.theme.accent, 0.75);
            drawText(cr, s.x + s.w * 0.5, s.y + kLabelH + kFaderH + 25.0, line, 8.0, false,
                     Align::Center);
         }
      }

      if (hasParam(st.pan))
         drawSlider(cr, sliderRect(i, 0), "P", mSpec.params[st.pan],
                    mDelegate.guiParamValue(st.pan), hovered && mMixerHit.kind == HitPan, !held);
      if (hasParam(st.width))
         drawSlider(cr, sliderRect(i, 1), "W", mSpec.params[st.width],
                    mDelegate.guiParamValue(st.width), hovered && mMixerHit.kind == HitWidth,
                    !held);

      if (st.master)
         return;
      drawMixerButton(cr, mixerButtonRect(i, false), "M", mMuted[k] != 0,
                      hovered && mMixerHit.kind == HitMute, kMuteRed);
      drawMixerButton(cr, mixerButtonRect(i, true), "S", mSoloed[k] != 0,
                      hovered && mMixerHit.kind == HitSolo, mSpec.theme.accent);
   }

   void drawMixer(cairo_t *cr) {
      setColor(cr, mSpec.theme.bgBottom, 0.88);
      cairo_rectangle(cr, 0, 0, mWindowW, currentH());
      cairo_fill(cr);

      const Rect p = mixerPanel();
      setColor(cr, mSpec.theme.panelFill);
      roundedRect(cr, p.x, p.y, p.w, p.h, 6);
      cairo_fill_preserve(cr);
      setColor(cr, mSpec.theme.accent, 0.55);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      setColor(cr, mSpec.theme.accent);
      drawText(cr, p.x + kMixerPad, p.y + 22.0, "MIXER", 11, true, Align::Left);

      // The title row doubles as the readout: hovering a control names it and
      // prints its value, which is where a strip has no room for either.
      char right[192];
      right[0] = 0;
      const uint32_t id = hitParam(mMixerHit);
      if (hasParam(id)) {
         char value[128];
         if (!paramValueToText(mSpec.params[id], mDelegate.guiParamValue(id), value,
                               sizeof(value)))
            std::snprintf(value, sizeof(value), "--");
         std::snprintf(right, sizeof(right), "%s   %s", mSpec.params[id].name, value);
      } else if (holdsActive()) {
         std::snprintf(right, sizeof(right), "%s", "M and S are not saved with the preset");
      } else {
         std::snprintf(right, sizeof(right), "%s", "Drag a fader, M to mute, S to solo");
      }
      setColor(cr, mSpec.theme.textDim);
      const double avail = p.w - 2 * kMixerPad - 46.0;
      if (textWidth(cr, right, 9.0, false) <= avail)
         drawText(cr, p.x + p.w - kMixerPad, p.y + 22.0, right, 9.0, false, Align::Right);

      for (int i = 0; i < stripCount(); ++i)
         if (stripValid(i))
            drawMixerStrip(cr, i);
   }

   // ----------------------------------------------------------- mixer events

   void onMixerDown(double x, double y, unsigned button, unsigned long timeMs, bool shift) {
      const MixerHit h = mixerHitAt(x, y);
      if (h.strip < 0) {
         if (button == kButtonLeft && !mixerPanel().contains(x, y))
            closeMixer();
         return;
      }
      const size_t k = static_cast<size_t>(h.strip);

      if (h.kind == HitMute || h.kind == HitSolo) {
         if (button == kButtonLeft) {
            auto &flag = h.kind == HitMute ? mMuted[k] : mSoloed[k];
            flag = flag ? 0 : 1;
            applyHolds();
         }
         return;
      }

      const uint32_t id = hitParam(h);
      if (!hasParam(id))
         return;
      // Editing a level the mixer is holding down releases the holds first:
      // the alternative is a fader that moves and then springs back the next
      // time a button is pressed.
      const bool releases = h.kind == HitFader && mHeld[k];

      if (button == kWheelUp || button == kWheelDown) {
         if (releases)
            clearHolds();
         nudge(id, button == kWheelUp ? 1 : -1, shift);
         return;
      }

      const bool doubleClick = button == kButtonLeft && mLastClickParam == static_cast<int>(id) &&
                               timeMs - mLastClickTime < 400;
      mLastClickParam = static_cast<int>(id);
      mLastClickTime = timeMs;
      if (releases)
         clearHolds();

      if (button == kButtonRight || doubleClick) {
         setParamNow(id, mSpec.params[id].def);
         mLastClickParam = -1;
         mDirty = true;
         return;
      }
      if (button != kButtonLeft)
         return;

      // A fader and a slider are dragged along their own travel rather than at
      // the knobs' fixed rate, so the handle stays under the pointer.
      mDrag = static_cast<int>(id);
      mDragHoriz = h.kind != HitFader;
      mDragStartX = x;
      mDragStartY = y;
      mDragStartValue = mDelegate.guiParamValue(id);
      const ParamDesc &d = mSpec.params[id];
      const double travel = mDragHoriz ? sliderRect(h.strip, h.kind == HitPan ? 0 : 1).w
                                       : kFaderH - kHandleH;
      mDragUnitsPerPx = (d.max - d.min) / std::max(1.0, travel);
      mDelegate.guiBeginEdit(id);
      mDirty = true;
   }


   // ------------------------------------------------------------- step grid
   //
   // Sixteen steps across, twelve semitones up, and three lanes of flags under
   // them, with the bank of sixty-four patterns beside it. This is the only
   // part of the window that is not in the shared one, and the reason the file
   // is a fork.
   //
   // The pattern is the plugin's, reached through PatternAccess: every read is
   // one relaxed atomic load of a packed word, so the window can draw while the
   // audio thread plays and an edit lands on the next step boundary. The grid
   // edits whichever pattern the Pattern parameter selects, which is not
   // necessarily the one sounding -- a running chain moves on without the
   // editor following it.

   static constexpr double kSeqTitleH = 22.0;
   static constexpr double kSeqNumH = 14.0;
   // The octave cells are the one control in the grid that is set by aiming at
   // a half of it rather than by hitting a row, so they are the one that has to
   // be big enough to aim at.
   static constexpr double kSeqOctH = 26.0;
   // The octave lane's second colour, and the only place this window uses a
   // hue the theme does not carry.
   //
   // Up is the accent; down is amber. Direction used to be carried by which
   // edge the bar was anchored to, and at two octaves the bar is most of the
   // box either way -- four pixels of blank edge, which is not something
   // anybody can read at a glance. Colour says which way and length says how
   // far, so neither has to be inferred from the other, and the length still
   // says it on its own for anybody who cannot tell the two hues apart.
   //
   // Amber at hue 31 against the accent's 82: far enough apart to be told
   // apart instantly, and still a lamp colour on a dark machine rather than a
   // warning light.
   static constexpr Rgb kOctaveDown = {0.961, 0.549, 0.129};
   static constexpr double kSeqOctBoxW = 22.0;
   static constexpr double kSeqRowH = 13.0;  // one semitone
   static constexpr double kSeqLaneH = 16.0; // one flag lane
   static constexpr double kSeqLabelW = 46.0;
   static constexpr double kSeqPad = 8.0;
   static constexpr double kSeqLaneGap = 5.0;
   // The scrollbar strip under the lanes, for a pattern longer than the grid
   // draws. Always reserved, drawn only when there is something to scroll.
   static constexpr double kSeqScrollH = 12.0;

   // How many columns the grid draws. Sixteen for a pattern of sixteen or
   // fewer -- which is every pattern this instrument had before Steps reached
   // 128, and is why a sixteen-step line looks exactly as it always did --
   // and thirty-two for anything longer, at half the cell width.
   //
   // Two sizes rather than a continuum: a cell that changed width every time
   // Steps moved would make the grid squirm under the pointer, and the second
   // size is the last one at which the octave box, the note block and the
   // three flag squares are all still things a person can hit.
   static constexpr int kSeqColsShort = 16;
   static constexpr int kSeqColsLong = 32;

   // The bank: eight by eight, and the two chain controls under it.
   static constexpr double kBankW = 232.0;
   static constexpr int kBankCols = 8;
   static constexpr double kBankCellH = 24.0;
   static constexpr double kBankRowGap = 8.0;
   static constexpr double kBankCtlH = 20.0;
   static constexpr double kBankCtlLabelW = 50.0;

   bool hasPattern() const { return mSpec.pattern && mSpec.patternSteps > 0; }
   bool hasBank() const { return hasPattern() && mSpec.patternCount > 1; }

   static double seqPaneHeight() { return kSeqPaneHeight; }
   static_assert(kSeqTitleH + kSeqNumH + kSeqOctH + 12.0 * kSeqRowH + kSeqLaneGap +
                       kNumLanes * kSeqLaneH + kSeqScrollH + kSeqPad ==
                    static_cast<double>(kSeqPaneHeight),
                 "kSeqPaneHeight no longer matches the parts of the grid");

   // How long the pattern is, from the Steps parameter, clamped to what this
   // plugin says it can hold.
   int seqLen() const {
      const int n = hasPattern() ? mSpec.pattern->seqLength() : 0;
      const int cap = mSpec.patternSteps > kMaxSteps ? kMaxSteps : mSpec.patternSteps;
      return n < 1 ? 1 : (n > cap ? cap : n);
   }

   // How many columns are on screen. Never more than the plugin has, so a
   // plugin with a twelve-step pattern still draws twelve.
   int seqCols() const {
      const int cap = mSpec.patternSteps > kMaxSteps ? kMaxSteps : mSpec.patternSteps;
      const int want = seqLen() <= kSeqColsShort ? kSeqColsShort : kSeqColsLong;
      return want > cap ? cap : want;
   }

   // The first step on screen. Zero whenever the whole pattern fits, which is
   // what keeps a sixteen-step pattern behaving exactly as it did.
   int seqMaxScroll() const { return seqLen() - seqCols() < 0 ? 0 : seqLen() - seqCols(); }
   int seqScroll() const {
      const int max = seqMaxScroll();
      return mSeqScroll < 0 ? 0 : (mSeqScroll > max ? max : mSeqScroll);
   }

   // Which step a drawn column is.
   int seqStepOfCol(int col) const { return seqScroll() + col; }

   void seqScrollTo(int first) {
      const int max = seqMaxScroll();
      const int next = first < 0 ? 0 : (first > max ? max : first);
      if (next != mSeqScroll) {
         mSeqScroll = next;
         mDirty = true;
      }
   }

   // Bring the playing step into view, so a long pattern shows what it is
   // doing rather than whichever page was last looked at. Only while the
   // sequencer is running and only when the step has actually left the
   // screen -- scrolling under an editing hand would be worse than useless.
   void seqFollowPlayhead(int head) {
      if (head < 0 || seqMaxScroll() == 0 || mSeqDragMode >= 0 || mSeqScrollDrag)
         return;
      const int first = seqScroll();
      const int cols = seqCols();
      if (head >= first && head < first + cols)
         return;
      seqScrollTo((head / cols) * cols);
   }
   // Which pattern the grid is editing.
   int editPattern() const { return hasPattern() ? mSpec.pattern->seqPattern() : 0; }
   double seqCellW() const {
      return (mSeqRect.w - 2.0 * kSeqPad - kSeqLabelW) / static_cast<double>(seqCols());
   }
   double seqGridX() const { return mSeqRect.x + kSeqPad + kSeqLabelW; }
   double seqOctY() const { return mSeqRect.y + kSeqTitleH + kSeqNumH; }
   double seqPitchY() const { return seqOctY() + kSeqOctH; }
   double seqLanesY() const { return seqPitchY() + 12.0 * kSeqRowH + kSeqLaneGap; }
   double seqScrollY() const { return seqLanesY() + kNumLanes * kSeqLaneH + 2.0; }

   // The scrollbar: a track the width of the grid with a thumb whose length is
   // the visible fraction of the pattern.
   Rect seqScrollTrack() const {
      return {seqGridX(), seqScrollY(), seqCellW() * seqCols(), kSeqScrollH - 4.0};
   }
   Rect seqScrollThumb() const {
      const Rect track = seqScrollTrack();
      const int len = seqLen();
      const double frac = static_cast<double>(seqCols()) / static_cast<double>(len);
      const double w = std::max(24.0, track.w * (frac > 1.0 ? 1.0 : frac));
      const double t = seqMaxScroll() > 0
                          ? seqScroll() / static_cast<double>(seqMaxScroll())
                          : 0.0;
      return {track.x + (track.w - w) * t, track.y, w, track.h};
   }

   // Which *step* a horizontal position is over, or -1. The column it is in is
   // relative to the scroll position; everything above this line works in
   // steps, so that an edit lands on the step the pointer is over whatever is
   // on screen.
   int seqColAt(double x) const {
      const double rel = (x - seqGridX()) / seqCellW();
      const int c = static_cast<int>(std::floor(rel));
      if (rel < 0.0 || c >= seqCols())
         return -1;
      const int step = seqStepOfCol(c);
      return step >= kMaxSteps ? -1 : step;
   }

   // PREV and NEXT are two buttons with two jobs. In map mode they are learn
   // targets, because a pad that steps the bank is the first thing anybody
   // assigns; the rest of the time they do the stepping themselves, which is
   // what makes the two agree by construction -- both go through
   // seqStepPattern().
   void mapTargetOrStep(int action, int delta, unsigned button) {
      if (mMapMode) {
         if (button == kButtonLeft)
            mSpec.pattern->seqSetLearnTarget(action);
         else
            mSpec.pattern->seqClearMap(action);
      } else {
         mSpec.pattern->seqStepPattern(delta);
      }
      mDirty = true;
   }

   void seqEdit(int step, const Step &value) {
      if (step < 0 || step >= kMaxSteps)
         return;
      mSpec.pattern->seqSetStep(editPattern(), step, value);
      mDirty = true;
   }

   // Whether the step is held into the one after it -- see stepsTied().
   bool seqTied(int pattern, int step) const {
      if (step < 0 || step + 1 >= kMaxSteps)
         return false;
      return stepsTied(mSpec.pattern->seqStep(pattern, step),
                       mSpec.pattern->seqStep(pattern, step + 1));
   }

   // Write a long note over the steps from `from` to `to`, either way round and
   // both ends included: every step takes the note and the anchor's octave, and
   // every one but the last slides into the next. A long note is nothing more
   // than that, so dragging one across eight steps writes the eight steps
   // somebody would otherwise have entered and tied by hand.
   //
   // Dragging back over a run shortens it rather than erasing it: the step the
   // drag now ends on stops sliding, and whatever was painted past it stays as
   // a note of its own. Pulling a slide out of the middle in the SLIDE lane
   // splits the run the same way, and putting it back joins it again -- that
   // needs no code here, because the slide flag *is* the tie.
   void seqPaintRun(int from, int to, int note) {
      const int lo = from < to ? from : to;
      const int hi = from < to ? to : from;
      const int octave = mSpec.pattern->seqStep(editPattern(), from).octave;
      for (int step = lo; step <= hi; ++step) {
         Step st = mSpec.pattern->seqStep(editPattern(), step);
         st.note = note;
         st.octave = octave;
         st.slide = step < hi;
         seqEdit(step, st);
      }
   }

   // ----------------------------------------------------------- bank layout

   int bankCount() const {
      const int n = mSpec.patternCount;
      return n > kMaxPatterns ? kMaxPatterns : n;
   }
   int bankRows() const { return (bankCount() + kBankCols - 1) / kBankCols; }
   double bankCellW() const { return (mBankRect.w - 2.0 * kSeqPad) / kBankCols; }
   double bankGridY() const { return mBankRect.y + kSeqTitleH; }

   Rect bankCellRect(int index) const {
      const double w = bankCellW();
      return {mBankRect.x + kSeqPad + (index % kBankCols) * w,
              bankGridY() + (index / kBankCols) * kBankCellH, w - 2.0, kBankCellH - 2.0};
   }

   int bankCellAt(double x, double y) const {
      for (int i = 0; i < bankCount(); ++i)
         if (bankCellRect(i).contains(x, y))
            return i;
      return -1;
   }

   double bankCtlY(int row) const {
      return bankGridY() + bankRows() * kBankCellH + kBankRowGap + row * (kBankCtlH + 4.0);
   }

   // DEL, COPY and PASTE sit in the bank's title row, beside the word
   // PATTERNS. There is no room under the chain controls -- the pane's height
   // is fixed by the step grid beside it and the eight rows already reach the
   // bottom of it.
   Rect bankDeleteRect() const {
      return hasBank() ? Rect{mBankRect.x + mBankRect.w - kSeqPad - 134.0, mBankRect.y + 4.0,
                              36.0, 14.0}
                       : Rect{0, 0, 0, 0};
   }
   Rect bankCopyRect() const {
      return hasBank() ? Rect{mBankRect.x + mBankRect.w - kSeqPad - 94.0, mBankRect.y + 4.0, 44.0,
                              14.0}
                       : Rect{0, 0, 0, 0};
   }
   Rect bankPasteRect() const {
      return hasBank() ? Rect{mBankRect.x + mBankRect.w - kSeqPad - 46.0, mBankRect.y + 4.0, 46.0,
                              14.0}
                       : Rect{0, 0, 0, 0};
   }

   Rect bankCtlRect(int row) const {
      return {mBankRect.x + kSeqPad + kBankCtlLabelW, bankCtlY(row),
              mBankRect.w - 2.0 * kSeqPad - kBankCtlLabelW, kBankCtlH};
   }

   // ------------------------------------------------------------ pane paint

   void drawSequencer(cairo_t *cr) {
      if (!hasPattern())
         return;
      const Theme &t = mSpec.theme;
      const int cols = seqCols();
      const int length = seqLen();
      const int head = mSpec.pattern->seqPlayhead();
      const int pat = editPattern();
      mLastPlayhead = head;
      mLastPlayingPattern = mSpec.pattern->seqPlayingPattern();
      const bool live = mSpec.pattern->seqEnabled();
      // The playhead only belongs on this grid while the pattern it is in is
      // the one on screen. A chain that has moved on leaves the editor showing
      // a pattern nothing is playing, and a cursor running through it would be
      // a lie.
      const bool headHere = live && mLastPlayingPattern == pat;
      if (headHere)
         seqFollowPlayhead(head);
      const int first = seqScroll();
      const double cw = seqCellW();
      const double gx = seqGridX();

      roundedRect(cr, mSeqRect.x, mSeqRect.y, mSeqRect.w, mSeqRect.h, 6);
      setColor(cr, t.panelFill);
      cairo_fill_preserve(cr);
      setColor(cr, t.panelEdge);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      char title[96];
      // Which part of the pattern is on screen, once there is more of it than
      // fits. A player editing step 97 has to be told it is step 97.
      if (seqMaxScroll() > 0)
         std::snprintf(title, sizeof(title), "PATTERN %d  %d-%d/%d", pat + 1, first + 1,
                       first + cols > length ? length : first + cols, length);
      else
         std::snprintf(title, sizeof(title), "PATTERN %d", pat + 1);
      // The title flashes with the bank cell while this pattern is queued.
      const bool waiting = mSpec.pattern->seqPendingPattern() == pat;
      setColor(cr, live && !(waiting && !pendingBlinkOn()) ? t.accent : t.textMute);
      drawText(cr, mSeqRect.x + kSeqPad, mSeqRect.y + 15, title, 9.5, true, Align::Left);
      if (mMapMode) {
         const int target = mSpec.pattern->seqLearnTarget();
         char hint[128];
         if (target == kNoteNone)
            std::snprintf(hint, sizeof(hint),
                          "- MAP: click a pattern, PREV or NEXT, then play the note for it");
         else if (target == kNotePrevPattern || target == kNoteNextPattern)
            std::snprintf(hint, sizeof(hint), "- play the note for %s",
                          target == kNotePrevPattern ? "PREV" : "NEXT");
         else
            std::snprintf(hint, sizeof(hint), "- play the note for pattern %d", target + 1);
         setColor(cr, t.accent);
         drawText(cr, mSeqRect.x + kSeqPad + 76, mSeqRect.y + 15, hint, 9.0, false, Align::Left);
      } else if (!live) {
         setColor(cr, t.textMute);
         drawText(cr, mSeqRect.x + kSeqPad + 76, mSeqRect.y + 15,
                  "- Mode is set to MIDI, the host is playing it", 9.0, false, Align::Left);
      }

      // Empty it, walk it sideways under the bar, or generate a new one. The
      // seed is shown between its two buttons, because a pattern you like is a
      // number worth writing down.
      drawSeqButton(cr, mSeqMidiRect, "MIDI");
      drawSeqButton(cr, mSeqClearRect, "CLEAR");
      drawSeqButton(cr, mSeqLeftRect, "<");
      drawSeqButton(cr, mSeqRightRect, ">");
      drawSeqButton(cr, mSeqSeedDownRect, "-");
      drawSeqButton(cr, mSeqSeedUpRect, "+");
      drawSeqButton(cr, mSeqGenRect, "GEN");
      if (mEntryParam == static_cast<int>(kParamRandSeed)) {
         // Typed, because a range this wide cannot be reached with two step
         // buttons. Drawn in the seed's own box rather than in a panel cell,
         // which is the whole reason the field carries its rect with it.
         drawEntryField(cr, mSeqSeedLabelRect);
      } else {
         char seed[32];
         std::snprintf(seed, sizeof(seed), "SEED %u", mSpec.pattern->seqSeed());
         setColor(cr, mSeqSeedLabelRect.contains(mMouseX, mMouseY) ? t.text : t.textDim);
         drawText(cr, mSeqSeedLabelRect.x + mSeqSeedLabelRect.w * 0.5,
                  mSeqSeedLabelRect.y + mSeqSeedLabelRect.h - 4.0, seed, 8.0, false,
                  Align::Center);
      }

      // Live mode's group. PREV and NEXT step the bank and are useful in any
      // mode; MAP is what turns the bank into the pad layout, and it is only
      // reachable where a pad layout means something.
      drawSeqButton(cr, mSeqOctDownRect, "-");
      drawSeqButton(cr, mSeqOctUpRect, "+");
      {
         const int oct = mSpec.pattern->seqPatternOctave();
         char label[24];
         std::snprintf(label, sizeof(label), "OCT %+d", oct);
         setColor(cr, oct != 0 ? t.accent : t.textDim);
         drawText(cr, mSeqOctLabelRect.x + mSeqOctLabelRect.w * 0.5,
                  mSeqOctLabelRect.y + mSeqOctLabelRect.h - 4.0, label, 8.0, oct != 0,
                  Align::Center);
      }
      drawStepButton(cr, mSeqPrevPatRect, "PREV", kNotePrevPattern);
      drawStepButton(cr, mSeqNextPatRect, "NEXT", kNoteNextPattern);
      {
         const bool armed = mMapMode;
         roundedRect(cr, mSeqMapRect.x, mSeqMapRect.y, mSeqMapRect.w, mSeqMapRect.h, 3);
         setColor(cr, armed ? t.accent : (mSeqMapRect.contains(mMouseX, mMouseY) ? t.track
                                                                                : t.knobFace),
                  mSpec.pattern->seqLive() ? 1.0 : 0.5);
         cairo_fill_preserve(cr);
         setColor(cr, t.panelEdge, mSpec.pattern->seqLive() ? 1.0 : 0.5);
         cairo_set_line_width(cr, 1.0);
         cairo_stroke(cr);
         setColor(cr, armed ? t.bgBottom : t.textDim, mSpec.pattern->seqLive() ? 1.0 : 0.45);
         drawText(cr, mSeqMapRect.x + mSeqMapRect.w * 0.5, mSeqMapRect.y + mSeqMapRect.h - 5.0,
                  "MAP", 8.0, armed, Align::Center);
      }

      // The playhead, behind everything, so the column it marks reads as lit
      // rather than as covered over.
      if (headHere && head >= first && head < first + cols) {
         setColor(cr, t.accent, 0.13);
         cairo_rectangle(cr, gx + (head - first) * cw, seqOctY() - 2, cw,
                         seqScrollY() - 2.0 - (seqOctY() - 2));
         cairo_fill(cr);
      }

      // Step numbers, in fours. A bass line is counted in fours and a grid
      // without the grouping is a wall of identical boxes.
      for (int c = 0; c < cols; ++c) {
         const int step = first + c;
         const bool inPattern = step < length;
         const bool bar = (step % 4) == 0;
         char num[16];
         std::snprintf(num, sizeof(num), "%d", step + 1);
         setColor(cr, inPattern ? (bar ? t.textDim : t.textMute) : t.textMute,
                  inPattern ? 1.0 : 0.4);
         drawText(cr, gx + (c + 0.5) * cw, mSeqRect.y + kSeqTitleH + 10, num, 8.5, bar,
                  Align::Center);
      }

      const double gridTop = seqOctY();
      const double gridBottom = seqLanesY() + kNumLanes * kSeqLaneH;

      // Bar lines every fourth step, and a dimmer one between.
      for (int c = 0; c <= cols; ++c) {
         const bool bar = ((first + c) % 4) == 0;
         setColor(cr, t.panelEdge, bar ? 1.0 : 0.45);
         cairo_set_line_width(cr, 1.0);
         cairo_move_to(cr, gx + c * cw, gridTop);
         cairo_line_to(cr, gx + c * cw, gridBottom);
         cairo_stroke(cr);
      }

      // Anything past the pattern's length is not played, and says so.
      if (length - first < cols) {
         const double from = length - first < 0 ? 0.0 : (length - first) * cw;
         setColor(cr, t.bgBottom, 0.55);
         cairo_rectangle(cr, gx + from, gridTop, cols * cw - from, gridBottom - gridTop);
         cairo_fill(cr);
      }

      // ---- the octave row. Two halves of one box: the top is up, the bottom
      // is down, and the half that is set is filled. Big enough to aim at,
      // which the fourteen-by-ten box it replaced was not.
      setColor(cr, t.textMute);
      drawText(cr, gx - 6, seqOctY() + 17, "OCT", 8.0, false, Align::Right);
      const double boxH = kSeqOctH - 6.0;
      const double boxW = std::min(kSeqOctBoxW, cw - 6.0);
      for (int c = 0; c < cols; ++c) {
         const Step st = mSpec.pattern->seqStep(pat, first + c);
         const double bx = gx + c * cw + (cw - boxW) * 0.5;
         const double by = seqOctY() + 3.0;
         roundedRect(cr, bx, by, boxW, boxH, 3.0);
         setColor(cr, t.knobFace);
         cairo_fill_preserve(cr);
         setColor(cr, t.panelEdge);
         cairo_set_line_width(cr, 1.0);
         cairo_stroke(cr);
         if (st.note >= 0 && st.octave != 0) {
            // Which way, in the colour; how far, in the length. One octave
            // fills its half of the box, two reach across the seam and leave a
            // clear third of it empty on the other side, so the edge the bar
            // grows from stays visible at both distances.
            const bool up = st.octave > 0;
            const int steps = st.octave > 1 || st.octave < -1 ? 2 : 1;
            const double h = steps == 2 ? boxH * 0.62 : boxH * 0.5 - 1.5;
            setColor(cr, up ? t.accent : kOctaveDown, steps == 2 ? 1.0 : 0.85);
            roundedRect(cr, bx + 1.5, up ? by + 1.5 : by + boxH - h - 1.5, boxW - 3.0, h, 2.0);
            cairo_fill(cr);
         }
         // The seam between the two halves, so both read as targets even on a
         // step that has no octave set.
         setColor(cr, t.panelEdge, 0.7);
         cairo_set_line_width(cr, 1.0);
         cairo_move_to(cr, bx + 3.0, std::floor(by + boxH * 0.5) + 0.5);
         cairo_line_to(cr, bx + boxW - 3.0, std::floor(by + boxH * 0.5) + 0.5);
         cairo_stroke(cr);
      }

      // ---- the pitch grid. B at the top, C at the bottom, like a keyboard
      // stood on end.
      static const char *const kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                             "F#", "G",  "G#", "A",  "A#", "B"};
      static const bool kBlack[12] = {false, true, false, true, false, false,
                                      true,  false, true, false, true, false};
      for (int row = 0; row < 12; ++row) {
         const int note = 11 - row; // row 0 is B
         const double ry = seqPitchY() + row * kSeqRowH;
         // The black keys get a darker lane, so the grid reads as a keyboard
         // rather than as squared paper.
         if (kBlack[note]) {
            setColor(cr, t.bgBottom, 0.5);
            cairo_rectangle(cr, gx, ry, cols * cw, kSeqRowH);
            cairo_fill(cr);
         }
         setColor(cr, note == 0 ? t.textDim : t.textMute, kBlack[note] ? 0.75 : 1.0);
         drawText(cr, gx - 6, ry + kSeqRowH - 3.5, kNames[note], 8.0, note == 0, Align::Right);

         for (int c = 0; c < cols; ++c) {
            const Step st = mSpec.pattern->seqStep(pat, first + c);
            if (st.note != note)
               continue;
            // A tied run -- consecutive steps on the same pitch, each sliding
            // into the next -- is one long note and is drawn as one bar. Only
            // the step that starts it draws; the ones it holds through are
            // skipped, and a run scrolled in from the left starts at the first
            // visible column so it reads as continuing off the edge.
            if (c > 0 && seqTied(pat, first + c - 1))
               continue;
            int last = c;
            while (last + 1 < cols && seqTied(pat, first + last))
               ++last;
            // An accented step is drawn bright, because an accent is the one
            // thing you look for when reading somebody's pattern. A run takes
            // the accent of the step that articulates it -- the ones it holds
            // through never retrigger, so theirs is the accent lane's business
            // and not the bar's.
            const double alpha = first + c < length ? (st.accent ? 1.0 : 0.62) : 0.28;
            setColor(cr, st.accent ? t.highlight : t.accent, alpha);
            roundedRect(cr, gx + c * cw + 2.0, ry + 1.5, (last - c) * cw + cw - 4.0,
                        kSeqRowH - 3.0, 2.0);
            cairo_fill(cr);
         }
      }

      // ---- the three flag lanes
      static const char *const kLaneNames[kNumLanes] = {"SLIDE", "ACCENT", "VIB"};
      for (int lane = 0; lane < kNumLanes; ++lane) {
         const double ly = seqLanesY() + lane * kSeqLaneH;
         setColor(cr, t.textMute);
         drawText(cr, gx - 6, ly + kSeqLaneH - 5.0, kLaneNames[lane], 8.0, false, Align::Right);
         for (int c = 0; c < cols; ++c) {
            const Step st = mSpec.pattern->seqStep(pat, first + c);
            const bool on = st.flag(lane);
            const double bx = gx + c * cw + cw * 0.5 - 5.0;
            const double by = ly + (kSeqLaneH - 10.0) * 0.5;
            roundedRect(cr, bx, by, 10.0, 10.0, 2.0);
            if (on) {
               setColor(cr, t.accent, first + c < length ? 0.95 : 0.3);
               cairo_fill(cr);
            } else {
               setColor(cr, t.knobFace);
               cairo_fill_preserve(cr);
               setColor(cr, t.panelEdge);
               cairo_set_line_width(cr, 1.0);
               cairo_stroke(cr);
            }
         }
      }

      // ---- the scrollbar, for a pattern longer than the grid draws. Drag the
      // thumb, or use the wheel anywhere over the grid.
      if (seqMaxScroll() > 0) {
         const Rect track = seqScrollTrack();
         setColor(cr, t.text, 0.08);
         roundedRect(cr, track.x, track.y, track.w, track.h, 3.0);
         cairo_fill(cr);
         const Rect thumb = seqScrollThumb();
         setColor(cr, t.accent, mSeqScrollDrag || thumb.contains(mMouseX, mMouseY) ? 0.8 : 0.55);
         roundedRect(cr, thumb.x, thumb.y, thumb.w, thumb.h, 3.0);
         cairo_fill(cr);
      }
   }

   // -------------------------------------------------------------- the bank
   //
   // Sixty-four patterns as an eight by eight grid, and under it the two
   // controls that say what order they play in. Three ordinary parameters --
   // automated and saved like any other -- drawn here rather than on a panel
   // because this is where they are used.

   // A pattern waiting for the playing one to end flashes, about twice a
   // second, until it takes over.
   static bool pendingBlinkOn() {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
      return (ms / 250) % 2 == 0;
   }

   void drawBank(cairo_t *cr) {
      if (!hasBank())
         return;
      const Theme &t = mSpec.theme;
      const int selected = editPattern();
      const int playing = mSpec.pattern->seqPlayingPattern();
      mLastPending = mSpec.pattern->seqPendingPattern();
      mLastBlinkOn = pendingBlinkOn();
      const int chainLen = paneValue(mSpec.chainLengthParam, 1);
      const int chainMode = paneValue(mSpec.chainModeParam, 0);
      // Stay never leaves the selected pattern, so shading a chain it does not
      // use would only claim something untrue.
      const bool chained = chainMode != kChainStay;
      const bool mapping = mMapMode && mSpec.pattern->seqLive();
      const int target = mapping ? mSpec.pattern->seqLearnTarget() : kNoteNone;

      roundedRect(cr, mBankRect.x, mBankRect.y, mBankRect.w, mBankRect.h, 6);
      setColor(cr, t.panelFill);
      cairo_fill_preserve(cr);
      setColor(cr, t.panelEdge);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      setColor(cr, t.accent, 0.85);
      drawText(cr, mBankRect.x + kSeqPad, mBankRect.y + 15, mapping ? "PATTERN MAP" : "PATTERNS",
               9.5, true, Align::Left);

      for (int i = 0; i < bankCount(); ++i) {
         const Rect r = bankCellRect(i);
         const bool sel = i == selected;
         const bool live = i == playing;
         const bool used = !mSpec.pattern->seqPatternEmpty(i);
         const bool inChain = chained && i < chainLen;
         const bool hot = r.contains(mMouseX, mMouseY);

         roundedRect(cr, r.x, r.y, r.w, r.h, 3.0);
         // A used pattern is filled, an empty one is not, and the ones the
         // chain will reach are lit a little either way: the grid answers
         // "what is written" and "what will play" at the same glance.
         if (sel)
            setColor(cr, t.accent, 0.30);
         else if (used)
            setColor(cr, t.track, inChain ? 0.95 : 0.65);
         else
            setColor(cr, t.knobFace, inChain ? 1.0 : 0.6);
         cairo_fill_preserve(cr);
         setColor(cr, sel ? t.accent : (live ? t.highlight : t.panelEdge),
                  hot && !sel ? 0.8 : 1.0);
         cairo_set_line_width(cr, sel || live ? 1.6 : 1.0);
         cairo_stroke(cr);

         char num[16];
         std::snprintf(num, sizeof(num), "%d", i + 1);
         const bool flash = i == mLastPending && mLastBlinkOn && !mapping;
         if (flash) {
            roundedRect(cr, r.x, r.y, r.w, r.h, 3.0);
            setColor(cr, t.accent, 0.85);
            cairo_fill(cr);
         }
         setColor(cr, flash ? t.bgBottom : (sel ? t.text : (used ? t.textDim : t.textMute)),
                  used || sel ? 1.0 : 0.75);
         drawText(cr, r.x + r.w * 0.5, r.y + r.h - 7.0, num, 8.0, sel, Align::Center);

         // In map mode the number is replaced by the note that selects it, so
         // the grid reads as the controller rather than as the bank. A cell
         // waiting for its note is filled in the accent, which is the only
         // thing on screen that is.
         if (mapping) {
            const int note = mSpec.pattern->seqMappedNote(i);
            const bool arming = target == i;
            if (arming || note >= 0) {
               roundedRect(cr, r.x, r.y, r.w, r.h, 3.0);
               setColor(cr, t.accent, arming ? 0.85 : 0.22);
               cairo_fill(cr);
            }
            char label[8];
            setColor(cr, arming ? t.bgBottom : (note >= 0 ? t.text : t.textMute),
                     note >= 0 || arming ? 1.0 : 0.6);
            drawText(cr, r.x + r.w * 0.5, r.y + r.h - 7.0,
                     note >= 0 ? noteLabel(note, label, sizeof(label)) : num, 8.0, arming,
                     Align::Center);
         }
      }

      // DEL empties the selected slot. It is the same edit CLEAR makes on the
      // grid, put where the patterns are: emptying one you are not editing is
      // a thing you do while looking at the bank, and going to the grid to do
      // it means selecting the pattern first, which loses your place.
      drawSeqButton(cr, bankDeleteRect(), "DEL", !mSpec.pattern->seqPatternEmpty(selected));
      drawSeqButton(cr, bankCopyRect(), "COPY");
      drawSeqButton(cr, bankPasteRect(), "PASTE", mPatternClipHeld);

      drawBankControl(cr, 0, mSpec.chainModeParam, "CHAIN");
      drawBankControl(cr, 1, mSpec.chainLengthParam, "LENGTH");
   }

   // One of the two chain controls: a label, a value, and an arrow at each end
   // that steps it. The value is also dragged like a knob, which is the only
   // civilised way to reach 64 from 1.
   void drawBankControl(cairo_t *cr, int row, uint32_t id, const char *label) {
      if (!hasParam(id))
         return;
      const Theme &t = mSpec.theme;
      const Rect r = bankCtlRect(row);
      const bool hot = r.contains(mMouseX, mMouseY) || mDrag == static_cast<int>(id);

      setColor(cr, t.textMute);
      drawText(cr, mBankRect.x + kSeqPad, r.y + r.h - 6.0, label, 8.0, true, Align::Left);

      roundedRect(cr, r.x, r.y, r.w, r.h, 3.0);
      setColor(cr, t.knobFace);
      cairo_fill_preserve(cr);
      setColor(cr, hot ? t.accent : t.panelEdge, hot ? 0.7 : 1.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      setColor(cr, t.textMute);
      drawTriangle(cr, r.x + 9, r.y + r.h * 0.5, 7, -1);
      drawTriangle(cr, r.x + r.w - 9, r.y + r.h * 0.5, 7, 1);

      char text[64];
      if (!paramValueToText(mSpec.params[id], mDelegate.guiParamValue(id), text, sizeof(text)))
         std::snprintf(text, sizeof(text), "--");
      setColor(cr, t.text);
      drawText(cr, r.x + r.w * 0.5, r.y + r.h - 6.0, text, 9.5, false, Align::Center);
   }

   int paneValue(uint32_t id, int fallback) const {
      if (!hasParam(id))
         return fallback;
      return static_cast<int>(std::floor(mDelegate.guiParamValue(id) + 0.5));
   }

   // A MIDI note as it is written on a keyboard. Key 36 is C2, which is the
   // same convention the manual uses for the transpose and the one the dragged
   // MIDI file is written in, so a pad named here is the pad named there.
   static const char *noteLabel(int note, char *buf, size_t size) {
      static const char *const kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                             "F#", "G",  "G#", "A",  "A#", "B"};
      const int n = note < 0 ? 0 : (note > 127 ? 127 : note);
      std::snprintf(buf, size, "%s%d", kNames[n % 12], n / 12 - 1);
      return buf;
   }

   // PREV or NEXT. Outside map mode it is an ordinary button; inside it, it
   // is a learn target and says which note it answers to, exactly as the bank
   // cells do -- the two are the same gesture and have to look like it.
   void drawStepButton(cairo_t *cr, const Rect &r, const char *label, int action) {
      if (!mMapMode || !mSpec.pattern->seqLive()) {
         drawSeqButton(cr, r, label);
         return;
      }
      const Theme &t = mSpec.theme;
      const int note = mSpec.pattern->seqMappedNote(action);
      const bool arming = mSpec.pattern->seqLearnTarget() == action;
      char buf[8];
      roundedRect(cr, r.x, r.y, r.w, r.h, 3);
      setColor(cr, arming || note >= 0 ? t.accent : t.knobFace, arming ? 0.85 : 0.22);
      cairo_fill_preserve(cr);
      setColor(cr, t.panelEdge);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      setColor(cr, arming ? t.bgBottom : (note >= 0 ? t.text : t.textDim));
      drawText(cr, r.x + r.w * 0.5, r.y + r.h - 5.0,
               note >= 0 ? noteLabel(note, buf, sizeof(buf)) : label, 8.0, arming,
               Align::Center);
   }

   void drawSeqButton(cairo_t *cr, const Rect &r, const char *label, bool enabled = true) {
      const Theme &t = mSpec.theme;
      const bool hot = enabled && r.contains(mMouseX, mMouseY);
      roundedRect(cr, r.x, r.y, r.w, r.h, 3);
      setColor(cr, hot ? t.track : t.knobFace, enabled ? 1.0 : 0.5);
      cairo_fill_preserve(cr);
      setColor(cr, t.panelEdge, enabled ? 1.0 : 0.5);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      setColor(cr, hot ? t.text : t.textDim, enabled ? 1.0 : 0.45);
      drawText(cr, r.x + r.w * 0.5, r.y + r.h - 5.0, label, 8.0, false, Align::Center);
   }

   // ------------------------------------------------------------ pane events

   // Copy and paste between slots. The clipboard is the editor's, not the
   // plugin's: it is a thing you are in the middle of doing, so it lives as
   // long as the window does and is not in the preset, the state blob or the
   // parameter list. Paste writes whichever slot is selected *now*, so the
   // gesture is copy, click another slot, paste.
   void patternCopy() {
      const int pat = editPattern();
      for (int c = 0; c < kMaxSteps; ++c)
         mPatternClip[c] = mSpec.pattern->seqStep(pat, c).pack();
      mPatternClipHeld = true;
      mDirty = true;
   }

   // Empties the selected slot, all kMaxSteps of it. Like CLEAR, COPY and
   // PASTE it is not undoable, which is the deal a hardware sequencer offers
   // and the reason the copy of it you took is still in the clipboard.
   void patternDelete() {
      const int pat = editPattern();
      for (int c = 0; c < kMaxSteps; ++c)
         mSpec.pattern->seqSetStep(pat, c, Step());
      mDirty = true;
   }

   void patternPaste() {
      if (!mPatternClipHeld)
         return;
      const int pat = editPattern();
      for (int c = 0; c < kMaxSteps; ++c)
         mSpec.pattern->seqSetStep(pat, c, Step::unpack(mPatternClip[c]));
      mDirty = true;
   }

   // Drag the selected pattern into the host as a MIDI file.
   //
   // The plugin writes the file -- what goes in it is the rate, the gate, the
   // swing and the tempo, none of which the window knows about -- and the
   // window hands the path to the desktop's drag protocol. A click that never
   // moves finds no target and drops nothing, which is the right answer for a
   // button pressed by accident.
   //
   // The drag runs a nested loop and does not come back until the button does,
   // so the window stops repainting for as long as it lasts. That is what both
   // platforms offer and what every other application does.
   void dragPatternAsMidi() {
      if (!hasPattern())
         return;
      const std::string path = mSpec.pattern->seqExportMidi();
      if (path.empty())
         return; // an empty pattern, or nowhere to write it
#if defined(_WIN32)
      dragFileOut(nullptr, reinterpret_cast<uintptr_t>(mWindow), path);
#else
      dragFileOut(mDisplay, static_cast<uintptr_t>(mWindow), path);
#endif
      mDirty = true;
   }

   // The three buttons that change a whole pattern at once. None of them is
   // undoable, which is the same deal a hardware sequencer offers.
   //
   // CLEAR empties the whole pattern rather than the part of it on screen. It
   // used to clear exactly the columns it drew, which was the same thing when
   // a pattern was sixteen steps and the grid drew sixteen; with a pattern
   // that scrolls it would leave whatever is off screen behind, and a CLEAR
   // that does not clear is worse than no button at all.
   void seqClear() {
      const int pat = editPattern();
      for (int c = 0; c < kMaxSteps; ++c)
         mSpec.pattern->seqSetStep(pat, c, Step());
      mDirty = true;
   }

   void seqShift(int by) {
      const int len = mSpec.pattern->seqLength();
      if (len <= 1)
         return;
      const int pat = editPattern();
      Step kept[kMaxSteps];
      for (int c = 0; c < len; ++c)
         kept[c] = mSpec.pattern->seqStep(pat, c);
      for (int c = 0; c < len; ++c) {
         int from = (c - by) % len;
         if (from < 0)
            from += len;
         mSpec.pattern->seqSetStep(pat, c, kept[from]);
      }
      mDirty = true;
   }

   // Returns true when the click was the bank's.
   bool onBankDown(double x, double y, unsigned button) {
      if (!hasBank() || !mBankRect.contains(x, y))
         return false;

      if (button == kButtonLeft && bankDeleteRect().contains(x, y)) {
         patternDelete();
         return true;
      }
      if (button == kButtonLeft && bankCopyRect().contains(x, y)) {
         patternCopy();
         return true;
      }
      if (button == kButtonLeft && bankPasteRect().contains(x, y)) {
         patternPaste();
         return true;
      }

      const int cell = bankCellAt(x, y);
      if (cell >= 0) {
         // In map mode the grid says what each pad does rather than which
         // pattern is being edited, so a click arms a target instead of
         // selecting. Nothing about the selected pattern moves while the map
         // is being built, which is what lets it be built while one plays.
         if (mMapMode && mSpec.pattern->seqLive()) {
            if (button == kButtonLeft)
               mSpec.pattern->seqSetLearnTarget(cell);
            else if (button == kButtonRight)
               mSpec.pattern->seqClearMap(cell);
            mDirty = true;
            return true;
         }
         if (!hasParam(mSpec.patternParam))
            return true;
         // The wheel steps through the bank rather than jumping to whichever
         // cell happens to be under the pointer, which is what a wheel is for
         // and what stops a scroll over the window changing the pattern by
         // accident.
         if (button == kWheelUp || button == kWheelDown)
            nudge(mSpec.patternParam, button == kWheelUp ? 1 : -1, false);
         else
            setParamThroughHost(mSpec.patternParam, cell + 1);
         mDirty = true;
         return true;
      }

      for (int row = 0; row < 2; ++row) {
         const uint32_t id = row == 0 ? mSpec.chainModeParam : mSpec.chainLengthParam;
         if (!hasParam(id))
            continue;
         const Rect r = bankCtlRect(row);
         if (!r.contains(x, y))
            continue;
         if (button == kButtonRight) {
            setParamThroughHost(id, mSpec.params[id].def);
            return true;
         }
         if (button == kWheelUp || button == kWheelDown) {
            nudge(id, button == kWheelUp ? 1 : -1, false);
            return true;
         }
         if (button != kButtonLeft)
            return true;
         if (x < r.x + 18.0) {
            nudge(id, -1, false);
         } else if (x > r.x + r.w - 18.0) {
            nudge(id, 1, false);
         } else {
            // The middle of the field is a knob lying on its side: the shared
            // drag machinery does not care that this one has no cell.
            mDrag = static_cast<int>(id);
            mDragStartY = y;
            mDragStartValue = mDelegate.guiParamValue(id);
            mDelegate.guiBeginEdit(id);
         }
         mDirty = true;
         return true;
      }
      return true; // inside the panel but on nothing: swallow it
   }

   // A parameter written as a complete gesture, so the host records it as one
   // edit rather than as a value that appeared from nowhere.
   void setParamThroughHost(uint32_t id, double value) {
      mDelegate.guiBeginEdit(id);
      mDelegate.guiSetParam(id, value);
      mDelegate.guiEndEdit(id);
   }

   // Returns true when the click was the pane's.
   bool onSeqDown(double x, double y, unsigned button) {
      if (!hasPattern())
         return false;
      if (onBankDown(x, y, button))
         return true;
      if (mSeqMidiRect.contains(x, y)) {
         if (button == kButtonLeft)
            dragPatternAsMidi();
         return true;
      }
      if (mSeqClearRect.contains(x, y)) {
         seqClear();
         return true;
      }
      if (mSeqGenRect.contains(x, y)) {
         // A new pattern every press. The - and + buttons are the deterministic
         // half: they step the seed and regenerate from it.
         mSpec.pattern->seqGenerateNew();
         mDirty = true;
         return true;
      }
      // The seed wraps at both ends rather than stopping, because it names a
      // pattern rather than measuring anything: one below zero is the last
      // seed, not an error. Done here in unsigned arithmetic so the whole
      // 32-bit range is reachable from the two buttons.
      if (mSeqSeedDownRect.contains(x, y)) {
         const uint32_t seed = mSpec.pattern->seqSeed();
         mSpec.pattern->seqSetSeed(seed == 0 ? mSpec.pattern->seqSeedMax() : seed - 1);
         mDirty = true;
         return true;
      }
      if (mSeqSeedUpRect.contains(x, y)) {
         const uint32_t seed = mSpec.pattern->seqSeed();
         mSpec.pattern->seqSetSeed(seed >= mSpec.pattern->seqSeedMax() ? 0 : seed + 1);
         mDirty = true;
         return true;
      }
      if (mSeqSeedLabelRect.contains(x, y)) {
         if (button == kButtonLeft)
            openEntryAt(kParamRandSeed, mSeqSeedLabelRect);
         return true;
      }

      // Live mode's group.
      if (mSeqOctDownRect.contains(x, y)) {
         mSpec.pattern->seqSetPatternOctave(mSpec.pattern->seqPatternOctave() - 1);
         mDirty = true;
         return true;
      }
      if (mSeqOctUpRect.contains(x, y)) {
         mSpec.pattern->seqSetPatternOctave(mSpec.pattern->seqPatternOctave() + 1);
         mDirty = true;
         return true;
      }
      if (mSeqOctLabelRect.contains(x, y)) {
         mSpec.pattern->seqSetPatternOctave(0); // the label is the reset
         mDirty = true;
         return true;
      }
      if (mSeqPrevPatRect.contains(x, y)) {
         mapTargetOrStep(kNotePrevPattern, -1, button);
         return true;
      }
      if (mSeqNextPatRect.contains(x, y)) {
         mapTargetOrStep(kNoteNextPattern, +1, button);
         return true;
      }
      if (mSeqMapRect.contains(x, y)) {
         if (!mSpec.pattern->seqLive())
            return true; // a pad layout does nothing in the other two modes
         if (button != kButtonLeft) {
            mSpec.pattern->seqClearMap(kNoteNone); // the whole map, at once
         } else {
            mMapMode = !mMapMode;
            mSpec.pattern->seqSetLearnTarget(kNoteNone);
         }
         mDirty = true;
         return true;
      }
      if (mSeqLeftRect.contains(x, y)) {
         seqShift(-1);
         return true;
      }
      if (mSeqRightRect.contains(x, y)) {
         seqShift(1);
         return true;
      }
      if (!mSeqRect.contains(x, y))
         return false;

      // The wheel scrolls the grid wherever it is over the pane, so a long
      // pattern is walked through without going to the scrollbar.
      if (button == kWheelUp || button == kWheelDown) {
         if (seqMaxScroll() > 0)
            seqScrollTo(seqScroll() + (button == kWheelUp ? -4 : 4));
         return true;
      }

      if (seqMaxScroll() > 0 && y >= seqScrollY() - 2.0 && y < seqScrollY() + kSeqScrollH) {
         if (button != kButtonLeft)
            return true;
         const Rect thumb = seqScrollThumb();
         if (thumb.contains(x, y)) {
            mSeqScrollDrag = true;
            mSeqScrollGrab = x - thumb.x;
         } else {
            // A click on the track jumps a screenful, the way a scrollbar
            // does, rather than teleporting the thumb under the pointer.
            seqScrollTo(seqScroll() + (x < thumb.x ? -seqCols() : seqCols()));
         }
         return true;
      }

      const int col = seqColAt(x);
      if (col < 0)
         return true; // inside the pane but off the grid: swallow it

      Step st = mSpec.pattern->seqStep(editPattern(), col);

      // The octave row: the top half steps up and the bottom half steps down,
      // one octave a click, as far as two either way. Stepping rather than
      // setting is what makes five values fit a control with two halves --
      // clicking back down through the middle is how you clear it, and the
      // right button clears it outright, the same as it does on the pitch grid.
      if (y >= seqOctY() && y < seqPitchY()) {
         if (button != kButtonLeft) {
            st.octave = 0;
         } else {
            const int dir = y < seqOctY() + kSeqOctH * 0.5 ? 1 : -1;
            const int next = st.octave + dir;
            st.octave = next < -kMaxOctave ? -kMaxOctave : (next > kMaxOctave ? kMaxOctave : next);
         }
         seqEdit(col, st);
         return true;
      }

      // The pitch grid.
      if (y >= seqPitchY() && y < seqPitchY() + 12.0 * kSeqRowH) {
         const int row = static_cast<int>((y - seqPitchY()) / kSeqRowH);
         const int note = 11 - (row < 0 ? 0 : (row > 11 ? 11 : row));
         // The right button always clears, and the left one clears a step that
         // is already on that note -- so a step can be turned off without
         // hunting for where its note is.
         if (button != kButtonLeft || st.note == note)
            st.note = -1;
         else
            st.note = note;
         // A step that has become a rest keeps no slide: there is nothing left
         // to slide out of, and the lane would show a slide on an empty step.
         if (st.note < 0)
            st.slide = false;
         seqEdit(col, st);
         mSeqDragMode = 0;
         mSeqDragNote = st.note;
         mSeqPaintAnchor = col;
         mSeqPaintNote = st.note;
         mSeqPaintRan = false;
         return true;
      }

      // The flag lanes.
      const double ly = seqLanesY();
      if (y >= ly && y < ly + kNumLanes * kSeqLaneH) {
         const int lane = static_cast<int>((y - ly) / kSeqLaneH);
         const bool want = !st.flag(lane);
         st.setFlag(lane, want);
         seqEdit(col, st);
         mSeqDragMode = 1 + lane;
         mSeqPaintValue = want;
         return true;
      }
      return true;
   }

   // The scrollbar thumb, dragged. The grab offset is kept so the thumb does
   // not jump to centre itself under the pointer on the first pixel of travel.
   void onSeqScrollMotion(double x) {
      const Rect track = seqScrollTrack();
      const Rect thumb = seqScrollThumb();
      const double travel = track.w - thumb.w;
      if (travel <= 0.0)
         return;
      const double t = (x - mSeqScrollGrab - track.x) / travel;
      seqScrollTo(static_cast<int>(std::floor(t * seqMaxScroll() + 0.5)));
   }

   // Dragging paints, which is the only civilised way to enter sixteen steps.
   void onSeqMotion(double x, double y) {
      if (mSeqDragMode < 0 || !hasPattern())
         return;
      const int col = seqColAt(x);
      if (col < 0)
         return;
      Step st = mSpec.pattern->seqStep(editPattern(), col);
      if (mSeqDragMode == 0) {
         if (y < seqPitchY() || y >= seqPitchY() + 12.0 * kSeqRowH)
            return;
         const int row = static_cast<int>((y - seqPitchY()) / kSeqRowH);
         const int note = mSeqDragNote < 0 ? -1 : 11 - (row < 0 ? 0 : (row > 11 ? 11 : row));
         // Along one row, the drag draws a single long note tied across the
         // steps it covers. Leave the row and it is a new note, and a new run
         // starts from there -- which is what keeps a diagonal drag a melody
         // rather than one impossible chord.
         if (note >= 0 && note == mSeqPaintNote && mSeqPaintAnchor >= 0) {
            // A drag that has never left the step it started on leaves that
            // step exactly as the click made it. Otherwise clicking a step in
            // the middle of a long note to move its pitch would drop its slide
            // on the first pixel of pointer jitter.
            if (col != mSeqPaintAnchor)
               mSeqPaintRan = true;
            if (mSeqPaintRan)
               seqPaintRun(mSeqPaintAnchor, col, note);
            return;
         }
         st.note = note;
         if (note < 0)
            st.slide = false;
         mSeqPaintAnchor = col;
         mSeqPaintNote = note;
         mSeqPaintRan = false;
      } else {
         st.setFlag(mSeqDragMode - 1, mSeqPaintValue);
      }
      seqEdit(col, st);
   }

   // ----------------------------------------------------------------- events

   // `None` is taken: X11 defines it as a macro.
   enum class Widget { NoWidget, Prev, Next, Name, Save, Mixer, Hold, Advanced, Reset };

#if defined(_WIN32)
   void pumpEvents() {
      MSG msg;
      while (mWindow && PeekMessageW(&msg, mWindow, 0, 0, PM_REMOVE)) {
         TranslateMessage(&msg);
         DispatchMessageW(&msg);
      }
   }

   // Called from wndProc, which is where Windows delivers what X11 hands over
   // through pumpEvents. Everything below this line is shared again.
   LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      switch (msg) {
      case WM_PAINT: {
         PAINTSTRUCT ps;
         const HDC dc = BeginPaint(mWindow, &ps);
         mDirty = true; // a paint request is not conditional on anything
         paintToDC(dc);
         EndPaint(mWindow, &ps);
         return 0;
      }
      case WM_ERASEBKGND:
         return 1; // every pixel is painted; erasing only causes a flicker
      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN: {
         SetCapture(mWindow);
         const unsigned button = msg == WM_LBUTTONDOWN ? 1u : 3u;
         onPointerDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), button, GetMessageTime(), shift);
         return 0;
      }
      case WM_LBUTTONUP:
      case WM_RBUTTONUP:
         ReleaseCapture();
         onButtonRelease();
         return 0;
      case WM_MOUSEWHEEL: {
         // Wheel coordinates arrive in screen space, unlike every other message.
         POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
         ScreenToClient(mWindow, &pt);
         const unsigned button = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 4u : 5u;
         onPointerDown(pt.x, pt.y, button, GetMessageTime(), shift);
         return 0;
      }
      case WM_MOUSEMOVE: {
         if (!mTrackingLeave) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, mWindow, 0};
            TrackMouseEvent(&tme);
            mTrackingLeave = true;
         }
         onMotion(GET_X_LPARAM(lp) / mScale, GET_Y_LPARAM(lp) / mScale, shift);
         return 0;
      }
      case WM_MOUSELEAVE:
         mTrackingLeave = false;
         if (mDrag < 0) {
            mHover = -1;
            mHoverWidget = Widget::NoWidget;
            mDirty = true;
         }
         return 0;
      case WM_KEYDOWN: {
         KeyCommand cmd = KeyCommand::NoCommand;
         if (wp == VK_ESCAPE)
            cmd = KeyCommand::Escape;
         else if (wp == VK_RETURN)
            cmd = KeyCommand::Accept;
         else if (wp == VK_BACK)
            cmd = KeyCommand::Backspace;
         else if (wp == VK_UP)
            cmd = KeyCommand::Up;
         else if (wp == VK_DOWN)
            cmd = KeyCommand::Down;
         if (!mSaveOpen && mEntryParam < 0) {
            onOverlayKey(cmd);
            return 0;
         }
         if (cmd != KeyCommand::NoCommand) {
            if (mSaveOpen)
               onSaveKey(cmd, nullptr, 0);
            else
               onEntryKey(cmd, nullptr, 0);
         }
         return 0; // the text itself arrives as WM_CHAR
      }
      case WM_CHAR: {
         if (!mSaveOpen && mEntryParam < 0)
            return 0;
         const char c = static_cast<char>(wp);
         if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
            if (mSaveOpen)
               onSaveKey(KeyCommand::NoCommand, &c, 1);
            else
               onEntryKey(KeyCommand::NoCommand, &c, 1);
         }
         return 0;
      }
      default:
         break;
      }
      return DefWindowProcW(mWindow, msg, wp, lp);
   }
#else
   void pumpEvents() {
      XEvent ev;
      while (XPending(mDisplay)) {
         XNextEvent(mDisplay, &ev);
         switch (ev.type) {
         case Expose:
            mDirty = true;
            break;
         case ButtonPress:
            onPointerDown(ev.xbutton.x, ev.xbutton.y, ev.xbutton.button, ev.xbutton.time,
                          (ev.xbutton.state & ShiftMask) != 0);
            break;
         case ButtonRelease:
            onButtonRelease();
            break;
         case MotionNotify:
            onMotion(ev.xmotion.x / mScale, ev.xmotion.y / mScale,
                     (ev.xmotion.state & ShiftMask) != 0);
            break;
         case LeaveNotify:
            if (mDrag < 0) {
               mHover = -1;
               mHoverWidget = Widget::NoWidget;
               mDirty = true;
            }
            break;
         case KeyPress: {
            char buf[32];
            KeySym sym = 0;
            const int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &sym, nullptr);
            KeyCommand cmd = KeyCommand::NoCommand;
            if (sym == XK_Escape)
               cmd = KeyCommand::Escape;
            else if (sym == XK_Return || sym == XK_KP_Enter)
               cmd = KeyCommand::Accept;
            else if (sym == XK_BackSpace)
               cmd = KeyCommand::Backspace;
            else if (sym == XK_Up || sym == XK_KP_Up)
               cmd = KeyCommand::Up;
            else if (sym == XK_Down || sym == XK_KP_Down)
               cmd = KeyCommand::Down;
            if (mSaveOpen)
               onSaveKey(cmd, buf, n);
            else if (mEntryParam >= 0)
               onEntryKey(cmd, buf, n);
            else
               onOverlayKey(cmd);
            break;
         }
         case ClientMessage:
            if (static_cast<Atom>(ev.xclient.data.l[0]) == mDeleteAtom)
               hide();
            break;
         default:
            break;
         }
      }
   }
#endif

   int cellAt(double x, double y) const {
      for (size_t i = 0; i < mCellRects.size(); ++i)
         if (mCellRects[i].contains(x, y))
            return static_cast<int>(mCells[i].param);
      return -1;
   }

   // Buttons: 1 left, 2 middle, 3 right, 4/5 wheel up/down, matching X11's
   // numbering because that is what the shared code below was written against.
   void onPointerDown(double px, double py, unsigned button, unsigned long timeMs,
                      bool shift) {
      const double x = px / mScale;
      const double y = py / mScale;
      const struct {
         unsigned button;
         unsigned long time;
      } be{button, timeMs};

      // An armed RESET survives exactly one click, which has to be the second
      // one on RESET itself. Anything else at all -- another button, a knob,
      // the grid, a miss -- puts it back to sleep.
      const bool wasArmed = mResetArmed;
      if (mResetArmed) {
         mResetArmed = false;
         mDirty = true;
      }

      if (mSaveOpen) {
         if (be.button == kButtonLeft) {
            if (saveOkRect().contains(x, y))
               commitSave();
            else if (saveCancelRect().contains(x, y) || !savePanel().contains(x, y))
               closeSaveDialog();
         }
         mDirty = true;
         return;
      }

      if (mEntryParam >= 0) {
         const bool inside = mEntryRect.contains(x, y);
         const bool second = be.button == kButtonLeft && mLastClickParam == mEntryParam &&
                             be.time - mLastClickTime < 400;
         if (inside && !second)
            return; // a click in the field it is typing into
         // Anywhere else cancels the entry, and the click then does what it
         // would have done; a double-click on the value falls through to the
         // reset it has always been.
         closeEntry();
      }

      if (mMenuParam >= 0) {
         if (be.button == kWheelUp || be.button == kWheelDown) {
            stepMenu(be.button == kWheelUp ? -1 : 1);
            return;
         }
         if (be.button == kButtonLeft) {
            const int item = menuItemAt(x, y);
            const bool inside = menuPanel().contains(x, y);
            if (item >= 0) {
               const uint32_t id = static_cast<uint32_t>(mMenuParam);
               mDelegate.guiBeginEdit(id);
               mDelegate.guiSetParam(id, static_cast<double>(item));
               mDelegate.guiEndEdit(id);
            }
            if (item >= 0 || !inside)
               closeMenu();
         }
         mDirty = true;
         return;
      }

      if (mBrowserOpen) {
         if (be.button == kWheelUp || be.button == kWheelDown) {
            scrollBrowser(be.button == kWheelUp ? -1 : 1);
            return;
         }
         if (be.button == kButtonLeft) {
            // The import list is on top of everything else in the panel, so it
            // is asked first and swallows the click either way.
            if (mImportOpen) {
               for (int row = 0; row < browserImportCount(); ++row) {
                  if (!browserImportRect(row).contains(x, y))
                     continue;
                  const int extra = row - static_cast<int>(mImportPacks.size());
                  if (extra < 0) {
                     importPack(mImportPacks[static_cast<size_t>(row)]);
                  } else if (extra == 0) {
                     mDelegate.guiKeyboardTaken();
                     importPack(openFileDialog("Import preset pack", "Preset pack",
                                               packExtensionFromPath(
                                                  mDelegate.guiPackPathFor("pack"))));
                  } else if (extra == 1) {
                     // One foreign pattern file rather than a pack. The plugin
                     // tells them apart by what the path is, so the window does
                     // not have to know the formats -- only enough to put a
                     // useful filter on the chooser.
                     mDelegate.guiKeyboardTaken();
                     importPack(openFileDialog("Import a pattern", "Pattern file", "pat"));
                  } else {
                     mDelegate.guiKeyboardTaken();
                     importPack(openFolderDialog("Import a folder of patterns"));
                  }
                  mDirty = true;
                  return;
               }
               mImportOpen = false;
               mDirty = true;
               return;
            }

            if (hasFolders()) {
               const int folder = browserFolderAt(x, y);
               if (folder != -2) {
                  mBrowserFolder = folder;
                  mBrowserScroll = 0;
                  mBrowserHover = -1;
                  mDirty = true;
                  return;
               }
               if (browserFooterRect(0).contains(x, y)) {
                  exportPack(false);
                  return;
               }
               if (fileDialogAvailable() && browserFooterRect(1).contains(x, y)) {
                  exportPack(true);
                  return;
               }
               if (browserFooterRect(2).contains(x, y)) {
                  mImportPacks = mDelegate.guiPresetPacks();
                  mImportHover = -1;
                  if (browserImportCount() == 0)
                     mBrowserStatus = "No packs found, and no file chooser to look with.";
                  else
                     mImportOpen = true;
                  mDirty = true;
                  return;
               }
            }

            const int item = browserItemAt(x, y);
            if (item >= 0)
               loadPreset(item);
            if (item >= 0 || !browserPanel().contains(x, y))
               mBrowserOpen = false;
         }
         mDirty = true;
         return;
      }

      if (mMixerOpen) {
         onMixerDown(x, y, be.button, be.time, shift);
         mDirty = true;
         return;
      }

      // The step grid, before the panels: it has its own hit testing and
      // swallows anything inside it, including the wheel, which would otherwise
      // find a knob underneath.
      if (onSeqDown(x, y, be.button)) {
         mDirty = true;
         return;
      }

      if (be.button == kWheelUp || be.button == kWheelDown) {
         const int id = cellAt(x, y);
         if (id >= 0)
            nudge(static_cast<uint32_t>(id), be.button == kWheelUp ? 1 : -1,
                  shift);
         return;
      }

      if (mPrevRect.contains(x, y)) {
         stepPreset(-1);
         return;
      }
      if (mNextRect.contains(x, y)) {
         stepPreset(1);
         return;
      }
      if (mSaveRect.contains(x, y)) {
         openSaveDialog();
         return;
      }
      if (mNameRect.contains(x, y)) {
         openBrowser();
         return;
      }
      if (be.button == kButtonLeft && hasMixer() && mMixerRect.contains(x, y)) {
         openMixer();
         return;
      }
      if (be.button == kButtonLeft && hasAdvanced() && mAdvancedRect.contains(x, y)) {
         toggleAdvanced();
         return;
      }
      if (be.button == kButtonLeft && mResetRect.contains(x, y)) {
         // Arm on the first click, act on the second. Any other click in the
         // window disarms it -- see the top of this function.
         if (wasArmed)
            resetToDefaults();
         else
            mResetArmed = true;
         mDirty = true;
         return;
      }
      if (be.button == kButtonLeft && holdsActive() && mHoldRect.contains(x, y)) {
         clearHolds();
         return;
      }
      if (be.button == kButtonLeft && mVersionRect.contains(x, y)) {
         mDelegate.guiVersionClicked();
         return;
      }

      const int id = cellAt(x, y);
      if (id < 0)
         return;
      const ParamDesc &d = mSpec.params[static_cast<uint32_t>(id)];

      // Right-click and double-click both mean "put it back where it was".
      const bool doubleClick =
         be.button == kButtonLeft && mLastClickParam == id && be.time - mLastClickTime < 400;
      mLastClickParam = id;
      mLastClickTime = be.time;

      if (be.button == kButtonRight || (doubleClick && !isChip(d))) {
         mDelegate.guiBeginEdit(static_cast<uint32_t>(id));
         mDelegate.guiSetParam(static_cast<uint32_t>(id), d.def);
         mDelegate.guiEndEdit(static_cast<uint32_t>(id));
         mLastClickParam = -1;
         mDirty = true;
         return;
      }
      if (be.button != kButtonLeft)
         return;

      if (isChip(d)) {
         // The two arrows still step by one; the name between them opens the
         // full list.
         const Rect chip = chipRect(cellRectFor(static_cast<uint32_t>(id)));
         const double chipX = chip.x;
         const double chipW = chip.w;
         if (x < chipX + 18.0) {
            nudge(static_cast<uint32_t>(id), -1, false);
         } else if (x > chipX + chipW - 18.0) {
            nudge(static_cast<uint32_t>(id), 1, false);
         } else {
            mMenuParam = id;
            mMenuHover = menuItemAt(x, y);
            mDirty = true;
         }
         return;
      }

      if (be.button == kButtonLeft &&
          valueRect(cellRectFor(static_cast<uint32_t>(id))).contains(x, y)) {
         openEntry(static_cast<uint32_t>(id));
         return;
      }

      mDrag = id;
      mDragStartY = y;
      mDragStartValue = mDelegate.guiParamValue(static_cast<uint32_t>(id));
      mDelegate.guiBeginEdit(static_cast<uint32_t>(id));
      mDirty = true;
   }

   // Every parameter back to its default, which for this instrument is the
   // machine before anybody was inside it: the mods, the Devil Fish additions
   // and the two stages that are not in the schematic all default to off or to
   // the stock circuit. What the spec's resetKeep names is left alone, because
   // it says which pattern is playing rather than what the instrument sounds
   // like, and losing that to a click on RESET would be losing the arrangement
   // along with the patch.
   //
   // Written through the delegate one parameter at a time, so the host records
   // it as automation and its own undo sees it.
   void resetToDefaults() {
      for (uint32_t i = 0; i < mSpec.paramCount; ++i) {
         bool keep = false;
         for (int k = 0; k < mSpec.resetKeepCount; ++k)
            keep = keep || mSpec.resetKeep[k] == i;
         if (keep)
            continue;
         const double def = mSpec.params[i].def;
         if (mDelegate.guiParamValue(i) == def)
            continue;
         mDelegate.guiBeginEdit(i);
         mDelegate.guiSetParam(i, def);
         mDelegate.guiEndEdit(i);
      }
      mDirty = true;
   }

   void onButtonRelease() {
      mSeqDragMode = -1;
      mSeqPaintAnchor = -1;
      mSeqScrollDrag = false;
      if (mDrag >= 0) {
         mDelegate.guiEndEdit(static_cast<uint32_t>(mDrag));
         mDrag = -1;
         mDragHoriz = false;
         mDragUnitsPerPx = 0.0;
         mDirty = true;
      }
   }

   void onMotion(double x, double y, bool shift) {
      // Kept for the step grid's own buttons, which highlight under the
      // pointer and are not parameters, so the shared hover machinery does not
      // know about them.
      if (hasPattern()) {
         const bool wasOver =
            mSeqRect.contains(mMouseX, mMouseY) || mBankRect.contains(mMouseX, mMouseY);
         mMouseX = x;
         mMouseY = y;
         if (wasOver || mSeqRect.contains(x, y) || mBankRect.contains(x, y))
            mDirty = true;
      }
      if (mSeqScrollDrag) {
         onSeqScrollMotion(x);
         return;
      }
      if (mSeqDragMode >= 0) {
         onSeqMotion(x, y);
         return;
      }
      if (mDrag >= 0) {
         const uint32_t id = static_cast<uint32_t>(mDrag);
         const ParamDesc &d = mSpec.params[id];
         const double span = d.max - d.min;
         const double fine = shift ? 0.2 : 1.0;
         // Knobs are dragged vertically at a fixed rate; the mixer's faders and
         // sliders set their own rate from their own travel, and the sliders
         // are dragged along themselves.
         const double delta = mDragHoriz ? x - mDragStartX : mDragStartY - y;
         const double perPx = mDragUnitsPerPx > 0.0 ? mDragUnitsPerPx : span / 200.0;
         double v = mDragStartValue + delta * perPx * fine;
         if (isStepped(d))
            v = std::floor(v + 0.5);
         v = std::min(d.max, std::max(d.min, v));
         mDelegate.guiSetParam(id, v);
         mDirty = true;
         return;
      }

      if (mSaveOpen)
         return;

      if (mMenuParam >= 0) {
         const int item = menuItemAt(x, y);
         if (item != mMenuHover) {
            mMenuHover = item;
            mDirty = true;
         }
         return;
      }

      if (mBrowserOpen) {
         const int item = browserItemAt(x, y);
         const int folder = hasFolders() ? browserFolderAt(x, y) : -2;
         int importRow = -1;
         if (mImportOpen)
            for (int row = 0; row < browserImportCount(); ++row)
               if (browserImportRect(row).contains(x, y))
                  importRow = row;
         // The footer buttons highlight under the pointer like the grid's do,
         // and drawSeqButton reads the pointer position for that.
         if (item != mBrowserHover || folder != mFolderHover || importRow != mImportHover ||
             hasFolders()) {
            mBrowserHover = item;
            mFolderHover = folder;
            mImportHover = importRow;
            mMouseX = x;
            mMouseY = y;
            mDirty = true;
         }
         return;
      }

      if (mMixerOpen) {
         const MixerHit h = mixerHitAt(x, y);
         if (h.strip != mMixerHit.strip || h.kind != mMixerHit.kind) {
            mMixerHit = h;
            mDirty = true;
         }
         return;
      }

      const int id = cellAt(x, y);
      Widget w = Widget::NoWidget;
      if (mPrevRect.contains(x, y))
         w = Widget::Prev;
      else if (mNextRect.contains(x, y))
         w = Widget::Next;
      else if (mNameRect.contains(x, y))
         w = Widget::Name;
      else if (mSaveRect.contains(x, y))
         w = Widget::Save;
      else if (hasMixer() && mMixerRect.contains(x, y))
         w = Widget::Mixer;
      else if (hasAdvanced() && mAdvancedRect.contains(x, y))
         w = Widget::Advanced;
      else if (mResetRect.contains(x, y))
         w = Widget::Reset;
      else if (holdsActive() && mHoldRect.contains(x, y))
         w = Widget::Hold;

      // The bank's two chain controls are parameters with no cell, so the help
      // line has to be told about them separately.
      int pane = -1;
      if (hasBank()) {
         for (int row = 0; row < 2; ++row) {
            const uint32_t pid = row == 0 ? mSpec.chainModeParam : mSpec.chainLengthParam;
            if (hasParam(pid) && bankCtlRect(row).contains(x, y))
               pane = static_cast<int>(pid);
         }
         if (pane < 0 && hasParam(mSpec.patternParam) && bankCellAt(x, y) >= 0)
            pane = static_cast<int>(mSpec.patternParam);
      }

      if (id != mHover || w != mHoverWidget || pane != mPaneHover) {
         mHover = id;
         mHoverWidget = w;
         mPaneHover = pane;
         mDirty = true;
      }
   }

   // Returns the preset's index in the plugin's list, not its place on the
   // shelf: everything outside the browser -- loading, the arrows, the name on
   // the bar -- works in those indices and must not have to know that the
   // browser is showing a subset.
   int browserItemAt(double x, double y) const {
      const std::vector<int> items = browserItems();
      for (size_t k = 0; k < items.size(); ++k) {
         if (!browserItemVisible(static_cast<int>(k)))
            continue;
         if (browserItemRect(static_cast<int>(k)).contains(x, y))
            return items[k];
      }
      return -1;
   }

   // The last component of a path, for the import list. std::filesystem is not
   // included here and this is not worth including it for.
   static std::string fileNameOf(const std::string &path) {
      const size_t cut = path.find_last_of("/\\");
      return cut == std::string::npos ? path : path.substr(cut + 1);
   }

   const Rect &cellRectFor(uint32_t id) const {
      for (size_t i = 0; i < mCells.size(); ++i)
         if (mCells[i].param == id)
            return mCellRects[i];
      return mCellRects[0];
   }

   void nudge(uint32_t id, int direction, bool fine) {
      const ParamDesc &d = mSpec.params[id];
      const double span = d.max - d.min;
      double v = mDelegate.guiParamValue(id);
      if (isStepped(d))
         v = std::floor(v + 0.5) + direction;
      else
         v += direction * span * (fine ? 0.005 : 0.02);
      v = std::min(d.max, std::max(d.min, v));
      mDelegate.guiBeginEdit(id);
      mDelegate.guiSetParam(id, v);
      mDelegate.guiEndEdit(id);
      mDirty = true;
   }

   void stepPreset(int direction) {
      const int n = static_cast<int>(mDelegate.guiPresets().size());
      if (n <= 0)
         return;
      int cur = mDelegate.guiCurrentPreset();
      if (cur < 0)
         cur = direction > 0 ? -1 : 0;
      int next = (cur + direction) % n;
      if (next < 0)
         next += n;
      loadPreset(next);
      mDirty = true;
   }

   // Every preset load goes through here. A preset replaces the levels the
   // mixer is holding down, which would leave the remembered values pointing at
   // the preset before it, so the holds are released while they still mean
   // something and the preset lands on top of the real values.
   // Opens or closes the collapsible section. The window lays itself out and
   // resizes first and asks the host second: a host that refuses the request
   // leaves the editor clipped, which is visibly wrong, where a window that
   // never relaid itself would be wrong and look fine.
   void toggleAdvanced() {
      if (!hasAdvanced())
         return;
      mAdvancedOpen = !mAdvancedOpen;
      if (mSpec.host)
         mSpec.host->windowSetAdvancedOpen(mAdvancedOpen);
      closeEntry();
      closeMenu();
      mBrowserOpen = false;
      mMixerOpen = false;
      mHover = -1;
      mHoverWidget = Widget::NoWidget;
      buildLayout();
      applySize();
      if (mSpec.host)
         mSpec.host->windowRequestResize(pixelW(), pixelH());
   }

   // The plugin owns whether the section is open, and a host can move it under
   // the window's feet by loading state or a project while the editor is up.
   // Checked once a frame rather than pushed, because a state load happens on
   // whatever thread the host chose and relaying out a window is not a thing to
   // do from there.
   void syncAdvanced() {
      if (!hasAdvanced())
         return;
      const bool want = mSpec.host->windowAdvancedOpen();
      if (want == mAdvancedOpen)
         return;
      mAdvancedOpen = want;
      closeEntry();
      closeMenu();
      mBrowserOpen = false;
      mHover = -1;
      mHoverWidget = Widget::NoWidget;
      buildLayout();
      applySize();
      mSpec.host->windowRequestResize(pixelW(), pixelH());
   }

   void loadPreset(int index) {
      clearHolds();
      mDelegate.guiLoadPreset(index);
      mDirty = true;
   }

   // ------------------------------------------------------------------ state

   static constexpr int kBrowserCols = 3;
   // Mixer geometry, in the same design pixels as everything else. The strip
   // height is built from its rows rather than written down, so moving a row
   // cannot leave the panel the wrong size for what is in it.
   static constexpr double kStripW = 96.0;
   static constexpr double kMixerPad = 14.0;
   static constexpr double kMixerGap = 18.0;
   static constexpr double kMixerTitleH = 34.0;
   static constexpr double kLabelH = 18.0;
   static constexpr double kFaderH = 132.0;
   static constexpr double kHandleH = 12.0;
   static constexpr double kValueH = 34.0;
   static constexpr double kSliderH = 10.0;
   static constexpr double kButtonH = 18.0;
   static constexpr double kMixerStripH =
      kLabelH + kFaderH + kValueH + 2 * (kSliderH + 14.0) + kButtonH + 4.0;
   // Mute is the one thing in the window that is not in the plugin's palette:
   // it means stop, and every theme's accent is the colour of go.
   static constexpr Rgb kMuteRed = {0.85, 0.35, 0.30};
   static constexpr int kBrowserPad = 14;
   static constexpr int kBrowserRowH = 26;
   static constexpr int kBrowserScrollW = 14;
   // The shelf column down the left of the browser, and the strip under it
   // that holds the two pack buttons. Both are zero for a plugin that does not
   // group its presets, and then the browser is the flat grid it always was.
   static constexpr double kBrowserFolderW = 190.0;
   static constexpr double kBrowserFooterH = 34.0;

   GuiDelegate &mDelegate;
   // By value, and that matters. A plugin describes itself once, as a constant
   // table, and fills the two per-instance pointers in -- the pattern access
   // and the window host -- just before it creates the window. Holding a
   // reference to the caller's spec meant that whatever the caller reused, the
   // window followed: SaeureKiste's createGui() kept one function-local
   // `static` spec, so opening a second instance's editor rewrote the first
   // one's, and both windows then drew and edited the second plugin's pattern
   // bank. Two tracks of the same plugin shared their patterns and their
   // pattern selection. A copy is thirty-odd scalars and it makes the window
   // own everything it reads.
   const WindowSpec mSpec;
   const int mWindowW;

#if defined(_WIN32)
   HWND mWindow = nullptr;
   HWND mPrevFocus = nullptr;
   bool mTrackingLeave = false;
#else
   Display *mDisplay = nullptr;
   Window mWindow = 0;
   Visual *mVisual = nullptr;
   Atom mDeleteAtom = 0;
#endif
   cairo_surface_t *mTarget = nullptr;
   cairo_t *mTargetCr = nullptr;
   cairo_surface_t *mBuffer = nullptr;
   cairo_t *mBufferCr = nullptr;
   double mScale = 1.0;

   std::vector<Panel> mPanels;
   std::vector<Cell> mCells;
   std::vector<Rect> mCellRects;
   Rect mPrevRect, mNameRect, mNextRect, mSaveRect, mVersionRect;
   Rect mMixerRect, mHoldRect, mAdvancedRect;
   Rect mResetRect;
   // Whether RESET is waiting for the click that confirms it.
   bool mResetArmed = false;
   int mBarY = 0, mHelpY = 0;
   bool mAdvancedOpen = false;

   bool mDirty = true;
   int mHover = -1;
   Widget mHoverWidget = Widget::NoWidget;
   int mDrag = -1;
   double mDragStartY = 0.0, mDragStartValue = 0.0;
   int mLastClickParam = -1;
   unsigned long mLastClickTime = 0; // X11 Time and Win32 GetMessageTime alike
   bool mDragHoriz = false;          // a mixer slider, dragged along itself
   double mDragStartX = 0.0;
   double mDragUnitsPerPx = 0.0;     // 0 means the knobs' fixed rate

   bool mBrowserOpen = false;
   int mBrowserHover = -1;
   int mBrowserScroll = 0; // first visible row
   // The library's shelves, as the browser lists them, and which one is on
   // screen: -1 is "All", which is the flat list the browser has always shown.
   std::vector<std::string> mFolders;
   int mBrowserFolder = -1;
   int mFolderHover = -2;
   // The packs the plugin can see, read when IMPORT is pressed rather than
   // held, so a pack copied into the folder while the editor is open is found.
   std::vector<std::string> mImportPacks;
   bool mImportOpen = false;
   int mImportHover = -1;
   // What the last export or import had to say. Cleared when the browser
   // opens, because a message about a file written ten minutes ago is noise.
   std::string mBrowserStatus;
   bool mSaveOpen = false;
   int mEntryParam = -1; // knob whose value is being typed, or -1
   Rect mEntryRect{0, 0, 0, 0}; // and where its field is drawn
   std::string mEntryText;
   bool mEntryFailed = false;
   bool mEntryFresh = false; // the current value is shown selected; typing replaces it
   bool mKeyboardGrabbed = false;
   bool mSaveFailed = false;
   std::string mSaveName;
   std::string mSaveStatus;
   int mMenuParam = -1; // enum parameter whose dropdown is open, or -1
   int mMenuHover = -1;

   bool mMixerOpen = false;

   // The step grid and the bank beside it.
   Rect mSeqRect{0, 0, 0, 0};
   Rect mBankRect{0, 0, 0, 0};
   // The pattern clipboard: sixteen packed steps and whether anything is in it.
   uint16_t mPatternClip[kMaxSteps] = {0};
   bool mPatternClipHeld = false;

   Rect mSeqClearRect{0, 0, 0, 0};
   Rect mSeqMidiRect{0, 0, 0, 0};
   Rect mSeqGenRect{0, 0, 0, 0};
   Rect mSeqSeedDownRect{0, 0, 0, 0};
   Rect mSeqSeedUpRect{0, 0, 0, 0};
   Rect mSeqSeedLabelRect{0, 0, 0, 0};
   Rect mSeqLeftRect{0, 0, 0, 0};
   Rect mSeqRightRect{0, 0, 0, 0};
   Rect mSeqMapRect{0, 0, 0, 0};
   Rect mSeqPrevPatRect{0, 0, 0, 0};
   Rect mSeqNextPatRect{0, 0, 0, 0};
   Rect mSeqOctDownRect{0, 0, 0, 0};
   Rect mSeqOctUpRect{0, 0, 0, 0};
   Rect mSeqOctLabelRect{0, 0, 0, 0};
   // Whether the bank is showing what each pad does rather than selecting
   // patterns. The window's, not the plugin's: it is a thing you are in the
   // middle of doing, like the pattern clipboard beside it.
   bool mMapMode = false;
   int mSeqDragMode = -1; // -1 none, 0 pitch, 1 + lane for a flag lane
   int mSeqDragNote = -1;
   bool mSeqPaintValue = false;
   // The long note being drawn: the step it started on and the note it is on.
   // Both only mean anything while a pitch drag is running.
   int mSeqPaintAnchor = -1;
   int mSeqPaintNote = -1;
   bool mSeqPaintRan = false; // whether it has reached a second step yet
   // Where the grid is scrolled to, in steps, and the thumb drag that moves
   // it. The window's, not the plugin's: which part of a pattern somebody is
   // looking at is not worth saving into a project, let alone a preset.
   int mSeqScroll = 0;
   bool mSeqScrollDrag = false;
   double mSeqScrollGrab = 0.0; // pointer offset inside the thumb, in pixels
   int mLastPlayhead = -1;
   int mLastPlayingPattern = -1;
   int mLastPending = -1;
   bool mLastBlinkOn = false;
   // A bank control under the pointer. Those three are parameters but they are
   // not on a panel, so the shared hover machinery cannot see them and the help
   // line would have nothing to say about the two that need explaining most.
   int mPaneHover = -1;
   double mMouseX = -1.0;
   double mMouseY = -1.0;
   MixerHit mMixerHit;
   // One entry per mixer strip. The buttons are the window's own state, not
   // the plugin's: a mute that reached the parameters would be saved into a
   // preset as a silent layer.
   std::vector<char> mMuted;
   std::vector<char> mSoloed;
   std::vector<char> mHeld;         // the mixer is holding this level down
   std::vector<double> mHeldLevel;  // and this is what it was

   // One entry per parameter; the count is only known at construction.
   std::vector<double> mShown;
};

} // namespace

#if defined(_WIN32)
// Windows hands the object back through the window's user data; the pointer is
// planted when CreateWindowEx delivers WM_NCCREATE.
LRESULT CALLBACK PluginWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
   if (msg == WM_NCCREATE) {
      auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      auto *self = static_cast<PluginWindow *>(cs->lpCreateParams);
      if (self)
         self->mWindow = hwnd;
      return DefWindowProcW(hwnd, msg, wp, lp);
   }
   auto *self = reinterpret_cast<PluginWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
   if (!self || !self->mWindow)
      return DefWindowProcW(hwnd, msg, wp, lp);
   return self->handleMessage(msg, wp, lp);
}
#endif

Gui *createWindow(GuiDelegate &delegate, const WindowSpec &spec) {
   auto *gui = new PluginWindow(delegate, spec);
   if (!gui->open()) {
      delete gui;
      return nullptr;
   }
   return gui;
}

} // namespace saeurekiste
