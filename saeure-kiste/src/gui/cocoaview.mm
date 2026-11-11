// The window's macOS backend, and the macOS half of dragfile.h. Compiled
// without ARC: every retain has its release written out.
//
// Adapted from ReSIDent's CocoaWindow (resident/src/gui/CocoaWindow.mm). The
// code is this plugin's own from here on, as everything copied between plugin
// folders is; ReSIDent is GPL, and what came across is the generic Cocoa
// plumbing, rewritten against this window's interface.

#include "gui/cocoaview.h"
#include "gui/dragfile.h"

#import <Cocoa/Cocoa.h>

#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace saeurekiste {

namespace {

// ------------------------------------------------------------------ the view class
//
// The view's class is made at run time under a name that carries the address
// this module is loaded at. Objective-C classes share one namespace per
// process: two plugin binaries defining a class of the same name -- the CLAP,
// the VST3 and the Audio Unit of this plugin, or two versions of it -- would
// each get one of the implementations, and run the other binary's code, with
// its own statically linked Cairo, over this one's windows. That is the
// Windows window-class bug again (root CLAUDE.md), and the cure is the same: a
// name per module. The class is never disposed of: a bundle with Objective-C
// in it is never unloaded, so its methods cannot dangle.

ptrdiff_t gOwnerOffset = 0;

CocoaView *&ownerOf(id self) {
   return *reinterpret_cast<CocoaView **>(reinterpret_cast<char *>(static_cast<void *>(self)) +
                                          gOwnerOffset);
}

Class superClass() { return [NSView class]; }

template <typename R, typename... A> R callSuper(id self, SEL sel, A... args) {
   struct objc_super sup = {self, superClass()};
   using Fn = R (*)(struct objc_super *, SEL, A...);
   return reinterpret_cast<Fn>(objc_msgSendSuper)(&sup, sel, args...);
}

BOOL yes(id, SEL) { return YES; }
BOOL yesFor(id, SEL, id) { return YES; }

// Only while the window has asked for the keyboard. Taking it on every click
// would take the space bar and every other shortcut away from the host.
BOOL acceptsFirstResponderImp(id self, SEL) {
   CocoaView *v = ownerOf(self);
   return v && v->wantsKeys() ? YES : NO;
}

void drawRectImp(id self, SEL, NSRect) {
   if (CocoaView *v = ownerOf(self))
      v->drawView();
}

void mouseDownImp(id self, SEL, NSEvent *e) {
   if (CocoaView *v = ownerOf(self))
      v->mouseDown(e, 1);
}
void rightMouseDownImp(id self, SEL, NSEvent *e) {
   if (CocoaView *v = ownerOf(self))
      v->mouseDown(e, 3);
}
void mouseUpImp(id self, SEL, NSEvent *e) {
   if (CocoaView *v = ownerOf(self))
      v->mouseUp(e, 1);
}
void rightMouseUpImp(id self, SEL, NSEvent *e) {
   if (CocoaView *v = ownerOf(self))
      v->mouseUp(e, 3);
}
void mouseMovedImp(id self, SEL, NSEvent *e) {
   if (CocoaView *v = ownerOf(self))
      v->mouseMoved(e);
}
void mouseExitedImp(id self, SEL, NSEvent *) {
   if (CocoaView *v = ownerOf(self))
      v->mouseExited();
}
void scrollWheelImp(id self, SEL, NSEvent *e) {
   if (CocoaView *v = ownerOf(self))
      v->scrollWheel(e);
}
void keyDownImp(id self, SEL sel, NSEvent *e) {
   CocoaView *v = ownerOf(self);
   if (!v || !v->keyDown(e))
      callSuper<void>(self, sel, e); // on to the host
}
BOOL resignFirstResponderImp(id self, SEL sel) {
   const BOOL r = callSuper<BOOL>(self, sel);
   if (CocoaView *v = ownerOf(self))
      v->focusLost();
   return r;
}
void viewDidMoveToWindowImp(id self, SEL sel) {
   callSuper<void>(self, sel);
   if (CocoaView *v = ownerOf(self))
      v->observeWindow([self window]);
}
void viewDidChangeBackingPropertiesImp(id self, SEL sel) {
   callSuper<void>(self, sel);
   [self setNeedsDisplay:YES];
}

// The view is the source of the pattern drag (dragFileOut below).
NSDragOperation sourceOperationMaskImp(id, SEL, NSDraggingSession *, NSDraggingContext) {
   return NSDragOperationCopy;
}

void addMethod(Class cls, SEL sel, IMP imp, const std::string &types) {
   class_addMethod(cls, sel, imp, types.c_str());
}

Class viewClass() {
   static Class cls = nil;
   if (cls)
      return cls;
   Dl_info info{};
   dladdr(reinterpret_cast<const void *>(&viewClass), &info);
   char name[80];
   std::snprintf(name, sizeof(name), "SaeureKisteView_%p", info.dli_fbase);
   // A second editor in the same module finds the class already registered.
   if (Class existing = objc_getClass(name)) {
      gOwnerOffset = ivar_getOffset(class_getInstanceVariable(existing, "skOwner"));
      cls = existing;
      return cls;
   }
   Class c = objc_allocateClassPair(superClass(), name, 0);
   if (!c)
      return nil;
   if (!class_addIvar(c, "skOwner", sizeof(void *),
                      static_cast<uint8_t>(std::log2(sizeof(void *))), "^v")) {
      objc_disposeClassPair(c);
      return nil;
   }
   const std::string v = @encode(void), b = @encode(BOOL), o = @encode(id), s = @encode(SEL);
   const std::string rect = @encode(NSRect), op = @encode(NSDragOperation),
                     ctx = @encode(NSDraggingContext);
   addMethod(c, @selector(isFlipped), reinterpret_cast<IMP>(&yes), b + o + s);
   addMethod(c, @selector(isOpaque), reinterpret_cast<IMP>(&yes), b + o + s);
   addMethod(c, @selector(acceptsFirstResponder), reinterpret_cast<IMP>(&acceptsFirstResponderImp),
             b + o + s);
   addMethod(c, @selector(acceptsFirstMouse:), reinterpret_cast<IMP>(&yesFor), b + o + s + o);
   addMethod(c, @selector(drawRect:), reinterpret_cast<IMP>(&drawRectImp), v + o + s + rect);
   addMethod(c, @selector(mouseDown:), reinterpret_cast<IMP>(&mouseDownImp), v + o + s + o);
   addMethod(c, @selector(rightMouseDown:), reinterpret_cast<IMP>(&rightMouseDownImp), v + o + s + o);
   addMethod(c, @selector(mouseUp:), reinterpret_cast<IMP>(&mouseUpImp), v + o + s + o);
   addMethod(c, @selector(rightMouseUp:), reinterpret_cast<IMP>(&rightMouseUpImp), v + o + s + o);
   addMethod(c, @selector(mouseMoved:), reinterpret_cast<IMP>(&mouseMovedImp), v + o + s + o);
   addMethod(c, @selector(mouseDragged:), reinterpret_cast<IMP>(&mouseMovedImp), v + o + s + o);
   addMethod(c, @selector(rightMouseDragged:), reinterpret_cast<IMP>(&mouseMovedImp), v + o + s + o);
   addMethod(c, @selector(mouseExited:), reinterpret_cast<IMP>(&mouseExitedImp), v + o + s + o);
   addMethod(c, @selector(scrollWheel:), reinterpret_cast<IMP>(&scrollWheelImp), v + o + s + o);
   addMethod(c, @selector(keyDown:), reinterpret_cast<IMP>(&keyDownImp), v + o + s + o);
   addMethod(c, @selector(resignFirstResponder), reinterpret_cast<IMP>(&resignFirstResponderImp),
             b + o + s);
   addMethod(c, @selector(viewDidMoveToWindow), reinterpret_cast<IMP>(&viewDidMoveToWindowImp),
             v + o + s);
   addMethod(c, @selector(viewDidChangeBackingProperties),
             reinterpret_cast<IMP>(&viewDidChangeBackingPropertiesImp), v + o + s);
   addMethod(c, @selector(draggingSession:sourceOperationMaskForDraggingContext:),
             reinterpret_cast<IMP>(&sourceOperationMaskImp), op + o + s + o + ctx);
   class_addProtocol(c, @protocol(NSDraggingSource));
   objc_registerClassPair(c);
   gOwnerOffset = ivar_getOffset(class_getInstanceVariable(c, "skOwner"));
   cls = c;
   return cls;
}

NSView *asView(void *p) { return static_cast<NSView *>(p); }
NSEvent *asEvent(void *p) { return static_cast<NSEvent *>(p); }

NSPoint pointIn(NSView *view, NSEvent *e) {
   return [view convertPoint:[e locationInWindow] fromView:nil];
}

bool shiftHeld(NSEvent *e) { return ([e modifierFlags] & NSEventModifierFlagShift) != 0; }

unsigned long timeMs(NSEvent *e) {
   return static_cast<unsigned long>([e timestamp] * 1000.0);
}

} // namespace

