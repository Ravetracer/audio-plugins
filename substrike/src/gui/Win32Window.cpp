#include "Win32Window.h"

#include <cairo/cairo-win32.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "Keys.h"

namespace substrike::gui {

namespace {

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// The module this code is linked into -- the plugin's own .clap or .vst3,
// never the host executable. A window class is keyed on (HINSTANCE, name);
// registering under GetModuleHandle(nullptr) would key it on the host, so a
// second plugin binary in the process using the same name would create its
// windows against this one's window procedure.
HINSTANCE moduleInstance()
{
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&wndProc), &mod);
    return reinterpret_cast<HINSTANCE>(mod);
}

// Per module as well, so a class left behind by another build of Substrike can
// never be picked up by this one.
const wchar_t* windowClassName()
{
    static wchar_t name[64] = {};
    if (!name[0])
        swprintf(name, 64, L"SubstrikeWindow_%p", static_cast<void*>(moduleInstance()));
    return name;
}

// The class is registered with the first window this module opens and
// unregistered with the last, so it never outlives the code its window
// procedure points into.
int windowCount = 0;

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (!self)
        return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handleMessage(hwnd, msg, wp, lp);
}

unsigned currentMods()
{
    unsigned m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        m |= ModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        m |= ModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000)
        m |= ModAlt;
    return m;
}

// Keys that produce no text, translated to the X11 keysym values the widgets
// test against. Zero for anything that arrives as WM_CHAR instead.
unsigned keysymFromVirtualKey(WPARAM vk, LPARAM lp)
{
    const bool extended = (lp & (1 << 24)) != 0;
    switch (vk)
    {
    case VK_BACK: return key::BackSpace;
    case VK_TAB: return key::Tab;
    case VK_RETURN: return extended ? key::KP_Enter : key::Return;
    case VK_ESCAPE: return key::Escape;
    case VK_HOME: return key::Home;
    case VK_END: return key::End;
    case VK_PRIOR: return key::PageUp;
    case VK_NEXT: return key::PageDown;
    case VK_LEFT: return key::Left;
    case VK_UP: return key::Up;
    case VK_RIGHT: return key::Right;
    case VK_DOWN: return key::Down;
    case VK_DELETE: return key::Delete;
    default: return 0;
    }
}

std::string toUtf8(const wchar_t* s)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// The data a file drag carries: one path, as CF_HDROP in global memory.
class FileDataObject final : public IDataObject
{
public:
    explicit FileDataObject(const std::wstring& path)
    {
        const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
        drop_ = GlobalAlloc(GHND, bytes);
        if (auto* df = static_cast<DROPFILES*>(GlobalLock(drop_)))
        {
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            auto* names = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(df) + sizeof(DROPFILES));
            std::wmemcpy(names, path.c_str(), path.size());
            // GHND zeroes the block, so the list already ends in two nulls.
            GlobalUnlock(drop_);
        }
    }
    ~FileDataObject()
    {
        if (drop_)
            GlobalFree(drop_);
    }
    bool valid() const { return drop_ != nullptr; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == IID_IUnknown || riid == IID_IDataObject)
        {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs_;
        if (r == 0)
            delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fmt, STGMEDIUM* medium) override
    {
        if (QueryGetData(fmt) != S_OK)
            return DV_E_FORMATETC;
        // The receiver owns what it gets, so it gets a copy.
        const SIZE_T size = GlobalSize(drop_);
        HGLOBAL copy = GlobalAlloc(GHND, size);
        if (!copy)
            return E_OUTOFMEMORY;
        void* dst = GlobalLock(copy);
        void* src = GlobalLock(drop_);
        std::memcpy(dst, src, size);
        GlobalUnlock(drop_);
        GlobalUnlock(copy);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = copy;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fmt) override
    {
        if (!fmt || fmt->cfFormat != CF_HDROP || !(fmt->tymed & TYMED_HGLOBAL) || fmt->dwAspect != DVASPECT_CONTENT)
            return DV_E_FORMATETC;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
    {
        out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    {
        if (direction != DATADIR_GET)
            return E_NOTIMPL;
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &fmt, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ULONG refs_ = 1;
    HGLOBAL drop_ = nullptr;
};

// Drops on release of the left button, cancels on Escape.
class FileDropSource final : public IDropSource
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource)
        {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs_;
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
    {
        if (escape)
            return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON))
            return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ULONG refs_ = 1;
};

std::wstring toWide(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1)
        return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

} // namespace

Win32Window::~Win32Window() { destroy(); }

