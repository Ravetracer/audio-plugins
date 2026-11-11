// The macOS window backend. Compiled without ARC: every retain has its
// release written out.

#include "CocoaWindow.h"

// Carbon's AssertMacros.h would otherwise define check() and verify().
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#import <Carbon/Carbon.h> // UCKeyTranslate, TIS*, LMGetKbdType, KBGetLayoutType
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

namespace resident::gui {

namespace {

// ------------------------------------------------------------------ keys

// macOS virtual key codes (kVK_*) as Linux evdev codes, which is what the
// listener's raw key events carry. Zero for keys that have none or that the
// C64 keyboard has no use for (volume, Fn, the JIS mode keys).
const uint8_t kEvdevFromKeyCode[128] = {
    30,  31,  32,  33,  35,  34,  44,  45,  // 0x00 A S D F H G Z X
    46,  47,  86,  48,  16,  17,  18,  19,  // 0x08 C V (ISO section) B Q W E R
    21,  20,  2,   3,   4,   5,   7,   6,   // 0x10 Y T 1 2 3 4 6 5
    13,  10,  8,   12,  9,   11,  27,  24,  // 0x18 = 9 7 - 8 0 ] O
    22,  26,  23,  25,  28,  38,  36,  40,  // 0x20 U [ I P Return L J '
    37,  39,  43,  51,  53,  49,  50,  52,  // 0x28 K ; \ , / N M .
    15,  57,  41,  14,  0,   1,   126, 125, // 0x30 Tab Space ` Backspace - Esc RCmd LCmd
    42,  58,  56,  29,  54,  100, 97,  0,   // 0x38 LShift Caps LOpt LCtrl RShift ROpt RCtrl Fn
    187, 83,  0,   55,  0,   78,  0,   69,  // 0x40 F17 KP. - KP* - KP+ - KPClear
    0,   0,   0,   98,  96,  0,   74,  188, // 0x48 VolUp VolDown Mute KP/ KPEnter - KP- F18
    189, 117, 82,  79,  80,  81,  75,  76,  // 0x50 F19 KP= KP0 KP1 KP2 KP3 KP4 KP5
    77,  71,  190, 72,  73,  124, 89,  121, // 0x58 KP6 KP7 F20 KP8 KP9 Yen Ro KPComma
    63,  64,  65,  61,  66,  67,  0,   87,  // 0x60 F5 F6 F7 F3 F8 F9 Eisu F11
    0,   183, 186, 184, 0,   68,  127, 88,  // 0x68 Kana F13 F16 F14 - F10 Menu F12
    0,   185, 110, 102, 104, 111, 62,  107, // 0x70 - F15 Help/Insert Home PageUp Delete F4 End
    60,  109, 59,  105, 106, 108, 103, 0,   // 0x78 F2 PageDown F1 Left Right Down Up -
};

bool isoKeyboard()
{
    return KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
}

int evdevFromKeyCode(unsigned short keyCode)
{
    // On an ISO keyboard macOS reports the key left of 1 as kVK_ISO_Section
    // and the key between left Shift and Z as kVK_ANSI_Grave -- the other way
    // round from their names and from evdev.
    if ((keyCode == 0x0A || keyCode == 0x32) && isoKeyboard())
        return keyCode == 0x0A ? 41 : 86;
    return keyCode < 128 ? kEvdevFromKeyCode[keyCode] : 0;
}

// Keys that produce no text, as the X11 keysym values the widgets test
// against; zero for everything else.
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

// The keysym the layout gives a key with the modifiers held now, for the
// C64's symbolic keyboard -- what Win32Window's ToUnicode and X11's
// XLookupString give: Shift, Option (the macOS AltGr) and Caps Lock applied,
// Control and Command left out, and a dead key gives its own character.
unsigned layoutKeysym(unsigned short keyCode, NSEventModifierFlags flags)
{
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (!source)
        return 0;
    unsigned out = 0;
    auto data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (data)
    {
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        UInt32 mods = 0;
        if (flags & NSEventModifierFlagShift)
            mods |= shiftKey >> 8;
        if (flags & NSEventModifierFlagOption)
            mods |= optionKey >> 8;
        if (flags & NSEventModifierFlagCapsLock)
            mods |= alphaLock >> 8;
        UInt32 deadState = 0;
        UniChar buf[4] = {};
        UniCharCount n = 0;
        if (UCKeyTranslate(layout, keyCode, kUCKeyActionDown, mods, LMGetKbdType(), kUCKeyTranslateNoDeadKeysMask,
                           &deadState, 4, &n, buf) == noErr &&
            n > 0)
        {
            uint32_t cp = buf[0];
            if (cp >= 0xD800 && cp < 0xDC00 && n > 1 && buf[1] >= 0xDC00 && buf[1] < 0xE000)
                cp = 0x10000 + ((cp - 0xD800) << 10) + (buf[1] - 0xDC00);
            if (printable(cp))
                out = keysymFromCodePoint(cp);
        }
    }
    CFRelease(source);
    return out;
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

// Device-dependent modifier bits (IOLLEvent.h): they tell left from right.
constexpr unsigned long kDevLCtrl = 0x0001, kDevLShift = 0x0002, kDevRShift = 0x0004, kDevLCmd = 0x0008,
                        kDevRCmd = 0x0010, kDevLAlt = 0x0020, kDevRAlt = 0x0040, kDevRCtrl = 0x2000;

// ------------------------------------------------------------------ the view class
//
// The view's class is made at run time under a name that carries the address
// this module is loaded at. Objective-C classes share one namespace per
// process: two plugin binaries defining a class of the same name (the CLAP
// and the VST3 of ReSIDent, or two versions of it) would get one of the two
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
void keyUpImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self))
        w->keyUp(e);
}
void flagsChangedImp(id self, SEL, NSEvent* e)
{
    if (CocoaWindow* w = ownerOf(self))
        w->flagsChanged(e);
}
BOOL resignFirstResponderImp(id self, SEL sel)
{
    const BOOL r = callSuper<BOOL>(self, sel);
    if (CocoaWindow* w = ownerOf(self))
        w->focusLost();
    return r;
}
void viewDidMoveToWindowImp(id self, SEL sel)
{
    callSuper<void>(self, sel);
    if (CocoaWindow* w = ownerOf(self))
        w->observeWindow([self window]);
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
    std::snprintf(name, sizeof(name), "ResidentView_%p", info.dli_fbase);
    Class c = objc_allocateClassPair(superClass(), name, 0);
    if (!c)
        return nil;
    if (!class_addIvar(c, "residentOwner", sizeof(void*), static_cast<uint8_t>(std::log2(sizeof(void*))), "^v"))
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
    addMethod(c, @selector(keyUp:), reinterpret_cast<IMP>(&keyUpImp), v + o + s + o);
    addMethod(c, @selector(flagsChanged:), reinterpret_cast<IMP>(&flagsChangedImp), v + o + s + o);
    addMethod(c, @selector(resignFirstResponder), reinterpret_cast<IMP>(&resignFirstResponderImp), b + o + s);
    addMethod(c, @selector(viewDidMoveToWindow), reinterpret_cast<IMP>(&viewDidMoveToWindowImp), v + o + s);
    addMethod(c, @selector(viewDidChangeBackingProperties),
              reinterpret_cast<IMP>(&viewDidChangeBackingPropertiesImp), v + o + s);
    addMethod(c, @selector(draggingEntered:), reinterpret_cast<IMP>(&draggingEnteredImp), op + o + s + o);
    addMethod(c, @selector(draggingUpdated:), reinterpret_cast<IMP>(&draggingEnteredImp), op + o + s + o);
    addMethod(c, @selector(performDragOperation:), reinterpret_cast<IMP>(&performDragOperationImp), b + o + s + o);
    objc_registerClassPair(c);
    ownerOffset = ivar_getOffset(class_getInstanceVariable(c, "residentOwner"));
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
    observeWindow(nullptr);
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
                                     kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, nullptr,
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
    const unsigned short keyCode = [ev keyCode];
    const NSEventModifierFlags flags = [ev modifierFlags];
    const unsigned own = keysymFromKeyCode(keyCode);
    if (const int code = evdevFromKeyCode(keyCode))
        listener_->onRawKey(code, true, own ? own : layoutKeysym(keyCode, flags));

    KeyEvent k;
    k.mods = modsFromFlags(flags);
    if (own)
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
    {
        // Still tell the editor when it has the keyboard for the C64, so the
        // key does not go on to the host.
        k.keysym = 0;
        return listener_->onKey(k);
    }
    k.keysym = keysymFromCodePoint(cp);
    if (const char* utf8 = [chars UTF8String])
        k.text = utf8;
    return listener_->onKey(k);
}

