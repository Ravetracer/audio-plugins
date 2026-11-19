// The WCLAP's two ends of the page protocol (src/web/protocol.h), included
// into the body of RumpelKistePlugin in src/plugin.cpp and only in a
// WebAssembly build. The same class is compiled twice for the WCLAP:
//
//  - module.wasm, without the window: the plugin the host runs. Its gui
//    extension offers the webview API, its page is /index.html, and the
//    page's messages arrive in webReceive() -- on the host's audio thread in
//    openDAW, so nothing here may block, and nothing here draws.
//
//  - ui.wasm, with the window (RUMPELKISTE_WITH_GUI): the mirror, a second
//    instance the editor edits in place of the plugin (see src/web/mirror.h).
//    It never processes audio. What the window changes in it is sent to the
//    plugin as the difference to what was last sent, and what the plugin
//    reports is written into it, so every reader the window has -- the
//    playhead, the hit lamps, the meters, the bank, the values -- reads the
//    playing instrument without knowing it is not in the same process.

public:
// Moved by anything that changes the plugin's state from outside the page: the
// host loading state or a preset, a note learned onto the pattern map, a host
// event that rewrites the chains. The page loads the whole state again when
// it sees it move. Edits the page made itself do not move it.
std::atomic<uint32_t> mWebStateSerial{1};
void webStateChanged() {
   uint32_t next = mWebStateSerial.load(std::memory_order_relaxed) + 1;
   if (next == 0)
      next = 1;
   mWebStateSerial.store(next, std::memory_order_relaxed);
}

private:
// The clap.state stream into and out of memory.
struct WebStream {
   std::string bytes;
   size_t pos = 0;
   static int64_t write(const clap_ostream_t *s, const void *data, uint64_t n) {
      auto *self = static_cast<WebStream *>(s->ctx);
      self->bytes.append(static_cast<const char *>(data), static_cast<size_t>(n));
      return static_cast<int64_t>(n);
   }
   static int64_t read(const clap_istream_t *s, void *data, uint64_t n) {
      auto *self = static_cast<WebStream *>(s->ctx);
      const size_t k = std::min<size_t>(static_cast<size_t>(n), self->bytes.size() - self->pos);
      std::memcpy(data, self->bytes.data() + self->pos, k);
      self->pos += k;
      return static_cast<int64_t>(k);
   }
};

std::string webSaveState() {
   WebStream s;
   clap_ostream_t os{&s, &WebStream::write};
   stateSave(&mPlugin, &os);
   return s.bytes;
}

bool webLoadState(const uint8_t *data, size_t size) {
   WebStream s;
   s.bytes.assign(reinterpret_cast<const char *>(data), size);
   clap_istream_t is{&s, &WebStream::read};
   return stateLoad(&mPlugin, &is);
}

#if !defined(RUMPELKISTE_WITH_GUI)
// ===================================================================== plugin

const clap_host_webview_t *mWebHost = nullptr;
const clap_host_state_t *mWebHostState = nullptr;
bool mWebHostAsked = false;
// The values as the pages last got them, and the serial they got them under.
std::vector<double> mWebSent;
uint32_t mWebValueSerial = 1;
// What the editor keeps about its preset fields; see kPageMemo.
std::vector<uint8_t> mWebMemo;
// The window's size and the layout's size at scale 1, in CSS pixels, as the
// page last reported them. Until it has, the closed window at scale 1.
uint32_t mWebW = kEditorWidth, mWebH = kEditorHeight;
uint32_t mWebDesignW = kEditorWidth, mWebDesignH = kEditorHeight;

void webFindHost() {
   if (mWebHostAsked || !mHost)
      return;
   mWebHostAsked = true;
   mWebHost = static_cast<const clap_host_webview_t *>(mHost->get_extension(mHost, CLAP_EXT_WEBVIEW));
   mWebHostState = static_cast<const clap_host_state_t *>(mHost->get_extension(mHost, CLAP_EXT_STATE));
}

void webSend(const web::Writer &w) {
   webFindHost();
   if (mWebHost && mWebHost->send)
      mWebHost->send(mHost, w.bytes.data(), static_cast<uint32_t>(w.bytes.size()));
}

// One undo step in hosts that make one per save (openDAW).
void webMarkDirty() {
   webFindHost();
   if (mWebHostState && mWebHostState->mark_dirty)
      mWebHostState->mark_dirty(mHost);
}

