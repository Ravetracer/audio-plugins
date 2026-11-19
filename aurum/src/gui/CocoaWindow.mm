// The macOS window backend. Compiled without ARC: every retain has its
// release written out.

#include "CocoaWindow.h"

// AssertMacros.h (reached through Cocoa) would otherwise define check() and
// verify().
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#import <Cocoa/Cocoa.h>

#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Keys.h"

namespace aurum::gui {

namespace {

// ------------------------------------------------------------------ keys

// Keys that produce no text, as the X11 keysym values the widgets test
// against; zero for everything else. The numbers are macOS virtual key codes
// (kVK_*).
unsigned keysymFromKeyCode(unsigned short keyCode)
{
    switch (keyCode)
    {
    case 0x33: return key::BackSpace;
    case 0x30: return key::Tab;
    case 0x24: return key::Return;
    case 0x4C: return key::KP_Enter;
    case 0x35: return key::Escape;
    case 0x73: return key::Home;
    case 0x77: return key::End;
    case 0x74: return key::PageUp;
    case 0x79: return key::PageDown;
    case 0x7B: return key::Left;
    case 0x7E: return key::Up;
    case 0x7C: return key::Right;
    case 0x7D: return key::Down;
    case 0x75: return key::Delete;
    default: return 0;
    }
}

bool printable(uint32_t cp)
{
    // NSEvent puts the function keys at U+F700..U+F8FF.
    return cp >= 0x20 && cp != 0x7F && !(cp >= 0xF700 && cp <= 0xF8FF) && !(cp >= 0xD800 && cp < 0xE000);
}

unsigned keysymFromCodePoint(uint32_t cp) { return cp < 0x100 ? cp : 0x01000000u + cp; }

uint32_t firstCodePoint(NSString* s)
{
    if (!s || [s length] == 0)
        return 0;
    const unichar hi = [s characterAtIndex:0];
    if (hi >= 0xD800 && hi < 0xDC00 && [s length] > 1)
    {
        const unichar lo = [s characterAtIndex:1];
        if (lo >= 0xDC00 && lo < 0xE000)
            return 0x10000 + ((static_cast<uint32_t>(hi) - 0xD800) << 10) + (lo - 0xDC00);
    }
    return hi;
}

unsigned modsFromFlags(NSEventModifierFlags f)
{
    unsigned m = 0;
    if (f & NSEventModifierFlagShift)
        m |= ModShift;
    // Command is what a Mac user presses for a shortcut; Control means the
    // same to the widgets.
    if (f & (NSEventModifierFlagCommand | NSEventModifierFlagControl))
        m |= ModCtrl;
    if (f & NSEventModifierFlagOption)
        m |= ModAlt;
    return m;
}

// ------------------------------------------------------------------ the view class
//
// The view's class is made at run time under a name that carries the address
// this module is loaded at. Objective-C classes share one namespace per
// process: two plugin binaries defining a class of the same name (the CLAP
// and the VST3 of Aurum, or two versions of it) would get one of the two
// implementations each, and run the other binary's code -- with its own
// statically linked Cairo -- over this one's windows. That is the Windows
// window-class bug again (CLAUDE.md), and the cure is the same: a name per
// module. The class is never disposed of: a bundle with Objective-C in it is
// never unloaded, so its methods cannot dangle.

ptrdiff_t ownerOffset = 0;

CocoaWindow*& ownerOf(id self)
{
    return *reinterpret_cast<CocoaWindow**>(reinterpret_cast<char*>(static_cast<void*>(self)) + ownerOffset);
}

Class superClass() { return [NSView class]; }

template <typename R, typename... A> R callSuper(id self, SEL sel, A... args)
{
    struct objc_super sup = {self, superClass()};
    using Fn = R (*)(struct objc_super*, SEL, A...);
    return reinterpret_cast<Fn>(objc_msgSendSuper)(&sup, sel, args...);
}

BOOL yes(id, SEL) { return YES; }
BOOL yesFor(id, SEL, id) { return YES; }

void drawRectImp(id self, SEL, NSRect)
{
    if (CocoaWindow* w = ownerOf(self))
        w->drawView();
}

void mouseDownImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self))
        w->mouseDown(e, 1);
}
void rightMouseDownImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self))
        w->mouseDown(e, 3);
}
void otherMouseDownImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self); w && [e buttonNumber] == 2)
        w->mouseDown(e, 2);
}
void mouseUpImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self))
        w->mouseUp(e, 1);
}
void rightMouseUpImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self))
        w->mouseUp(e, 3);
}
void otherMouseUpImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self); w && [e buttonNumber] == 2)
        w->mouseUp(e, 2);
}
void mouseMovedImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self))
        w->mouseMoved(e);
}
void mouseExitedImp(id self, SEL, NSEvent*)
{
    if (CocoaWindow* w = ownerOf(self))
        w->mouseExited();
}
void scrollWheelImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self))
        w->scrollWheel(e);
}

