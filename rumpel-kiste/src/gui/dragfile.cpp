#include "dragfile.h"

#include <cstring>
#include <string>

#if defined(_WIN32)

#include <windows.h>

#include <ole2.h>
#include <shlobj.h>

namespace rumpelkiste {
namespace {

// The smallest IDataObject that a drop target will take a file from: one
// format, CF_HDROP, holding one path.
//
// Written by hand rather than with ATL because the plugin links neither ATL nor
// the C runtime's COM helpers, and because what is needed is small: a target
// asks whether we have CF_HDROP, then asks for it.
class FileDataObject : public IDataObject {
public:
   explicit FileDataObject(const std::wstring &path) : mPath(path) {}
   virtual ~FileDataObject() = default;

   // --- IUnknown
   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
      if (!out)
         return E_POINTER;
      if (riid == IID_IUnknown || riid == IID_IDataObject) {
         *out = static_cast<IDataObject *>(this);
         AddRef();
         return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
   }
   ULONG STDMETHODCALLTYPE AddRef() override { return ++mRefs; }
   ULONG STDMETHODCALLTYPE Release() override {
      const ULONG n = --mRefs;
      if (n == 0)
         delete this;
      return n;
   }

   // --- IDataObject
   HRESULT STDMETHODCALLTYPE GetData(FORMATETC *fmt, STGMEDIUM *med) override {
      if (!fmt || !med)
         return E_POINTER;
      if (QueryGetData(fmt) != S_OK)
         return DV_E_FORMATETC;

      // DROPFILES, then the path, then two terminating nulls: the shell's
      // format for "these files", with exactly one in it.
      const size_t chars = mPath.size() + 2;
      const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
      HGLOBAL mem = GlobalAlloc(GHND, bytes);
      if (!mem)
         return E_OUTOFMEMORY;
      auto *df = static_cast<DROPFILES *>(GlobalLock(mem));
      if (!df) {
         GlobalFree(mem);
         return E_OUTOFMEMORY;
      }
      std::memset(df, 0, bytes);
      df->pFiles = sizeof(DROPFILES);
      df->fWide = TRUE;
      wchar_t *dst = reinterpret_cast<wchar_t *>(reinterpret_cast<char *>(df) + sizeof(DROPFILES));
      std::memcpy(dst, mPath.c_str(), mPath.size() * sizeof(wchar_t));
      dst[mPath.size()] = L'\0';
      dst[mPath.size() + 1] = L'\0';
      GlobalUnlock(mem);

      med->tymed = TYMED_HGLOBAL;
      med->hGlobal = mem;
      med->pUnkForRelease = nullptr;
      return S_OK;
   }

   HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }

   HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *fmt) override {
      if (!fmt)
         return E_POINTER;
      if (fmt->cfFormat != CF_HDROP)
         return DV_E_FORMATETC;
      if (!(fmt->tymed & TYMED_HGLOBAL))
         return DV_E_TYMED;
      if (fmt->dwAspect != DVASPECT_CONTENT)
         return DV_E_DVASPECT;
      return S_OK;
   }

   HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
      if (out)
         out->ptd = nullptr;
      return E_NOTIMPL;
   }
   HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override {
      return E_NOTIMPL;
   }
   HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override;
   HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
      return OLE_E_ADVISENOTSUPPORTED;
   }
   HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
   HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override {
      return OLE_E_ADVISENOTSUPPORTED;
   }

private:
   std::wstring mPath;
   ULONG mRefs = 1;
};