// A value from the editor, as the window's own edits arrive on the desktop:
// stored, and reported to the host as an edit so automation records it.
void webSetParam(uint32_t id, double value) {
   const ParamDesc *d = paramById(id);
   if (!d || isChainHandle(id))
      return;
   const double v = clampv(value, d->min, d->max);
   mValues[id].store(v, std::memory_order_relaxed);
   mParamsDirty.store(true, std::memory_order_release);
   pushGuiEdit(id, v, EditKind::Value);
}

void webSendStatus(uint32_t pageId, uint32_t opsSeen, uint32_t pageSerial) {
   bool changed = mWebSent.size() != kNumParams;
   for (uint32_t i = 0; i < kNumParams && !changed; ++i)
      changed = mWebSent[i] != mValues[i].load(std::memory_order_relaxed);
   if (changed) {
      mWebSent.resize(kNumParams);
      for (uint32_t i = 0; i < kNumParams; ++i)
         mWebSent[i] = mValues[i].load(std::memory_order_relaxed);
      if (++mWebValueSerial == 0)
         mWebValueSerial = 1;
   }
   const bool withValues = pageSerial != mWebValueSerial;
   web::Writer w(web::kPluginStatus);
   w.bytes.reserve(web::kStatusHeader + (withValues ? 8u * kNumParams : 0u));
   w.put(pageId);
   w.put(opsSeen);
   w.put(mWebValueSerial);
   w.put(mWebStateSerial.load(std::memory_order_relaxed));
   w.put(mPeakL.load(std::memory_order_relaxed));
   w.put(mPeakR.load(std::memory_order_relaxed));
   w.put(mVoiceMeter.load(std::memory_order_relaxed));
   w.put(mHitCounter.load(std::memory_order_relaxed));
   w.put(static_cast<int32_t>(mPlayhead.load(std::memory_order_relaxed)));
   w.put(static_cast<int32_t>(mPlayingPattern.load(std::memory_order_relaxed)));
   w.put(static_cast<int32_t>(mActivePattern.load(std::memory_order_relaxed)));
   w.put(static_cast<int32_t>(mLearnTarget.load(std::memory_order_relaxed)));
   w.put(mTempoPublished.load(std::memory_order_relaxed));
   w.put(static_cast<uint8_t>(mFreeRun.load(std::memory_order_relaxed) ? 1 : 0));
   w.put(static_cast<uint8_t>(mHostPlaying.load(std::memory_order_relaxed) ? 1 : 0));
   w.put(static_cast<uint16_t>(0));
   for (int v = 0; v < kNumVoices; ++v)
      w.put(mHits[v].load(std::memory_order_relaxed));
   w.put(static_cast<uint32_t>(withValues ? kNumParams : 0u));
   if (withValues)
      w.putBytes(mWebSent.data(), 8u * kNumParams);
   webSend(w);
}

void webSendState(uint32_t pageId, uint32_t opsSeen) {
   const std::string state = webSaveState();
   web::Writer w(web::kPluginState);
   w.bytes.reserve(24 + mWebMemo.size() + state.size());
   w.put(pageId);
   w.put(opsSeen);
   w.put(mWebStateSerial.load(std::memory_order_relaxed));
   w.put(static_cast<uint32_t>(mWebMemo.size()));
   w.putBytes(mWebMemo.data(), mWebMemo.size());
   w.put(static_cast<uint32_t>(state.size()));
   w.putBytes(state.data(), state.size());
   webSend(w);
}