void keyDownImp(id self, SEL sel, NSEvent* e)
{
    CocoaWindow* w = ownerOf(self);
    if (!w || !w->keyDown(e))
        callSuper<void>(self, sel, e); // on to the host
}
void viewDidChangeBackingPropertiesImp(id self, SEL sel)
{
    callSuper<void>(self, sel);
    [self setNeedsDisplay:YES];
}

bool hasFiles(id<NSDraggingInfo> info)
{
    NSDictionary* opts = @{NSPasteboardURLReadingFileURLsOnlyKey : @YES};
    return [[info draggingPasteboard] canReadObjectForClasses:@[ [NSURL class] ] options:opts];
}
NSDragOperation draggingEnteredImp(id self, SEL, id<NSDraggingInfo> info)
{
    return ownerOf(self) && hasFiles(info) ? NSDragOperationCopy : NSDragOperationNone;
}
BOOL performDragOperationImp(id self, SEL, id<NSDraggingInfo> info)
{
    CocoaWindow* w = ownerOf(self);
    if (!w || !hasFiles(info))
        return NO;
    w->filesDropped([info draggingPasteboard]);
    return YES;
}

void addMethod(Class cls, SEL sel, IMP imp, const std::string& types)
{
    class_addMethod(cls, sel, imp, types.c_str());
}

