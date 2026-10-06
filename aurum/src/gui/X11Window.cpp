#include "X11Window.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace aurum::gui {

namespace {

enum AtomIndex
{
    A_XdndAware,
    A_XdndEnter,
    A_XdndPosition,
    A_XdndStatus,
    A_XdndDrop,
    A_XdndFinished,
    A_XdndActionCopy,
    A_XdndSelection,
    A_XdndLeave,
    A_UriList,
    A_Property,
    A_Count
};

const char* const kAtomNames[A_Count] = {"XdndAware",      "XdndEnter",       "XdndPosition", "XdndStatus",
                                         "XdndDrop",       "XdndFinished",    "XdndActionCopy",
                                         "XdndSelection",  "XdndLeave",       "text/uri-list", "AURUM_DND"};

// The local paths in a text/uri-list; anything that is not a file:// URI is
// skipped.
std::vector<std::string> pathsFromUriList(const std::string& uris)
{
    std::vector<std::string> paths;
    size_t start = 0;
    while (start < uris.size())
    {
        size_t end = uris.find_first_of("\r\n", start);
        if (end == std::string::npos)
            end = uris.size();
        const std::string line = uris.substr(start, end - start);
        start = end + 1;
        if (line.rfind("file://", 0) != 0)
            continue;
        std::string path;
        for (size_t i = 7; i < line.size(); ++i)
        {
            if (line[i] == '%' && i + 2 < line.size())
            {
                path += static_cast<char>(std::stoi(line.substr(i + 1, 2), nullptr, 16));
                i += 2;
            }
            else
                path += line[i];
        }
        paths.push_back(path);
    }
    return paths;
}

unsigned modsFromState(unsigned state)
{
    unsigned m = 0;
    if (state & ShiftMask)
        m |= ModShift;
    if (state & ControlMask)
        m |= ModCtrl;
    if (state & Mod1Mask)
        m |= ModAlt;
    return m;
}

} // namespace

X11Window::~X11Window() { destroy(); }

double X11Window::systemScale()
{
    Display* d = XOpenDisplay(nullptr);
    if (!d)
        return 1.0;
    double scale = 1.0;
    if (const char* rms = XResourceManagerString(d))
    {
        XrmInitialize();
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db)
        {
            char* type = nullptr;
            XrmValue value;
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr)
            {
                const double dpi = std::atof(value.addr);
                if (dpi > 0)
                    scale = dpi / 96.0;
            }
            XrmDestroyDatabase(db);
        }
    }
    XCloseDisplay(d);
    return std::clamp(scale, 1.0, 4.0);
}

bool X11Window::attach(uintptr_t parent, int physW, int physH)
{
    destroy();
    display_ = XOpenDisplay(nullptr);
    if (!display_)
        return false;
    parent_ = parent;
    width_ = physW;
    height_ = physH;

    XSetWindowAttributes attr{};
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                      KeyReleaseMask | LeaveWindowMask | EnterWindowMask | StructureNotifyMask | FocusChangeMask;
    attr.background_pixel = 0x14171c;
    attr.border_pixel = 0;
    const int screen = DefaultScreen(display_);
    Visual* visual = DefaultVisual(display_, screen);
    window_ = XCreateWindow(display_, static_cast<Window>(parent), 0, 0, static_cast<unsigned>(physW), static_cast<unsigned>(physH), 0,
                            CopyFromParent, InputOutput, visual, CWEventMask | CWBackPixel | CWBorderPixel, &attr);
    if (!window_)
        return false;

    for (int i = 0; i < A_Count; ++i)
        atoms_[i] = XInternAtom(display_, kAtomNames[i], False);
    const unsigned long version = 5;
    XChangeProperty(display_, window_, atoms_[A_XdndAware], XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&version), 1);

    xsurface_ = cairo_xlib_surface_create(display_, window_, visual, physW, physH);
    XMapWindow(display_, window_);
    XFlush(display_);
    return true;
}

void X11Window::destroy()
{
    if (back_)
    {
        cairo_surface_destroy(back_);
        back_ = nullptr;
    }
    if (xsurface_)
    {
        cairo_surface_destroy(xsurface_);
        xsurface_ = nullptr;
    }
    if (display_)
    {
        if (window_)
            XDestroyWindow(display_, window_);
        XCloseDisplay(display_);
    }
    display_ = nullptr;
    window_ = 0;
}

