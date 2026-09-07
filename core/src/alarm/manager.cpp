// SPDX-License-Identifier: MIT
#include "meatpilot/alarm/manager.h"

#include <cmath>

#include "meatpilot/sensor/derived.h"

namespace meatpilot::alarm {
namespace {

constexpr std::size_t idx(Key key) noexcept {
    return static_cast<std::size_t>(key);
}

/// Worst-case excursion. Spec §4.3 requires the alarm layer to examine raw
/// readings as well as filtered ones, so that a fast excursion is not smoothed
/// away before anything can react to it.
float peakHigh(const Reading& r) noexcept {
    return r.raw > r.filtered ? r.raw : r.filtered;
}
float peakLow(const Reading& r) noexcept {
    return r.raw < r.filtered ? r.raw : r.filtered;
}

}  // namespace

// --- StartLog ---------------------------------------------------------------

void Manager::StartLog::note(Seconds now) noexcept {
    at[next] = now;
    next = (next + 1) % kMaxActuatorStarts;
    if (count < kMaxActuatorStarts) ++count;
}

std::size_t Manager::StartLog::within(Seconds window, Seconds now) const noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (now - at[i] <= window) ++n;
    }
    return n;
}

bool Manager::StartLog::lastIntervalShorterThan(Seconds period,
                                                Seconds now) const noexcept {
    if (count < 2) return false;
    // The two most recent entries, walking the ring backwards.
    const std::size_t newest = (next + kMaxActuatorStarts - 1) % kMaxActuatorStarts;
    const std::size_t before = (next + kMaxActuatorStarts - 2) % kMaxActuatorStarts;
    // Only interesting if the newest start is recent; an old short interval is
    // history, not a live condition.
    if (now - at[newest] > period) return false;
    return (at[newest] - at[before]) < period;
}

// --- Manager ----------------------------------------------------------------

Manager::Manager(const Config& cfg) noexcept : cfg_(cfg) {
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        const Key key = static_cast<Key>(i);
        const bool critical = severity(key) == Severity::Critical;
        // Criticals fire immediately and latch; warnings wait out the delay
        // and resolve themselves (§5).
        conditions_[i].configure(critical ? 0.0 : cfg_.warning_delay,
                                 cfg_.reminder_interval, critical);
    }
    // The ones whose onset delay is inherent to the rule rather than to
    // severity.
    conditions_[idx(Key::DoorOpenTooLong)].configure(
        cfg_.door_open_limit, cfg_.reminder_interval, false);
    conditions_[idx(Key::HighTempDrying)].configure(
        cfg_.high_temp_drying_duration, cfg_.reminder_interval, true);
    conditions_[idx(Key::DegradedMode)].configure(
        cfg_.degraded_delay, cfg_.reminder_interval, false);
    conditions_[idx(Key::CaseHardening)].configure(
        cfg_.degraded_delay, cfg_.reminder_interval, false);
    conditions_[idx(Key::CondensationRisk)].configure(
        0.0, cfg_.reminder_interval, false);
    // Cycling rules average over their own window, which is the delay. Adding
    // the generic warning delay on top would only postpone a real alarm.
    conditions_[idx(Key::ShortCycling)].configure(
        0.0, cfg_.reminder_interval, false);
    conditions_[idx(Key::ExcessiveStarts)].configure(
        0.0, cfg_.reminder_interval, false);
    // AL-03 carries its own delay in `response_timeout`, so the condition
    // itself fires as soon as the rule concludes there was no response.
    const Key no_response[4] = {Key::NoResponseCooling, Key::NoResponseHeating,
                                Key::NoResponseHumidify,
                                Key::NoResponseDehumidify};
    for (Key k : no_response) {
        conditions_[idx(k)].configure(0.0, cfg_.reminder_interval, false);
    }
}

void Manager::acknowledge(Key key) noexcept { conditions_[idx(key)].acknowledge(); }

void Manager::acknowledgeAll() noexcept {
    for (Condition& c : conditions_) c.acknowledge();
}