Class viewClass()
{
    static Class cls = nil;
    if (cls)
        return cls;
    Dl_info info{};
    dladdr(reinterpret_cast<const void*>(&viewClass), &info);
    char name[80];
    std::snprintf(name, sizeof(name), "AurumView_%p", info.dli_fbase);
    Class c = objc_allocateClassPair(superClass(), name, 0);
    if (!c)
        return nil;
    if (!class_addIvar(c, "aurumOwner", sizeof(void*), static_cast<uint8_t>(std::log2(sizeof(void*))), "^v"))
    {
        objc_disposeClassPair(c);
        return nil;
    }
    const std::string v = @encode(void), b = @encode(BOOL), o = @encode(id), s = @encode(SEL);
    const std::string op = @encode(NSDragOperation), rect = @encode(NSRect);
    addMethod(c, @selector(isFlipped), reinterpret_cast<IMP>(&yes), b + o + s);
    addMethod(c, @selector(isOpaque), reinterpret_cast<IMP>(&yes), b + o + s);
    addMethod(c, @selector(acceptsFirstResponder), reinterpret_cast<IMP>(&yes), b + o + s);
    addMethod(c, @selector(acceptsFirstMouse:), reinterpret_cast<IMP>(&yesFor), b + o + s + o);
    addMethod(c, @selector(drawRect:), reinterpret_cast<IMP>(&drawRectImp), v + o + s + rect);
    addMethod(c, @selector(mouseDown:), reinterpret_cast<IMP>(&mouseDownImp), v + o + s + o);
    addMethod(c, @selector(rightMouseDown:), reinterpret_cast<IMP>(&rightMouseDownImp), v + o + s + o);
    addMethod(c, @selector(otherMouseDown:), reinterpret_cast<IMP>(&otherMouseDownImp), v + o + s + o);
    addMethod(c, @selector(mouseUp:), reinterpret_cast<IMP>(&mouseUpImp), v + o + s + o);
    addMethod(c, @selector(rightMouseUp:), reinterpret_cast<IMP>(&rightMouseUpImp), v + o + s + o);
    addMethod(c, @selector(otherMouseUp:), reinterpret_cast<IMP>(&otherMouseUpImp), v + o + s + o);
    addMethod(c, @selector(mouseMoved:), reinterpret_cast<IMP>(&mouseMovedImp), v + o + s + o);
    addMethod(c, @selector(mouseDragged:), reinterpret_cast<IMP>(&mouseMovedImp), v + o + s + o);
    addMethod(c, @selector(rightMouseDragged:), reinterpret_cast<IMP>(&mouseMovedImp), v + o + s + o);
    addMethod(c, @selector(otherMouseDragged:), reinterpret_cast<IMP>(&mouseMovedImp), v + o + s + o);
    addMethod(c, @selector(mouseExited:), reinterpret_cast<IMP>(&mouseExitedImp), v + o + s + o);
    addMethod(c, @selector(scrollWheel:), reinterpret_cast<IMP>(&scrollWheelImp), v + o + s + o);
    addMethod(c, @selector(keyDown:), reinterpret_cast<IMP>(&keyDownImp), v + o + s + o);
    addMethod(c, @selector(viewDidChangeBackingProperties),
              reinterpret_cast<IMP>(&viewDidChangeBackingPropertiesImp), v + o + s);
    addMethod(c, @selector(draggingEntered:), reinterpret_cast<IMP>(&draggingEnteredImp), op + o + s + o);
    addMethod(c, @selector(draggingUpdated:), reinterpret_cast<IMP>(&draggingEnteredImp), op + o + s + o);
    addMethod(c, @selector(performDragOperation:), reinterpret_cast<IMP>(&performDragOperationImp), b + o + s + o);
    objc_registerClassPair(c);
    ownerOffset = ivar_getOffset(class_getInstanceVariable(c, "aurumOwner"));
    cls = c;
    return cls;
}

NSView* asView(void* p) { return static_cast<NSView*>(p); }
NSEvent* asEvent(void* p) { return static_cast<NSEvent*>(p); }

} // namespace

// ------------------------------------------------------------------ CocoaWindow

CocoaWindow::~CocoaWindow() { destroy(); }

bool CocoaWindow::attach(uintptr_t parent, int physW, int physH)
{
    destroy();
    NSView* parentView = static_cast<NSView*>(reinterpret_cast<void*>(parent));
    Class cls = viewClass();
    if (!parentView || !cls)
        return false;
    width_ = physW;
    height_ = physH;
    NSView* view = [[cls alloc] initWithFrame:NSMakeRect(0, 0, physW, physH)];
    if (!view)
        return false;
    ownerOf(view) = this;
    view_ = view;
    colorSpace_ = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    // Moves and leaves without a button held; with one held Cocoa sends the
    // drags to the view the press went to anyway.
    NSTrackingArea* area = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect
               owner:view
            userInfo:nil];
    [view addTrackingArea:area];
    [area release];
    [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    [parentView addSubview:view];
    return true;
}

void CocoaWindow::destroy()
{
    if (view_)
    {
        NSView* view = asView(view_);
        view_ = nullptr;
        // The host may hold on to the view a little longer; whatever reaches
        // it from now on finds no owner.
        ownerOf(view) = nullptr;
        [view removeFromSuperview];
        [view release];
    }
    if (colorSpace_)
    {
        CGColorSpaceRelease(static_cast<CGColorSpaceRef>(colorSpace_));
        colorSpace_ = nullptr;
    }
    if (back_)
    {
        cairo_surface_destroy(back_);
        back_ = nullptr;
    }
    ctrlClick_ = false;
    wheelAccum_ = 0.0;
}

