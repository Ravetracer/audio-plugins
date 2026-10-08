#include "X11Window.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace substrike::gui {

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
    A_TextPlain,
    A_Targets,
    A_Count
};

const char* const kAtomNames[A_Count] = {"XdndAware",      "XdndEnter",       "XdndPosition", "XdndStatus",
                                         "XdndDrop",       "XdndFinished",    "XdndActionCopy",
                                         "XdndSelection",  "XdndLeave",       "text/uri-list", "SUBSTRIKE_DND",
                                         "text/plain",     "TARGETS"};

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

// A local path as a file:// URI, percent-encoding everything but the
// unreserved characters and the separators.
std::string fileUri(const std::string& path)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out = "file://";
    for (unsigned char c : path)
    {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

double secondsNow()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
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
    drag_ = Drag{};
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
    // A target that never finished a drop: give up on it.
    if (drag_.active && drag_.dropped && secondsNow() - drag_.droppedAt > 10.0)
        dragEnd();
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
        lastTime_ = ev.xbutton.time;
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
        lastTime_ = ev.xbutton.time;
        if (drag_.active && !drag_.dropped && ev.xbutton.button == Button1)
            dragRelease(ev.xbutton.time);
        if (ev.xbutton.button <= Button3)
        {
            MouseEvent e = toMouse(ev.xbutton.x, ev.xbutton.y, ev.xbutton.state);
            e.button = ev.xbutton.button == Button1 ? 1 : (ev.xbutton.button == Button2 ? 2 : 3);
            listener_->onMouseUp(e);
        }
        break;
    case MotionNotify:
        lastTime_ = ev.xmotion.time;
        // While a file is dragged out the widgets see nothing of the pointer.
        if (drag_.active && !drag_.dropped)
            dragMotion(ev.xmotion.x_root, ev.xmotion.y_root, ev.xmotion.time);
        else
            listener_->onMouseMove(toMouse(ev.xmotion.x, ev.xmotion.y, ev.xmotion.state));
        break;
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
        else if (static_cast<unsigned long>(ev.xclient.message_type) == atoms_[A_XdndStatus])
            dragStatus(xevent);
        else if (static_cast<unsigned long>(ev.xclient.message_type) == atoms_[A_XdndFinished])
        {
            if (drag_.active && static_cast<unsigned long>(ev.xclient.data.l[0]) == drag_.target)
                dragEnd();
        }
        break;
    case SelectionNotify: handleSelectionNotify(xevent); break;
    case SelectionRequest: answerSelectionRequest(xevent); break;
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

// ----------------------------------------------------------- drag source

bool X11Window::startFileDrag(const std::string& path)
{
    if (!display_ || !window_ || drag_.active)
        return false;
    XSetSelectionOwner(display_, atoms_[A_XdndSelection], window_, lastTime_);
    if (XGetSelectionOwner(display_, atoms_[A_XdndSelection]) != window_)
        return false;
    drag_ = Drag{};
    drag_.active = true;
    drag_.uri = fileUri(path);
    return true;
}

// The window under the pointer that takes XDND drops: walk down from the
// root, through the window manager's frames, to the first window that says
// it is XdndAware. Never this window itself.
unsigned long X11Window::findDropTarget(int rootX, int rootY, int& version)
{
    const Window root = DefaultRootWindow(display_);
    Window cur = root;
    for (int depth = 0; depth < 32; ++depth)
    {
        int x, y;
        Window child = 0;
        if (!XTranslateCoordinates(display_, root, cur, rootX, rootY, &x, &y, &child))
            return 0;
        if (cur != root)
        {
            Atom type;
            int format;
            unsigned long count, remaining;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display_, cur, atoms_[A_XdndAware], 0, 1, False, AnyPropertyType, &type, &format,
                                   &count, &remaining, &data) == Success &&
                data)
            {
                const long v = count > 0 ? *reinterpret_cast<long*>(data) : 0;
                XFree(data);
                if (count > 0 && v >= 3)
                {
                    if (cur == window_)
                        return 0;
                    version = std::min(5, static_cast<int>(v));
                    return cur;
                }
            }
            else if (data)
                XFree(data);
        }
        if (!child)
            return 0;
        cur = child;
    }
    return 0;
}