// One format, enumerated. Targets that ask what we have rather than whether we
// have CF_HDROP need this to exist.
class OneFormatEnum : public IEnumFORMATETC {
public:
   virtual ~OneFormatEnum() = default;
   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
      if (!out)
         return E_POINTER;
      if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
         *out = static_cast<IEnumFORMATETC *>(this);
         AddRef();
         return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
   }
   ULONG STDMETHODCALLTYPE AddRef() override { return ++mRefs; }
   ULONG STDMETHODCALLTYPE Release() override {
      const ULONG n = --mRefs;
      if (n == 0)
         delete this;
      return n;
   }

   HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC *out, ULONG *fetched) override {
      ULONG n = 0;
      if (count > 0 && mAt == 0 && out) {
         std::memset(out, 0, sizeof(*out));
         out->cfFormat = CF_HDROP;
         out->dwAspect = DVASPECT_CONTENT;
         out->lindex = -1;
         out->tymed = TYMED_HGLOBAL;
         mAt = 1;
         n = 1;
      }
      if (fetched)
         *fetched = n;
      return n == count ? S_OK : S_FALSE;
   }
   HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
      mAt += count;
      return mAt <= 1 ? S_OK : S_FALSE;
   }
   HRESULT STDMETHODCALLTYPE Reset() override {
      mAt = 0;
      return S_OK;
   }
   HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC **out) override {
      if (!out)
         return E_POINTER;
      auto *copy = new OneFormatEnum();
      copy->mAt = mAt;
      *out = copy;
      return S_OK;
   }

private:
   ULONG mAt = 0;
   ULONG mRefs = 1;
};

HRESULT STDMETHODCALLTYPE FileDataObject::EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) {
   if (!out)
      return E_POINTER;
   if (direction != DATADIR_GET) {
      *out = nullptr;
      return E_NOTIMPL;
   }
   *out = new OneFormatEnum();
   return S_OK;
}

// The drag itself: carry on while the left button is down, cancel on Escape.
class DropSource : public IDropSource {
public:
   virtual ~DropSource() = default;
   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
      if (!out)
         return E_POINTER;
      if (riid == IID_IUnknown || riid == IID_IDropSource) {
         *out = static_cast<IDropSource *>(this);
         AddRef();
         return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
   }
   ULONG STDMETHODCALLTYPE AddRef() override { return ++mRefs; }
   ULONG STDMETHODCALLTYPE Release() override {
      const ULONG n = --mRefs;
      if (n == 0)
         delete this;
      return n;
   }

   HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keyState) override {
      if (escape)
         return DRAGDROP_S_CANCEL;
      if (!(keyState & MK_LBUTTON))
         return DRAGDROP_S_DROP;
      return S_OK;
   }
   HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
   ULONG mRefs = 1;
};

} // namespace

bool dragFileOut(void *, uintptr_t, const std::string &path) {
   if (path.empty())
      return false;

   const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
   if (wide <= 0)
      return false;
   std::wstring wpath(static_cast<size_t>(wide - 1), L'\0');
   MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wide);

   // The host's GUI thread is usually already an apartment; joining it is fine
   // and RPC_E_CHANGED_MODE only means somebody got there first.
   const HRESULT init = OleInitialize(nullptr);
   const bool ours = init == S_OK;

   auto *data = new FileDataObject(wpath);
   auto *source = new DropSource();
   DWORD effect = DROPEFFECT_NONE;
   const HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
   data->Release();
   source->Release();

   if (ours)
      OleUninitialize();
   return hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
}

} // namespace rumpelkiste

#else // ------------------------------------------------------------------ X11

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>

#include <ctime>

