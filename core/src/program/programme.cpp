// SPDX-License-Identifier: MIT
#include "meatpilot/program/programme.h"

namespace meatpilot::program {

Validation validate(const Programme& p) noexcept {
    if (p.phase_count == 0) return Validation::NoPhases;
    if (p.phase_count > kMaxPhases) return Validation::TooManyPhases;

    for (std::size_t i = 0; i < p.phase_count; ++i) {
        const Phase& ph = p.phases[i];

        if (!ph.has_temp && !ph.has_humidity) return Validation::NoTargets;

        if (ph.has_temp && !(ph.temp_deadband > 0.0F)) {
            return Validation::InvalidDeadband;
        }
        if (ph.has_humidity && !(ph.humidity_deadband > 0.0F)) {
            return Validation::InvalidDeadband;
        }
        if (ph.has_limits && !(ph.temp_abs_min < ph.temp_abs_max)) {
            return Validation::InvalidLimits;
        }

        switch (ph.end) {
            case EndKind::Duration:
                if (!(ph.duration > 0.0)) return Validation::ZeroDuration;
                break;

            case EndKind::ReferenceLoss:
            case EndKind::FirstOfTargets:
            case EndKind::LastOfTargets:
                // Spec §4.1 calls max_duration the "safety fallback when weight
                // data is unavailable". Treating a missing cap as a
                // configuration error rather than an unbounded phase is the
                // difference between a recipe that finishes late and a chamber
                // that sits at fermentation temperature for a month because a
                // load cell came unplugged.
                if (!(ph.max_duration > 0.0)) {
                    return Validation::WeightPhaseWithoutCap;
                }
                break;

            case EndKind::Manual:
                break;
        }
    }
    return Validation::Ok;
}

const char* name(Validation v) noexcept {
    switch (v) {
        case Validation::Ok:                    return "ok";
        case Validation::NoPhases:              return "no_phases";
        case Validation::TooManyPhases:         return "too_many_phases";
        case Validation::NoTargets:             return "no_targets";
        case Validation::ZeroDuration:          return "zero_duration";
        case Validation::WeightPhaseWithoutCap: return "weight_phase_without_cap";
        case Validation::InvalidDeadband:       return "invalid_deadband";
        case Validation::InvalidLimits:         return "invalid_limits";
    }
    return "unknown";
}

const char* name(EndKind k) noexcept {
    switch (k) {
        case EndKind::Duration:       return "duration";
        case EndKind::Manual:         return "manual";
        case EndKind::ReferenceLoss:  return "reference_loss";
        case EndKind::FirstOfTargets: return "first_of_targets";
        case EndKind::LastOfTargets:  return "last_of_targets";
    }
    return "unknown";
}

}  // namespace meatpilot::program