void X11Window::dragSend(unsigned long target, int message, long l1, long l2, long l3, long l4)
{
    XEvent e{};
    e.xclient.type = ClientMessage;
    e.xclient.display = display_;
    e.xclient.window = static_cast<Window>(target);
    e.xclient.message_type = atoms_[message];
    e.xclient.format = 32;
    e.xclient.data.l[0] = static_cast<long>(window_);
    e.xclient.data.l[1] = l1;
    e.xclient.data.l[2] = l2;
    e.xclient.data.l[3] = l3;
    e.xclient.data.l[4] = l4;
    XSendEvent(display_, static_cast<Window>(target), False, NoEventMask, &e);
    XFlush(display_);
}

void X11Window::dragSendPosition()
{
    dragSend(drag_.target, A_XdndPosition, 0, (static_cast<long>(drag_.x) << 16) | (drag_.y & 0xffff),
             static_cast<long>(drag_.time), static_cast<long>(atoms_[A_XdndActionCopy]));
    drag_.waitingStatus = true;
    drag_.positionQueued = false;
}

void X11Window::dragMotion(int rootX, int rootY, unsigned long time)
{
    int version = 5;
    const unsigned long target = findDropTarget(rootX, rootY, version);
    if (target != drag_.target)
    {
        if (drag_.target)
            dragSend(drag_.target, A_XdndLeave, 0, 0, 0, 0);
        drag_.target = target;
        drag_.accepted = false;
        drag_.waitingStatus = false;
        drag_.positionQueued = false;
        if (target)
        {
            drag_.version = version;
            dragSend(target, A_XdndEnter, static_cast<long>(version) << 24, static_cast<long>(atoms_[A_UriList]),
                     static_cast<long>(atoms_[A_TextPlain]), 0);
        }
    }
    if (!target)
        return;
    drag_.x = rootX;
    drag_.y = rootY;
    drag_.time = time;
    // One position at a time: the next goes out when the status comes back.
    if (drag_.waitingStatus)
        drag_.positionQueued = true;
    else
        dragSendPosition();
}

void X11Window::dragStatus(void* xevent)
{
    XEvent& ev = *static_cast<XEvent*>(xevent);
    if (!drag_.active || static_cast<unsigned long>(ev.xclient.data.l[0]) != drag_.target)
        return;
    drag_.accepted = (ev.xclient.data.l[1] & 1) != 0;
    drag_.waitingStatus = false;
    if (drag_.dropPending)
    {
        drag_.dropPending = false;
        dragRelease(drag_.time);
        return;
    }
    if (drag_.positionQueued)
        dragSendPosition();
}

void X11Window::dragRelease(unsigned long time)
{
    if (!drag_.target)
    {
        dragEnd();
        return;
    }
    if (drag_.waitingStatus)
    {
        // Let the target answer the last position first.
        drag_.dropPending = true;
        drag_.time = time;
        return;
    }
    if (!drag_.accepted)
    {
        dragSend(drag_.target, A_XdndLeave, 0, 0, 0, 0);
        dragEnd();
        return;
    }
    dragSend(drag_.target, A_XdndDrop, 0, drag_.version >= 1 ? static_cast<long>(time) : 0, 0, 0);
    drag_.dropped = true;
    drag_.droppedAt = secondsNow();
}

void X11Window::dragEnd() { drag_ = Drag{}; }

void X11Window::answerSelectionRequest(void* xevent)
{
    XSelectionRequestEvent& req = static_cast<XEvent*>(xevent)->xselectionrequest;
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = display_;
    reply.xselection.requestor = req.requestor;
    reply.xselection.selection = req.selection;
    reply.xselection.target = req.target;
    reply.xselection.time = req.time;
    reply.xselection.property = None;
    const Atom property = req.property != None ? req.property : req.target;
    if (static_cast<unsigned long>(req.selection) == atoms_[A_XdndSelection] && !drag_.uri.empty())
    {
        if (static_cast<unsigned long>(req.target) == atoms_[A_UriList] ||
            static_cast<unsigned long>(req.target) == atoms_[A_TextPlain])
        {
            const std::string data = drag_.uri + "\r\n";
            XChangeProperty(display_, req.requestor, property, req.target, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
            reply.xselection.property = property;
        }
        else if (static_cast<unsigned long>(req.target) == atoms_[A_Targets])
        {
            const Atom targets[3] = {static_cast<Atom>(atoms_[A_Targets]), static_cast<Atom>(atoms_[A_UriList]),
                                     static_cast<Atom>(atoms_[A_TextPlain])};
            XChangeProperty(display_, req.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(targets), 3);
            reply.xselection.property = property;
        }
    }
    XSendEvent(display_, req.requestor, False, NoEventMask, &reply);
    XFlush(display_);
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

} // namespace substrike::gui
