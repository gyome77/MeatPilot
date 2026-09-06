// SPDX-License-Identifier: MIT
#include "meatpilot/control/engine.h"

#include <cmath>

namespace meatpilot::control {
namespace {

/// All-or-nothing hysteresis around a setpoint.
///
/// Decreasing actuator (cool, dehumidify): starts at target + deadband, stops
/// on return to target. Increasing actuator (heat, humidify): starts at
/// target - deadband, stops on return to target.
///
/// The bands do not overlap, which is why a mutual-exclusion pair can never be
/// requested ON simultaneously by this function.
bool hysteresis(bool current, float value, float target, float deadband,
                Direction dir) noexcept {
    if (dir == Direction::Decreasing) {
        if (value >= target + deadband) return true;
        if (value <= target) return false;
    } else if (dir == Direction::Increasing) {
        if (value <= target - deadband) return true;
        if (value >= target) return false;
    }
    return current;  // inside the band: hold
}

std::uint32_t remaining(Seconds deadline, Seconds now) noexcept {
    const Seconds left = deadline - now;
    return left <= 0.0 ? 0U : static_cast<std::uint32_t>(std::ceil(left));
}

}  // namespace

Engine::Engine(Seconds boot_time) noexcept : boot_time_(boot_time) {
    for (Seconds& t : last_change_) t = boot_time;
    temp_valid_since_ = boot_time;
    humidity_valid_since_ = boot_time;
    door_closed_at_ = boot_time;
}

void Engine::restore(Actuator a, bool on, Seconds since) noexcept {
    state_[index(a)] = on;
    last_change_[index(a)] = since;
}

void Engine::syncFeedback(Actuator a, bool is_on, Seconds now) noexcept {
    if (state_[index(a)] != is_on) {
        state_[index(a)] = is_on;
        last_change_[index(a)] = now;
    }
}

// --- Stage A: inhibitions. May only force OFF. ------------------------------

void Engine::stageInhibit(const Inputs& in, const RegulationConfig& cfg,
                          Mode mode, Seconds now,
                          Reason (&inhibit)[kActuatorCount]) noexcept {
    for (Reason& r : inhibit) r = Reason::None;

    for (Actuator a : kAllActuators) {
        if (!cfg.has(a)) inhibit[index(a)] = Reason::NotConfigured;
    }

    // Rank 1-2: trip and mode-wide shutdowns.
    Reason blanket = Reason::None;
    if (in.safety_trip)              blanket = Reason::SafetyTrip;
    else if (mode == Mode::Safe)     blanket = Reason::SafeMode;
    else if (mode == Mode::Maintenance) blanket = Reason::MaintenanceMode;
    else if (mode == Mode::Off)      blanket = Reason::ModeOff;

    if (blanket != Reason::None) {
        for (Actuator a : kAllActuators) {
            if (inhibit[index(a)] == Reason::None) inhibit[index(a)] = blanket;
        }
        return;
    }

    // Track how long each quantity has been continuously valid. Post-restart
    // stabilisation (spec 4.3) waits on this before automatic control resumes;
    // MANUAL is the operator's responsibility and is not gated.
    if (!in.temperature.valid) {
        temp_valid_ = false;
    } else if (!temp_valid_) {
        temp_valid_ = true;
        temp_valid_since_ = now;
    }
    if (!in.humidity.valid) {
        humidity_valid_ = false;
    } else if (!humidity_valid_) {
        humidity_valid_ = true;
        humidity_valid_since_ = now;
    }

    const bool automatic = (mode == Mode::Auto || mode == Mode::Programme);
    const Seconds temp_ready = temp_valid_since_ + cfg.stabilisation_period;
    const Seconds humidity_ready = humidity_valid_since_ + cfg.stabilisation_period;

    // Never regulate blind: an invalid quantity inhibits the actuators that act
    // on it, and says so -- reporting a disconnected probe as "stabilising"
    // would be a misleading answer to DP-05. Circulation and fresh air are
    // unaffected: they are harmless and keep the chamber homogeneous while a
    // probe is replaced.
    const Actuator temp_actuators[] = {Actuator::Cool, Actuator::Heat};
    for (Actuator a : temp_actuators) {
        if (inhibit[index(a)] != Reason::None) continue;
        if (!temp_valid_) {
            inhibit[index(a)] = Reason::SensorInvalid;
        } else if (automatic && now < temp_ready) {
            inhibit[index(a)] = Reason::Stabilising;
            stabilise_until_[index(a)] = temp_ready;
        }
    }
    const Actuator humidity_actuators[] = {Actuator::Humidify, Actuator::Dehumidify};
    for (Actuator a : humidity_actuators) {
        if (inhibit[index(a)] != Reason::None) continue;
        if (!humidity_valid_) {
            inhibit[index(a)] = Reason::SensorInvalid;
        } else if (automatic && now < humidity_ready) {
            inhibit[index(a)] = Reason::Stabilising;
            stabilise_until_[index(a)] = humidity_ready;
        }
    }

    // Rank 3: absolute limits inhibit whichever actuator makes it worse
    // (FR-T-05). The corrective actuator stays available.
    if (in.temperature.valid) {
        const float t = in.temperature.filtered;
        if (t >= cfg.temp_abs_max && inhibit[index(Actuator::Heat)] == Reason::None)
            inhibit[index(Actuator::Heat)] = Reason::AbsoluteLimitHigh;
        if (t <= cfg.temp_abs_min && inhibit[index(Actuator::Cool)] == Reason::None)
            inhibit[index(Actuator::Cool)] = Reason::AbsoluteLimitLow;
    }
    if (in.humidity.valid) {
        const float h = in.humidity.filtered;
        if (h >= cfg.humidity_abs_max && inhibit[index(Actuator::Humidify)] == Reason::None)
            inhibit[index(Actuator::Humidify)] = Reason::AbsoluteLimitHigh;
        if (h <= cfg.humidity_abs_min && inhibit[index(Actuator::Dehumidify)] == Reason::None)
            inhibit[index(Actuator::Dehumidify)] = Reason::AbsoluteLimitLow;
    }
}

// --- Stage B: requests, in priority order. ---------------------------------

bool Engine::duty(const DutyCycle& d, Seconds now) const noexcept {
    if (d.continuous) return true;
    if (d.period <= 0.0 || d.run <= 0.0) return false;
    const Seconds phase = std::fmod(now - boot_time_, d.period);
    return phase < d.run;
}

void Engine::stageRequest(const Inputs& in, const RegulationConfig& cfg,
                          Mode mode, Seconds now,
                          Request (&req)[kActuatorCount]) const noexcept {
    for (Request& r : req) r = Request{};

    if (mode == Mode::Manual) {
        for (Actuator a : kAllActuators) {
            req[index(a)] = {in.manual_request[index(a)], Reason::ManualCommand};
        }
        return;
    }
    if (mode != Mode::Auto && mode != Mode::Programme) return;

    // Rank 5: temperature. Highest regulating priority (spec 4.2).
    bool climate_active = false;
    if (cfg.has_temp_target && in.temperature.valid) {
        const float t = in.temperature.filtered;
        const bool cool = hysteresis(state_[index(Actuator::Cool)], t,
                                     cfg.target_temp, cfg.temp_deadband,
                                     Direction::Decreasing);
        const bool heat = hysteresis(state_[index(Actuator::Heat)], t,
                                     cfg.target_temp, cfg.temp_deadband,
                                     Direction::Increasing);
        req[index(Actuator::Cool)] = {
            cool, cool ? Reason::TemperatureHigh : Reason::TemperatureInBand};
        req[index(Actuator::Heat)] = {
            heat, heat ? Reason::TemperatureLow : Reason::TemperatureInBand};
        climate_active = cool || heat;
    }

    // Rank 6: humidity, accounting for the interaction with temperature
    // (FR-H-04). Cooling and heating both lower relative humidity, so while
    // either runs the dehumidifier is redundant and the humidifier is allowed
    // to compensate.
    if (cfg.has_humidity_target && in.humidity.valid) {
        const float h = in.humidity.filtered;
        bool hum = hysteresis(state_[index(Actuator::Humidify)], h,
                              cfg.target_humidity, cfg.humidity_deadband,
                              Direction::Increasing);
        bool dehum = hysteresis(state_[index(Actuator::Dehumidify)], h,
                                cfg.target_humidity, cfg.humidity_deadband,
                                Direction::Decreasing);
        Reason hum_reason = hum ? Reason::HumidityLow : Reason::HumidityInBand;
        Reason dehum_reason = dehum ? Reason::HumidityHigh : Reason::HumidityInBand;
        if (climate_active) {
            if (dehum) { dehum = false; dehum_reason = Reason::RedundantWithClimate; }
            if (hum) hum_reason = Reason::HumidityCompensation;
        }
        req[index(Actuator::Humidify)] = {hum, hum_reason};
        req[index(Actuator::Dehumidify)] = {dehum, dehum_reason};
    }

    // Rank 7: fresh-air exchange. Inhibited while an absolute limit is
    // breached, since drawing outside air would deepen the excursion
    // (FR-A-03). A safety purge is a higher-level decision, not this stage's.
    {
        const bool temp_critical =
            in.temperature.valid && (in.temperature.filtered >= cfg.temp_abs_max ||
                                     in.temperature.filtered <= cfg.temp_abs_min);
        const bool hum_critical =
            in.humidity.valid && (in.humidity.filtered >= cfg.humidity_abs_max ||
                                  in.humidity.filtered <= cfg.humidity_abs_min);
        bool want = duty(cfg.fresh_air, now);
        Reason reason = want ? Reason::PhaseSchedule : Reason::None;
        if (cfg.co2_demand_enabled && in.has_co2 && in.co2 >= cfg.co2_threshold) {
            want = true;
            reason = Reason::Co2Demand;
        }
        if (want && (temp_critical || hum_critical)) {
            want = false;
            reason = temp_critical ? Reason::AbsoluteLimitHigh : Reason::AbsoluteLimitHigh;
        }
        if (in.door_open) { want = false; reason = Reason::DoorOpen; }
        req[index(Actuator::FreshAir)] = {want, reason};
    }

    // Rank 8: routine circulation, paused while the door is open, with a
    // post-door cycle to re-homogenise the chamber (FR-V-04).
    {
        bool want = duty(cfg.circulation, now);
        Reason reason = want ? Reason::PhaseSchedule : Reason::None;
        if (!want && door_ever_opened_ &&
            (now - door_closed_at_) < cfg.post_door_circulation) {
            want = true;
            reason = Reason::DoorOpen;
        }
        if (in.door_open) { want = false; reason = Reason::DoorOpen; }
        req[index(Actuator::Circulate)] = {want, reason};
    }
}

// --- Stage C: guards. May only force OFF. ----------------------------------

void Engine::stageGuard(const RegulationConfig& cfg, Seconds now,
                        const Reason (&inhibit)[kActuatorCount],
                        const Request (&req)[kActuatorCount],
                        Outputs& out) noexcept {
    for (Actuator a : kAllActuators) {
        const std::size_t i = index(a);
        const ActuatorConfig& ac = cfg.actuator(a);
        Decision d{};
        d.actuator = a;
        d.desired = req[i].desired;
        d.reason = req[i].reason;

        // An inhibition forces the actuator off. It deliberately bypasses
        // min-ON: stopping a compressor early may shorten its life, but
        // ignoring an absolute limit or a safety trip risks the product and
        // the equipment. Safety wins.
        if (inhibit[i] != Reason::None) {
            d.reason = inhibit[i];
            d.commanded = false;
            if (d.desired) {
                if (inhibit[i] == Reason::NotConfigured) {
                    d.blocked_by = Blocker::NotConfigured;
                } else if (inhibit[i] == Reason::Stabilising) {
                    // This one clears on a schedule, so report the countdown
                    // rather than an opaque "inhibited" (DP-05).
                    d.blocked_by = Blocker::Stabilising;
                    d.blocked_for_s = remaining(stabilise_until_[i], now);
                } else {
                    d.blocked_by = Blocker::Inhibited;
                }
            }
            d.desired = false;
            out.decisions[i] = d;
            out.commands[i] = false;
            continue;
        }

        const bool on = state_[i];
        bool commanded = on;

        if (d.desired && !on) {
            const Seconds off_ready = last_change_[i] + ac.min_off;
            const Seconds boot_ready = boot_time_ + cfg.startup_lockout;
            if (now < boot_ready) {
                d.blocked_by = Blocker::StartupLockout;
                d.blocked_for_s = remaining(boot_ready, now);
            } else if (now < off_ready) {
                d.blocked_by = Blocker::MinOffTimer;
                d.blocked_for_s = remaining(off_ready, now);
            } else if ((a == Actuator::Humidify || a == Actuator::Dehumidify) &&
                       humidity_ever_started_ &&
                       now < humidity_last_start_ + cfg.humidity_anti_oscillation) {
                d.blocked_by = Blocker::AntiOscillation;
                d.blocked_for_s =
                    remaining(humidity_last_start_ + cfg.humidity_anti_oscillation, now);
            } else {
                commanded = true;
            }
        } else if (!d.desired && on) {
            const Seconds on_ready = last_change_[i] + ac.min_on;
            if (now < on_ready) {
                d.blocked_by = Blocker::MinOnTimer;
                d.blocked_for_s = remaining(on_ready, now);
            } else {
                commanded = false;
            }
        }

        d.commanded = commanded;
        out.decisions[i] = d;
        out.commands[i] = commanded;
    }
}

// --- Final invariant: mutual exclusion (FR-T-03, FR-H-03). -----------------

void Engine::enforceExclusion(const bool (&was_on)[kActuatorCount],
                              Outputs& out) noexcept {
    for (const ExclusionPair& p : kMutualExclusion) {
        const std::size_t ia = index(p.a);
        const std::size_t ib = index(p.b);
        if (!out.commands[ia] || !out.commands[ib]) continue;

        // The actuator already running holds the pair; the one trying to start
        // is refused. If neither was running, this is a logic fault upstream --
        // drop both, which is the safe outcome and makes the fault visible.
        const bool keep_a = was_on[ia] && !was_on[ib];
        const bool keep_b = was_on[ib] && !was_on[ia];

        if (!keep_a) {
            out.commands[ia] = false;
            out.decisions[ia].commanded = false;
            out.decisions[ia].blocked_by = Blocker::MutualExclusion;
        }
        if (!keep_b) {
            out.commands[ib] = false;
            out.decisions[ib].commanded = false;
            out.decisions[ib].blocked_by = Blocker::MutualExclusion;
        }
    }
}

Outputs Engine::tick(const Inputs& in, const RegulationConfig& cfg, Mode mode,
                     Seconds now) noexcept {
    Outputs out{};
    out.mode = in.safety_trip ? Mode::Safe : mode;
    out.safety_active = in.safety_trip || mode == Mode::Safe;

    // Door tracking runs before the request stage, so the post-door
    // circulation cycle starts on the tick the door closes, not the next one.
    if (door_was_open_ && !in.door_open) door_closed_at_ = now;
    if (in.door_open) door_ever_opened_ = true;
    door_was_open_ = in.door_open;

    bool was_on[kActuatorCount];
    for (std::size_t i = 0; i < kActuatorCount; ++i) was_on[i] = state_[i];

    Reason inhibit[kActuatorCount];
    stageInhibit(in, cfg, out.mode, now, inhibit);

    Request req[kActuatorCount];
    stageRequest(in, cfg, out.mode, now, req);

    stageGuard(cfg, now, inhibit, req, out);
    enforceExclusion(was_on, out);

    // Commit. Only here does the engine's memory change.
    for (Actuator a : kAllActuators) {
        const std::size_t i = index(a);
        if (state_[i] != out.commands[i]) {
            state_[i] = out.commands[i];
            last_change_[i] = now;
            if (out.commands[i] &&
                (a == Actuator::Humidify || a == Actuator::Dehumidify)) {
                humidity_last_start_ = now;
                humidity_ever_started_ = true;
            }
        }
    }
    return out;
}

}  // namespace meatpilot::control
