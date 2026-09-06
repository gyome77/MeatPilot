// SPDX-License-Identifier: MIT
#pragma once

#include "meatpilot/control/actuator.h"
#include "meatpilot/control/config.h"

namespace meatpilot::control {

/// Conditioned readings come from core/sensor; the engine consumes them and
/// never decides validity itself.
using meatpilot::Reading;

/// Operating mode (spec section 2).
enum class Mode : std::uint8_t {
    Off,         ///< automatic outputs off; monitoring and critical alarms live
    Manual,      ///< operator drives outputs, still subject to limits and timers
    Auto,        ///< fixed setpoints
    Programme,   ///< setpoints follow a multi-phase recipe
    Maintenance, ///< outputs forced off, nuisance alarms suppressed
    Safe,        ///< critical fault: dangerous outputs off, alarm latched
};

/// What the actuator is physically doing, where feedback is fitted (AL-04).
struct Feedback {
    bool present{false};
    bool is_on{false};
};

/// Processed snapshot handed to the engine. Values are already filtered and
/// validated by the acquisition layer, so the engine never decides validity.
struct Inputs {
    Reading temperature{};
    Reading humidity{};
    Reading temperature_backup{};  ///< DS18B20 cross-check (FR-T-06)

    bool  has_dew_point{false};
    float dew_point{0.0F};

    bool  door_open{false};

    bool  has_co2{false};
    float co2{0.0F};

    /// Independent safety thermostat / trip input. Software cannot override it
    /// (spec 4.2 rank 1); it is read so the controller can alarm and go SAFE.
    bool safety_trip{false};

    Feedback feedback[kActuatorCount]{};

    /// Operator requests in MANUAL mode; ignored in every other mode.
    bool manual_request[kActuatorCount]{};
};

}  // namespace meatpilot::control