void X11Window::setSize(int physW, int physH)
{
    width_ = physW;
    height_ = physH;
    if (!display_ || !window_)
        return;
    XResizeWindow(display_, window_, static_cast<unsigned>(physW), static_cast<unsigned>(physH));
    if (xsurface_)
        cairo_xlib_surface_set_size(xsurface_, physW, physH);
    XFlush(display_);
}

void X11Window::show()
{
    if (display_ && window_)
    {
        XMapWindow(display_, window_);
        XFlush(display_);
    }
}

void X11Window::hide()
{
    if (display_ && window_)
    {
        XUnmapWindow(display_, window_);
        XFlush(display_);
    }
}

int X11Window::fd() const { return display_ ? ConnectionNumber(display_) : -1; }

void X11Window::grabKeyboard()
{
    if (display_ && window_)
    {
        XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
        XFlush(display_);
    }
}

MouseEvent X11Window::toMouse(int x, int y, unsigned state) const
{
    MouseEvent e;
    e.x = static_cast<float>(x / scale_);
    e.y = static_cast<float>(y / scale_);
    e.mods = modsFromState(state);
    return e;
}

void X11Window::processEvents()
{
    if (!display_)
        return;
    // Coalesce motion: only the latest motion event matters.
    while (XPending(display_))
    {
        XEvent ev;
        XNextEvent(display_, &ev);
        if (ev.type == MotionNotify)
        {
            XEvent next;
            while (XCheckTypedWindowEvent(display_, window_, MotionNotify, &next))
                ev = next;
        }
        handleEvent(&ev);
    }
}

void X11Window::handleEvent(void* xevent)
{
    XEvent& ev = *static_cast<XEvent*>(xevent);
    switch (ev.type)
    {
    case Expose:
        if (ev.xexpose.count == 0)
            render();
        break;
    case ConfigureNotify:
        if (ev.xconfigure.window == window_ &&
            (ev.xconfigure.width != width_ || ev.xconfigure.height != height_))
        {
            width_ = ev.xconfigure.width;
            height_ = ev.xconfigure.height;
            if (xsurface_)
                cairo_xlib_surface_set_size(xsurface_, width_, height_);
        }
        break;
    case ButtonPress:
    {
        MouseEvent e = toMouse(ev.xbutton.x, ev.xbutton.y, ev.xbutton.state);
        if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5)
        {
            e.wheel = ev.xbutton.button == Button4 ? 1.0f : -1.0f;
            listener_->onMouseWheel(e);
        }
        else if (ev.xbutton.button <= Button3)
        {
            e.button = ev.xbutton.button == Button1 ? 1 : (ev.xbutton.button == Button2 ? 2 : 3);
            listener_->onMouseDown(e);
        }
        break;
    }
    case ButtonRelease:
        if (ev.xbutton.button <= Button3)
        {
            MouseEvent e = toMouse(ev.xbutton.x, ev.xbutton.y, ev.xbutton.state);
            e.button = ev.xbutton.button == Button1 ? 1 : (ev.xbutton.button == Button2 ? 2 : 3);
            listener_->onMouseUp(e);
        }
        break;
    case MotionNotify: listener_->onMouseMove(toMouse(ev.xmotion.x, ev.xmotion.y, ev.xmotion.state)); break;
    case LeaveNotify: listener_->onMouseLeave(); break;
    case KeyPress:
    {
        char buf[32] = {};
        KeySym sym = 0;
        const int n = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &sym, nullptr);
        KeyEvent k;
        k.keysym = static_cast<unsigned>(sym);
        k.mods = modsFromState(ev.xkey.state);
        if (n > 0)
        {
            // XLookupString yields Latin-1; convert to UTF-8.
            for (int i = 0; i < n; ++i)
            {
                const unsigned char c = static_cast<unsigned char>(buf[i]);
                if (c < 0x80)
                    k.text += static_cast<char>(c);
                else
                {
                    k.text += static_cast<char>(0xC0 | (c >> 6));
                    k.text += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
        }
        listener_->onKey(k);
        break;
    }
    case ClientMessage:
        if (static_cast<unsigned long>(ev.xclient.message_type) == atoms_[A_XdndEnter])
            handleXdndEnter(xevent);
        else if (static_cast<unsigned long>(ev.xclient.message_type) == atoms_[A_XdndPosition])
            handleXdndPosition(xevent);
        else if (static_cast<unsigned long>(ev.xclient.message_type) == atoms_[A_XdndDrop])
            handleXdndDrop(xevent);
        break;
    case SelectionNotify: handleSelectionNotify(xevent); break;
    default: break;
    }
}