void CocoaWindow::setSize(int physW, int physH)
{
    width_ = physW;
    height_ = physH;
    if (view_)
    {
        [asView(view_) setFrameSize:NSMakeSize(physW, physH)];
        [asView(view_) setNeedsDisplay:YES];
    }
}

void CocoaWindow::show()
{
    if (view_)
        [asView(view_) setHidden:NO];
}

void CocoaWindow::hide()
{
    if (view_)
        [asView(view_) setHidden:YES];
}

void CocoaWindow::render()
{
    if (view_)
        [asView(view_) setNeedsDisplay:YES];
}

void CocoaWindow::grabKeyboard()
{
    if (!view_)
        return;
    NSView* view = asView(view_);
    if (NSWindow* win = [view window])
        [win makeFirstResponder:view];
}

void CocoaWindow::drawView()
{
    NSView* view = asView(view_);
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (!view || !ctx || !colorSpace_)
        return;
    const NSRect bounds = [view bounds];
    const double w = bounds.size.width, h = bounds.size.height;
    if (w < 1 || h < 1)
        return;
    width_ = static_cast<int>(std::lround(w));
    height_ = static_cast<int>(std::lround(h));
    // Retina: the buffer is in device pixels, and the scale goes into the
    // Cairo matrix, so everything (the editor's cached layers too) is drawn
    // at full resolution.
    const double backing = [view window] ? [[view window] backingScaleFactor] : 1.0;
    const int pw = std::max(1, static_cast<int>(std::lround(w * backing)));
    const int ph = std::max(1, static_cast<int>(std::lround(h * backing)));
    if (!back_ || backW_ != pw || backH_ != ph)
    {
        if (back_)
            cairo_surface_destroy(back_);
        back_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw, ph);
        backW_ = pw;
        backH_ = ph;
    }
    cairo_t* cr = cairo_create(back_);
    cairo_scale(cr, scale_ * backing, scale_ * backing);
    listener_->onPaint(cr, width_, height_);
    cairo_destroy(cr);
    cairo_surface_flush(back_);

    // Cairo's RGB24 is 32-bit native-endian xRGB: BGRx in memory.
    const int stride = cairo_image_surface_get_stride(back_);
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, cairo_image_surface_get_data(back_), static_cast<size_t>(stride) * static_cast<size_t>(ph), nullptr);
    CGImageRef image = CGImageCreate(static_cast<size_t>(pw), static_cast<size_t>(ph), 8, 32,
                                     static_cast<size_t>(stride), static_cast<CGColorSpaceRef>(colorSpace_),
                                     static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) |
                                         static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst), provider, nullptr,
                                     false, kCGRenderingIntentDefault);
    if (image)
    {
        // The view is flipped; CGContextDrawImage draws bottom-up.
        CGContextSaveGState(ctx);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
        CGContextTranslateCTM(ctx, 0, h);
        CGContextScaleCTM(ctx, 1, -1);
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
        CGContextRestoreGState(ctx);
        CGImageRelease(image);
    }
    CGDataProviderRelease(provider);
}

MouseEvent CocoaWindow::toMouse(void* event) const
{
    NSEvent* e = asEvent(event);
    const NSPoint p = [asView(view_) convertPoint:[e locationInWindow] fromView:nil];
    MouseEvent m;
    m.x = static_cast<float>(p.x / scale_);
    m.y = static_cast<float>(p.y / scale_);
    m.mods = modsFromFlags([e modifierFlags]);
    return m;
}

