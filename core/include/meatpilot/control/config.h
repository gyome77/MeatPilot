// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "meatpilot/control/actuator.h"
#include "meatpilot/model/types.h"

namespace meatpilot::control {

using meatpilot::Seconds;

/// Presence and timing guards for one actuator.
struct ActuatorConfig {
    bool    present{false};
    Seconds min_on{0.0};   ///< cannot stop before this has elapsed  (FR-T-04)
    Seconds min_off{0.0};  ///< cannot start before this has elapsed (FR-T-04)
};

/// Circulation and fresh-air scheduling (FR-V-02, FR-A-02).
struct DutyCycle {
    Seconds period{0.0};  ///< 0 disables the schedule
    Seconds run{0.0};     ///< run time within each period
    bool    continuous{false};

    constexpr bool enabled() const noexcept {
        return continuous || (period > 0.0 && run > 0.0);
    }
};

/// Everything the engine needs for one tick. Immutable during the tick.
///
/// Targets are optional: a phase may regulate temperature only, humidity only,
/// or neither (monitoring). A null target means "do not regulate this quantity".
struct RegulationConfig {
    bool  has_temp_target{false};
    float target_temp{13.0F};
    float temp_deadband{0.5F};

    bool  has_humidity_target{false};
    float target_humidity{75.0F};
    float humidity_deadband{3.0F};

    ActuatorConfig actuators[kActuatorCount]{};

    /// Power-up lockout: no actuator starts within this window of boot
    /// (FR-T-04). Applies on top of, not instead of, min_off.
    Seconds startup_lockout{120.0};

    /// Sensors must read valid continuously for this long before automatic
    /// control resumes after a restart (spec 4.3).
    Seconds stabilisation_period{60.0};

    /// Humidity actuators may change state at most once per window. Humidity
    /// in a small chamber responds slowly and noisily; without this a
    /// humidifier short-cycles against its own mist. [EXT]
    Seconds humidity_anti_oscillation{600.0};

    // Absolute limits (FR-T-05). A breach inhibits whichever actuator makes
    // the condition worse.
    float temp_abs_min{0.0F};
    float temp_abs_max{30.0F};
    float humidity_abs_min{40.0F};
    float humidity_abs_max{99.0F};

    DutyCycle circulation{};  ///< FR-V-02
    DutyCycle fresh_air{};    ///< FR-A-02

    bool  co2_demand_enabled{false};
    float co2_threshold{1500.0F};

    /// Post-door circulation cycle: how long to keep circulating after the
    /// door closes (FR-V-04).
    Seconds post_door_circulation{300.0};

    constexpr bool has(Actuator a) const noexcept {
        return actuators[index(a)].present;
    }
    constexpr const ActuatorConfig& actuator(Actuator a) const noexcept {
        return actuators[index(a)];
    }
};

}  // namespace meatpilot::control