bool Manager::visible(Key key) const noexcept {
    return conditions_[idx(key)].visible();
}

bool Manager::anyCritical() const noexcept {
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        if (conditions_[i].visible() &&
            severity(static_cast<Key>(i)) == Severity::Critical) {
            return true;
        }
    }
    return false;
}

bool Manager::requiresSafeState() const noexcept {
    // Not every critical means SAFE. A high-temperature excursion is handled
    // by the priority ladder cutting the aggravating actuator; losing the
    // probes means the controller cannot know what it is doing at all, and
    // that is what SAFE is for (spec §4.2 rank 2).
    return conditions_[idx(Key::TempSensorFault)].triggered() &&
           conditions_[idx(Key::HumiditySensorFault)].triggered();
}

void Manager::emit(Key key, Transition t, Seconds now, float value, float limit,
                   Event* out, std::size_t capacity,
                   std::size_t& written) noexcept {
    if (t == Transition::None) return;
    if (written >= capacity) {
        ++dropped_;
        return;
    }
    Event& e = out[written++];
    e.key = key;
    e.severity = severity(key);
    e.kind = t == Transition::Raised    ? EventKind::Raised
             : t == Transition::Reminder ? EventKind::Reminder
                                         : EventKind::Cleared;
    e.at = now;
    e.value = value;
    e.limit = limit;
    e.action = key == Key::DegradedMode ? manual_action_ : ManualAction::None;
}

void Manager::track(Key key, bool active, Seconds now, float value, float limit,
                    Event* out, std::size_t capacity,
                    std::size_t& written) noexcept {
    const Transition t = conditions_[idx(key)].update(active, now);
    emit(key, t, now, value, limit, out, capacity, written);
}

void Manager::evaluateResponse(const Signals& s, Seconds now, Event* out,
                               std::size_t capacity,
                               std::size_t& written) noexcept {
    using control::Actuator;
    using control::index;

    struct Watched {
        Actuator actuator;
        Key      key;
        bool     expect_increase;
        bool     temperature;
    };
    const Watched watched[4] = {
        {Actuator::Cool, Key::NoResponseCooling, false, true},
        {Actuator::Heat, Key::NoResponseHeating, true, true},
        {Actuator::Humidify, Key::NoResponseHumidify, true, false},
        {Actuator::Dehumidify, Key::NoResponseDehumidify, false, false},
    };

    for (const Watched& w : watched) {
        const std::size_t i = index(w.actuator);
        const Reading& r = w.temperature ? s.inputs.temperature : s.inputs.humidity;
        const bool commanded = s.outputs.commands[i];
        ResponseWatch& rw = response_[i];

        if (!commanded || !r.valid) {
            rw.armed = false;
            track(w.key, false, now, r.filtered, 0.0F, out, capacity, written);
            continue;
        }
        if (!rw.armed) {
            rw.armed = true;
            rw.since = now;
            rw.value_at_start = r.filtered;
            track(w.key, false, now, r.filtered, 0.0F, out, capacity, written);
            continue;
        }

        bool no_response = false;
        if (now - rw.since >= cfg_.response_timeout) {
            const float moved = r.filtered - rw.value_at_start;
            const float progress = w.expect_increase ? moved : -moved;
            no_response = progress < cfg_.response_min_delta;
        }
        track(w.key, no_response, now, r.filtered, rw.value_at_start, out,
              capacity, written);
    }
}

