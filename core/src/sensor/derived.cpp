// SPDX-License-Identifier: MIT
#include "meatpilot/sensor/derived.h"

#include <cmath>

namespace meatpilot::sensor {
namespace {
// Magnus coefficients over water, 0-60 C.
constexpr float kB = 17.62F;
constexpr float kC = 243.12F;
}  // namespace

float dewPoint(float temp_c, float rh_pct) noexcept {
    if (rh_pct <= 0.0F) return -273.15F;
    if (rh_pct > 100.0F) rh_pct = 100.0F;
    const float gamma =
        std::log(rh_pct / 100.0F) + (kB * temp_c) / (kC + temp_c);
    return (kC * gamma) / (kB - gamma);
}

float absoluteHumidity(float temp_c, float rh_pct) noexcept {
    if (rh_pct < 0.0F) rh_pct = 0.0F;
    // Same Magnus coefficients as dewPoint(), so the two agree exactly on
    // where saturation is.
    const float saturation = 6.112F * std::exp((kB * temp_c) / (temp_c + kC));
    return (saturation * rh_pct * 2.1674F) / (273.15F + temp_c);
}

}  // namespace meatpilot::sensor
