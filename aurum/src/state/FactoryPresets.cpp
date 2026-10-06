// Factory preset library. All settings are original to Aurum.
#include <cmath>
#include <functional>

#include "PresetManager.h"
#include "dsp/RoomModel.h"
#include "plugin/Params.h"

namespace aurum {

namespace {

class Builder
{
public:
    Builder() : doc_{{}, defaultValues()} {}

    Builder& set(uint32_t id, double v)
    {
        doc_.values[static_cast<size_t>(ParamTable::get().indexOf(id))] = v;
        return *this;
    }
    Builder& decaySeconds(double s) { return set(pid::Space, dsp::spaceForT60(s)); }
    Builder& space(double v) { return set(pid::Space, v); }
    Builder& rate(double pct) { return set(pid::DecayRate, (std::log2(pct / 100.0) + 2.0) / 4.0); }
    Builder& style(int s) { return set(pid::Style, s); }
    Builder& predelay(double ms) { return set(pid::Predelay, conv::predelayToValue(ms)); }
    Builder& character(double pct) { return set(pid::Character, pct / 100.0); }
    Builder& brightness(double pct) { return set(pid::Brightness, pct / 100.0); }
    Builder& distance(double pct) { return set(pid::Distance, pct / 100.0); }
    Builder& thickness(double pct) { return set(pid::Thickness, pct / 100.0); }
    Builder& ducking(double pct) { return set(pid::Ducking, pct / 100.0); }
    Builder& width(double pct) { return set(pid::Width, pct / 150.0); }
    Builder& mix(double pct) { return set(pid::Mix, pct / 100.0); }
    Builder& gate(double holdMs)
    {
        set(pid::GateEnabled, 1.0);
        return set(pid::GateHold, std::log(holdMs / 10.0) / std::log(200.0));
    }
    // shape: 0 bell, 1 low shelf, 2 high shelf, 3 notch
    Builder& decayBand(int b, int shape, double hz, double pct, double q = 1.0)
    {
        using namespace pid;
        set(decay(b, DUsed), 1).set(decay(b, DEnabled), 1).set(decay(b, DShape), shape);
        set(decay(b, DFreq), conv::freqToValue(hz)).set(decay(b, DRate), conv::rateLog2ToValue(std::log2(pct / 100.0)));
        return set(decay(b, DQ), conv::qToValue(q));
    }
    // shape: 0 bell, 1 low shelf, 2 high shelf, 3 low cut, 4 high cut
    Builder& postBand(int b, int shape, double hz, double db = 0.0, double q = 0.7071, int slope = 1, int place = 0)
    {
        using namespace pid;
        set(post(b, PUsed), 1).set(post(b, PEnabled), 1).set(post(b, PShape), shape);
        set(post(b, PFreq), conv::freqToValue(hz)).set(post(b, PGain), conv::gainDbToValue(db));
        // Cuts use Q 1 for a plain Butterworth slope.
        set(post(b, PQ), conv::qToValue(shape >= 3 && q == 0.7071 ? 1.0 : q)).set(post(b, PSlope), slope);
        return set(post(b, PPlacement), place);
    }
    Builder& meta(const std::string& tags, const std::string& description)
    {
        doc_.meta["author"] = "Aurum";
        doc_.meta["tags"] = tags;
        doc_.meta["description"] = description;
        return *this;
    }
    StateDocument doc() const { return doc_; }

private:
    StateDocument doc_;
};

} // namespace

std::vector<std::pair<std::string, StateDocument>> factoryPresets()
{
    std::vector<std::pair<std::string, StateDocument>> out;
    auto add = [&](const std::string& rel, Builder b) { out.emplace_back(rel, b.doc()); };

    add("Init", Builder().meta("default", "Neutral concert hall, a good starting point."));

    const std::string rooms = "01 Ambience & Rooms/";
    add(rooms + "Tight Ambience",
        Builder().decaySeconds(0.25).distance(15).character(10).brightness(55).width(90).mix(25).postBand(0, 3, 120)
            .meta("ambience|drums|small", "Short, dense early field. Adds space without an audible tail."));
    add(rooms + "Vocal Booth",
        Builder().decaySeconds(0.32).distance(20).character(5).brightness(45).thickness(55).width(70).mix(20)
            .postBand(0, 3, 160, 0, 0.71, 2).postBand(1, 0, 3500, -2.0, 1.2)
            .meta("vocal|small|dry", "Small treated room, keeps vocals upfront."));
    add(rooms + "Drum Room",
        Builder().decaySeconds(0.7).distance(35).character(30).brightness(60).thickness(65).width(110).mix(30)
            .decayBand(0, 1, 150, 130, 0.7).postBand(0, 3, 70)
            .meta("drums|room|punchy", "Live drum room with pronounced reflections."));
    add(rooms + "Wooden Room",
        Builder().decaySeconds(0.6).distance(30).character(20).brightness(35).thickness(55).width(100).mix(25)
            .decayBand(0, 2, 4000, 60, 0.7).postBand(0, 0, 600, 1.5, 0.8)
            .meta("room|warm|acoustic", "Warm wooden walls, soft highs."));
    add(rooms + "Bright Studio",
        Builder().decaySeconds(0.5).distance(25).character(15).brightness(75).width(100).mix(22).postBand(0, 3, 200)
            .meta("room|bright|studio", "Live studio floor with a lively top end."));
    add(rooms + "Small Chamber",
        Builder().decaySeconds(1.2).distance(40).character(25).brightness(50).thickness(60).width(100).mix(25)
            .decayBand(0, 1, 250, 120, 0.7)
            .meta("chamber|vocal|snare", "Echo chamber character, dense and smooth."));

    const std::string halls = "02 Halls/";
    add(halls + "Concert Hall",
        Builder().decaySeconds(2.4).predelay(18).distance(45).character(20).brightness(45).width(100).mix(30)
            .decayBand(0, 1, 200, 125, 0.7).postBand(0, 3, 60)
            .meta("hall|orchestral|natural", "Natural concert hall from mid stalls."));
    add(halls + "Warm Hall",
        Builder().decaySeconds(2.8).predelay(24).distance(50).character(25).brightness(32).width(105).mix(30)
            .decayBand(0, 2, 3000, 55, 0.7).postBand(0, 3, 80).postBand(1, 2, 8000, -3.0)
            .meta("hall|warm|dark", "Dark, enveloping hall for pads and strings."));
    add(halls + "Vocal Hall",
        Builder().decaySeconds(2.1).predelay(45).distance(40).character(35).brightness(55).ducking(35).width(100)
            .mix(25).postBand(0, 3, 180, 0, 0.71, 2).postBand(1, 0, 450, -2.5, 0.9).postBand(2, 4, 11000)
            .meta("vocal|hall|ducking", "Predelayed hall that ducks under the voice."));
    add(halls + "Bright Hall",
        Builder().decaySeconds(2.2).predelay(20).distance(35).character(30).brightness(72).width(110).mix(28)
            .decayBand(0, 2, 6000, 120, 0.7).postBand(0, 3, 120)
            .meta("hall|bright|airy", "Open, airy hall with extended highs."));
    add(halls + "Orchestral Stage",
        Builder().decaySeconds(3.2).predelay(30).distance(70).character(15).brightness(42).thickness(60).width(100)
            .mix(35).decayBand(0, 1, 150, 140, 0.7).decayBand(1, 2, 5000, 60, 0.7)
            .meta("orchestral|hall|distant", "Large stage heard from the back of the hall."));

    const std::string large = "03 Large Spaces/";
    add(large + "Stone Church",
        Builder().decaySeconds(5.5).predelay(35).distance(60).character(20).brightness(40).width(105).mix(30)
            .decayBand(0, 2, 2500, 55, 0.6).decayBand(1, 0, 400, 130, 0.8).postBand(0, 3, 70)
            .meta("church|large|choir", "Stone church with long, warm low mids."));
    add(large + "Cathedral",
        Builder().decaySeconds(9.0).predelay(50).distance(75).character(25).brightness(35).thickness(60).width(110)
            .mix(30).decayBand(0, 2, 3000, 50, 0.6).postBand(0, 3, 60).postBand(1, 4, 9000)
            .meta("cathedral|huge|ambient", "Vast cathedral, slow build-up and endless decay."));
    add(large + "Arena",
        Builder().decaySeconds(4.2).predelay(60).distance(65).character(40).brightness(50).width(120).mix(25)
            .decayBand(0, 1, 180, 80, 0.7).postBand(0, 3, 100)
            .meta("arena|live|large", "Big arena with discrete late echoes."));
    add(large + "Endless Space",
        Builder().space(1.0).rate(250).distance(85).character(60).brightness(50).thickness(70).width(130).mix(40)
            .postBand(0, 3, 150, 0, 0.71, 2)
            .meta("ambient|pad|huge", "Very long modulated tail for ambient textures."));

    const std::string plates = "04 Plates/";
    add(plates + "Classic Plate",
        Builder().style(2).decaySeconds(2.0).predelay(10).distance(20).character(20).brightness(60).width(100).mix(25)
            .postBand(0, 3, 150).meta("plate|vocal|classic", "Studio plate, quick build-up and smooth tail."));
    add(plates + "Vocal Plate",
        Builder().style(2).decaySeconds(1.6).predelay(60).distance(25).character(30).brightness(65).ducking(25)
            .width(100).mix(22).postBand(0, 3, 220, 0, 0.71, 2).postBand(1, 0, 3000, -2.0, 1.0)
            .meta("plate|vocal|ducking", "Bright vocal plate with predelay and light ducking."));
    add(plates + "Snare Plate",
        Builder().style(2).decaySeconds(1.1).distance(15).character(15).brightness(70).thickness(65).width(110).mix(30)
            .postBand(0, 3, 250, 0, 0.71, 3).postBand(1, 0, 1200, 2.0, 1.0)
            .meta("plate|drums|snare", "Short, splashy plate for snares."));
    add(plates + "Dark Plate",
        Builder().style(2).decaySeconds(2.6).predelay(15).distance(30).character(25).brightness(30).width(100).mix(25)
            .decayBand(0, 2, 4000, 55, 0.7).meta("plate|dark|warm", "Damped plate for warm mixes."));

    const std::string classic = "05 Classic Digital/";
    add(classic + "Classic Digital Hall",
        Builder().style(1).decaySeconds(3.0).predelay(25).distance(45).character(45).brightness(45).width(110).mix(30)
            .postBand(0, 3, 100).meta("classic|hall|lush", "Early digital hall with gentle shimmer."));
    add(classic + "Digital Chamber",
        Builder().style(1).decaySeconds(1.4).predelay(12).distance(30).character(35).brightness(55).thickness(60)
            .width(100).mix(25).meta("vintage|chamber|80s", "Bright, grainy chamber of the early digital era."));
    add(classic + "Shimmer Wash",
        Builder().style(1).decaySeconds(7.0).rate(150).predelay(40).distance(70).character(75).brightness(65)
            .width(130).mix(40).postBand(0, 3, 300, 0, 0.71, 2)
            .meta("vintage|ambient|modulated", "Long, heavily modulated tail for pads."));
    add(classic + "Lo-Fi Room",
        Builder().style(1).decaySeconds(0.8).distance(35).character(55).brightness(25).thickness(80).width(80).mix(30)
            .postBand(0, 3, 250, 0, 0.71, 3).postBand(1, 4, 5000, 0, 0.71, 3)
            .meta("vintage|lofi|dirty", "Band-limited, saturated small room."));

    const std::string creative = "06 Creative/";
    add(creative + "Gated Drums",
        Builder().decaySeconds(1.8).distance(30).character(40).brightness(60).thickness(75).width(120).mix(35).gate(180)
            .postBand(0, 3, 120).meta("gate|drums|80s", "Big room cut off by the auto gate."));
    add(creative + "Ducked Delay Space",
        Builder().decaySeconds(2.5).predelay(250).distance(40).character(50).brightness(55).ducking(70).width(120)
            .mix(30).meta("ducking|vocal|creative", "Long predelay with strong ducking: reverb blooms in the gaps."));
    add(creative + "Chorus Cloud",
        Builder().decaySeconds(2.8).distance(55).character(100).brightness(55).width(130).mix(35)
            .meta("chorus|synth|modulated", "Maximum Motion: chorus-like reverb for synths."));
    add(creative + "Freeze Pad",
        Builder().decaySeconds(6.0).distance(80).character(50).brightness(45).width(130).mix(50)
            .meta("freeze|pad|drone", "Long pad. Play a chord, then hold the Freeze button to sustain it."));
    add(creative + "Mono Ambience",
        Builder().decaySeconds(0.4).distance(20).character(10).brightness(50).width(0).mix(25)
            .meta("mono|ambience|utility", "Mono reverb for centred sources."));
    add(creative + "Wide Space",
        Builder().decaySeconds(1.5).distance(40).character(30).brightness(55).width(150).mix(25)
            .postBand(0, 3, 300, 0, 0.71, 2, 4).meta("wide|stereo|utility", "Extra wide tail with a side-only low cut."));
    return out;
}

} // namespace aurum
