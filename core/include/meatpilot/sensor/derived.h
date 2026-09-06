// SPDX-License-Identifier: MIT
#pragma once

namespace meatpilot::sensor {

/// Dew point in degrees Celsius, Magnus-Tetens approximation.
///
/// Accurate to about 0.35 C over 0-60 C, which is far better than the
/// chamber's own uniformity. Required by FR-H-01, and used by the condensation
/// alarm (AL-09): when chamber temperature approaches dew point, moisture
/// forms on the product surface and on the probe itself.
float dewPoint(float temp_c, float rh_pct) noexcept;

/// Absolute humidity in grams of water per cubic metre.
///
/// Unlike relative humidity, this does not move when temperature does, which
/// makes it the honest quantity to watch when cooling and humidifying interact
/// (FR-H-04).
float absoluteHumidity(float temp_c, float rh_pct) noexcept;

}  // namespace meatpilot::sensor
