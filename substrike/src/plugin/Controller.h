#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include "dsp/Curve.h"
#include "state/StateIO.h"

namespace substrike {

// What the editor sees of the plugin. Main thread only.
class Controller
{
public:
    virtual ~Controller() = default;

    // Stored values, indexed like ParamTable. An edit is a gesture: begin,
    // any number of values, end.
    virtual double paramValue(int index) const = 0;
    virtual void beginEdit(int index) = 0;
    virtual void performEdit(int index, double value) = 0;
    virtual void endEdit(int index) = 0;

    // The breakpoint curves (dsp::curveIndex()), which are state.
    virtual const dsp::Curve& curve(int index) const = 0;
    virtual void setCurve(int index, const dsp::Curve& c) = 0;

    // Plays one hit of the root note at full velocity through the plugin's
    // own output, for designing without a keyboard.
    virtual void audition() = 0;

    // The rate a hit is rendered at for the preview and the export: the
    // host's, once the plugin has been activated.
    virtual double sampleRate() const = 0;

    // Presets. A document replaces the whole state, like a song being loaded;
    // `name` is what the browser shows for it from then on.
    virtual void loadDocument(const StateDocument& doc, const std::string& name) = 0;
    // The state as it is now, with its preset name in meta["preset"].
    virtual StateDocument currentDocument() const = 0;
    virtual std::string presetName() const = 0;

    // Where the modulation matrix has a parameter right now, as a stored
    // value; NaN when no route moves it. The generation changes whenever one
    // of these values does.
    virtual double modulatedValue(int /*index*/) const { return std::nan(""); }
    virtual uint32_t modulationGeneration() const { return 0; }

    // The live oscilloscope: the main output as pairs of minimum and maximum
    // over kScopeBin samples each, oldest first; returns how many it gave.
    static constexpr int kScopeBin = 64;
    virtual int readScope(float* /*lo*/, float* /*hi*/, int /*max*/) { return 0; }

    // Undo and redo. checkpoint() keeps the state as a step after an edit is
    // finished (a gesture, a curve edit, a preset).
    virtual void checkpoint() {}
    virtual bool undo() { return false; }
    virtual bool redo() { return false; }
    virtual bool canUndo() const { return false; }
    virtual bool canRedo() const { return false; }
};

} // namespace substrike