void CocoaWindow::mouseDown(void* event, int button)
{
    NSView* view = asView(view_);
    // A child view is not focused by clicking it, and without focus the keys
    // go to the host.
    if (NSWindow* win = [view window]; win && [win firstResponder] != view)
        [win makeFirstResponder:view];
    MouseEvent e = toMouse(event);
    if (button == 1 && ([asEvent(event) modifierFlags] & NSEventModifierFlagControl))
    {
        // Control+click is the one-button Mac's right click.
        ctrlClick_ = true;
        button = 3;
        e.mods &= ~static_cast<unsigned>(ModCtrl);
        if ([asEvent(event) modifierFlags] & NSEventModifierFlagCommand)
            e.mods |= ModCtrl;
    }
    e.button = button;
    listener_->onMouseDown(e);
}

void CocoaWindow::mouseUp(void* event, int button)
{
    MouseEvent e = toMouse(event);
    if (button == 1 && ctrlClick_)
    {
        ctrlClick_ = false;
        button = 3;
    }
    e.button = button;
    listener_->onMouseUp(e);
}

void CocoaWindow::mouseMoved(void* event) { listener_->onMouseMove(toMouse(event)); }

void CocoaWindow::mouseExited() { listener_->onMouseLeave(); }

void CocoaWindow::scrollWheel(void* event)
{
    NSEvent* ev = asEvent(event);
    double dy = [ev scrollingDeltaY];
    // The direction of the hand, not of the content: with natural scrolling
    // on (the macOS default), a wheel turned up must still turn a knob up,
    // as it does on the other platforms.
    if ([ev isDirectionInvertedFromDevice])
        dy = -dy;
    // A trackpad (or a smooth mouse) reports points; about a dozen make a
    // notch. A plain wheel reports lines, one per notch.
    if ([ev hasPreciseScrollingDeltas])
        dy /= 12.0;
    if ((dy > 0 && wheelAccum_ < 0) || (dy < 0 && wheelAccum_ > 0))
        wheelAccum_ = 0;
    wheelAccum_ += dy;
    const double notches = std::trunc(wheelAccum_);
    if (notches == 0)
        return;
    wheelAccum_ -= notches;
    MouseEvent e = toMouse(event);
    e.wheel = static_cast<float>(std::clamp(notches, -8.0, 8.0));
    listener_->onMouseWheel(e);
}

bool CocoaWindow::keyDown(void* event)
{
    NSEvent* ev = asEvent(event);
    KeyEvent k;
    k.mods = modsFromFlags([ev modifierFlags]);
    if (const unsigned own = keysymFromKeyCode([ev keyCode]))
    {
        k.keysym = own;
        return listener_->onKey(k);
    }
    if (k.mods & ModCtrl)
    {
        // A shortcut: the key's character without Control or Command, no
        // text -- what X11 reports for Ctrl+letter.
        const uint32_t cp = firstCodePoint([ev charactersIgnoringModifiers]);
        if (!printable(cp))
            return false;
        k.keysym = keysymFromCodePoint(cp);
        return listener_->onKey(k);
    }
    // Text, dead keys already composed by the system (a dead key itself
    // brings no characters).
    NSString* chars = [ev characters];
    const uint32_t cp = firstCodePoint(chars);
    if (!printable(cp))
        return false;
    k.keysym = keysymFromCodePoint(cp);
    if (const char* utf8 = [chars UTF8String])
        k.text = utf8;
    return listener_->onKey(k);
}

void CocoaWindow::filesDropped(void* pasteboard)
{
    NSPasteboard* pb = static_cast<NSPasteboard*>(pasteboard);
    NSDictionary* opts = @{NSPasteboardURLReadingFileURLsOnlyKey : @YES};
    NSArray* urls = [pb readObjectsForClasses:@[ [NSURL class] ] options:opts];
    std::vector<std::string> paths;
    for (NSURL* url in urls)
        if (const char* p = [url fileSystemRepresentation])
            paths.emplace_back(p);
    if (!paths.empty())
        listener_->onFilesDropped(paths);
}

} // namespace aurum::gui