bool webReceive(const void *data, uint32_t size) {
   if (!data || size < 4)
      return false;
   web::Reader r(data, size);
   switch (static_cast<const uint8_t *>(data)[0]) {
   case web::kPagePoll: {
      uint32_t pageId = 0, ops = 0, serial = 0;
      if (!r.get(pageId) || !r.get(ops) || !r.get(serial))
         return false;
      webSendStatus(pageId, ops, serial);
      return true;
   }
   case web::kPageBegin:
   case web::kPageEnd: {
      uint32_t id = 0;
      if (!r.get(id) || !paramById(id) || isChainHandle(id))
         return false;
      const bool begin = static_cast<const uint8_t *>(data)[0] == web::kPageBegin;
      pushGuiEdit(id, 0.0, begin ? EditKind::GestureBegin : EditKind::GestureEnd);
      if (!begin)
         webMarkDirty(); // one undo step per drag
      return true;
   }
   case web::kPageSet: {
      uint32_t id = 0;
      double v = 0.0;
      if (!r.get(id) || !r.get(v))
         return false;
      webSetParam(id, v);
      return true;
   }
   case web::kPageValues: {
      uint32_t count = 0;
      if (!r.get(count) || count > kNumParams)
         return false;
      for (uint32_t i = 0; i < count; ++i) {
         uint32_t id = 0;
         double v = 0.0;
         if (!r.get(id) || !r.get(v))
            return false;
         const ParamDesc *d = paramById(id);
         if (!d)
            continue;
         mValues[id].store(clampv(v, d->min, d->max), std::memory_order_relaxed);
      }
      mParamsDirty.store(true, std::memory_order_release);
      notifyParamValuesChanged();
      return true;
   }
   case web::kPagePattern: {
      uint32_t pat = 0;
      uint32_t steps[kMaxSteps];
      if (!r.get(pat) || pat >= static_cast<uint32_t>(kMaxPatterns) ||
          !r.getBytes(steps, sizeof(steps)))
         return false;
      for (int i = 0; i < kMaxSteps; ++i)
         mPattern[pat][i].store(steps[i], std::memory_order_relaxed);
      return true;
   }
   case web::kPageChains: {
      if (r.left() < static_cast<size_t>(kMaxPatterns) * 4)
         return false;
      for (int p = 0; p < kMaxPatterns; ++p) {
         uint8_t mode = 0, pad = 0;
         uint16_t repeat = 1;
         r.get(mode);
         r.get(pad);
         r.get(repeat);
         setChain(p, mode, repeat);
      }
      return true;
   }
   case web::kPageNoteMap: {
      int8_t map[128];
      if (!r.getBytes(map, sizeof(map)))
         return false;
      for (int note = 0; note < 128; ++note) {
         const int action = map[note];
         const bool valid = action == kNoteNone || action == kNotePrevPattern ||
                            action == kNoteNextPattern || (action >= 0 && action < kMaxPatterns);
         mNoteMap[note].store(static_cast<int8_t>(valid ? action : kNoteNone),
                              std::memory_order_relaxed);
      }
      return true;
   }
   case web::kPageAdvanced: {
      uint32_t open = 0;
      if (!r.get(open))
         return false;
      mAdvancedOpen.store(open != 0, std::memory_order_relaxed);
      return true;
   }
   case web::kPageLearn: {
      int32_t target = kNoteNone;
      if (!r.get(target))
         return false;
      mLearnTarget.store(target, std::memory_order_release);
      return true;
   }
   case web::kPageMutes: {
      uint8_t mutes[kNumVoices];
      if (!r.getBytes(mutes, sizeof(mutes)))
         return false;
      for (int v = 0; v < kNumVoices; ++v)
         mMuted[v].store(mutes[v] != 0, std::memory_order_relaxed);
      return true;
   }
   case web::kPageAudition: {
      uint32_t bits = 0;
      if (!r.get(bits))
         return false;
      mAudition.fetch_or(bits & ((1u << kNumVoices) - 1u), std::memory_order_relaxed);
      if (mHost && mHost->request_process)
         mHost->request_process(mHost);
      return true;
   }
   case web::kPageFreeRun: {
      uint32_t on = 0;
      if (!r.get(on))
         return false;
      mFreeRun.store(on != 0, std::memory_order_relaxed);
      if (mHost && mHost->request_process)
         mHost->request_process(mHost);
      return true;
   }
   case web::kPageMemo:
      mWebMemo.assign(r.here(), r.here() + r.left());
      return true;
   case web::kPageCommit:
      webMarkDirty();
      return true;
   case web::kPageGetState: {
      uint32_t pageId = 0, ops = 0;
      if (!r.get(pageId) || !r.get(ops))
         return false;
      webSendState(pageId, ops);
      return true;
   }
   case web::kPageResize: {
      uint32_t w = 0, h = 0, dw = 0, dh = 0;
      if (!r.get(w) || !r.get(h) || !r.get(dw) || !r.get(dh) || !w || !h || !dw || !dh)
         return false;
      const bool hintsMoved = dw != mWebDesignW || dh != mWebDesignH;
      mWebDesignW = dw;
      mWebDesignH = dh;
      if (!mHost)
         return true;
      auto *hostGui = static_cast<const clap_host_gui_t *>(mHost->get_extension(mHost, CLAP_EXT_GUI));
      if (!hostGui)
         return true;
      if (hintsMoved && hostGui->resize_hints_changed)
         hostGui->resize_hints_changed(mHost);
      webFitSize(&w, &h);
      if ((w != mWebW || h != mWebH) && hostGui->request_resize && hostGui->request_resize(mHost, w, h)) {
         mWebW = w;
         mWebH = h;
      }
      return true;
   }
   default:
      return false;
   }
}