bool Win32Window::startFileDrag(const std::string& path)
{
    if (!hwnd_)
        return false;
    // OLE drag and drop needs OLE on this thread, which a host has usually
    // set up already; every successful call is balanced, S_FALSE included.
    const HRESULT init = OleInitialize(nullptr);
    if (FAILED(init))
        return false;
    auto* data = new FileDataObject(toWide(path));
    auto* source = new FileDropSource();
    bool ok = false;
    if (data->valid())
    {
        HWND hwnd = static_cast<HWND>(hwnd_);
        if (GetCapture() == hwnd)
            ReleaseCapture();
        DWORD effect = DROPEFFECT_NONE;
        ok = DoDragDrop(data, source, DROPEFFECT_COPY, &effect) == DRAGDROP_S_DROP;
    }
    data->Release();
    source->Release();
    OleUninitialize();

    // The modal loop ate the button release.
    buttonsDown_ = 0;
    POINT pt{};
    GetCursorPos(&pt);
    if (hwnd_)
        ScreenToClient(static_cast<HWND>(hwnd_), &pt);
    MouseEvent e;
    e.x = static_cast<float>(pt.x / scale_);
    e.y = static_cast<float>(pt.y / scale_);
    e.button = 1;
    listener_->onMouseUp(e);
    return ok;
}

double Win32Window::systemScale()
{
    HDC dc = GetDC(nullptr);
    if (!dc)
        return 1.0;
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? std::clamp(dpi / 96.0, 1.0, 4.0) : 1.0;
}

bool Win32Window::attach(uintptr_t parent, int physW, int physH)
{
    destroy();
    width_ = physW;
    height_ = physH;
    if (windowCount == 0)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &wndProc;
        wc.hInstance = moduleInstance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // every pixel is painted, so never erase
        wc.lpszClassName = windowClassName();
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, windowClassName(), L"Substrike",
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, physW, physH,
                                reinterpret_cast<HWND>(parent), nullptr, moduleInstance(), this);
    if (!hwnd)
    {
        if (windowCount == 0)
            UnregisterClassW(windowClassName(), moduleInstance());
        return false;
    }
    ++windowCount;
    hwnd_ = hwnd;
    DragAcceptFiles(hwnd, TRUE);
    return true;
}

void Win32Window::destroy()
{
    if (hwnd_)
    {
        HWND hwnd = static_cast<HWND>(hwnd_);
        hwnd_ = nullptr;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        DestroyWindow(hwnd);
        if (--windowCount == 0)
            UnregisterClassW(windowClassName(), moduleInstance());
    }
    if (back_)
    {
        cairo_surface_destroy(back_);
        back_ = nullptr;
    }
    buttonsDown_ = 0;
    trackingLeave_ = false;
}

