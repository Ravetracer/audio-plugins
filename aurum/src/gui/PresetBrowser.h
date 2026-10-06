#pragma once

#include <string>
#include <vector>

#include "Widgets.h"
#include "plugin/PresetSession.h"

namespace aurum::gui {

class Editor;

class PresetBrowser : public Widget
{
public:
    PresetBrowser(Editor& editor, PresetSession& session);

    void layout() override;
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    void mouseLeave() override;
    bool keyDown(const KeyEvent& e) override;

private:
    struct Row
    {
        int presetIndex;
    };
    void refresh();
    void close();
    void loadRow(int row, bool closeAfter);
    int rowAt(float y) const;
    int folderAt(float y) const;
    const PresetInfo* detailPreset() const;
    void showOptions(float x, float y);
    void promptSaveAs();
    void editMeta(int field); // 0 author, 1 description, 2 add tag
    void paintDetails(cairo_t* cr, const Rect& r);

    Editor& editor_;
    PresetSession& session_;
    std::string search_;
    bool favoritesOnly_ = false;
    int folder_ = 0; // 0 = all, else folders_[folder_-1]
    std::vector<std::string> folders_;
    std::vector<int> rows_; // preset indices
    int cursor_ = -1;
    int hoverRow_ = -1;
    int hoverFolder_ = -1;
    int scroll_ = 0;
    bool loadedFromHere_ = false;
    std::vector<std::pair<Rect, int>> tagRemoveRects_; // detail tags
    Rect frame_, header_, folderRect_, listRect_, detailRect_, optionsRect_, searchRect_, favRect_, closeRect_;
    Rect starRect_, authorRect_, descRect_, addTagRect_;
};

} // namespace aurum::gui