// ------------------------------------------------- the gui extension, webview

// The largest size at the layout's aspect ratio inside the one asked for,
// between half and four times the layout's own size, as the window does.
void webFitSize(uint32_t *w, uint32_t *h) const {
   double s = std::min(*w / static_cast<double>(mWebDesignW), *h / static_cast<double>(mWebDesignH));
   s = std::min(4.0, std::max(0.5, s));
   *w = static_cast<uint32_t>(mWebDesignW * s + 0.5);
   *h = static_cast<uint32_t>(mWebDesignH * s + 0.5);
}

static bool webGuiIsApiSupported(const clap_plugin_t *, const char *api, bool isFloating) {
   return !isFloating && api && std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) == 0;
}
static bool webGuiGetPreferredApi(const clap_plugin_t *, const char **api, bool *isFloating) {
   *api = CLAP_WINDOW_API_WEBVIEW;
   *isFloating = false;
   return true;
}
static bool webGuiCreate(const clap_plugin_t *p, const char *api, bool isFloating) {
   RumpelKistePlugin *plug = self(p);
   if (!webGuiIsApiSupported(p, api, isFloating))
      return false;
   // A project opened with the section open starts at that height.
   if (plug->mWebDesignH == kEditorHeight && plug->mAdvancedOpen.load(std::memory_order_relaxed)) {
      plug->mWebDesignH = kEditorExpandedHeight;
      plug->mWebH = kEditorExpandedHeight;
   }
   return true;
}
static void webGuiDestroy(const clap_plugin_t *) {}
// The webview API has no scale: sizes are logical, and the page draws at the
// screen's density itself.
static bool webGuiSetScale(const clap_plugin_t *, double) { return false; }
static bool webGuiGetSize(const clap_plugin_t *p, uint32_t *w, uint32_t *h) {
   *w = self(p)->mWebW;
   *h = self(p)->mWebH;
   return true;
}
static bool webGuiCanResize(const clap_plugin_t *) { return true; }
static bool webGuiGetResizeHints(const clap_plugin_t *p, clap_gui_resize_hints_t *hints) {
   if (!hints)
      return false;
   hints->can_resize_horizontally = true;
   hints->can_resize_vertically = true;
   hints->preserve_aspect_ratio = true;
   hints->aspect_ratio_width = self(p)->mWebDesignW;
   hints->aspect_ratio_height = self(p)->mWebDesignH;
   return true;
}
static bool webGuiAdjustSize(const clap_plugin_t *p, uint32_t *w, uint32_t *h) {
   if (!w || !h)
      return false;
   self(p)->webFitSize(w, h);
   return true;
}
static bool webGuiSetSize(const clap_plugin_t *p, uint32_t w, uint32_t h) {
   self(p)->mWebW = w;
   self(p)->mWebH = h;
   return true;
}
static bool webGuiSetParent(const clap_plugin_t *, const clap_window_t *) { return true; }
static bool webGuiSetTransient(const clap_plugin_t *, const clap_window_t *) { return false; }
static void webGuiSuggestTitle(const clap_plugin_t *, const char *) {}
static bool webGuiShow(const clap_plugin_t *) { return true; }
static bool webGuiHide(const clap_plugin_t *) { return true; }

static int32_t webviewGetUri(const clap_plugin_t *, char *uri, uint32_t capacity) {
   static const char page[] = "/index.html";
   if (uri && capacity > 0)
      std::snprintf(uri, capacity, "%s", page);
   return static_cast<int32_t>(sizeof(page));
}
// The host serves the bundle's files (index.html, ui.wasm, the fonts) itself.
static bool webviewGetResource(const clap_plugin_t *, const char *, char *, uint32_t,
                               const clap_ostream_t *) {
   return false;
}
static bool webviewReceive(const clap_plugin_t *p, const void *data, uint32_t size) {
   return self(p)->webReceive(data, size);
}

