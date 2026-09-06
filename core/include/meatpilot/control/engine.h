// SPDX-License-Identifier: MIT
#pragma once

#include "meatpilot/control/actuator.h"
#include "meatpilot/control/config.h"
#include "meatpilot/control/decision.h"
#include "meatpilot/control/inputs.h"

namespace meatpilot::control {

/// Result of one engine tick.
struct Outputs {
    bool     commands[kActuatorCount]{};
    Decision decisions[kActuatorCount]{};
    Mode     mode{Mode::Off};
    bool     safety_active{false};

    constexpr bool on(Actuator a) const noexcept { return commands[index(a)]; }
    constexpr const Decision& decision(Actuator a) const noexcept {
        return decisions[index(a)];
    }
};

/// The regulation engine.
///
/// A pure function of (inputs, config, mode, now) plus its own commanded-state
/// memory. It reads no clock, touches no I/O and allocates nothing, so the
/// whole of the specification's section 15 acceptance suite runs on the host.
///
/// One tick is evaluated in three ordered stages, implementing the priority
/// ladder of spec section 4.2:
///
///   Stage A  INHIBIT  (ranks 1-3)  safety trip, SAFE mode, sensor validity,
///                                  absolute limits. Can only force OFF.
///   Stage B  REQUEST  (ranks 5-8)  temperature, then humidity, then fresh
///                                  air, then circulation.
///   Stage C  GUARD    (rank 4)     min ON/OFF, startup lockout, humidity
///                                  anti-oscillation, then mutual exclusion
///                                  as a final invariant. Can only force OFF.
///
/// Because stages A and C can only remove authority, and mutual exclusion is
/// applied last as a pure filter, the engine cannot emit a command set that
/// violates FR-T-03 or FR-H-03 regardless of what any earlier stage decided.
class Engine {
public:
    explicit Engine(Seconds boot_time = 0.0) noexcept;

    /// Restore a commanded state from the persisted journal after a restart.
    ///
    /// `since` is when the actuator entered that state. After a power loss
    /// with no valid clock, the caller must pass `now` so the full min-OFF
    /// applies -- the worst-case assumption required by spec 13.1. See
    /// docs/ARCHITECTURE.md section 3.4.
    void restore(Actuator a, bool on, Seconds since) noexcept;

    /// Reconcile with physical feedback where fitted (AL-04).
    void syncFeedback(Actuator a, bool is_on, Seconds now) noexcept;

    Outputs tick(const Inputs& in, const RegulationConfig& cfg, Mode mode,
                 Seconds now) noexcept;

    constexpr bool state(Actuator a) const noexcept { return state_[index(a)]; }
    constexpr Seconds lastChange(Actuator a) const noexcept {
        return last_change_[index(a)];
    }

private:
    struct Request {
        bool   desired{false};
        Reason reason{Reason::None};
    };

    void stageInhibit(const Inputs& in, const RegulationConfig& cfg, Mode mode,
                      Seconds now, Reason (&inhibit)[kActuatorCount]) noexcept;
    void stageRequest(const Inputs& in, const RegulationConfig& cfg, Mode mode,
                      Seconds now, Request (&req)[kActuatorCount]) const noexcept;
    void stageGuard(const RegulationConfig& cfg, Seconds now,
                    const Reason (&inhibit)[kActuatorCount],
                    const Request (&req)[kActuatorCount],
                    Outputs& out) noexcept;
    static void enforceExclusion(const bool (&was_on)[kActuatorCount],
                                 Outputs& out) noexcept;

    bool duty(const DutyCycle& d, Seconds now) const noexcept;

    Seconds boot_time_{0.0};
    bool    state_[kActuatorCount]{};
    Seconds last_change_[kActuatorCount]{};
    Seconds humidity_last_start_{0.0};
    bool    humidity_ever_started_{false};

    // Stabilisation is tracked per quantity: replacing the humidity probe must
    // not restart the window for temperature, and a probe that is simply
    // missing must be reported as invalid rather than as stabilising.
    bool    temp_valid_{false};
    Seconds temp_valid_since_{0.0};
    bool    humidity_valid_{false};
    Seconds humidity_valid_since_{0.0};
    Seconds stabilise_until_[kActuatorCount]{};
    Seconds door_closed_at_{0.0};
    bool    door_was_open_{false};
    bool    door_ever_opened_{false};
};

}  // namespace meatpilot::control
