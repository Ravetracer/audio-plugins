#include "EqPanel.h"

#include <X11/keysym.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "dsp/PostEq.h"
#include "dsp/ReverbEngine.h"

namespace aurum::gui {

namespace {

constexpr double kMinFreq = 20.0;
constexpr double kMaxFreq = 20000.0;
// Decay Contour scale: reverb time in seconds, logarithmic.
constexpr double kSecMin = 0.05, kSecMax = 50.0;
// Tone EQ scale.
constexpr double kToneRange = 18.0;
constexpr float kPad = 12.0f;
constexpr float kScaleW = 30.0f;
constexpr float kHeaderH = 28.0f;
constexpr float kRowH = 22.0f;
constexpr int kNumBars = 31; // third octaves, 20 Hz .. 20 kHz

double barFreq(int k) { return 1000.0 * std::exp2((k - 17) / 3.0); }

} // namespace

EqPanel::EqPanel(ParamContext& ctx, bool tone) : ctx_(ctx), tone_(tone), accent_(tone ? theme::postEq : theme::decay)
{
    for (int b = 0; b < kBands; ++b)
    {
        Inspector& in = insp_[static_cast<size_t>(b)];
        in.on = add<ParamToggle>(ctx_, pidx(b, FEnabled), "On");
        in.on->fontSize = 10.5f;
        in.shape = add<ParamSelector>(ctx_, pidx(b, FShape));
        in.shape->fontSize = 10.5f;
        in.freq = add<ParamField>(ctx_, pidx(b, FFreq));
        in.level = add<ParamField>(ctx_, pidx(b, FLevel));
        in.q = add<ParamField>(ctx_, pidx(b, FQ), "Q ");
        in.freq->accent = in.level->accent = in.q->accent = accent_;
        if (tone_)
        {
            in.slope = add<ParamSelector>(ctx_, pidx(b, FSlope));
            in.slope->fontSize = 10.5f;
            in.place = add<ParamSelector>(ctx_, pidx(b, FPlacement));
            in.place->fontSize = 10.5f;
        }
        in.remove = add<Button>("", [this, b] { deleteBand(b); });
        in.remove->icon = icons::close;
        in.remove->setTooltip("Delete band");
    }
    add_ = add<Button>("+ Add", [this] { showAddMenu(); });
    add_->fontSize = 10.5f;
    add_->setTooltip(tone_ ? "Add a Tone EQ band" : "Add a Decay Contour band");
}

// ------------------------------------------------------------ parameters

int EqPanel::pidx(int b, int field) const
{
    using namespace pid;
    if (tone_)
    {
        static const PostField map[] = {PUsed, PEnabled, PShape, PFreq, PGain, PQ, PSlope, PPlacement};
        return ctx_.index(post(b, map[field]));
    }
    static const DecayField map[] = {DUsed, DEnabled, DShape, DFreq, DRate, DQ, DQ, DQ};
    return ctx_.index(decay(b, map[field]));
}

bool EqPanel::used(int b) const { return ctx_.value(pidx(b, FUsed)) > 0.5; }
bool EqPanel::enabled(int b) const { return ctx_.value(pidx(b, FEnabled)) > 0.5; }
int EqPanel::shape(int b) const { return static_cast<int>(std::lround(ctx_.value(pidx(b, FShape)))); }
double EqPanel::freq(int b) const { return conv::freqHz(ctx_.value(pidx(b, FFreq))); }
double EqPanel::q(int b) const { return conv::q(ctx_.value(pidx(b, FQ))); }

double EqPanel::level(int b) const
{
    const double v = ctx_.value(pidx(b, FLevel));
    return tone_ ? conv::gainDb(v) : conv::rateLog2(v);
}

bool EqPanel::hasLevel(int b) const
{
    if (!tone_)
        return true;
    const int s = shape(b);
    return s != static_cast<int>(dsp::PostShape::LowCut) && s != static_cast<int>(dsp::PostShape::HighCut);
}

int EqPanel::placement(int b) const
{
    return tone_ ? static_cast<int>(std::lround(ctx_.value(pidx(b, FPlacement)))) : 0;
}

// ------------------------------------------------------------ geometry

Rect EqPanel::graph() const
{
    const float x = bounds_.x + kPad + kScaleW;
    const float y = bounds_.y + kHeaderH;
    const float bottomArea = 14.0f + 8.0f + kRowH + 6.0f + kRowH + kPad;
    return {x, y, bounds_.right() - kPad - x, std::max(40.0f, bounds_.bottom() - bottomArea - y)};
}

Rect EqPanel::chipRow() const
{
    const Rect g = graph();
    return {bounds_.x + kPad, g.bottom() + 14.0f + 8.0f, bounds_.w - 2 * kPad, kRowH};
}

Rect EqPanel::inspectorRow() const
{
    const Rect c = chipRow();
    return {c.x, c.bottom() + 6.0f, c.w, kRowH};
}

Rect EqPanel::chipRect(int slot) const
{
    const Rect c = chipRow();
    return {c.x + 44.0f + slot * 28.0f, c.y, 24.0f, c.h};
}

float EqPanel::xForFreq(double f) const
{
    const Rect g = graph();
    return g.x + static_cast<float>(std::log(f / kMinFreq) / std::log(kMaxFreq / kMinFreq)) * g.w;
}

double EqPanel::freqForX(float x) const
{
    const Rect g = graph();
    return kMinFreq * std::pow(kMaxFreq / kMinFreq, (x - g.x) / g.w);
}

float EqPanel::yFor(double v) const
{
    const Rect g = graph();
    if (tone_)
        return g.cy() - static_cast<float>(v / kToneRange) * g.h * 0.5f;
    const double l0 = std::log2(kSecMin), l1 = std::log2(kSecMax);
    return g.bottom() - static_cast<float>((v - l0) / (l1 - l0)) * g.h;
}

double EqPanel::valueForY(float y) const
{
    const Rect g = graph();
    if (tone_)
        return (g.cy() - y) / (g.h * 0.5f) * kToneRange;
    const double l0 = std::log2(kSecMin), l1 = std::log2(kSecMax);
    return l0 + (g.bottom() - y) / g.h * (l1 - l0);
}

float EqPanel::handleY(int b) const
{
    const Rect g = graph();
    double v;
    if (!tone_)
        v = std::log2(std::max(model_.t60At(freq(b)), 1e-3));
    else if (!hasLevel(b))
        v = 0.0;
    else
        v = level(b) * ((shape(b) == 1 || shape(b) == 2) ? 0.5 : 1.0); // shelves: half gain at the corner
    return std::clamp(yFor(v), g.y + 4, g.bottom() - 4);
}

int EqPanel::handleAt(float x, float y) const
{
    int best = -1;
    float bestD = 10.0f;
    for (int b = 0; b < kBands; ++b)
    {
        if (!used(b))
            continue;
        const float d = std::hypot(handleX(b) - x, handleY(b) - y);
        if (d < bestD)
        {
            bestD = d;
            best = b;
        }
    }
    return best;
}

int EqPanel::chipAt(float x, float y) const
{
    int slot = 0;
    for (int b = 0; b < kBands; ++b)
    {
        if (!used(b))
            continue;
        if (chipRect(slot).contains(x, y))
            return b;
        ++slot;
    }
    return -1;
}

// ------------------------------------------------------------ editing

int EqPanel::addBand(int shapeIdx, double f, double levelValue)
{
    for (int b = 0; b < kBands; ++b)
    {
        if (used(b))
            continue;
        const bool shelf = shapeIdx == 1 || shapeIdx == 2;
        std::vector<std::pair<int, double>> changes = {
            {pidx(b, FEnabled), 1.0},
            {pidx(b, FShape), static_cast<double>(shapeIdx)},
            {pidx(b, FFreq), conv::freqToValue(f)},
            {pidx(b, FLevel), tone_ ? conv::gainDbToValue(levelValue) : conv::rateLog2ToValue(levelValue)},
            {pidx(b, FQ), conv::qToValue(shelf ? 0.7071 : 1.0)}, // cuts: Q 1 = Butterworth
            {pidx(b, FUsed), 1.0},
        };
        if (tone_)
        {
            changes.push_back({pidx(b, FSlope), 3.0});
            changes.push_back({pidx(b, FPlacement), 0.0});
        }
        for (auto& c : changes)
            ctx_.begin(c.first);
        for (auto& c : changes)
            ctx_.set(c.first, c.second);
        for (auto& c : changes)
            ctx_.end(c.first);
        select(b);
        return b;
    }
    return -1;
}

void EqPanel::deleteBand(int b)
{
    ctx_.change(pidx(b, FUsed), 0.0);
    if (selected_ == b)
        selected_ = -1;
    if (hover_ == b)
        hover_ = -1;
    sync();
    repaint();
}

void EqPanel::select(int b)
{
    selected_ = b;
    sync();
    repaint();
}

void EqPanel::showAddMenu()
{
    const ParamDef& sd = ctx_.table().def(pidx(0, FShape));
    // Default frequency and level per shape.
    static const double decayFreq[] = {1000.0, 250.0, 4000.0, 1000.0};
    static const double decayLevel[] = {-1.0, -1.0, -1.0, -2.0};
    static const double toneFreq[] = {1000.0, 150.0, 6000.0, 80.0, 12000.0};
    std::vector<MenuItem> items;
    for (int i = 0; i < sd.steps; ++i)
        items.push_back({sd.labels[static_cast<size_t>(i)], [this, i] {
                             if (tone_)
                                 addBand(i, toneFreq[i], 0.0);
                             else
                                 addBand(i, decayFreq[i], decayLevel[i]);
                         }});
    showMenu(root(), std::move(items), add_->bounds().x, add_->bounds().bottom() + 2);
}

void EqPanel::showBandMenu(int b, float x, float y)
{
    std::vector<MenuItem> items;
    items.push_back({"Enabled", [this, b] { ctx_.change(pidx(b, FEnabled), enabled(b) ? 0.0 : 1.0); }, enabled(b)});
    items.push_back({"", nullptr, false, true, true});
    items.push_back({"Delete Band", [this, b] { deleteBand(b); }});
    showMenu(root(), std::move(items), x, y);
}

bool EqPanel::hoverInside()
{
    RootWidget* r = root();
    for (Widget* w = r ? r->hovered() : nullptr; w; w = w->parent())
        if (w == this)
            return true;
    return false;
}

// ------------------------------------------------------------ inspector

void EqPanel::layout() { sync(); }

void EqPanel::sync()
{
    if (selected_ >= 0 && !used(selected_))
        selected_ = -1;
    int count = 0;
    for (int b = 0; b < kBands; ++b)
        if (used(b))
        {
            if (selected_ < 0)
                selected_ = b;
            ++count;
        }

    const Rect row = inspectorRow();
    for (int b = 0; b < kBands; ++b)
    {
        Inspector& in = insp_[static_cast<size_t>(b)];
        const bool show = b == selected_;
        std::vector<std::pair<Widget*, float>> items;
        items.push_back({in.on, 38});
        items.push_back({in.shape, 84});
        items.push_back({in.freq, 76});
        const bool lvl = hasLevel(b);
        if (lvl)
            items.push_back({in.level, 66});
        if (in.slope)
            items.push_back({in.slope, 82});
        items.push_back({in.q, 60});
        if (in.place)
            items.push_back({in.place, 70});
        items.push_back({in.remove, 24});

        if (in.slope && lvl)
            items.erase(std::remove_if(items.begin(), items.end(), [&](auto& it) { return it.first == in.slope; }),
                        items.end());
        if (show)
        {
            constexpr float gap = 6.0f;
            float total = 0.0f;
            for (auto& it : items)
                total += it.second;
            const float avail = row.w - gap * static_cast<float>(items.size() - 1);
            const float k = std::min(1.0f, avail / total);
            float x = row.x;
            for (auto& [w, width] : items)
            {
                const float ww = std::floor(width * k);
                w->setBounds({x, row.y, ww, row.h});
                x += ww + gap;
            }
        }
        // Each widget's visibility is set once, so an unchanged state causes no repaint.
        for (Widget* w : {static_cast<Widget*>(in.on), static_cast<Widget*>(in.shape), static_cast<Widget*>(in.freq),
                          static_cast<Widget*>(in.level), static_cast<Widget*>(in.q), static_cast<Widget*>(in.slope),
                          static_cast<Widget*>(in.place), static_cast<Widget*>(in.remove)})
            if (w)
                w->setVisible(show && std::any_of(items.begin(), items.end(), [w](auto& it) { return it.first == w; }));
    }
    const Rect next = chipRect(count);
    add_->setBounds({next.x + (count ? 4.0f : 0.0f), next.y, 58, next.h});
    add_->setVisible(count < kBands);
}

// ------------------------------------------------------------ curves

dsp::DecayModel EqPanel::decayModel() const
{
    std::vector<double> values(static_cast<size_t>(ctx_.table().count()));
    for (int i = 0; i < ctx_.table().count(); ++i)
        values[static_cast<size_t>(i)] = ctx_.value(i);
    return dsp::ReverbEngine::decayModelFor(buildEngineParams(values.data(), ctx_.controller().tempo()));
}

double EqPanel::toneCurveAt(int b, double f) const
{
    dsp::PostBand p;
    p.used = true;
    p.enabled = true;
    p.shape = static_cast<dsp::PostShape>(shape(b));
    p.freq = freq(b);
    p.gainDb = level(b);
    p.q = q(b);
    p.slope = static_cast<int>(std::lround(ctx_.value(pidx(b, FSlope))));
    return 10.0 * std::log10(std::max(dsp::PostEq::bandPow(p, f, ctx_.controller().sampleRate()), 1e-12));
}

void EqPanel::updateCache()
{
    const Rect g = graph();
    std::vector<double> key(static_cast<size_t>(ctx_.table().count()) + 3);
    for (int i = 0; i < ctx_.table().count(); ++i)
        key[static_cast<size_t>(i)] = ctx_.value(i);
    key[key.size() - 3] = g.w;
    key[key.size() - 2] = g.h;
    key[key.size() - 1] = ctx_.controller().sampleRate();
    if (key == cacheKey_)
        return;
    cacheKey_ = std::move(key);

    if (!tone_)
    {
        model_ = decayModel();
        nominal_ = model_.nominalT60();
        barSeconds_.resize(kNumBars);
        for (int k = 0; k < kNumBars; ++k)
            barSeconds_[static_cast<size_t>(k)] = model_.t60At(barFreq(k));
        return;
    }
    const int cols = static_cast<int>(g.w) + 1;
    toneCurve_.assign(static_cast<size_t>(cols), 0.0);
    for (auto& c : placeCurves_)
        c.clear();
    for (int b = 0; b < kBands; ++b)
    {
        if (!used(b) || !enabled(b))
            continue;
        const int place = placement(b);
        std::vector<double>& target = place > 0 ? placeCurves_[static_cast<size_t>(place)] : toneCurve_;
        if (target.empty())
            target.assign(static_cast<size_t>(cols), 0.0);
        for (int x = 0; x < cols; ++x)
            target[static_cast<size_t>(x)] += toneCurveAt(b, freqForX(g.x + x));
    }
}

// ------------------------------------------------------------ painting

void EqPanel::paint(cairo_t* cr)
{
    updateCache();
    fillRounded(cr, bounds_, 8, theme::panel);
    setFont(cr, 11, true);
    drawText(cr, tone_ ? "TONE EQ" : "DECAY CONTOUR", {bounds_.x + kPad, bounds_.y + 6, 200, 18}, Align::Left, accent_);
    setFont(cr, 10);
    drawText(cr, tone_ ? "output filter, dB" : "reverb time per frequency",
             {bounds_.right() - kPad - 220, bounds_.y + 6, 220, 18}, Align::Right, theme::textFaint);

    const Rect g = graph();
    fillRounded(cr, {g.x - 2, g.y, g.w + 4, g.h}, 4, theme::plot);
    paintGrid(cr, g);
    cairo_save(cr);
    cairo_rectangle(cr, g.x, g.y, g.w, g.h);
    cairo_clip(cr);
    if (tone_)
        paintTone(cr, g);
    else
        paintDecay(cr, g);
    paintHandles(cr);
    cairo_restore(cr);
    paintChips(cr);
}

void EqPanel::paintGrid(cairo_t* cr, const Rect& g)
{
    cairo_set_line_width(cr, 1.0);
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
    {
        const bool major = f == 100.0 || f == 1000.0 || f == 10000.0;
        const float x = std::round(xForFreq(f)) + 0.5f;
        setColor(cr, Color(1, 1, 1, major ? 0.06f : 0.03f));
        cairo_move_to(cr, x, g.y);
        cairo_line_to(cr, x, g.bottom());
        cairo_stroke(cr);
    }
    setFont(cr, 9.5f);
    const std::pair<double, const char*> labels[] = {{50, "50"},   {100, "100"}, {200, "200"}, {500, "500"},
                                                     {1000, "1k"}, {2000, "2k"}, {5000, "5k"}, {10000, "10k"}};
    for (auto& [f, t] : labels)
        drawText(cr, t, {xForFreq(f) - 20, g.bottom() + 1, 40, 13}, Align::Center, theme::textFaint);

    float lastLabelY = -1e9f;
    auto hline = [&](double v, float alpha, const char* text) {
        const float y = std::round(yFor(v)) + 0.5f;
        if (y < g.y || y > g.bottom())
            return;
        setColor(cr, Color(1, 1, 1, alpha));
        cairo_move_to(cr, g.x, y);
        cairo_line_to(cr, g.right(), y);
        cairo_stroke(cr);
        if (std::fabs(y - lastLabelY) < 13.0f) // skip labels that would overlap on small sizes
            return;
        lastLabelY = y;
        drawText(cr, text, {bounds_.x + 2, y - 7, kPad + kScaleW - 6, 14}, Align::Right, theme::textFaint);
    };
    if (tone_)
    {
        for (int db = -18; db <= 18; db += 6)
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), db > 0 ? "+%d" : "%d", db);
            hline(db, db == 0 ? 0.09f : 0.03f, buf);
        }
        return;
    }
    const std::pair<double, const char*> secs[] = {{0.1, "0.1s"}, {0.2, "0.2"}, {0.5, "0.5"}, {1, "1s"},
                                                   {2, "2"},      {5, "5"},     {10, "10s"},  {20, "20"}};
    for (auto& [s, t] : secs)
        hline(std::log2(s), (s == 1 || s == 10) ? 0.06f : 0.03f, t);
}