const void *webExtension(const char *id) const {
   static const clap_plugin_gui_t kWebGuiExt = {
      webGuiIsApiSupported, webGuiGetPreferredApi, webGuiCreate,     webGuiDestroy,
      webGuiSetScale,       webGuiGetSize,         webGuiCanResize,  webGuiGetResizeHints,
      webGuiAdjustSize,     webGuiSetSize,         webGuiSetParent,  webGuiSetTransient,
      webGuiSuggestTitle,   webGuiShow,            webGuiHide};
   static const clap_plugin_webview_t kWebviewExt = {webviewGetUri, webviewGetResource,
                                                    webviewReceive};
   if (std::strcmp(id, CLAP_EXT_GUI) == 0)
      return &kWebGuiExt;
   if (std::strcmp(id, CLAP_EXT_WEBVIEW) == 0)
      return &kWebviewExt;
   return nullptr;
}

#else
// ===================================================================== mirror

std::function<void(const uint8_t *, size_t)> mWebSendFn;
// Which page this is, so an answer to another page's poll is not taken for
// one (a host may show the page twice).
uint32_t mWebPageId = 0;
// Edits sent so far, polls and state requests not counted; the plugin says
// how many it had seen when it answered. A value the editor changed in a
// message the plugin had not seen yet is newer than the one it reports.
uint32_t mWebOps = 0;
std::vector<uint32_t> mWebLastOpFor;
uint32_t mWebLearnOp = 0;
uint32_t mWebFreeRunOp = 0;
uint32_t mWebValueSerial = 0;
uint32_t mWebStateKnown = 0;
bool mWebStateAsked = false;
bool mWebReady = false;
bool mWebUncommitted = false;
// What the plugin was last told, or last said: the difference to these is
// what the next webCollect() sends.
std::vector<double> mWebShadow;
std::vector<uint32_t> mWebShadowSteps;
int mWebShadowChainMode[kMaxPatterns] = {};
int mWebShadowChainRepeat[kMaxPatterns] = {};
int8_t mWebShadowMap[128] = {};
bool mWebShadowAdvanced = false;
int mWebShadowLearn = kNoteNone;
bool mWebShadowMuted[kNumVoices] = {};
bool mWebShadowFreeRun = false;
std::vector<uint8_t> mWebShadowMemo;

void webSetSender(std::function<void(const uint8_t *, size_t)> send) override {
   mWebSendFn = std::move(send);
   if (!mWebPageId) {
      const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
      mWebPageId = static_cast<uint32_t>(now) ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
      if (!mWebPageId)
         mWebPageId = 1;
   }
   webResync();
}

bool webReady() const override { return mWebReady; }

void webSendRaw(const web::Writer &w) {
   if (mWebSendFn)
      mWebSendFn(w.bytes.data(), w.bytes.size());
}

// An edit: counted, so the plugin's answers can be told apart from it.
uint32_t webSendEdit(const web::Writer &w) {
   webSendRaw(w);
   return ++mWebOps;
}

WebCanvas *webOpenEditor() override {
   if (!mGui) {
      ensurePresetList();
      mGui = rumpelkiste::createGui(*this, *this, *this);
   }
   return mGui ? webCanvasOf(mGui) : nullptr;
}

void webTick() override {
   if (mGui)
      mGui->tick();
}

// What the editor keeps about its preset field: which preset it came from
// and whether it has been edited. The plugin holds it for the page, so a
// reopened editor shows the field as it was left.
std::vector<uint8_t> webMemo() const {
   web::Writer w(0);
   std::string current;
   if (mCurrentPreset >= 0 && mCurrentPreset < static_cast<int>(mPresets.size())) {
      const GuiPreset &p = mPresets[static_cast<size_t>(mCurrentPreset)];
      current = p.loadKey.empty() ? p.path : "key:" + p.loadKey;
   }
   w.put(static_cast<uint8_t>(1)); // layout version
   w.put(static_cast<uint8_t>(mPresetEdited));
   w.put(static_cast<uint32_t>(current.size()));
   w.putBytes(current.data(), current.size());
   return std::vector<uint8_t>(w.bytes.begin() + 4, w.bytes.end());
}