void Manager::evaluateDegraded(const Signals& s,
                               const control::RegulationConfig& cfg, Seconds now,
                               Event* out, std::size_t capacity,
                               std::size_t& written) noexcept {
    using control::Actuator;

    manual_action_ = ManualAction::None;
    bool degraded = false;
    float value = 0.0F;
    float limit = 0.0F;

    // Temperature first: it outranks humidity throughout the design.
    if (cfg.has_temp_target && s.inputs.temperature.valid) {
        const float band = cfg.temp_deadband * cfg_.warn_band_factor;
        const float t = s.inputs.temperature.filtered;
        if (t > cfg.target_temp + band && !cfg.has(Actuator::Cool)) {
            degraded = true;
            manual_action_ = ManualAction::AddCooling;
            value = t;
            limit = cfg.target_temp + band;
        } else if (t < cfg.target_temp - band && !cfg.has(Actuator::Heat)) {
            degraded = true;
            manual_action_ = ManualAction::AddHeating;
            value = t;
            limit = cfg.target_temp - band;
        }
    }

    if (!degraded && cfg.has_humidity_target && s.inputs.humidity.valid) {
        const float band = cfg.humidity_deadband * cfg_.warn_band_factor;
        const float h = s.inputs.humidity.filtered;
        if (h < cfg.target_humidity - band && !cfg.has(Actuator::Humidify)) {
            degraded = true;
            manual_action_ = ManualAction::AddHumidity;
            value = h;
            limit = cfg.target_humidity - band;
        } else if (h > cfg.target_humidity + band &&
                   !cfg.has(Actuator::Dehumidify)) {
            degraded = true;
            manual_action_ = ManualAction::RemoveHumidity;
            value = h;
            limit = cfg.target_humidity + band;
        }
    }

    // Stale air with no way to exchange it.
    if (!degraded && s.inputs.has_co2 && !cfg.has(Actuator::FreshAir) &&
        cfg.co2_demand_enabled && s.inputs.co2 >= cfg.co2_threshold) {
        degraded = true;
        manual_action_ = ManualAction::Ventilate;
        value = s.inputs.co2;
        limit = cfg.co2_threshold;
    }

    track(Key::DegradedMode, degraded, now, value, limit, out, capacity, written);
}

