// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "meatpilot/control/actuator.h"

namespace meatpilot::control {

/// Why an actuator was asked to be in its state.
///
/// DP-05 requires the controller to expose the reason for each decision, not
/// only the ON/OFF state. Making this a field of the emitted command rather
/// than a log line means it cannot drift out of sync with the decision.
enum class Reason : std::uint8_t {
    None,
    NotConfigured,        ///< actuator absent from this chamber
    ModeOff,              ///< OFF mode: automatic outputs are down
    MaintenanceMode,      ///< MAINTENANCE: outputs forced off
    SafetyTrip,           ///< independent safety input asserted (rank 1)
    SafeMode,             ///< critical fault latched (rank 2)
    SensorInvalid,        ///< never regulate blind
    Stabilising,          ///< post-boot sensor stabilisation window
    AbsoluteLimitHigh,    ///< quantity above its absolute maximum (rank 3)
    AbsoluteLimitLow,     ///< quantity below its absolute minimum (rank 3)
    TemperatureHigh,      ///< above target + deadband (rank 5)
    TemperatureLow,       ///< below target - deadband (rank 5)
    TemperatureInBand,
    HumidityHigh,         ///< above target + deadband (rank 6)
    HumidityLow,          ///< below target - deadband (rank 6)
    HumidityInBand,
    HumidityCompensation, ///< running to offset drying by cooling/heating
    RedundantWithClimate, ///< suppressed: cooling/heating already drying the air
    PhaseSchedule,        ///< duty cycle from the running phase (ranks 7, 8)
    Co2Demand,            ///< fresh air on measured demand (FR-A-02)
    DoorOpen,             ///< door-open pause (FR-V-04)
    ManualCommand,        ///< operator command in MANUAL mode
};

/// What prevented a requested change from taking effect.
///
/// Paired with `blocked_for_s`, this is what lets the display and the web UI
/// say "cooling wanted, blocked 4 min 12 s by minimum-off timer" instead of
/// showing an unexplained OFF.
enum class Blocker : std::uint8_t {
    None,
    NotConfigured,
    Inhibited,        ///< a stage A safety inhibition
    Stabilising,      ///< post-boot sensor stabilisation window
    MinOnTimer,       ///< cannot stop yet   (FR-T-04)
    MinOffTimer,      ///< cannot start yet  (FR-T-04)
    StartupLockout,   ///< power-up lockout  (FR-T-04)
    AntiOscillation,  ///< humidity flip window
    MutualExclusion,  ///< the opposing actuator holds the pair
};

/// One actuator's outcome for one tick.
struct Decision {
    Actuator actuator{Actuator::Cool};
    bool     desired{false};        ///< what regulation asked for
    bool     commanded{false};      ///< what is actually emitted
    Reason   reason{Reason::None};
    Blocker  blocked_by{Blocker::None};
    std::uint32_t blocked_for_s{0}; ///< remaining time on the blocking timer

    constexpr bool blocked() const noexcept { return blocked_by != Blocker::None; }
};

const char* name(Reason r) noexcept;
const char* name(Blocker b) noexcept;

}  // namespace meatpilot::control
