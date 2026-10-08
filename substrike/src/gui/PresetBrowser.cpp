#include "PresetBrowser.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "Editor.h"
#include "util/Path.h"

namespace substrike::gui {

namespace {

// Lines of `text` that fit `width` at the current font.
std::vector<std::string> wrap(cairo_t* cr, const std::string& text, float width)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&] {
        if (word.empty())
            return;
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && textWidth(cr, tryLine) > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
            line = tryLine;
        word.clear();
    };
    for (char c : text)
    {
        if (c == ' ')
            flushWord();
        else
            word += c;
    }
    flushWord();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

} // namespace

// ------------------------------------------------------------ PresetBar

void PresetBar::paint(cairo_t* cr)
{
    fillRounded(cr, bounds_, 5, hover_ == 2 ? theme::panelLight.mix(Color(1, 1, 1, 1), 0.04f) : theme::panelLight);
    strokeRounded(cr, bounds_, 5, ed_.presetBrowserOpen() ? theme::accent.withAlpha(0.8f) : theme::outline);
    auto arrow = [&](const Rect& r, bool leftward, bool hot) {
        const Rect a = r.reduced(9, 9);
        (leftward ? icons::arrowLeft : icons::arrowRight)(cr, a, hot ? theme::accentBright : theme::textDim);
    };
    arrow(left(), true, hover_ == -1);
    arrow(right(), false, hover_ == 1);
    setFont(cr, 12.0f, true);
    drawText(cr, ed_.controller().presetName(), {bounds_.x + 30, bounds_.y, bounds_.w - 60, bounds_.h}, Align::Center,
             theme::text);
}

bool PresetBar::mouseDown(const MouseEvent& e)
{
    if (e.button != 1)
        return false;
    if (left().contains(e.x, e.y))
        ed_.stepPreset(-1);
    else if (right().contains(e.x, e.y))
        ed_.stepPreset(1);
    else
        ed_.togglePresetBrowser();
    return true;
}

void PresetBar::mouseMove(const MouseEvent& e)
{
    const int h = left().contains(e.x, e.y) ? -1 : right().contains(e.x, e.y) ? 1 : 2;
    if (h != hover_)
    {
        hover_ = h;
        repaint();
    }
}

void PresetBar::mouseLeave()
{
    hover_ = 0;
    repaint();
}

std::string PresetBar::tooltip() const
{
    if (hover_ == -1)
        return "Previous preset";
    if (hover_ == 1)
        return "Next preset";
    return "Browse the presets";
}

// ------------------------------------------------------------ PresetBrowser

void PresetBrowser::refresh()
{
    all_ = factoryPresets();
    const std::vector<PresetInfo> user = userPresets();
    for (PresetInfo p : user)
    {
        p.category = "User";
        all_.push_back(std::move(p));
    }
    categories_ = {"All"};
    for (const std::string& c : factoryCategories())
        for (const PresetInfo& p : all_)
            if (p.category == c)
            {
                categories_.push_back(c);
                break;
            }
    if (!user.empty())
        categories_.push_back("User");
    // Open on the loaded preset's category.
    const std::string current = ed_.controller().presetName();
    for (const PresetInfo& p : all_)
        if (p.name == current)
        {
            const auto it = std::find(categories_.begin(), categories_.end(), p.category);
            if (it != categories_.end())
                category_ = static_cast<int>(it - categories_.begin());
            break;
        }
    category_ = std::clamp(category_, 0, static_cast<int>(categories_.size()) - 1);
    scroll_ = 0;
    repaint();
}

std::vector<int> PresetBrowser::shown() const
{
    std::vector<int> out;
    const std::string& c = categories_.empty() ? std::string() : categories_[static_cast<size_t>(category_)];
    for (size_t i = 0; i < all_.size(); ++i)
        if (c == "All" || all_[i].category == c)
            out.push_back(static_cast<int>(i));
    return out;
}

Rect PresetBrowser::categoryArea() const { return {bounds_.x + 12, bounds_.y + 40, 150, bounds_.h - 52}; }
Rect PresetBrowser::listArea() const { return {bounds_.x + 172, bounds_.y + 40, 300, bounds_.h - 52}; }
Rect PresetBrowser::infoArea() const
{
    return {bounds_.x + 486, bounds_.y + 40, bounds_.w - 498, bounds_.h - 52};
}
Rect PresetBrowser::button(int i) const
{
    return {bounds_.right() - 12 - (i == 1 ? 70.0f : 170.0f), bounds_.y + 8, i == 1 ? 70.0f : 94.0f, 24};
}