void EqPanel::paintDecay(cairo_t* cr, const Rect& g)
{
    const double step = std::exp2(1.0 / 6.0);
    for (int k = 0; k < kNumBars && k < static_cast<int>(barSeconds_.size()); ++k)
    {
        const double fc = barFreq(k);
        const float x0 = std::max(g.x, xForFreq(fc / step)) + 1.5f;
        const float x1 = std::min(g.right(), xForFreq(fc * step)) - 1.5f;
        if (x1 <= x0)
            continue;
        const float y = std::clamp(yFor(std::log2(std::max(barSeconds_[static_cast<size_t>(k)], 1e-3))), g.y, g.bottom());
        setColor(cr, accent_.withAlpha(0.28f));
        cairo_rectangle(cr, x0, y, x1 - x0, g.bottom() - y);
        cairo_fill(cr);
        setColor(cr, accent_);
        cairo_rectangle(cr, x0, y, x1 - x0, 2.0);
        cairo_fill(cr);
    }
    // Reference: the room's nominal time (Room x Length).
    const float y = std::round(yFor(std::log2(std::max(nominal_, 1e-3)))) + 0.5f;
    if (y > g.y && y < g.bottom())
    {
        const double dash[] = {4.0, 4.0};
        cairo_set_dash(cr, dash, 2, 0);
        setColor(cr, theme::text.withAlpha(0.35f));
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, g.x, y);
        cairo_line_to(cr, g.right(), y);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        char buf[24];
        std::snprintf(buf, sizeof(buf), nominal_ < 10.0 ? "%.2f s" : "%.1f s", nominal_);
        setFont(cr, 9.5f);
        drawText(cr, buf, {g.right() - 64, y - 15, 60, 13}, Align::Right, theme::textDim);
    }
}