namespace rumpelkiste {
namespace {

// XDND version 5. Everything in the protocol is a ClientMessage between the
// source window and the target window, with the payload handed over through a
// selection afterwards.
constexpr long kXdndVersion = 5;

struct Atoms {
   Atom aware, selection, enter, position, status, drop, leave, finished, actionCopy, uriList,
      plainText;
};

Atoms internAtoms(Display *dpy) {
   Atoms a;
   a.aware = XInternAtom(dpy, "XdndAware", False);
   a.selection = XInternAtom(dpy, "XdndSelection", False);
   a.enter = XInternAtom(dpy, "XdndEnter", False);
   a.position = XInternAtom(dpy, "XdndPosition", False);
   a.status = XInternAtom(dpy, "XdndStatus", False);
   a.drop = XInternAtom(dpy, "XdndDrop", False);
   a.leave = XInternAtom(dpy, "XdndLeave", False);
   a.finished = XInternAtom(dpy, "XdndFinished", False);
   a.actionCopy = XInternAtom(dpy, "XdndActionCopy", False);
   a.uriList = XInternAtom(dpy, "text/uri-list", False);
   a.plainText = XInternAtom(dpy, "text/plain", False);
   return a;
}

int ignoreXError(Display *, XErrorEvent *) { return 0; }

// The XdndAware window for a given window: itself if it has the property, else
// the nearest ancestor that does. A toolkit may put it on the shell rather than
// on the leaf the pointer is actually over.
Window awareAncestor(Display *dpy, const Atoms &a, Window w, long *versionOut) {
   while (w != None) {
      Atom type = None;
      int format = 0;
      unsigned long items = 0, after = 0;
      unsigned char *data = nullptr;
      if (XGetWindowProperty(dpy, w, a.aware, 0, 1, False, AnyPropertyType, &type, &format, &items,
                             &after, &data) == Success) {
         const bool has = type != None && data != nullptr && items > 0;
         long version = has ? static_cast<long>(*reinterpret_cast<unsigned char *>(data)) : 0;
         if (data)
            XFree(data);
         if (has) {
            if (versionOut)
               *versionOut = version < kXdndVersion ? version : kXdndVersion;
            return w;
         }
      }
      Window root = None, parent = None, *children = nullptr;
      unsigned int count = 0;
      if (!XQueryTree(dpy, w, &root, &parent, &children, &count))
         return None;
      if (children)
         XFree(children);
      if (parent == None || parent == root)
         return None;
      w = parent;
   }
   return None;
}

// The window under the pointer, descending from the root.
Window windowUnderPointer(Display *dpy, Window root, int *rootX, int *rootY) {
   Window child = root, result = root, dummy = None;
   int x = 0, y = 0, wx = 0, wy = 0;
   unsigned int mask = 0;
   while (child != None) {
      result = child;
      if (!XQueryPointer(dpy, result, &dummy, &child, &x, &y, &wx, &wy, &mask))
         break;
      if (rootX)
         *rootX = x;
      if (rootY)
         *rootY = y;
      if (child == None)
         break;
   }
   return result;
}

void sendMessage(Display *dpy, Window to, Window from, Atom type, long d1, long d2, long d3,
                 long d4) {
   XClientMessageEvent ev;
   std::memset(&ev, 0, sizeof(ev));
   ev.type = ClientMessage;
   ev.display = dpy;
   ev.window = to;
   ev.message_type = type;
   ev.format = 32;
   ev.data.l[0] = static_cast<long>(from);
   ev.data.l[1] = d1;
   ev.data.l[2] = d2;
   ev.data.l[3] = d3;
   ev.data.l[4] = d4;
   XSendEvent(dpy, to, False, NoEventMask, reinterpret_cast<XEvent *>(&ev));
}

// Percent-encode what a URI may not carry literally. A preset name can be
// anything the user typed, and the path it lands in goes into the drop.
std::string toFileUri(const std::string &path) {
   static const char *hex = "0123456789ABCDEF";
   std::string out = "file://";
   for (unsigned char c : path) {
      const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
      if (safe) {
         out.push_back(static_cast<char>(c));
      } else {
         out.push_back('%');
         out.push_back(hex[c >> 4]);
         out.push_back(hex[c & 0x0F]);
      }
   }
   out += "\r\n";
   return out;
}

double nowSeconds() {
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

} // namespace

bool dragFileOut(void *displayPtr, uintptr_t windowId, const std::string &path) {
   auto *dpy = static_cast<Display *>(displayPtr);
   const Window src = static_cast<Window>(windowId);
   if (!dpy || src == None || path.empty())
      return false;

   const Atoms a = internAtoms(dpy);
   const std::string uri = toFileUri(path);
   const Window root = DefaultRootWindow(dpy);

   // A host that has taken the pointer elsewhere, or a second client already
   // owning the selection, is a reason to give up quietly rather than to sulk.
   XSetSelectionOwner(dpy, a.selection, src, CurrentTime);
   if (XGetSelectionOwner(dpy, a.selection) != src)
      return false;

   Atom offered[2] = {a.uriList, a.plainText};
   XChangeProperty(dpy, src, XInternAtom(dpy, "XdndTypeList", False), XA_ATOM, 32, PropModeReplace,
                   reinterpret_cast<unsigned char *>(offered), 2);

   Cursor cursor = XCreateFontCursor(dpy, XC_hand2);
   XGrabPointer(dpy, src, True, ButtonMotionMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, cursor, CurrentTime);
   XFlush(dpy);

   XErrorHandler previous = XSetErrorHandler(&ignoreXError);

   Window target = None;
   long targetVersion = kXdndVersion;
   bool willAccept = false, dropped = false, finished = false, took = false;
   // Two limits, because the two halves fail differently: a drag nobody ever
   // ends, and a target that takes the drop and never says it is finished.
   const double dragDeadline = nowSeconds() + 120.0;
   double dropDeadline = 0.0;

   while (!finished) {
      const double now = nowSeconds();
      if (!dropped && now > dragDeadline)
         break;
      if (dropped && now > dropDeadline)
         break;

      // Serving the selection carries on after the button comes up: the target
      // asks for the data once it has taken the drop, not before.
      if (!XPending(dpy)) {
         XFlush(dpy);
         struct timespec nap = {0, 2 * 1000 * 1000};
         nanosleep(&nap, nullptr);
         continue;
      }

      XEvent ev;
      XNextEvent(dpy, &ev);
      switch (ev.type) {
      case MotionNotify: {
         if (dropped)
            break;
         int rx = 0, ry = 0;
         const Window under = windowUnderPointer(dpy, root, &rx, &ry);
         long version = kXdndVersion;
         const Window found = awareAncestor(dpy, a, under, &version);
         if (found != target) {
            if (target != None)
               sendMessage(dpy, target, src, a.leave, 0, 0, 0, 0);
            target = found;
            targetVersion = version;
            willAccept = false;
            if (target != None)
               sendMessage(dpy, target, src, a.enter, (targetVersion << 24),
                           static_cast<long>(a.uriList), static_cast<long>(a.plainText), 0);
         }
         if (target != None)
            sendMessage(dpy, target, src, a.position, 0, (rx << 16) | (ry & 0xFFFF),
                        static_cast<long>(CurrentTime), static_cast<long>(a.actionCopy));
         break;
      }
      case ButtonRelease: {
         if (dropped)
            break;
         XUngrabPointer(dpy, CurrentTime);
         if (target != None && willAccept) {
            sendMessage(dpy, target, src, a.drop, 0, static_cast<long>(CurrentTime), 0, 0);
            dropped = true;
            took = true; // until an XdndFinished says otherwise
            dropDeadline = nowSeconds() + 3.0;
         } else {
            if (target != None)
               sendMessage(dpy, target, src, a.leave, 0, 0, 0, 0);
            finished = true;
         }
         break;
      }
      case SelectionRequest: {
         const XSelectionRequestEvent &rq = ev.xselectionrequest;
         XSelectionEvent note;
         std::memset(&note, 0, sizeof(note));
         note.type = SelectionNotify;
         note.display = rq.display;
         note.requestor = rq.requestor;
         note.selection = rq.selection;
         note.target = rq.target;
         note.time = rq.time;
         note.property = None;
         const Atom targets = XInternAtom(dpy, "TARGETS", False);
         if (rq.target == targets) {
            Atom list[3] = {targets, a.uriList, a.plainText};
            XChangeProperty(dpy, rq.requestor, rq.property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<unsigned char *>(list), 3);
            note.property = rq.property;
         } else if (rq.target == a.uriList || rq.target == a.plainText) {
            XChangeProperty(dpy, rq.requestor, rq.property, rq.target, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char *>(uri.c_str()),
                            static_cast<int>(uri.size()));
            note.property = rq.property;
         }
         XSendEvent(dpy, rq.requestor, False, NoEventMask, reinterpret_cast<XEvent *>(&note));
         XFlush(dpy);
         break;
      }
      case ClientMessage: {
         if (ev.xclient.message_type == a.status) {
            willAccept = (ev.xclient.data.l[1] & 1) != 0;
         } else if (ev.xclient.message_type == a.finished) {
            finished = true;
            // Version 5 says in bit 0 whether the target actually took it.
            if (targetVersion >= 5)
               took = (ev.xclient.data.l[1] & 1) != 0;
         }
         break;
      }
      default:
         break;
      }
   }

   XUngrabPointer(dpy, CurrentTime);
   if (cursor != None)
      XFreeCursor(dpy, cursor);
   XSetErrorHandler(previous);
   XFlush(dpy);
   return took;
}

} // namespace rumpelkiste

#endif