int PresetBrowser::rowAt(float y) const
{
    const Rect l = listArea();
    if (y < l.y)
        return -1;
    const int row = static_cast<int>((y - l.y) / kRowH) + scroll_;
    const std::vector<int> s = shown();
    return row >= 0 && row < static_cast<int>(s.size()) ? s[static_cast<size_t>(row)] : -1;
}

void PresetBrowser::paint(cairo_t* cr)
{
    fillRounded(cr, bounds_, 8, theme::panel);
    strokeRounded(cr, bounds_, 8, theme::accent.withAlpha(0.5f));
    setFont(cr, 11, true);
    drawText(cr, "PRESETS", {bounds_.x + 12, bounds_.y + 10, 200, 18}, Align::Left, theme::accent);
    setFont(cr, 10.5f);
    drawText(cr, "click to load and play  \xc2\xb7  up and down step through  \xc2\xb7  Esc closes",
             {bounds_.x + 90, bounds_.y + 10, 500, 18}, Align::Left, theme::textFaint);
    for (int i = 0; i < 2; ++i)
    {
        const Rect b = button(i);
        fillRounded(cr, b, 5, hoverButton_ == i ? theme::accent.withAlpha(0.25f) : theme::panelLight);
        strokeRounded(cr, b, 5, theme::outline);
        setFont(cr, 11);
        drawText(cr, i == 0 ? "Save As..." : "Close", b, Align::Center, theme::text);
    }

    // Categories.
    const Rect c = categoryArea();
    for (size_t i = 0; i < categories_.size(); ++i)
    {
        const Rect r{c.x, c.y + i * kRowH, c.w, kRowH - 2};
        if (r.bottom() > c.bottom())
            break;
        const bool on = static_cast<int>(i) == category_;
        if (on || hoverCategory_ == static_cast<int>(i))
            fillRounded(cr, r, 4, on ? theme::accent.withAlpha(0.2f) : theme::panelLight);
        setFont(cr, 11, on);
        drawText(cr, categories_[i], r.reduced(8, 0), Align::Left, on ? theme::text : theme::textDim);
    }

    // Presets of the category.
    const Rect l = listArea();
    fillRounded(cr, l, 5, theme::plot);
    const std::vector<int> s = shown();
    const std::string current = ed_.controller().presetName();
    const int rows = static_cast<int>(l.h / kRowH);
    for (int row = 0; row < rows; ++row)
    {
        const int k = row + scroll_;
        if (k >= static_cast<int>(s.size()))
            break;
        const PresetInfo& p = all_[static_cast<size_t>(s[static_cast<size_t>(k)])];
        const Rect r{l.x + 4, l.y + row * kRowH + 1, l.w - 8, kRowH - 2};
        const bool on = p.name == current, hot = hover_ == s[static_cast<size_t>(k)];
        if (on || hot)
            fillRounded(cr, r, 4, on ? theme::accent.withAlpha(0.25f) : theme::panelLight);
        setFont(cr, 11, on);
        drawText(cr, p.name, r.reduced(8, 0), Align::Left, on ? theme::text : theme::textDim);
        if (category_ == 0)
        {
            setFont(cr, 9.5f);
            drawText(cr, p.category, r.reduced(8, 0), Align::Right, theme::textFaint);
        }
    }
    if (static_cast<int>(s.size()) > rows)
    {
        // A scroll bar.
        const float h = l.h * rows / static_cast<float>(s.size());
        const float y = l.y + (l.h - h) * scroll_ / static_cast<float>(std::max(1, static_cast<int>(s.size()) - rows));
        fillRounded(cr, {l.right() - 4, y, 3, h}, 1.5f, theme::outline);
    }

    // What the hovered preset is, or the loaded one.
    const PresetInfo* info = nullptr;
    if (hover_ >= 0)
        info = &all_[static_cast<size_t>(hover_)];
    else
        for (const PresetInfo& p : all_)
            if (p.name == current)
                info = &p;
    const Rect in = infoArea();
    if (!info)
    {
        setFont(cr, 11);
        drawText(cr, current + " (not a saved preset)", {in.x, in.y, in.w, 20}, Align::Left, theme::textDim);
        return;
    }
    setFont(cr, 18, true);
    drawText(cr, info->name, {in.x, in.y, in.w, 26}, Align::Left, theme::text);
    setFont(cr, 11);
    drawText(cr, info->category + (info->author.empty() ? "" : "  \xc2\xb7  " + info->author), {in.x, in.y + 28, in.w, 18},
             Align::Left, theme::accent);
    float y = in.y + 56;
    setFont(cr, 12);
    for (const std::string& line : wrap(cr, info->description, in.w))
    {
        drawText(cr, line, {in.x, y, in.w, 18}, Align::Left, theme::textDim);
        y += 19;
    }
    // The tags as chips.
    y += 10;
    float x = in.x;
    setFont(cr, 10);
    for (const std::string& t : info->tags)
    {
        const float w = textWidth(cr, t) + 16;
        if (x + w > in.right())
        {
            x = in.x;
            y += 24;
        }
        fillRounded(cr, {x, y, w, 19}, 9.5f, theme::panelLight);
        drawText(cr, t, {x, y, w, 19}, Align::Center, theme::amp);
        x += w + 6;
    }
}