void EqPanel::paintTone(cairo_t* cr, const Rect& g)
{
    const int cols = static_cast<int>(toneCurve_.size());
    const float zero = yFor(0.0);
    auto trace = [&](const std::vector<double>& c) {
        for (int x = 0; x < cols && x < static_cast<int>(c.size()); x += 2)
        {
            const float y = std::clamp(yFor(c[static_cast<size_t>(x)]), g.y - 2, g.bottom() + 2);
            if (x == 0)
                cairo_move_to(cr, g.x, y);
            else
                cairo_line_to(cr, g.x + x, y);
        }
    };
    if (cols > 1)
    {
        trace(toneCurve_);
        cairo_line_to(cr, g.right(), zero);
        cairo_line_to(cr, g.x, zero);
        cairo_close_path(cr);
        setColor(cr, accent_.withAlpha(0.11f));
        cairo_fill(cr);
        trace(toneCurve_);
        setColor(cr, accent_);
        cairo_set_line_width(cr, 1.8);
        cairo_stroke(cr);
    }
    // Bands working on one channel or on mid/side only.
    static const char* const kPlace[5] = {"", "L", "R", "M", "S"};
    const double dash[] = {3.0, 3.0};
    for (int p = 1; p < 5; ++p)
    {
        const auto& c = placeCurves_[static_cast<size_t>(p)];
        if (c.empty())
            continue;
        cairo_set_dash(cr, dash, 2, 0);
        trace(c);
        setColor(cr, theme::text.withAlpha(0.55f));
        cairo_set_line_width(cr, 1.3);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        setFont(cr, 9.0f, true);
        const float y = std::clamp(yFor(c.back()), g.y + 6, g.bottom() - 6);
        drawText(cr, kPlace[p], {g.right() - 14, y - 14, 12, 12}, Align::Center, theme::text);
    }
}

