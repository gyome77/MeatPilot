// SPDX-License-Identifier: MIT
//
// Stable machine-readable identifiers. These strings go into the decision log,
// the REST API and MQTT payloads, so they are part of the interface contract:
// change them only with an API version bump. Human-readable, translated text
// lives in the UI layer, keyed off these identifiers.
#include "meatpilot/control/actuator.h"
#include "meatpilot/control/decision.h"

namespace meatpilot::control {

const char* name(Actuator a) noexcept {
    switch (a) {
        case Actuator::Cool:       return "cool";
        case Actuator::Heat:       return "heat";
        case Actuator::Humidify:   return "humidify";
        case Actuator::Dehumidify: return "dehumidify";
        case Actuator::Circulate:  return "circulate";
        case Actuator::FreshAir:   return "fresh_air";
    }
    return "unknown";
}

const char* name(Reason r) noexcept {
    switch (r) {
        case Reason::None:                 return "none";
        case Reason::NotConfigured:        return "not_configured";
        case Reason::ModeOff:              return "mode_off";
        case Reason::MaintenanceMode:      return "maintenance_mode";
        case Reason::SafetyTrip:           return "safety_trip";
        case Reason::SafeMode:             return "safe_mode";
        case Reason::SensorInvalid:        return "sensor_invalid";
        case Reason::Stabilising:          return "stabilising";
        case Reason::AbsoluteLimitHigh:    return "absolute_limit_high";
        case Reason::AbsoluteLimitLow:     return "absolute_limit_low";
        case Reason::TemperatureHigh:      return "temperature_high";
        case Reason::TemperatureLow:       return "temperature_low";
        case Reason::TemperatureInBand:    return "temperature_in_band";
        case Reason::HumidityHigh:         return "humidity_high";
        case Reason::HumidityLow:          return "humidity_low";
        case Reason::HumidityInBand:       return "humidity_in_band";
        case Reason::HumidityCompensation: return "humidity_compensation";
        case Reason::RedundantWithClimate: return "redundant_with_climate";
        case Reason::PhaseSchedule:        return "phase_schedule";
        case Reason::Co2Demand:            return "co2_demand";
        case Reason::DoorOpen:             return "door_open";
        case Reason::ManualCommand:        return "manual_command";
    }
    return "unknown";
}

const char* name(Blocker b) noexcept {
    switch (b) {
        case Blocker::None:            return "none";
        case Blocker::NotConfigured:   return "not_configured";
        case Blocker::Inhibited:       return "inhibited";
        case Blocker::Stabilising:     return "stabilising";
        case Blocker::MinOnTimer:      return "min_on_timer";
        case Blocker::MinOffTimer:     return "min_off_timer";
        case Blocker::StartupLockout:  return "startup_lockout";
        case Blocker::AntiOscillation: return "anti_oscillation";
        case Blocker::MutualExclusion: return "mutual_exclusion";
    }
    return "unknown";
}

}  // namespace meatpilot::control
