#include "PresetBrowser.h"

#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "Editor.h"
#include "state/Settings.h"

namespace aurum::gui {

namespace {

constexpr float kRowH = 22.0f;

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Folder names may carry an ordering prefix ("02 Halls"); hide it.
std::string displayFolder(const std::string& f)
{
    std::string leaf = std::filesystem::path(f).filename().string();
    if (leaf.size() > 3 && std::isdigit(static_cast<unsigned char>(leaf[0])) &&
        std::isdigit(static_cast<unsigned char>(leaf[1])) && leaf[2] == ' ')
        leaf = leaf.substr(3);
    const size_t depth = static_cast<size_t>(std::count(f.begin(), f.end(), '/'));
    return std::string(depth * 3, ' ') + leaf;
}

// Simple word wrap.
std::vector<std::string> wrap(cairo_t* cr, const std::string& text, float width)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&] {
        if (!word.empty())
        {
            const std::string test = line.empty() ? word : line + " " + word;
            if (textWidth(cr, test) > width && !line.empty())
            {
                lines.push_back(line);
                line = word;
            }
            else
                line = test;
            word.clear();
        }
    };
    for (char c : text)
    {
        if (c == ' ' || c == '\n')
        {
            flush();
            if (c == '\n')
            {
                lines.push_back(line);
                line.clear();
            }
        }
        else
            word += c;
    }
    flush();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

} // namespace

PresetBrowser::PresetBrowser(Editor& editor, PresetSession& session) : editor_(editor), session_(session)
{
    const Rect eb = editor.bounds();
    const float w = std::min(800.0f, eb.w - 40), h = std::min(460.0f, eb.h - 80);
    bounds_ = {eb.cx() - w * 0.5f, 40, w, h};
    refresh();
    // Start on the folder of the current preset.
    const int cur = PresetManager::get().indexOf(session_.path());
    if (cur >= 0)
    {
        const std::string f = PresetManager::get().presets()[static_cast<size_t>(cur)].folder;
        for (size_t i = 0; i < folders_.size(); ++i)
            if (folders_[i] == f)
                folder_ = static_cast<int>(i) + 1;
        refresh();
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i] == cur)
                cursor_ = static_cast<int>(i);
    }
    layout();
}

void PresetBrowser::layout()
{
    frame_ = bounds_;
    header_ = {frame_.x + 8, frame_.y + 8, frame_.w - 16, 26};
    optionsRect_ = {header_.x, header_.y, 28, 26};
    searchRect_ = {header_.x + 34, header_.y, header_.w - 34 - 70, 26};
    favRect_ = {searchRect_.right() + 6, header_.y, 28, 26};
    closeRect_ = {header_.right() - 28, header_.y, 28, 26};
    const float top = header_.bottom() + 8, h = frame_.bottom() - top - 8;
    folderRect_ = {frame_.x + 8, top, 180, h};
    detailRect_ = {frame_.right() - 8 - 230, top, 230, h};
    listRect_ = {folderRect_.right() + 6, top, detailRect_.x - folderRect_.right() - 12, h};
}