std::size_t Manager::evaluate(const Signals& s,
                              const control::RegulationConfig& cfg, Seconds now,
                              Event* out, std::size_t capacity) noexcept {
    std::size_t written = 0;
    dropped_ = 0;

    const Reading& t = s.inputs.temperature;
    const Reading& h = s.inputs.humidity;

    // --- AL-02: sensor health. Evaluated first: everything below depends on
    // whether the readings mean anything.
    track(Key::TempSensorFault, !t.valid, now, t.raw, 0.0F, out, capacity, written);
    track(Key::HumiditySensorFault, !h.valid, now, h.raw, 0.0F, out, capacity,
          written);
    track(Key::ProbeDivergence, s.divergence_checked && s.divergent, now,
          s.divergence_delta, 0.0F, out, capacity, written);

    // --- AL-01: limits. Criticals use the worst of raw and filtered.
    if (t.valid) {
        track(Key::TemperatureCriticalHigh, peakHigh(t) >= cfg.temp_abs_max, now,
              peakHigh(t), cfg.temp_abs_max, out, capacity, written);
        track(Key::TemperatureCriticalLow, peakLow(t) <= cfg.temp_abs_min, now,
              peakLow(t), cfg.temp_abs_min, out, capacity, written);
        if (cfg.has_temp_target) {
            const float band = cfg.temp_deadband * cfg_.warn_band_factor;
            track(Key::TemperatureWarnHigh, t.filtered > cfg.target_temp + band,
                  now, t.filtered, cfg.target_temp + band, out, capacity, written);
            track(Key::TemperatureWarnLow, t.filtered < cfg.target_temp - band,
                  now, t.filtered, cfg.target_temp - band, out, capacity, written);
        }
    }
    if (h.valid) {
        track(Key::HumidityCriticalHigh, peakHigh(h) >= cfg.humidity_abs_max, now,
              peakHigh(h), cfg.humidity_abs_max, out, capacity, written);
        track(Key::HumidityCriticalLow, peakLow(h) <= cfg.humidity_abs_min, now,
              peakLow(h), cfg.humidity_abs_min, out, capacity, written);
        if (cfg.has_humidity_target) {
            const float band = cfg.humidity_deadband * cfg_.warn_band_factor;
            track(Key::HumidityWarnHigh, h.filtered > cfg.target_humidity + band,
                  now, h.filtered, cfg.target_humidity + band, out, capacity,
                  written);
            track(Key::HumidityWarnLow, h.filtered < cfg.target_humidity - band,
                  now, h.filtered, cfg.target_humidity - band, out, capacity,
                  written);
        }
    }

    // --- AL-03.
    evaluateResponse(s, now, out, capacity, written);

    // --- AL-04: feedback disagrees with the command. A relay welded closed
    // reports ON while we command OFF, and no environmental rule catches it.
    bool mismatch = false;
    for (control::Actuator a : control::kAllActuators) {
        const std::size_t i = control::index(a);
        const control::Feedback& fb = s.inputs.feedback[i];
        if (fb.present && fb.is_on != s.outputs.commands[i]) mismatch = true;
    }
    track(Key::ActuatorFeedbackMismatch, mismatch, now, 0.0F, 0.0F, out, capacity,
          written);

    // --- AL-05: door, short cycling, starts per hour.
    track(Key::DoorOpenTooLong, s.inputs.door_open, now, 0.0F,
          static_cast<float>(cfg_.door_open_limit), out, capacity, written);

    bool short_cycling = false;
    bool excessive = false;
    for (control::Actuator a : control::kAllActuators) {
        const std::size_t i = control::index(a);
        StartLog& log = starts_[i];
        const bool on = s.outputs.commands[i];
        if (on && !log.was_on) log.note(now);
        log.was_on = on;

        if (a != control::Actuator::Cool && a != control::Actuator::Heat) continue;
        if (log.lastIntervalShorterThan(cfg_.min_cycle_period, now)) {
            short_cycling = true;
        }
        if (log.within(3600.0, now) > cfg_.max_starts_per_hour) excessive = true;
    }
    track(Key::ShortCycling, short_cycling, now, 0.0F, 0.0F, out, capacity, written);
    track(Key::ExcessiveStarts, excessive, now, 0.0F,
          static_cast<float>(cfg_.max_starts_per_hour), out, capacity, written);

    // --- AL-06: system health.
    track(Key::LowSupplyVoltage, !s.supply_ok, now, 0.0F, 0.0F, out, capacity,
          written);
    track(Key::RebootLoop, s.recent_reboots >= cfg_.reboot_loop_threshold, now,
          static_cast<float>(s.recent_reboots),
          static_cast<float>(cfg_.reboot_loop_threshold), out, capacity, written);
    track(Key::StorageFault, !s.storage_ok, now, 0.0F, 0.0F, out, capacity, written);
    track(Key::ClockInvalid, !s.clock_valid, now, 0.0F, 0.0F, out, capacity, written);
    track(Key::NetworkDown, !s.network_ok, now, 0.0F, 0.0F, out, capacity, written);
    track(Key::NotificationFailed, !s.notifications_ok, now, 0.0F, 0.0F, out,
          capacity, written);

    // --- AL-07: weighing.
    track(Key::WeightChannelFault, !s.weight_channels_ok, now, 0.0F, 0.0F, out,
          capacity, written);
    track(Key::WeighInOverdue, s.progress.any_weigh_in_overdue, now, 0.0F, 0.0F,
          out, capacity, written);

    // --- AL-08 [EXT]: case hardening. Two independent symptoms of the same
    // failure: the surface drying faster than the interior can follow.
    bool hardening = false;
    float rate_value = 0.0F;
    if (s.has_drying_rate && s.drying_rate_pct_per_day > cfg_.case_hardening_rate) {
        hardening = true;
        rate_value = s.drying_rate_pct_per_day;
    }
    if (cfg.has_humidity_target && h.valid &&
        h.filtered < cfg.target_humidity - cfg.humidity_deadband) {
        hardening = true;
    }
    track(Key::CaseHardening, hardening, now, rate_value, cfg_.case_hardening_rate,
          out, capacity, written);

    // --- AL-09 [EXT]: condensation. Free, because dew point is already
    // computed for FR-H-01.
    bool condensation = false;
    float margin = 0.0F;
    if (t.valid && h.valid) {
        const float dp = sensor::dewPoint(t.filtered, h.filtered);
        margin = t.filtered - dp;
        condensation = margin < cfg_.condensation_margin;
    }
    track(Key::CondensationRisk, condensation, now, margin, cfg_.condensation_margin,
          out, capacity, written);

    // --- AL-10 [EXT]: sustained warmth while drying, independent of the
    // absolute limit. A rest phase at 22 C is correct; a drying phase at 22 C
    // is a food-safety problem, and no fixed limit can tell them apart.
    const bool high_drying = s.drying_phase && t.valid &&
                             t.filtered > cfg_.high_temp_drying_limit;
    track(Key::HighTempDrying, high_drying, now, t.filtered,
          cfg_.high_temp_drying_limit, out, capacity, written);

    // --- AL-11 [EXT].
    evaluateDegraded(s, cfg, now, out, capacity, written);

    return written;
}

