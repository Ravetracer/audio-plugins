#pragma once

#include <array>
#include <vector>

#include "Widgets.h"
#include "dsp/DecayModel.h"

namespace aurum::gui {

// Band editor for one of the two six-band EQs: the Decay Contour (reverb time
// per frequency, shown as third-octave bars in seconds) or the Tone EQ (output
// filter curve in dB). Bands are picked with numbered chips and edited in an
// inspector row; handles in the graph can also be dragged.
class EqPanel : public Widget
{
public:
    EqPanel(ParamContext& ctx, bool tone);

    // Updates the inspector to the current parameter state (band list,
    // selection, controls that depend on the band shape).
    void sync();

    void layout() override;
    void paint(cairo_t* cr) override;
    bool mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    void mouseLeave() override;
    bool keyDown(const KeyEvent& e) override;

private:
    static constexpr int kBands = 6;
    enum Field { FUsed, FEnabled, FShape, FFreq, FLevel, FQ, FSlope, FPlacement };

    // Parameter access
    int pidx(int band, int field) const;
    bool used(int b) const;
    bool enabled(int b) const;
    int shape(int b) const;
    double freq(int b) const;
    double level(int b) const; // rate log2 (decay) or gain dB (tone)
    double q(int b) const;
    bool hasLevel(int b) const;
    int placement(int b) const;

    // Geometry
    Rect graph() const;
    Rect chipRow() const;
    Rect inspectorRow() const;
    Rect chipRect(int slot) const;
    float xForFreq(double f) const;
    double freqForX(float x) const;
    float yFor(double v) const; // decay: log2 seconds, tone: dB
    double valueForY(float y) const;
    float handleX(int b) const { return xForFreq(freq(b)); }
    float handleY(int b) const;

    int handleAt(float x, float y) const;
    int chipAt(float x, float y) const;
    int addBand(int shapeIdx, double f, double levelValue);
    void deleteBand(int b);
    void select(int b);
    void showAddMenu();
    void showBandMenu(int b, float x, float y);
    bool hoverInside();

    void updateCache();
    dsp::DecayModel decayModel() const;
    double toneCurveAt(int b, double f) const;
    void paintGrid(cairo_t* cr, const Rect& g);
    void paintDecay(cairo_t* cr, const Rect& g);
    void paintTone(cairo_t* cr, const Rect& g);
    void paintHandles(cairo_t* cr);
    void paintChips(cairo_t* cr);

    ParamContext& ctx_;
    const bool tone_;
    const Color accent_;
    int selected_ = -1;
    int hover_ = -1;

    bool dragging_ = false;
    float startX_ = 0, startY_ = 0;
    double startFreq_ = 0, startLevel_ = 0;

    struct Inspector
    {
        ParamToggle* on = nullptr;
        ParamSelector* shape = nullptr;
        ParamField* freq = nullptr;
        ParamField* level = nullptr;
        ParamField* q = nullptr;
        ParamSelector* slope = nullptr;
        ParamSelector* place = nullptr;
        Button* remove = nullptr;
    };
    std::array<Inspector, kBands> insp_;
    Button* add_ = nullptr;

    // Cached curves, rebuilt when parameters or the size change.
    std::vector<double> cacheKey_;
    std::vector<double> barSeconds_;   // decay: one value per third-octave bar
    double nominal_ = 1.0;             // decay: Room x Length in seconds
    dsp::DecayModel model_;
    std::vector<double> toneCurve_;    // tone: per plot column (stereo bands)
    std::array<std::vector<double>, 5> placeCurves_; // tone: L, R, M, S curves
};

} // namespace aurum::gui