void PresetBrowser::refresh()
{
    PresetManager& pm = PresetManager::get();
    folders_ = pm.folders();
    if (folder_ > static_cast<int>(folders_.size()))
        folder_ = 0;
    rows_.clear();
    const std::string q = lower(search_);
    const auto& list = pm.presets();
    for (size_t i = 0; i < list.size(); ++i)
    {
        const PresetInfo& p = list[i];
        if (folder_ > 0 && q.empty())
        {
            const std::string& f = folders_[static_cast<size_t>(folder_ - 1)];
            if (p.folder != f && p.folder.rfind(f + "/", 0) != 0)
                continue;
        }
        if (favoritesOnly_ && !pm.isFavorite(p.relPath))
            continue;
        if (!q.empty())
        {
            bool match = lower(p.name).find(q) != std::string::npos || lower(p.folder).find(q) != std::string::npos;
            for (const auto& t : p.tags)
                match |= lower(t).find(q) != std::string::npos;
            if (!match)
                continue;
        }
        rows_.push_back(static_cast<int>(i));
    }
    cursor_ = std::min(cursor_, static_cast<int>(rows_.size()) - 1);
    scroll_ = std::clamp(scroll_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
    repaint();
}

void PresetBrowser::close()
{
    if (RootWidget* r = root())
        r->closeOverlay(this);
}

void PresetBrowser::loadRow(int row, bool closeAfter)
{
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;
    cursor_ = row;
    session_.load(PresetManager::get().presets()[static_cast<size_t>(rows_[static_cast<size_t>(row)])].path);
    loadedFromHere_ = true;
    if (closeAfter)
        close();
    repaint();
}

int PresetBrowser::rowAt(float y) const
{
    if (y < listRect_.y + 4)
        return -1;
    const int r = static_cast<int>((y - listRect_.y - 4) / kRowH) + scroll_;
    return r < static_cast<int>(rows_.size()) ? r : -1;
}

int PresetBrowser::folderAt(float y) const
{
    if (y < folderRect_.y + 4)
        return -1;
    const int r = static_cast<int>((y - folderRect_.y - 4) / kRowH);
    return r <= static_cast<int>(folders_.size()) ? r : -1;
}

const PresetInfo* PresetBrowser::detailPreset() const
{
    const auto& list = PresetManager::get().presets();
    int row = hoverRow_ >= 0 ? hoverRow_ : cursor_;
    if (row >= 0 && row < static_cast<int>(rows_.size()))
        return &list[static_cast<size_t>(rows_[static_cast<size_t>(row)])];
    const int cur = PresetManager::get().indexOf(session_.path());
    return cur >= 0 ? &list[static_cast<size_t>(cur)] : nullptr;
}

void PresetBrowser::paint(cairo_t* cr)
{
    fillRounded(cr, {frame_.x + 2, frame_.y + 4, frame_.w, frame_.h}, 10, Color(0, 0, 0, 0.45f));
    fillRounded(cr, frame_, 10, theme::panelLight);
    strokeRounded(cr, frame_, 10, theme::outline);

    // Header
    fillRounded(cr, optionsRect_, 5, theme::panel);
    icons::menu(cr, optionsRect_, theme::textDim);
    fillRounded(cr, searchRect_, 5, theme::panel);
    strokeRounded(cr, searchRect_, 5, search_.empty() ? theme::outline : theme::gold.withAlpha(0.6f));
    setFont(cr, 12);
    if (search_.empty())
        drawText(cr, "Type to search presets, folders and tags", searchRect_.reduced(10, 0), Align::Left,
                 theme::textFaint);
    else
        drawText(cr, search_ + "|", searchRect_.reduced(10, 0), Align::Left, theme::text);
    fillRounded(cr, favRect_, 5, favoritesOnly_ ? theme::gold.withAlpha(0.25f) : theme::panel);
    setFont(cr, 15);
    drawText(cr, favoritesOnly_ ? "★" : "☆", favRect_, Align::Center,
             favoritesOnly_ ? theme::goldBright : theme::textDim);
    icons::close(cr, closeRect_, theme::textDim);

    // Folders
    fillRounded(cr, folderRect_, 6, theme::panel);
    setFont(cr, 11.5f);
    for (int i = 0; i <= static_cast<int>(folders_.size()); ++i)
    {
        const Rect row{folderRect_.x + 3, folderRect_.y + 4 + i * kRowH, folderRect_.w - 6, kRowH};
        if (row.bottom() > folderRect_.bottom())
            break;
        if (i == folder_ && search_.empty())
            fillRounded(cr, row, 4, theme::gold.withAlpha(0.18f));
        else if (i == hoverFolder_)
            fillRounded(cr, row, 4, Color(1, 1, 1, 0.05f));
        const std::string label = i == 0 ? "All Presets" : displayFolder(folders_[static_cast<size_t>(i - 1)]);
        drawText(cr, label, row.reduced(8, 0), Align::Left, i == folder_ ? theme::text : theme::textDim);
    }

    // Presets
    fillRounded(cr, listRect_, 6, theme::panel);
    cairo_save(cr);
    cairo_rectangle(cr, listRect_.x, listRect_.y, listRect_.w, listRect_.h);
    cairo_clip(cr);
    const int cur = PresetManager::get().indexOf(session_.path());
    const auto& list = PresetManager::get().presets();
    for (int r = scroll_; r < static_cast<int>(rows_.size()); ++r)
    {
        const Rect row{listRect_.x + 3, listRect_.y + 4 + (r - scroll_) * kRowH, listRect_.w - 6, kRowH};
        if (row.y > listRect_.bottom())
            break;
        const PresetInfo& p = list[static_cast<size_t>(rows_[static_cast<size_t>(r)])];
        const bool isCur = rows_[static_cast<size_t>(r)] == cur;
        if (r == cursor_)
            fillRounded(cr, row, 4, theme::gold.withAlpha(0.2f));
        else if (r == hoverRow_)
            fillRounded(cr, row, 4, Color(1, 1, 1, 0.05f));
        setFont(cr, 12, isCur);
        drawText(cr, p.name, {row.x + 10, row.y, row.w - 40, row.h}, Align::Left, isCur ? theme::goldBright : theme::text);
        if (PresetManager::get().isFavorite(p.relPath))
        {
            setFont(cr, 11);
            drawText(cr, "★", {row.right() - 22, row.y, 16, row.h}, Align::Center, theme::gold);
        }
        if (!search_.empty() || folder_ == 0)
        {
            setFont(cr, 10);
            drawText(cr, displayFolder(p.folder), {row.x + 10, row.y, row.w - 34, row.h}, Align::Right, theme::textFaint);
        }
    }
    if (rows_.empty())
    {
        setFont(cr, 12);
        drawText(cr, "No presets found", listRect_, Align::Center, theme::textFaint);
    }
    cairo_restore(cr);

    paintDetails(cr, detailRect_);
}

void PresetBrowser::paintDetails(cairo_t* cr, const Rect& r)
{
    fillRounded(cr, r, 6, theme::panel);
    tagRemoveRects_.clear();
    const PresetInfo* p = detailPreset();
    if (!p)
        return;
    float y = r.y + 10;
    setFont(cr, 14, true);
    drawText(cr, p->name, {r.x + 12, y, r.w - 48, 22}, Align::Left, theme::text);
    starRect_ = {r.right() - 34, y, 24, 22};
    const bool fav = PresetManager::get().isFavorite(p->relPath);
    setFont(cr, 16);
    drawText(cr, fav ? "★" : "☆", starRect_, Align::Center, fav ? theme::goldBright : theme::textDim);
    y += 26;
    setFont(cr, 10.5f);
    drawText(cr, displayFolder(p->folder), {r.x + 12, y, r.w - 24, 14}, Align::Left, theme::textFaint);
    y += 22;
    setFont(cr, 11);
    drawText(cr, "Author", {r.x + 12, y, 60, 16}, Align::Left, theme::textFaint);
    authorRect_ = {r.x + 64, y, r.w - 76, 16};
    drawText(cr, p->author.empty() ? "-" : p->author, authorRect_, Align::Left, theme::text);
    y += 24;
    drawText(cr, "Tags", {r.x + 12, y, 60, 16}, Align::Left, theme::textFaint);
    float tx = r.x + 64, ty = y;
    setFont(cr, 10.5f);
    for (size_t i = 0; i < p->tags.size(); ++i)
    {
        const float tw = textWidth(cr, p->tags[i]) + 22;
        if (tx + tw > r.right() - 10)
        {
            tx = r.x + 64;
            ty += 22;
        }
        const Rect chip{tx, ty - 1, tw, 18};
        fillRounded(cr, chip, 9, theme::decay.withAlpha(0.18f));
        drawText(cr, p->tags[i], {chip.x + 8, chip.y, chip.w - 18, chip.h}, Align::Left, theme::text);
        const Rect x{chip.right() - 14, chip.y + 2, 12, 14};
        icons::close(cr, x, theme::textFaint);
        tagRemoveRects_.push_back({x, static_cast<int>(i)});
        tx += tw + 4;
    }
    if (tx + 20 > r.right() - 10)
    {
        tx = r.x + 64;
        ty += 22;
    }
    addTagRect_ = {tx, ty - 1, 20, 18};
    fillRounded(cr, addTagRect_, 9, theme::panelLight);
    drawText(cr, "+", addTagRect_, Align::Center, theme::textDim);
    y = ty + 30;
    setFont(cr, 11);
    drawText(cr, "Description", {r.x + 12, y, r.w - 24, 16}, Align::Left, theme::textFaint);
    y += 18;
    descRect_ = {r.x + 12, y, r.w - 24, r.bottom() - y - 30};
    const std::vector<std::string> lines = wrap(cr, p->description.empty() ? "-" : p->description, descRect_.w);
    for (size_t i = 0; i < lines.size() && y + 16 < descRect_.bottom(); ++i, y += 16)
        drawText(cr, lines[i], {r.x + 12, y, r.w - 24, 16}, Align::Left, theme::text);
    setFont(cr, 9.5f);
    drawText(cr, "Double-click author or description to edit", {r.x + 12, r.bottom() - 22, r.w - 24, 14}, Align::Left,
             theme::textFaint);
}

bool PresetBrowser::mouseDown(const MouseEvent& e)
{
    if (!frame_.contains(e.x, e.y))
    {
        close();
        return true;
    }
    if (closeRect_.contains(e.x, e.y))
    {
        close();
        return true;
    }
    if (optionsRect_.contains(e.x, e.y))
    {
        showOptions(optionsRect_.x, optionsRect_.bottom() + 2);
        return true;
    }
    if (favRect_.contains(e.x, e.y))
    {
        favoritesOnly_ = !favoritesOnly_;
        refresh();
        return true;
    }
    if (searchRect_.contains(e.x, e.y))
    {
        if (RootWidget* r = root())
        {
            r->setKeyFocus(this);
            if (r->grabKeyboard)
                r->grabKeyboard();
        }
        return true;
    }
    if (folderRect_.contains(e.x, e.y))
    {
        const int f = folderAt(e.y);
        if (f >= 0)
        {
            folder_ = f;
            search_.clear();
            scroll_ = 0;
            cursor_ = -1;
            refresh();
        }
        return true;
    }
    if (listRect_.contains(e.x, e.y))
    {
        const int r = rowAt(e.y);
        if (r >= 0)
            loadRow(r, e.clicks == 2);
        return true;
    }
    if (detailRect_.contains(e.x, e.y))
    {
        const PresetInfo* p = detailPreset();
        if (!p)
            return true;
        if (starRect_.contains(e.x, e.y))
            PresetManager::get().setFavorite(p->relPath, !PresetManager::get().isFavorite(p->relPath));
        else if (addTagRect_.contains(e.x, e.y))
            editMeta(2);
        else if (e.clicks == 2 && authorRect_.contains(e.x, e.y))
            editMeta(0);
        else if (e.clicks == 2 && descRect_.contains(e.x, e.y))
            editMeta(1);
        else
            for (const auto& [rect, idx] : tagRemoveRects_)
                if (rect.contains(e.x, e.y))
                {
                    std::vector<std::string> tags = p->tags;
                    tags.erase(tags.begin() + idx);
                    PresetManager::get().updateMeta(p->path, p->author, p->description, tags);
                    refresh();
                    break;
                }
        repaint();
    }
    return true;
}

void PresetBrowser::mouseMove(const MouseEvent& e)
{
    const int hr = listRect_.contains(e.x, e.y) ? rowAt(e.y) : -1;
    const int hf = folderRect_.contains(e.x, e.y) ? folderAt(e.y) : -1;
    if (hr != hoverRow_ || hf != hoverFolder_)
    {
        hoverRow_ = hr;
        hoverFolder_ = hf;
        repaint();
    }
}

void PresetBrowser::mouseLeave()
{
    hoverRow_ = -1;
    hoverFolder_ = -1;
    // After choosing a preset, leaving the browser closes it.
    if (loadedFromHere_)
        close();
    repaint();
}

bool PresetBrowser::mouseWheel(const MouseEvent& e)
{
    const int visible = static_cast<int>((listRect_.h - 8) / kRowH);
    scroll_ = std::clamp(scroll_ - static_cast<int>(e.wheel) * 3, 0, std::max(0, static_cast<int>(rows_.size()) - visible));
    repaint();
    return true;
}

bool PresetBrowser::keyDown(const KeyEvent& e)
{
    const int visible = static_cast<int>((listRect_.h - 8) / kRowH);
    auto ensureVisible = [&] {
        if (cursor_ < scroll_)
            scroll_ = cursor_;
        if (cursor_ >= scroll_ + visible)
            scroll_ = cursor_ - visible + 1;
    };
    switch (e.keysym)
    {
    case XK_Escape: close(); return true;
    case XK_Down:
        cursor_ = std::min(cursor_ + 1, static_cast<int>(rows_.size()) - 1);
        ensureVisible();
        repaint();
        return true;
    case XK_Up:
        cursor_ = std::max(cursor_ - 1, 0);
        ensureVisible();
        repaint();
        return true;
    case XK_Return:
    case XK_KP_Enter: loadRow(cursor_, true); return true;
    case XK_Right: loadRow(cursor_, false); return true;
    case XK_BackSpace:
        if (!search_.empty())
        {
            search_.pop_back();
            refresh();
        }
        return true;
    case XK_bracketleft:
        if (search_.empty())
        {
            session_.step(-1);
            return true;
        }
        break;
    case XK_bracketright:
        if (search_.empty())
        {
            session_.step(1);
            return true;
        }
        break;
    default: break;
    }
    const bool typeToSearch = Settings::get().getDouble("type_to_search", 1) > 0.5;
    if (!e.text.empty() && static_cast<unsigned char>(e.text[0]) >= 0x20 &&
        (typeToSearch || (root() && root()->keyFocus() == this)))
    {
        search_ += e.text;
        cursor_ = 0;
        scroll_ = 0;
        refresh();
        return true;
    }
    return true;
}

void PresetBrowser::editMeta(int field)
{
    const PresetInfo* p = detailPreset();
    if (!p)
        return;
    const std::string path = p->path;
    const PresetInfo info = *p;
    const Rect anchor = field == 0 ? authorRect_ : (field == 1 ? descRect_ : addTagRect_);
    const std::string initial = field == 0 ? info.author : (field == 1 ? info.description : std::string());
    auto ed = std::make_unique<TextEditor>(initial, [this, path, info, field](const std::string& t) {
        std::string author = info.author, desc = info.description;
        std::vector<std::string> tags = info.tags;
        if (field == 0)
            author = t;
        else if (field == 1)
            desc = t;
        else if (!t.empty())
            tags.push_back(t);
        PresetManager::get().updateMeta(path, author, desc, tags);
        refresh();
    });
    const float w = std::max(200.0f, anchor.w);
    ed->setBounds({std::min(anchor.x, bounds_.right() - w - 10), anchor.y - 3, w, 24});
    Widget* raw = ed.get();
    RootWidget* r = root();
    r->pushOverlay(std::move(ed));
    r->setKeyFocus(raw);
    if (r->grabKeyboard)
        r->grabKeyboard();
}

void PresetBrowser::promptSaveAs()
{
    const std::string def = "User/" + session_.name();
    auto ed = std::make_unique<TextEditor>(def, [this](const std::string& t) {
        if (t.empty())
            return;
        std::string rel = t;
        if (rel.size() > 6 && rel.substr(rel.size() - 6) == PresetManager::kExtension)
            rel = rel.substr(0, rel.size() - 6);
        session_.saveAs(PresetManager::get().root() + "/" + rel + PresetManager::kExtension);
        refresh();
    });
    ed->setBounds({searchRect_.x, searchRect_.y, searchRect_.w, 26});
    Widget* raw = ed.get();
    RootWidget* r = root();
    r->pushOverlay(std::move(ed));
    r->setKeyFocus(raw);
    if (r->grabKeyboard)
        r->grabKeyboard();
}

void PresetBrowser::showOptions(float x, float y)
{
    Settings& s = Settings::get();
    const bool tts = s.getDouble("type_to_search", 1) > 0.5;
    Editor* ed = &editor_;
    std::vector<MenuItem> items;
    items.push_back({"Type To Search", [tts] {
                         Settings::get().set("type_to_search", tts ? 0.0 : 1.0);
                         Settings::get().save();
                     }, tts});
    items.push_back({"", nullptr, false, true, true});
    items.push_back({"Save As...", [this] { promptSaveAs(); }});
    items.push_back({"Save", [this] { session_.saveCurrent(); }, false, !session_.path().empty()});
    items.push_back({"Save As Default", [this] { session_.saveAsDefault(); }});
    items.push_back({"", nullptr, false, true, true});
    items.push_back({"Open Other Preset...", [ed, this] {
                         PresetSession* s = &session_;
                         ed->runFileDialog(FileDialog::Mode::OpenFile, "Open preset",
                                           "Presets | *.aurum *.ffp", [s](const std::string& p) { s->load(p); });
                     }});
    items.push_back({"Import IR...", [ed] {
                         ed->runFileDialog(FileDialog::Mode::OpenFile, "Import impulse response",
                                           "Impulse responses | *.wav *.WAV *.aif *.aiff *.AIF *.AIFF",
                                           [ed](const std::string& p) {
                                               if (ed->onImportIr)
                                                   ed->onImportIr(p);
                                           });
                     }});
    items.push_back({"Import .ffp Preset Folder...", [ed, this] {
                         PresetSession* s = &session_;
                         ed->runFileDialog(FileDialog::Mode::OpenFolder, "Folder with .ffp presets", {},
                                           [s](const std::string& p) {
                                               std::string last;
                                               if (PresetManager::get().importFfp(p, &last) > 0 && !last.empty())
                                                   s->load(last);
                                           });
                     }});
    items.push_back({"", nullptr, false, true, true});
    items.push_back({"Change Preset Folder...", [ed] {
                         ed->runFileDialog(FileDialog::Mode::OpenFolder, "Preset folder", {},
                                           [](const std::string& p) { PresetManager::get().setRoot(p); });
                     }});
    items.push_back({"Restore Factory Presets", [this] {
                         PresetManager::get().restoreFactory();
                         refresh();
                     }});
    items.push_back({"Refresh", [this] {
                         PresetManager::get().rescan();
                         refresh();
                     }});
    // Keep the browser open underneath the menu.
    RootWidget* r = root();
    r->pushOverlay(std::make_unique<PopupMenu>(std::move(items), x, y, r->bounds().w, r->bounds().h));
}

} // namespace aurum::gui
