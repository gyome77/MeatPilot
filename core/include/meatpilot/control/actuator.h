// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace meatpilot::control {

/// The controllable outputs. Every actuator is optional in a given chamber:
/// a chamber may have cooling only, or none at all (monitoring-only).
enum class Actuator : std::uint8_t {
    Cool,        ///< Compressor / cooling circuit  (FR-T-02)
    Heat,        ///< Heater                        (FR-T-02)
    Humidify,    ///< Humidifier                    (FR-H-02)
    Dehumidify,  ///< Dehumidifier                  (FR-H-02)
    Circulate,   ///< Internal circulation fan      (FR-V-01)
    FreshAir,    ///< Intake/exhaust fan or damper  (FR-A-01)
};

inline constexpr std::size_t kActuatorCount = 6;

inline constexpr std::size_t index(Actuator a) noexcept {
    return static_cast<std::size_t>(a);
}

inline constexpr Actuator kAllActuators[kActuatorCount] = {
    Actuator::Cool,      Actuator::Heat,      Actuator::Humidify,
    Actuator::Dehumidify, Actuator::Circulate, Actuator::FreshAir,
};

/// Pairs that must never be energised together (FR-T-03, FR-H-03).
///
/// Enforced as the final filter over the emitted command set, so no earlier
/// stage can produce a violating pair regardless of what it decided.
struct ExclusionPair {
    Actuator a;
    Actuator b;
};

inline constexpr std::size_t kExclusionPairCount = 2;

inline constexpr ExclusionPair kMutualExclusion[kExclusionPairCount] = {
    {Actuator::Cool, Actuator::Heat},
    {Actuator::Humidify, Actuator::Dehumidify},
};

/// Which way an actuator moves the quantity it controls. Used by the
/// hysteresis helper and by the absolute-limit stage, which must inhibit
/// whichever actuator makes an out-of-range condition worse (spec 4.2 rank 3).
enum class Direction : std::uint8_t {
    Decreasing,  ///< Cool, Dehumidify
    Increasing,  ///< Heat, Humidify
    Neutral,     ///< Circulate, FreshAir
};

inline constexpr Direction direction(Actuator a) noexcept {
    switch (a) {
        case Actuator::Cool:
        case Actuator::Dehumidify:
            return Direction::Decreasing;
        case Actuator::Heat:
        case Actuator::Humidify:
            return Direction::Increasing;
        default:
            return Direction::Neutral;
    }
}

const char* name(Actuator a) noexcept;

}  // namespace meatpilot::control