Severity Manager::severity(Key key) noexcept {
    switch (key) {
        case Key::TemperatureCriticalHigh:
        case Key::TemperatureCriticalLow:
        case Key::HumidityCriticalHigh:
        case Key::HumidityCriticalLow:
        case Key::TempSensorFault:
        case Key::HumiditySensorFault:
        case Key::ActuatorFeedbackMismatch:
        case Key::LowSupplyVoltage:
        case Key::RebootLoop:
        case Key::HighTempDrying:
            return Severity::Critical;

        case Key::WeighInOverdue:
            return Severity::Info;

        default:
            return Severity::Warning;
    }
}

const char* Manager::name(Key key) noexcept {
    switch (key) {
        case Key::TemperatureWarnHigh:      return "temperature_warn_high";
        case Key::TemperatureWarnLow:       return "temperature_warn_low";
        case Key::TemperatureCriticalHigh:  return "temperature_critical_high";
        case Key::TemperatureCriticalLow:   return "temperature_critical_low";
        case Key::HumidityWarnHigh:         return "humidity_warn_high";
        case Key::HumidityWarnLow:          return "humidity_warn_low";
        case Key::HumidityCriticalHigh:     return "humidity_critical_high";
        case Key::HumidityCriticalLow:      return "humidity_critical_low";
        case Key::TempSensorFault:          return "temp_sensor_fault";
        case Key::HumiditySensorFault:      return "humidity_sensor_fault";
        case Key::ProbeDivergence:          return "probe_divergence";
        case Key::NoResponseCooling:        return "no_response_cooling";
        case Key::NoResponseHeating:        return "no_response_heating";
        case Key::NoResponseHumidify:       return "no_response_humidify";
        case Key::NoResponseDehumidify:     return "no_response_dehumidify";
        case Key::ActuatorFeedbackMismatch: return "actuator_feedback_mismatch";
        case Key::DoorOpenTooLong:          return "door_open_too_long";
        case Key::ShortCycling:             return "short_cycling";
        case Key::ExcessiveStarts:          return "excessive_starts";
        case Key::LowSupplyVoltage:         return "low_supply_voltage";
        case Key::RebootLoop:               return "reboot_loop";
        case Key::StorageFault:             return "storage_fault";
        case Key::ClockInvalid:             return "clock_invalid";
        case Key::NetworkDown:              return "network_down";
        case Key::NotificationFailed:       return "notification_failed";
        case Key::WeightChannelFault:       return "weight_channel_fault";
        case Key::WeighInOverdue:           return "weigh_in_overdue";
        case Key::CaseHardening:            return "case_hardening";
        case Key::CondensationRisk:         return "condensation_risk";
        case Key::HighTempDrying:           return "high_temp_drying";
        case Key::DegradedMode:             return "degraded_mode";
    }
    return "unknown";
}

const char* Manager::name(ManualAction a) noexcept {
    switch (a) {
        case ManualAction::None:           return "none";
        case ManualAction::AddHumidity:    return "add_humidity";
        case ManualAction::RemoveHumidity: return "remove_humidity";
        case ManualAction::AddCooling:     return "add_cooling";
        case ManualAction::AddHeating:     return "add_heating";
        case ManualAction::Ventilate:      return "ventilate";
    }
    return "unknown";
}

}  // namespace meatpilot::alarm