void CocoaWindow::keyUp(void* event)
{
    if (const int code = evdevFromKeyCode([asEvent(event) keyCode]))
        listener_->onRawKey(code, false, 0);
}

void CocoaWindow::flagsChanged(void* event)
{
    // Modifiers come as flag changes, not as key presses: the key is down
    // when its own (left or right) bit is set. Caps Lock is down while it is
    // locked, which is what the C64's latching SHIFT LOCK does as well.
    NSEvent* ev = asEvent(event);
    const unsigned short keyCode = [ev keyCode];
    const unsigned long f = static_cast<unsigned long>([ev modifierFlags]);
    bool down;
    switch (keyCode)
    {
    case 0x38: down = f & kDevLShift; break;
    case 0x3C: down = f & kDevRShift; break;
    case 0x3B: down = f & kDevLCtrl; break;
    case 0x3E: down = f & kDevRCtrl; break;
    case 0x3A: down = f & kDevLAlt; break;
    case 0x3D: down = f & kDevRAlt; break;
    case 0x37: down = f & kDevLCmd; break;
    case 0x36: down = f & kDevRCmd; break;
    case 0x39: down = f & NSEventModifierFlagCapsLock; break;
    default: return;
    }
    if (const int code = evdevFromKeyCode(keyCode))
        listener_->onRawKey(code, down, 0);
}

void CocoaWindow::focusLost() { listener_->onFocusLost(); }

void CocoaWindow::observeWindow(void* window)
{
    if (resignObserver_)
    {
        id token = static_cast<id>(resignObserver_);
        [[NSNotificationCenter defaultCenter] removeObserver:token];
        [token release];
        resignObserver_ = nullptr;
    }
    if (!window || !view_)
        return;
    // The host window losing the keyboard takes it from the view as well,
    // without a resignFirstResponder.
    CocoaWindow* owner = this;
    id token = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification
                                                                 object:static_cast<NSWindow*>(window)
                                                                  queue:nil
                                                             usingBlock:^(NSNotification*) {
                                                               owner->focusLost();
                                                             }];
    resignObserver_ = [token retain];
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

} // namespace resident::gui