void webApplyMemo(const uint8_t *data, size_t size) {
   if (size < 6 || data[0] != 1)
      return;
   uint32_t n = 0;
   std::memcpy(&n, data + 2, 4);
   if (6 + static_cast<size_t>(n) > size)
      return;
   const std::string current(reinterpret_cast<const char *>(data + 6), n);
   ensurePresetList();
   mCurrentPreset = -1;
   for (size_t i = 0; i < mPresets.size() && !current.empty(); ++i) {
      const GuiPreset &p = mPresets[i];
      if ((p.loadKey.empty() ? p.path : "key:" + p.loadKey) == current) {
         mCurrentPreset = static_cast<int>(i);
         break;
      }
   }
   mPresetEdited = data[1] != 0;
}

// Takes everything as it is now as what the plugin has.
void webResync() {
   mWebShadow.resize(kNumParams);
   mWebLastOpFor.resize(kNumParams, 0);
   for (uint32_t i = 0; i < kNumParams; ++i)
      mWebShadow[i] = mValues[i].load(std::memory_order_relaxed);
   mWebShadowSteps.resize(static_cast<size_t>(kMaxPatterns) * kMaxSteps);
   for (int p = 0; p < kMaxPatterns; ++p)
      for (int i = 0; i < kMaxSteps; ++i)
         mWebShadowSteps[static_cast<size_t>(p * kMaxSteps + i)] =
            mPattern[p][i].load(std::memory_order_relaxed);
   for (int p = 0; p < kMaxPatterns; ++p) {
      mWebShadowChainMode[p] = mChainMode[p].load(std::memory_order_relaxed);
      mWebShadowChainRepeat[p] = mChainRepeat[p].load(std::memory_order_relaxed);
   }
   for (int note = 0; note < 128; ++note)
      mWebShadowMap[note] = mNoteMap[note].load(std::memory_order_relaxed);
   mWebShadowAdvanced = mAdvancedOpen.load(std::memory_order_relaxed);
   mWebShadowLearn = mLearnTarget.load(std::memory_order_relaxed);
   for (int v = 0; v < kNumVoices; ++v)
      mWebShadowMuted[v] = mMuted[v].load(std::memory_order_relaxed);
   mWebShadowFreeRun = mFreeRun.load(std::memory_order_relaxed);
   mWebShadowMemo = webMemo();
   mEditRead.store(mEditWrite.load(std::memory_order_acquire), std::memory_order_release);
   mAudition.store(0, std::memory_order_relaxed);
   mWebUncommitted = false;
}