void EqPanel::paintHandles(cairo_t* cr)
{
    const Rect g = graph();
    const float base = tone_ ? yFor(0.0) : g.bottom();
    setFont(cr, 9.0f, true);
    for (int b = 0; b < kBands; ++b)
    {
        if (!used(b))
            continue;
        const float x = handleX(b), y = handleY(b);
        const bool sel = b == selected_, hov = b == hover_;
        setColor(cr, accent_.withAlpha(sel ? 0.55f : 0.25f));
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, std::round(x) + 0.5f, base);
        cairo_line_to(cr, std::round(x) + 0.5f, y);
        cairo_stroke(cr);

        const float s = (sel || hov) ? 7.0f : 6.0f;
        cairo_move_to(cr, x, y - s);
        cairo_line_to(cr, x + s, y);
        cairo_line_to(cr, x, y + s);
        cairo_line_to(cr, x - s, y);
        cairo_close_path(cr);
        setColor(cr, enabled(b) ? (sel ? accent_.mix(Color(1, 1, 1, 1), 0.25f) : accent_) : theme::plot);
        cairo_fill_preserve(cr);
        setColor(cr, sel ? theme::text : accent_);
        cairo_set_line_width(cr, sel ? 1.6 : 1.0);
        cairo_stroke(cr);
        drawText(cr, std::to_string(b + 1), {x - 8, y - s - 14, 16, 12}, Align::Center,
                 sel ? theme::text : theme::textDim);
    }
}

