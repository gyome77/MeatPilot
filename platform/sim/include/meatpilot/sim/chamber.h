// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "meatpilot/control/actuator.h"
#include "meatpilot/model/types.h"

namespace meatpilot::sim {

using meatpilot::Seconds;

/// A simulated curing chamber, for closing the loop around the control engine
/// on the host.
///
/// This is deliberately not a quantitative model of anyone's fridge. It needs
/// the right *signs* and roughly the right *time constants*, so that tests can
/// tell the difference between "the actuator is not working" and "the actuator
/// is working but slow" -- which is the whole difficulty of AL-03, and
/// something no amount of injected constants can exercise.
class Chamber {
public:
    struct Params {
        float ambient_temp{20.0F};      ///< room the chamber sits in
        float ambient_rh{55.0F};

        /// Insulation. Larger means better insulated: the chamber drifts back
        /// towards ambient with this time constant.
        Seconds thermal_tau{5400.0};
        Seconds humidity_tau{7200.0};

        float cool_rate{-0.010F};       ///< degC/s while cooling
        float heat_rate{0.008F};        ///< degC/s while heating
        float humidify_rate{0.010F};    ///< %RH/s while humidifying
        float dehumidify_rate{-0.008F}; ///< %RH/s while dehumidifying

        /// Cooling and heating both dry the air: the coil condenses moisture
        /// out, and warmer air holds more (spec FR-H-04).
        float cool_drying{-0.004F};     ///< %RH/s while cooling
        float heat_drying{-0.003F};     ///< %RH/s while heating

        /// An open door, or fresh-air exchange, couples the chamber to the
        /// room far faster than its insulation does.
        float door_leak_factor{25.0F};
        float fresh_air_leak_factor{8.0F};

        /// Measurement noise on the simulated probes.
        ///
        /// Not decoration. Without it the chamber settles to a mathematically
        /// exact equilibrium and the readings stop changing altogether, at
        /// which point the conditioner's freeze detector correctly concludes
        /// the probe is stuck. Real air and real sensors always dither; a
        /// noiseless simulator is the unrealistic case, and it produced a
        /// false frozen-probe alarm the first time these tests ran.
        float noise_temp{0.02F};
        float noise_rh{0.10F};
    };

    /// Injected hardware faults. Setting one makes the corresponding command
    /// have no physical effect, which is how a "commanded ON, nothing happens"
    /// condition is produced without touching the engine.
    struct Faults {
        bool cooling_dead{false};
        bool heating_dead{false};
        bool humidifier_dead{false};
        bool dehumidifier_dead{false};
    };

    Chamber(const Params& params, float temp_c, float rh_pct) noexcept;

    void step(const bool (&commands)[control::kActuatorCount], bool door_open,
              Seconds dt) noexcept;

    /// Ground truth, for assertions.
    constexpr float temperature() const noexcept { return temp_; }
    constexpr float humidity() const noexcept { return rh_; }

    /// What a probe in this chamber would report: ground truth plus noise.
    /// Non-const because it advances the noise generator.
    float readTemperature() noexcept;
    float readHumidity() noexcept;

    Params& params() noexcept { return params_; }
    Faults& faults() noexcept { return faults_; }

private:
    float noise(float amplitude) noexcept;

    Params        params_;
    Faults        faults_{};
    float         temp_;
    float         rh_;
    std::uint64_t rng_{0x9E3779B97F4A7C15ULL};  ///< fixed seed: reproducible
};

}  // namespace meatpilot::sim