// ------------------------------------------------------------------ CocoaView

CocoaView::~CocoaView() { destroy(); }

bool CocoaView::create(int width, int height) {
   destroy();
   Class cls = viewClass();
   if (!cls)
      return false;
   NSView *view = [[cls alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
   if (!view)
      return false;
   ownerOf(view) = this;
   mView = view;
   mColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

   // Moves and leaves without a button held; with one held Cocoa sends the
   // drags to the view the press went to anyway.
   NSTrackingArea *area = [[NSTrackingArea alloc]
      initWithRect:NSZeroRect
           options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                   NSTrackingActiveAlways | NSTrackingInVisibleRect
             owner:view
          userInfo:nil];
   [view addTrackingArea:area];
   [area release];
   return true;
}

bool CocoaView::embed(uintptr_t parentView) {
   NSView *parent = static_cast<NSView *>(reinterpret_cast<void *>(parentView));
   if (!mView || !parent)
      return false;
   [parent addSubview:asView(mView)];
   [asView(mView) setNeedsDisplay:YES];
   return true;
}

void CocoaView::destroy() {
   releaseKeyboard();
   observeWindow(nullptr);
   if (mView) {
      NSView *view = asView(mView);
      mView = nullptr;
      // The host may hold on to the view a little longer; whatever reaches it
      // from now on finds no owner.
      ownerOf(view) = nullptr;
      [view removeFromSuperview];
      [view release];
   }
   if (mColorSpace) {
      CGColorSpaceRelease(static_cast<CGColorSpaceRef>(mColorSpace));
      mColorSpace = nullptr;
   }
   mCtrlClick = false;
   mWheelAccum = 0.0;
}

void CocoaView::setSize(int width, int height) {
   if (!mView)
      return;
   [asView(mView) setFrameSize:NSMakeSize(width, height)];
   [asView(mView) setNeedsDisplay:YES];
}

void CocoaView::show() {
   if (mView) {
      [asView(mView) setHidden:NO];
      [asView(mView) setNeedsDisplay:YES];
   }
}

void CocoaView::hide() {
   if (mView)
      [asView(mView) setHidden:YES];
}

void CocoaView::invalidate() {
   if (mView)
      [asView(mView) setNeedsDisplay:YES];
}

bool CocoaView::takeKeyboard() {
   if (!mView)
      return false;
   NSView *view = asView(mView);
   NSWindow *win = [view window];
   if (!win)
      return false;
   if ([win firstResponder] == view)
      return mWantsKeys;
   mWantsKeys = true;
   mPrevResponder = [win firstResponder];
   if (![win makeFirstResponder:view]) {
      mWantsKeys = false;
      mPrevResponder = nullptr;
      return false;
   }
   return true;
}

void CocoaView::releaseKeyboard() {
   if (!mWantsKeys)
      return;
   mWantsKeys = false;
   NSView *view = asView(mView);
   NSWindow *win = view ? [view window] : nil;
   NSResponder *prev = static_cast<NSResponder *>(mPrevResponder);
   mPrevResponder = nullptr;
   if (!win || [win firstResponder] != view)
      return;
   // The previous responder is not retained, so it is only handed the focus
   // back if it is still a view in this window; otherwise the window takes it.
   bool stillThere = false;
   if (prev == win)
      stillThere = true;
   else if ([prev isKindOfClass:[NSView class]])
      stillThere = [static_cast<NSView *>(prev) window] == win;
   [win makeFirstResponder:stillThere ? prev : nil];
}

void CocoaView::drawView() {
   NSView *view = asView(mView);
   CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
   if (!view || !ctx || !mColorSpace)
      return;
   const NSRect bounds = [view bounds];
   const double w = bounds.size.width, h = bounds.size.height;
   if (w < 1 || h < 1)
      return;
   const double backing = [view window] ? [[view window] backingScaleFactor] : 1.0;
   cairo_surface_t *surface = mListener.cocoaRender(backing);
   if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
      return;
   cairo_surface_flush(surface);
   const int pw = cairo_image_surface_get_width(surface);
   const int ph = cairo_image_surface_get_height(surface);
   const int stride = cairo_image_surface_get_stride(surface);
   if (pw < 1 || ph < 1)
      return;

   // Cairo's RGB24 is 32-bit native-endian xRGB: BGRx in memory.
   CGDataProviderRef provider = CGDataProviderCreateWithData(
      nullptr, cairo_image_surface_get_data(surface),
      static_cast<size_t>(stride) * static_cast<size_t>(ph), nullptr);
   CGImageRef image = CGImageCreate(
      static_cast<size_t>(pw), static_cast<size_t>(ph), 8, 32, static_cast<size_t>(stride),
      static_cast<CGColorSpaceRef>(mColorSpace),
      static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
         static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst),
      provider, nullptr, false, kCGRenderingIntentDefault);
   if (image) {
      // The surface is drawn at its own size in points, which is the window's
      // size; the view is flipped and CGContextDrawImage draws bottom-up.
      const double iw = pw / backing, ih = ph / backing;
      CGContextSaveGState(ctx);
      CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
      CGContextTranslateCTM(ctx, 0, ih);
      CGContextScaleCTM(ctx, 1, -1);
      CGContextDrawImage(ctx, CGRectMake(0, 0, iw, ih), image);
      CGContextRestoreGState(ctx);
      CGImageRelease(image);
   }
   CGDataProviderRelease(provider);
}

void CocoaView::mouseDown(void *event, unsigned button) {
   NSEvent *e = asEvent(event);
   if (button == 1 && ([e modifierFlags] & NSEventModifierFlagControl)) {
      // Control+click is the one-button Mac's right click.
      mCtrlClick = true;
      button = 3;
   }
   const NSPoint p = pointIn(asView(mView), e);
   mListener.cocoaMouseDown(p.x, p.y, button, timeMs(e), shiftHeld(e));
}

void CocoaView::mouseUp(void *, unsigned) {
   mCtrlClick = false;
   mListener.cocoaMouseUp();
}

void CocoaView::mouseMoved(void *event) {
   NSEvent *e = asEvent(event);
   const NSPoint p = pointIn(asView(mView), e);
   mListener.cocoaMouseMoved(p.x, p.y, shiftHeld(e));
}

void CocoaView::mouseExited() { mListener.cocoaMouseExited(); }

void CocoaView::scrollWheel(void *event) {
   NSEvent *e = asEvent(event);
   double dy = [e scrollingDeltaY];
   // The direction of the hand, not of the content: with natural scrolling on
   // (the macOS default), a wheel turned up must still turn a knob up, as it
   // does on the other platforms.
   if ([e isDirectionInvertedFromDevice])
      dy = -dy;
   // A trackpad or a smooth mouse reports points, about a dozen to a notch; a
   // plain wheel reports lines, one per notch.
   if ([e hasPreciseScrollingDeltas])
      dy /= 12.0;
   if ((dy > 0 && mWheelAccum < 0) || (dy < 0 && mWheelAccum > 0))
      mWheelAccum = 0;
   mWheelAccum += dy;
   const double notches = std::trunc(mWheelAccum);
   if (notches == 0)
      return;
   mWheelAccum -= notches;
   const NSPoint p = pointIn(asView(mView), e);
   const int n = static_cast<int>(std::min(8.0, std::fabs(notches)));
   const unsigned button = notches > 0 ? 4u : 5u;
   for (int i = 0; i < n; ++i)
      mListener.cocoaMouseDown(p.x, p.y, button, timeMs(e), shiftHeld(e));
}

bool CocoaView::keyDown(void *event) {
   NSEvent *e = asEvent(event);
   // Command is a shortcut, and shortcuts are the host's.
   if ([e modifierFlags] & NSEventModifierFlagCommand)
      return false;
   using Key = CocoaViewListener::Key;
   Key key = Key::None;
   switch ([e keyCode]) {
   case 0x35: key = Key::Escape; break;
   case 0x24: // Return
   case 0x4C: // keypad Enter
      key = Key::Accept;
      break;
   case 0x33: key = Key::Backspace; break;
   case 0x7E: key = Key::Up; break;
   case 0x7D: key = Key::Down; break;
   default: break;
   }
   if (key != Key::None)
      return mListener.cocoaKey(key, nullptr, 0);
   // Text, dead keys already composed by the system. ASCII only: the fields
   // edit their text byte by byte, so a character of several UTF-8 bytes
   // would be cut in half by Backspace -- the other platforms hand over
   // single bytes too.
   NSString *chars = [e characters];
   std::string text;
   for (NSUInteger i = 0; chars && i < [chars length]; ++i) {
      const unichar c = [chars characterAtIndex:i];
      if (c >= 0x20 && c < 0x7F)
         text.push_back(static_cast<char>(c));
   }
   return mListener.cocoaKey(Key::None, text.c_str(), static_cast<int>(text.size()));
}

void CocoaView::focusLost() {
   if (!mWantsKeys)
      return;
   mWantsKeys = false;
   mPrevResponder = nullptr;
   mListener.cocoaFocusLost();
}

void CocoaView::observeWindow(void *window) {
   if (mResignObserver) {
      id token = static_cast<id>(mResignObserver);
      [[NSNotificationCenter defaultCenter] removeObserver:token];
      [token release];
      mResignObserver = nullptr;
   }
   if (!window || !mView)
      return;
   // The host window losing the keyboard takes it from the view as well,
   // without a resignFirstResponder.
   CocoaView *owner = this;
   id token = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidResignKeyNotification
                  object:static_cast<NSWindow *>(window)
                   queue:nil
              usingBlock:^(NSNotification *) {
                owner->focusLost();
              }];
   mResignObserver = [token retain];
}