void webCollect(bool final) override {
   if (!mWebReady || !mWebSendFn) {
      webResync();
      return;
   }

   // Gestures and the values inside them, in the order the window made them.
   const uint32_t write = mEditWrite.load(std::memory_order_acquire);
   uint32_t read = mEditRead.load(std::memory_order_relaxed);
   for (; read != write; ++read) {
      const ParamEdit &edit = mEditQueue[read & (kEditQueueSize - 1)];
      if (edit.id >= kNumParams)
         continue;
      if (edit.kind == EditKind::Value) {
         web::Writer w(web::kPageSet);
         w.put(edit.id);
         w.put(edit.value);
         mWebLastOpFor[edit.id] = webSendEdit(w);
         mWebShadow[edit.id] = edit.value;
      } else {
         web::Writer w(edit.kind == EditKind::GestureBegin ? web::kPageBegin : web::kPageEnd);
         w.put(edit.id);
         webSendEdit(w);
      }
   }
   mEditRead.store(read, std::memory_order_release);

   // Values that moved without a gesture: a preset, a kit, RESET.
   {
      web::Writer w(web::kPageValues);
      w.put(static_cast<uint32_t>(0));
      uint32_t count = 0;
      std::vector<uint32_t> ids;
      for (uint32_t i = 0; i < kNumParams; ++i) {
         const double v = mValues[i].load(std::memory_order_relaxed);
         if (v == mWebShadow[i])
            continue;
         w.put(i);
         w.put(v);
         mWebShadow[i] = v;
         ids.push_back(i);
         ++count;
      }
      if (count > 0) {
         std::memcpy(w.bytes.data() + 4, &count, 4);
         const uint32_t op = webSendEdit(w);
         for (uint32_t id : ids)
            mWebLastOpFor[id] = op;
         mWebUncommitted = true;
      }
   }

   for (int p = 0; p < kMaxPatterns; ++p) {
      uint32_t steps[kMaxSteps];
      bool moved = false;
      for (int i = 0; i < kMaxSteps; ++i) {
         steps[i] = mPattern[p][i].load(std::memory_order_relaxed);
         uint32_t &shadow = mWebShadowSteps[static_cast<size_t>(p * kMaxSteps + i)];
         moved = moved || steps[i] != shadow;
         shadow = steps[i];
      }
      if (!moved)
         continue;
      web::Writer w(web::kPagePattern);
      w.put(static_cast<uint32_t>(p));
      w.putBytes(steps, sizeof(steps));
      webSendEdit(w);
      mWebUncommitted = true;
   }

   bool chainsMoved = false;
   for (int p = 0; p < kMaxPatterns; ++p) {
      const int mode = mChainMode[p].load(std::memory_order_relaxed);
      const int repeat = mChainRepeat[p].load(std::memory_order_relaxed);
      chainsMoved = chainsMoved || mode != mWebShadowChainMode[p] || repeat != mWebShadowChainRepeat[p];
      mWebShadowChainMode[p] = mode;
      mWebShadowChainRepeat[p] = repeat;
   }
   if (chainsMoved) {
      web::Writer w(web::kPageChains);
      for (int p = 0; p < kMaxPatterns; ++p) {
         w.put(static_cast<uint8_t>(mWebShadowChainMode[p]));
         w.put(static_cast<uint8_t>(0));
         w.put(static_cast<uint16_t>(mWebShadowChainRepeat[p]));
      }
      webSendEdit(w);
      mWebUncommitted = true;
   }

   bool mapMoved = false;
   for (int note = 0; note < 128; ++note) {
      const int8_t action = mNoteMap[note].load(std::memory_order_relaxed);
      mapMoved = mapMoved || action != mWebShadowMap[note];
      mWebShadowMap[note] = action;
   }
   if (mapMoved) {
      web::Writer w(web::kPageNoteMap);
      w.putBytes(mWebShadowMap, sizeof(mWebShadowMap));
      webSendEdit(w);
      mWebUncommitted = true;
   }

   const bool advanced = mAdvancedOpen.load(std::memory_order_relaxed);
   if (advanced != mWebShadowAdvanced) {
      mWebShadowAdvanced = advanced;
      web::Writer w(web::kPageAdvanced);
      w.put(static_cast<uint32_t>(advanced ? 1 : 0));
      webSendEdit(w);
      mWebUncommitted = true;
   }

   const int learn = mLearnTarget.load(std::memory_order_relaxed);
   if (learn != mWebShadowLearn) {
      mWebShadowLearn = learn;
      web::Writer w(web::kPageLearn);
      w.put(static_cast<int32_t>(learn));
      mWebLearnOp = webSendEdit(w);
   }

   bool mutesMoved = false;
   uint8_t mutes[kNumVoices];
   for (int v = 0; v < kNumVoices; ++v) {
      const bool m = mMuted[v].load(std::memory_order_relaxed);
      mutesMoved = mutesMoved || m != mWebShadowMuted[v];
      mWebShadowMuted[v] = m;
      mutes[v] = m ? 1 : 0;
   }
   if (mutesMoved) {
      web::Writer w(web::kPageMutes);
      w.putBytes(mutes, sizeof(mutes));
      webSendEdit(w);
      mWebUncommitted = true;
   }

   // PLAY is not saved, so it makes no undo step.
   const bool freeRun = mFreeRun.load(std::memory_order_relaxed);
   if (freeRun != mWebShadowFreeRun) {
      mWebShadowFreeRun = freeRun;
      web::Writer w(web::kPageFreeRun);
      w.put(static_cast<uint32_t>(freeRun ? 1 : 0));
      mWebFreeRunOp = webSendEdit(w);
   }

   if (const uint32_t audition = mAudition.exchange(0, std::memory_order_relaxed)) {
      web::Writer w(web::kPageAudition);
      w.put(audition);
      webSendEdit(w);
   }

   std::vector<uint8_t> memo = webMemo();
   if (memo != mWebShadowMemo) {
      web::Writer w(web::kPageMemo);
      w.putBytes(memo.data(), memo.size());
      webSendEdit(w);
      mWebShadowMemo.swap(memo);
   }

   if (final && mWebUncommitted) {
      mWebUncommitted = false;
      webSendEdit(web::Writer(web::kPageCommit));
   }
}