bool PresetBrowser::mouseDown(const MouseEvent& e)
{
    if (button(0).contains(e.x, e.y) && e.button == 1)
    {
        ed_.savePresetAs();
        return true;
    }
    if (button(1).contains(e.x, e.y) && e.button == 1)
    {
        ed_.togglePresetBrowser();
        return true;
    }
    const Rect c = categoryArea();
    if (c.contains(e.x, e.y))
    {
        const int i = static_cast<int>((e.y - c.y) / kRowH);
        if (i >= 0 && i < static_cast<int>(categories_.size()))
        {
            category_ = i;
            scroll_ = 0;
            repaint();
        }
        return true;
    }
    if (listArea().contains(e.x, e.y))
    {
        const int k = rowAt(e.y);
        if (k < 0)
            return true;
        const PresetInfo& p = all_[static_cast<size_t>(k)];
        if (e.button == 3 && !p.factory)
        {
            const std::string path = p.key, name = p.name;
            showMenu(root(), {{"Delete " + name, [this, path] {
                                   std::error_code ec;
                                   std::filesystem::remove(toPath(path), ec);
                                   refresh();
                               }}},
                     e.x, e.y);
            return true;
        }
        if (e.button == 1)
            ed_.loadPreset(p);
        return true;
    }
    return true; // the panel swallows clicks; nothing under it should get them
}

void PresetBrowser::step(int dir)
{
    const std::vector<int> s = shown();
    if (s.empty())
        return;
    const std::string current = ed_.controller().presetName();
    int at = -1;
    for (size_t k = 0; k < s.size(); ++k)
        if (all_[static_cast<size_t>(s[k])].name == current)
            at = static_cast<int>(k);
    at = at < 0 ? 0 : (at + dir + static_cast<int>(s.size())) % static_cast<int>(s.size());
    ed_.loadPreset(all_[static_cast<size_t>(s[static_cast<size_t>(at)])]);
    // Keep it in view.
    const int rows = static_cast<int>(listArea().h / kRowH);
    if (at < scroll_)
        scroll_ = at;
    else if (at >= scroll_ + rows)
        scroll_ = at - rows + 1;
    repaint();
}

void PresetBrowser::mouseMove(const MouseEvent& e)
{
    int h = listArea().contains(e.x, e.y) ? rowAt(e.y) : -1;
    const Rect c = categoryArea();
    int hc = -1;
    if (c.contains(e.x, e.y))
    {
        hc = static_cast<int>((e.y - c.y) / kRowH);
        if (hc >= static_cast<int>(categories_.size()))
            hc = -1;
    }
    int hb = button(0).contains(e.x, e.y) ? 0 : button(1).contains(e.x, e.y) ? 1 : -1;
    if (h != hover_ || hc != hoverCategory_ || hb != hoverButton_)
    {
        hover_ = h;
        hoverCategory_ = hc;
        hoverButton_ = hb;
        repaint();
    }
}

void PresetBrowser::mouseLeave()
{
    hover_ = hoverCategory_ = hoverButton_ = -1;
    repaint();
}

bool PresetBrowser::mouseWheel(const MouseEvent& e)
{
    const int rows = static_cast<int>(listArea().h / kRowH);
    const int most = std::max(0, static_cast<int>(shown().size()) - rows);
    scroll_ = std::clamp(scroll_ - static_cast<int>(e.wheel * 3), 0, most);
    repaint();
    return true;
}

} // namespace substrike::gui