void X11Window::handleXdndEnter(void* xevent)
{
    XEvent& ev = *static_cast<XEvent*>(xevent);
    xdndSource_ = static_cast<unsigned long>(ev.xclient.data.l[0]);
    xdndVersion_ = static_cast<int>(ev.xclient.data.l[1] >> 24);
}

void X11Window::handleXdndPosition(void* xevent)
{
    XEvent& ev = *static_cast<XEvent*>(xevent);
    XEvent reply{};
    reply.xclient.type = ClientMessage;
    reply.xclient.display = display_;
    reply.xclient.window = static_cast<Window>(ev.xclient.data.l[0]);
    reply.xclient.message_type = atoms_[A_XdndStatus];
    reply.xclient.format = 32;
    reply.xclient.data.l[0] = static_cast<long>(window_);
    reply.xclient.data.l[1] = 1; // accept
    reply.xclient.data.l[4] = static_cast<long>(atoms_[A_XdndActionCopy]);
    XSendEvent(display_, reply.xclient.window, False, NoEventMask, &reply);
    XFlush(display_);
}

void X11Window::handleXdndDrop(void* xevent)
{
    XEvent& ev = *static_cast<XEvent*>(xevent);
    xdndSource_ = static_cast<unsigned long>(ev.xclient.data.l[0]);
    const Time t = xdndVersion_ >= 1 ? static_cast<Time>(ev.xclient.data.l[2]) : CurrentTime;
    XConvertSelection(display_, atoms_[A_XdndSelection], atoms_[A_UriList], atoms_[A_Property], window_, t);
    XFlush(display_);
}

void X11Window::handleSelectionNotify(void* xevent)
{
    XEvent& ev = *static_cast<XEvent*>(xevent);
    if (static_cast<unsigned long>(ev.xselection.property) != atoms_[A_Property])
        return;
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display_, window_, atoms_[A_Property], 0, 65536, True, AnyPropertyType, &type, &format,
                           &count, &remaining, &data) == Success &&
        data)
    {
        const std::string uris(reinterpret_cast<char*>(data), count);
        XFree(data);
        listener_->onFilesDropped(pathsFromUriList(uris));
    }
    if (xdndSource_)
    {
        XEvent fin{};
        fin.xclient.type = ClientMessage;
        fin.xclient.display = display_;
        fin.xclient.window = static_cast<Window>(xdndSource_);
        fin.xclient.message_type = atoms_[A_XdndFinished];
        fin.xclient.format = 32;
        fin.xclient.data.l[0] = static_cast<long>(window_);
        fin.xclient.data.l[1] = 1;
        fin.xclient.data.l[2] = static_cast<long>(atoms_[A_XdndActionCopy]);
        XSendEvent(display_, fin.xclient.window, False, NoEventMask, &fin);
        XFlush(display_);
    }
}

void X11Window::ensureBackBuffer()
{
    if (back_ && backW_ == width_ && backH_ == height_)
        return;
    if (back_)
        cairo_surface_destroy(back_);
    back_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, std::max(width_, 1), std::max(height_, 1));
    backW_ = width_;
    backH_ = height_;
}

void X11Window::render()
{
    if (!display_ || !xsurface_ || width_ <= 0 || height_ <= 0)
        return;
    ensureBackBuffer();
    cairo_t* cr = cairo_create(back_);
    cairo_scale(cr, scale_, scale_);
    listener_->onPaint(cr, width_, height_);
    cairo_destroy(cr);
    cairo_surface_flush(back_);

    cairo_t* xcr = cairo_create(xsurface_);
    cairo_set_source_surface(xcr, back_, 0, 0);
    cairo_set_operator(xcr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(xcr);
    cairo_destroy(xcr);
    cairo_surface_flush(xsurface_);
    XFlush(display_);
}

} // namespace aurum::gui