void Win32Window::setSize(int physW, int physH)
{
    width_ = physW;
    height_ = physH;
    if (hwnd_)
        SetWindowPos(static_cast<HWND>(hwnd_), nullptr, 0, 0, physW, physH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32Window::show()
{
    if (hwnd_)
        ShowWindow(static_cast<HWND>(hwnd_), SW_SHOWNA);
}

void Win32Window::hide()
{
    if (hwnd_)
        ShowWindow(static_cast<HWND>(hwnd_), SW_HIDE);
}

void Win32Window::grabKeyboard()
{
    if (hwnd_)
        SetFocus(static_cast<HWND>(hwnd_));
}

void Win32Window::ensureBackBuffer()
{
    if (back_ && backW_ == width_ && backH_ == height_)
        return;
    if (back_)
        cairo_surface_destroy(back_);
    back_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, std::max(width_, 1), std::max(height_, 1));
    backW_ = width_;
    backH_ = height_;
}

void Win32Window::paintBackBuffer()
{
    ensureBackBuffer();
    cairo_t* cr = cairo_create(back_);
    cairo_scale(cr, scale_, scale_);
    listener_->onPaint(cr, width_, height_);
    cairo_destroy(cr);
    cairo_surface_flush(back_);
}

void Win32Window::blit(void* hdc)
{
    cairo_surface_t* target = cairo_win32_surface_create(static_cast<HDC>(hdc));
    cairo_t* cr = cairo_create(target);
    cairo_set_source_surface(cr, back_, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(target);
    cairo_surface_destroy(target);
}

void Win32Window::render()
{
    if (!hwnd_ || width_ <= 0 || height_ <= 0)
        return;
    paintBackBuffer();
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (HDC dc = GetDC(hwnd))
    {
        blit(dc);
        ReleaseDC(hwnd, dc);
    }
}

MouseEvent Win32Window::toMouse(intptr_t lp, uintptr_t wp) const
{
    MouseEvent e;
    e.x = static_cast<float>(GET_X_LPARAM(lp) / scale_);
    e.y = static_cast<float>(GET_Y_LPARAM(lp) / scale_);
    e.mods = 0;
    if (wp & MK_SHIFT)
        e.mods |= ModShift;
    if (wp & MK_CONTROL)
        e.mods |= ModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000)
        e.mods |= ModAlt;
    return e;
}

void Win32Window::emitChar(uint32_t cp)
{
    KeyEvent k;
    k.mods = currentMods();
    if (cp < 0x20 || cp == 0x7F)
    {
        // Backspace, Tab, Return and Escape were delivered as WM_KEYDOWN. What
        // is left is Ctrl+letter, which arrives as a control code; X11 reports
        // it as the letter's keysym, and the shortcuts test for that.
        if (!(k.mods & ModCtrl) || cp == 0x08 || cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x1B ||
            cp == 0x7F)
            return;
        if (cp > 26)
            return;
        k.keysym = ((k.mods & ModShift) ? 'A' : 'a') + (cp - 1);
        listener_->onKey(k);
        return;
    }
    // Latin-1 code points are their own keysyms; beyond that X11 uses
    // 0x01000000 + code point.
    k.keysym = cp < 0x100 ? cp : 0x01000000u + cp;
    appendUtf8(k.text, cp);
    listener_->onKey(k);
}

intptr_t Win32Window::handleMessage(void* handle, unsigned msg, uintptr_t wp, intptr_t lp)
{
    // Not hwnd_: messages arrive during CreateWindowEx, before it is set, and
    // during DestroyWindow, after it is cleared.
    HWND hwnd = static_cast<HWND>(handle);
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (width_ > 0 && height_ > 0)
        {
            paintBackBuffer();
            blit(dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
    {
        const int w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0)
        {
            width_ = w;
            height_ = h;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    {
        const int button = msg == WM_LBUTTONDOWN ? 1 : (msg == WM_MBUTTONDOWN ? 2 : 3);
        // A child window is not focused by clicking it, and without focus the
        // keys (undo, Delete on an EQ band, the browser) go to the host.
        if (GetFocus() != hwnd)
            SetFocus(hwnd);
        if (buttonsDown_ == 0)
            SetCapture(hwnd);
        buttonsDown_ |= 1u << button;
        MouseEvent e = toMouse(lp, wp);
        e.button = button;
        listener_->onMouseDown(e);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    {
        const int button = msg == WM_LBUTTONUP ? 1 : (msg == WM_MBUTTONUP ? 2 : 3);
        buttonsDown_ &= ~(1u << button);
        if (buttonsDown_ == 0 && GetCapture() == hwnd)
            ReleaseCapture();
        MouseEvent e = toMouse(lp, wp);
        e.button = button;
        listener_->onMouseUp(e);
        return 0;
    }
    case WM_CAPTURECHANGED: buttonsDown_ = 0; return 0;
    case WM_MOUSEMOVE:
        if (!trackingLeave_)
        {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            trackingLeave_ = true;
        }
        listener_->onMouseMove(toMouse(lp, wp));
        return 0;
    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        listener_->onMouseLeave();
        return 0;
    case WM_MOUSEWHEEL:
    {
        // Wheel coordinates arrive in screen space, unlike every other message.
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        MouseEvent e = toMouse(MAKELPARAM(pt.x, pt.y), GET_KEYSTATE_WPARAM(wp));
        e.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        listener_->onMouseWheel(e);
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_KEYDOWN:
    {
        if (const unsigned sym = keysymFromVirtualKey(wp, lp))
        {
            KeyEvent k;
            k.keysym = sym;
            k.mods = currentMods();
            listener_->onKey(k);
            return 0;
        }
        break; // printable keys come back as WM_CHAR through TranslateMessage
    }
    case WM_CHAR:
    {
        const auto unit = static_cast<uint16_t>(wp);
        if (unit >= 0xD800 && unit < 0xDC00)
        {
            highSurrogate_ = unit;
            return 0;
        }
        uint32_t cp = unit;
        if (unit >= 0xDC00 && unit < 0xE000)
        {
            if (!highSurrogate_)
                return 0;
            cp = 0x10000 + ((static_cast<uint32_t>(highSurrogate_) - 0xD800) << 10) + (unit - 0xDC00);
        }
        highSurrogate_ = 0;
        emitChar(cp);
        return 0;
    }
    case WM_DROPFILES:
    {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        std::vector<std::string> paths;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring buf(len + 1, L'\0');
            DragQueryFileW(drop, i, buf.data(), len + 1);
            paths.push_back(toUtf8(buf.c_str()));
        }
        DragFinish(drop);
        listener_->onFilesDropped(paths);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace substrike::gui
