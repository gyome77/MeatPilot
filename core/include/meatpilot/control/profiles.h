// SPDX-License-Identifier: MIT
#pragma once

#include "meatpilot/control/config.h"

namespace meatpilot::control {

/// How the cooling output is physically connected.
///
/// This is not cosmetic: it changes what a "Cool ON" command actually means,
/// and therefore what the timing guards have to protect against.
enum class CoolingKind : std::uint8_t {
    /// We switch the compressor itself, through a contactor. The command and
    /// the compressor state are the same thing, and FR-T-04's defaults apply
    /// directly.
    DirectCompressor,

    /// We switch mains to a complete appliance -- a fridge or wine cooler that
    /// has its own thermostat. "Cool ON" means *the appliance is powered and
    /// permitted to cool*, not "the compressor is running": its own thermostat
    /// decides that, and cycles the compressor independently, invisibly to us.
    ///
    /// Requires the appliance's own setpoint to be below the chamber target,
    /// so that whenever we grant it power it actually cools.
    ApplianceWithThermostat,

    /// A thermoelectric (Peltier) wine cooler. No compressor, so there is
    /// nothing to protect from restart-against-pressure; the guards only need
    /// to stop the relay chattering. Cooling capacity is weak -- typically
    /// 10-15 K below ambient at best -- and it barely dehumidifies.
    Thermoelectric,
};

/// Fill in the cooling actuator's timing guards for the given connection.
///
/// Overwrites `min_on`, `min_off` and `startup_lockout`; leaves everything
/// else alone, so it is safe to call before applying user configuration.
void applyCoolingProfile(RegulationConfig& cfg, CoolingKind kind) noexcept;

const char* name(CoolingKind kind) noexcept;

}  // namespace meatpilot::control