void EqPanel::paintChips(cairo_t* cr)
{
    const Rect row = chipRow();
    setFont(cr, 10);
    drawText(cr, "Bands", {row.x, row.y, 40, row.h}, Align::Left, theme::textFaint);
    int slot = 0;
    setFont(cr, 10.5f, true);
    for (int b = 0; b < kBands; ++b)
    {
        if (!used(b))
            continue;
        const Rect c = chipRect(slot++);
        const bool sel = b == selected_;
        if (sel)
            fillRounded(cr, c, 4, accent_);
        else
        {
            fillRounded(cr, c, 4, theme::panelLight);
            strokeRounded(cr, c, 4, b == hover_ ? accent_.withAlpha(0.7f) : theme::outline);
        }
        const Color tc = sel ? theme::plot : (enabled(b) ? theme::text : theme::textFaint);
        drawText(cr, std::to_string(b + 1), c, Align::Center, tc);
    }
    if (selected_ < 0)
    {
        setFont(cr, 10);
        drawText(cr, "No bands. Use + Add, or double-click the graph.", inspectorRow(), Align::Left,
                 theme::textFaint);
    }
}

// ------------------------------------------------------------ interaction

bool EqPanel::mouseDown(const MouseEvent& e)
{
    if (const int c = chipAt(e.x, e.y); c >= 0)
    {
        select(c);
        if (e.button == 3)
            showBandMenu(c, e.x, e.y);
        return true;
    }
    const Rect g = graph();
    if (const int b = handleAt(e.x, e.y); b >= 0)
    {
        select(b);
        if (e.button == 3)
        {
            showBandMenu(b, e.x, e.y);
            return true;
        }
        if (e.button != 1)
            return true;
        if (e.clicks == 2)
        {
            openParamEditor(ctx_, pidx(b, FFreq), {handleX(b) - 40, handleY(b) - 30, 80, 20}, root());
            return true;
        }
        dragging_ = true;
        startX_ = e.x;
        startY_ = e.y;
        startFreq_ = freq(b);
        startLevel_ = level(b);
        ctx_.begin(pidx(b, FFreq));
        ctx_.begin(pidx(b, FLevel));
        return true;
    }
    if (e.button == 1 && e.clicks == 2 && g.contains(e.x, e.y))
    {
        const double f = freqForX(e.x);
        if (tone_)
            addBand(static_cast<int>(dsp::PostShape::Bell), f, std::clamp(valueForY(e.y), -kToneRange, kToneRange));
        else
        {
            // Start the band where the click was, relative to the current decay there.
            const double here = std::log2(std::max(model_.t60At(f), 1e-3));
            addBand(static_cast<int>(dsp::DecayShape::Bell), f, std::clamp(valueForY(e.y) - here, -3.0, 3.0));
        }
        return true;
    }
    return true;
}