void webAskState() {
   web::Writer w(web::kPageGetState);
   w.put(mWebPageId);
   w.put(mWebOps);
   webSendRaw(w);
   mWebStateAsked = true;
}

void webPoll() override {
   if (!mWebReady && !mWebStateAsked)
      webAskState();
   web::Writer w(web::kPagePoll);
   w.put(mWebPageId);
   w.put(mWebOps);
   w.put(mWebValueSerial);
   webSendRaw(w);
}

void webReceive(const uint8_t *data, size_t size) override {
   if (!data || size < 4)
      return;
   web::Reader r(data, size);
   uint32_t pageId = 0, opsSeen = 0;
   if (!r.get(pageId) || !r.get(opsSeen) || pageId != mWebPageId)
      return;

   if (data[0] == web::kPluginState) {
      mWebStateAsked = false;
      // Edits sent after the request are not in this state, and loading it
      // would take them back out of the editor. Ask again; the answer to a
      // request made once the editor is still is the one taken.
      if (opsSeen != mWebOps) {
         webAskState();
         return;
      }
      uint32_t serial = 0, memoLen = 0, stateLen = 0;
      if (!r.get(serial) || !r.get(memoLen) || r.left() < memoLen)
         return;
      const uint8_t *memo = r.here();
      r.pos += memoLen;
      if (!r.get(stateLen) || r.left() < stateLen)
         return;
      webLoadState(r.here(), stateLen);
      webApplyMemo(memo, memoLen);
      mWebStateKnown = serial;
      // The values in the state are the plugin's; the next poll's answer
      // carries them again, which costs nothing and keeps one path.
      mWebValueSerial = 0;
      webResync();
      mWebReady = true;
      return;
   }

   if (data[0] != web::kPluginStatus || size < web::kStatusHeader)
      return;
   uint32_t valueSerial = 0, stateSerial = 0, voices = 0, hitCounter = 0, count = 0;
   uint8_t freeRun = 0, hostPlaying = 0;
   uint16_t pad = 0;
   float peakL = 0.0f, peakR = 0.0f;
   int32_t playhead = -1, playing = -1, active = -1, learn = kNoteNone;
   double tempo = 120.0;
   r.get(valueSerial);
   r.get(stateSerial);
   r.get(peakL);
   r.get(peakR);
   r.get(voices);
   r.get(hitCounter);
   r.get(playhead);
   r.get(playing);
   r.get(active);
   r.get(learn);
   r.get(tempo);
   r.get(freeRun);
   r.get(hostPlaying);
   r.get(pad);
   for (int v = 0; v < kNumVoices; ++v) {
      uint32_t hits = 0;
      r.get(hits);
      mHits[v].store(hits, std::memory_order_relaxed);
   }
   r.get(count);
   mPeakL.store(peakL, std::memory_order_relaxed);
   mPeakR.store(peakR, std::memory_order_relaxed);
   mVoiceMeter.store(voices, std::memory_order_relaxed);
   mHitCounter.store(hitCounter, std::memory_order_relaxed);
   mHostPlaying.store(hostPlaying != 0, std::memory_order_relaxed);
   if (mWebFreeRunOp <= opsSeen) {
      mFreeRun.store(freeRun != 0, std::memory_order_relaxed);
      mWebShadowFreeRun = freeRun != 0;
   }
   mPlayhead.store(playhead, std::memory_order_relaxed);
   mPlayingPattern.store(playing, std::memory_order_relaxed);
   mActivePattern.store(active, std::memory_order_relaxed);
   mTempoPublished.store(tempo, std::memory_order_relaxed);
   if (mWebLearnOp <= opsSeen) {
      mLearnTarget.store(learn, std::memory_order_relaxed);
      mWebShadowLearn = learn;
   }
   if (count == kNumParams && r.left() >= 8u * kNumParams) {
      mWebValueSerial = valueSerial;
      for (uint32_t i = 0; i < kNumParams; ++i) {
         double v = 0.0;
         r.get(v);
         if (mWebLastOpFor.size() == kNumParams && mWebLastOpFor[i] > opsSeen)
            continue; // the editor has sent a newer one
         mValues[i].store(v, std::memory_order_relaxed);
         if (mWebShadow.size() == kNumParams)
            mWebShadow[i] = v;
      }
   }
   if (mWebReady && stateSerial != mWebStateKnown && !mWebStateAsked)
      webAskState();
}

#endif
