// SPDX-License-Identifier: MIT
#include "meatpilot/sim/chamber.h"

#include <cstdint>

namespace meatpilot::sim {
namespace {
constexpr float clampf(float v, float lo, float hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}
}  // namespace

Chamber::Chamber(const Params& params, float temp_c, float rh_pct) noexcept
    : params_(params), temp_(temp_c), rh_(rh_pct) {}

float Chamber::noise(float amplitude) noexcept {
    if (amplitude <= 0.0F) return 0.0F;
    rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto bits = static_cast<std::uint32_t>(rng_ >> 33);
    const float unit = static_cast<float>(bits % 20001) / 10000.0F - 1.0F;
    return unit * amplitude;
}

float Chamber::readTemperature() noexcept {
    return temp_ + noise(params_.noise_temp);
}

float Chamber::readHumidity() noexcept {
    return rh_ + noise(params_.noise_rh);
}

void Chamber::step(const bool (&commands)[control::kActuatorCount],
                   bool door_open, Seconds dt) noexcept {
    using control::Actuator;
    using control::index;

    const bool cool = commands[index(Actuator::Cool)] && !faults_.cooling_dead;
    const bool heat = commands[index(Actuator::Heat)] && !faults_.heating_dead;
    const bool humidify =
        commands[index(Actuator::Humidify)] && !faults_.humidifier_dead;
    const bool dehumidify =
        commands[index(Actuator::Dehumidify)] && !faults_.dehumidifier_dead;
    const bool fresh_air = commands[index(Actuator::FreshAir)];

    // Coupling to the room: insulation normally, wide open when the door is,
    // and somewhere between while fresh air is being drawn in.
    float leak = 1.0F;
    if (door_open) leak = params_.door_leak_factor;
    else if (fresh_air) leak = params_.fresh_air_leak_factor;

    const float step_s = static_cast<float>(dt);

    float d_temp = (params_.ambient_temp - temp_) /
                   static_cast<float>(params_.thermal_tau) * leak;
    if (cool) d_temp += params_.cool_rate;
    if (heat) d_temp += params_.heat_rate;

    float d_rh = (params_.ambient_rh - rh_) /
                 static_cast<float>(params_.humidity_tau) * leak;
    if (humidify) d_rh += params_.humidify_rate;
    if (dehumidify) d_rh += params_.dehumidify_rate;
    if (cool) d_rh += params_.cool_drying;
    if (heat) d_rh += params_.heat_drying;

    temp_ = clampf(temp_ + d_temp * step_s, -40.0F, 90.0F);
    rh_ = clampf(rh_ + d_rh * step_s, 0.0F, 100.0F);
}

}  // namespace meatpilot::sim
