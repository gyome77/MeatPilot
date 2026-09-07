// SPDX-License-Identifier: MIT
#include "meatpilot/control/profiles.h"

namespace meatpilot::control {

void applyCoolingProfile(RegulationConfig& cfg, CoolingKind kind) noexcept {
    ActuatorConfig& cool = cfg.actuators[index(Actuator::Cool)];

    switch (kind) {
        case CoolingKind::DirectCompressor:
            // FR-T-04 defaults: we command the compressor, so we know exactly
            // how long it has been off.
            cool.min_off = 7 * 60.0;
            cool.min_on = 3 * 60.0;
            cfg.startup_lockout = 2 * 60.0;
            break;

        case CoolingKind::ApplianceWithThermostat:
            // Longer min-OFF than a direct compressor, for a reason worth
            // stating: when we cut power to the appliance we do not know
            // whether its compressor was running at that instant. On restore,
            // its thermostat may call for cooling immediately and start the
            // compressor against undissipated head pressure. Our relay is the
            // only thing standing between the appliance and a stalled restart,
            // so it must cover the compressor's full recovery time rather than
            // the remainder of it.
            cool.min_off = 10 * 60.0;
            // And a longer min-ON, because the appliance spends the first part
            // of every power-up booting its own controller rather than
            // cooling. Cutting power again too soon achieves nothing but wear.
            cool.min_on = 5 * 60.0;
            cfg.startup_lockout = 2 * 60.0;
            break;

        case CoolingKind::Thermoelectric:
            // No compressor to protect. Keep short guards purely to stop the
            // relay chattering around the deadband.
            cool.min_off = 60.0;
            cool.min_on = 60.0;
            cfg.startup_lockout = 30.0;
            break;
    }
}

const char* name(CoolingKind kind) noexcept {
    switch (kind) {
        case CoolingKind::DirectCompressor:        return "direct_compressor";
        case CoolingKind::ApplianceWithThermostat: return "appliance_with_thermostat";
        case CoolingKind::Thermoelectric:          return "thermoelectric";
    }
    return "unknown";
}

}  // namespace meatpilot::control
