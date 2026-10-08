#pragma once

#include "dsp/Curve.h"

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
};

} // namespace substrike
