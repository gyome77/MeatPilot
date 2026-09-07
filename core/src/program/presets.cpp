// SPDX-License-Identifier: MIT
#include "meatpilot/program/presets.h"

namespace meatpilot::program {
namespace {

constexpr Seconds kHour = 3600.0;
constexpr Seconds kWeek = 7 * 24 * 3600.0;

void copyText(char* dst, std::size_t cap, const char* src) noexcept {
    std::size_t i = 0;
    for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

/// A warm rest: the fermentation or equalisation step before drying starts.
Phase restPhase(float temp, float rh, Seconds hours) noexcept {
    Phase p{};
    copyText(p.name, kPhaseNameLength, "rest");
    p.has_temp = true;
    p.target_temp = temp;
    p.temp_deadband = 0.8F;
    p.has_humidity = true;
    p.target_humidity = rh;
    p.humidity_deadband = 3.0F;
    p.end = EndKind::Duration;
    p.duration = hours * kHour;
    p.circulation.period = 1800.0;
    p.circulation.run = 300.0;
    p.fresh_air.period = 6 * kHour;
    p.fresh_air.run = 180.0;
    p.drying = false;
    return p;
}

/// The long cool dry, ending on the reference product's weight loss with a
/// duration cap as the no-scale fallback.
Phase dryPhase(float temp, float rh, Seconds cap) noexcept {
    Phase p{};
    copyText(p.name, kPhaseNameLength, "drying");
    p.has_temp = true;
    p.target_temp = temp;
    p.temp_deadband = 0.8F;
    p.has_humidity = true;
    p.target_humidity = rh;
    p.humidity_deadband = 3.0F;
    p.end = EndKind::ReferenceLoss;
    p.max_duration = cap;
    p.circulation.period = 3600.0;
    p.circulation.run = 300.0;
    p.fresh_air.period = 4 * kHour;
    p.fresh_air.run = 120.0;
    p.drying = true;
    p.has_limits = true;
    p.temp_abs_min = 4.0F;
    p.temp_abs_max = 20.0F;
    return p;
}

void build(Programme& out, const char* id, const char* label) noexcept {
    out = Programme{};
    copyText(out.id, kProgrammeNameLength, id);
    copyText(out.name, kProgrammeNameLength, label);
    out.revision = 1;
    out.hold_last_on_complete = true;
}

}  // namespace

void load(Preset preset, Programme& out) noexcept {
    switch (preset) {
        case Preset::SaucissonSec:
            build(out, "saucisson_sec", "Saucisson sec");
            out.phases[0] = restPhase(22.0F, 80.0F, 36.0);
            out.phases[1] = dryPhase(13.0F, 76.0F, 5 * kWeek);
            out.phase_count = 2;
            break;

        case Preset::Coppa:
            build(out, "coppa", "Coppa");
            out.phases[0] = restPhase(22.0F, 80.0F, 24.0);
            out.phases[1] = dryPhase(13.0F, 75.0F, 8 * kWeek);
            out.phase_count = 2;
            break;

        case Preset::Bresaola:
            build(out, "bresaola", "Bresaola");
            out.phases[0] = restPhase(20.0F, 75.0F, 24.0);
            out.phases[1] = dryPhase(13.0F, 72.0F, 6 * kWeek);
            out.phase_count = 2;
            break;

        case Preset::PancettaRoulee:
            build(out, "pancetta_roulee", "Pancetta roulee");
            out.phases[0] = restPhase(22.0F, 80.0F, 24.0);
            out.phases[1] = dryPhase(13.0F, 75.0F, 4 * kWeek);
            out.phase_count = 2;
            break;

        case Preset::Lonzo:
            build(out, "lonzo", "Lonzo");
            out.phases[0] = dryPhase(13.0F, 75.0F, 4 * kWeek);
            out.phase_count = 1;
            break;

        case Preset::CellarHold:
            // No drying target: hold indefinitely until the operator says so.
            build(out, "cellar_hold", "Cellar hold");
            {
                Phase hold{};
                copyText(hold.name, kPhaseNameLength, "hold");
                hold.has_temp = true;
                hold.target_temp = 12.0F;
                hold.temp_deadband = 1.0F;
                hold.has_humidity = true;
                hold.target_humidity = 78.0F;
                hold.humidity_deadband = 3.0F;
                hold.end = EndKind::Manual;
                hold.circulation.period = 3600.0;
                hold.circulation.run = 180.0;
                hold.fresh_air.period = 8 * kHour;
                hold.fresh_air.run = 120.0;
                out.phases[0] = hold;
            }
            out.phase_count = 1;
            break;
    }
}

const char* name(Preset p) noexcept {
    switch (p) {
        case Preset::SaucissonSec:   return "saucisson_sec";
        case Preset::Coppa:          return "coppa";
        case Preset::Bresaola:       return "bresaola";
        case Preset::PancettaRoulee: return "pancetta_roulee";
        case Preset::Lonzo:          return "lonzo";
        case Preset::CellarHold:     return "cellar_hold";
    }
    return "unknown";
}

}  // namespace meatpilot::program