void EqPanel::mouseDrag(const MouseEvent& e)
{
    if (!dragging_ || selected_ < 0)
        return;
    const Rect g = graph();
    const float fine = (e.mods & ModShift) ? 0.15f : 1.0f;
    const float dx = (e.x - startX_) * fine, dy = (e.y - startY_) * fine;
    const double f = std::clamp(startFreq_ * std::pow(kMaxFreq / kMinFreq, dx / g.w), 10.0, 30000.0);
    ctx_.set(pidx(selected_, FFreq), conv::freqToValue(f));
    if (hasLevel(selected_))
    {
        double delta = valueForY(startY_ + dy) - valueForY(startY_);
        if (tone_)
        {
            if (shape(selected_) == 1 || shape(selected_) == 2)
                delta *= 2.0; // shelf handles sit at half gain
            ctx_.set(pidx(selected_, FLevel), conv::gainDbToValue(std::clamp(startLevel_ + delta, -30.0, 30.0)));
        }
        else
            ctx_.set(pidx(selected_, FLevel), conv::rateLog2ToValue(std::clamp(startLevel_ + delta, -3.0, 3.0)));
    }
    repaint();
}

void EqPanel::mouseUp(const MouseEvent&)
{
    if (dragging_ && selected_ >= 0)
    {
        ctx_.end(pidx(selected_, FFreq));
        ctx_.end(pidx(selected_, FLevel));
    }
    dragging_ = false;
}

void EqPanel::mouseMove(const MouseEvent& e)
{
    int h = handleAt(e.x, e.y);
    if (h < 0)
        h = chipAt(e.x, e.y);
    if (h != hover_)
    {
        hover_ = h;
        repaint();
    }
}

void EqPanel::mouseLeave()
{
    if (hover_ >= 0)
    {
        hover_ = -1;
        repaint();
    }
}

bool EqPanel::mouseWheel(const MouseEvent& e)
{
    const int b = handleAt(e.x, e.y);
    if (b < 0)
        return false;
    const double factor = std::exp2(e.wheel * ((e.mods & ModShift) ? 0.03 : 0.15));
    ctx_.change(pidx(b, FQ), conv::qToValue(q(b) * factor));
    return true;
}

bool EqPanel::keyDown(const KeyEvent& e)
{
    if ((e.keysym == XK_Delete || e.keysym == XK_BackSpace) && selected_ >= 0 && hoverInside())
    {
        deleteBand(selected_);
        return true;
    }
    return false;
}

} // namespace aurum::gui