// ------------------------------------------------------------------ dragfile.h

// The macOS drag is not modal, unlike the other two: AppKit runs the session
// from the event loop and this returns as soon as it has started. It has to be
// started from inside the mouse-down that asked for it, which is where the
// window calls it from, so the current event is that press. Whether a target
// took the file is not known here; true means the drag began.
bool dragFileOut(void *, uintptr_t window, const std::string &path) {
   NSView *view = static_cast<NSView *>(reinterpret_cast<void *>(window));
   NSEvent *event = [NSApp currentEvent];
   if (!view || ![view window] || !event || path.empty())
      return false;
   NSString *file = [NSString stringWithUTF8String:path.c_str()];
   if (!file)
      return false;
   NSURL *url = [NSURL fileURLWithPath:file];
   NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:url];
   NSImage *icon = [[NSWorkspace sharedWorkspace] iconForFile:file];
   const NSPoint at = pointIn(view, event);
   const CGFloat size = 32.0;
   [item setDraggingFrame:NSMakeRect(at.x - size / 2, at.y - size / 2, size, size) contents:icon];
   NSDraggingSession *session =
      [view beginDraggingSessionWithItems:@[ item ]
                                    event:event
                                   source:(id<NSDraggingSource>)(id)view];
   [item release];
   return session != nil;
}

} // namespace saeurekiste
