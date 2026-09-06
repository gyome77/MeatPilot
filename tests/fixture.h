// SPDX-License-Identifier: MIT
#pragma once

#include "meatpilot/control/engine.h"

namespace meatpilot::test {

using namespace meatpilot::control;

/// A chamber with every actuator fitted and realistic default timings, driven
/// by an explicit clock. Nothing here reads a real clock or touches I/O.
struct Rig {
    Engine  engine{0.0};
    RegulationConfig cfg{};
    Inputs  in{};
    Mode    mode{Mode::Auto};
    Seconds now{0.0};

    Rig() {
        for (Actuator a : kAllActuators) {
            cfg.actuators[index(a)].present = true;
        }
        // Compressor guards, spec FR-T-04 defaults.
        cfg.actuators[index(Actuator::Cool)].min_off = 7 * 60.0;
        cfg.actuators[index(Actuator::Cool)].min_on = 3 * 60.0;
        cfg.startup_lockout = 2 * 60.0;
        cfg.stabilisation_period = 60.0;
        cfg.humidity_anti_oscillation = 600.0;

        cfg.has_temp_target = true;
        cfg.target_temp = 13.0F;
        cfg.temp_deadband = 0.8F;
        cfg.has_humidity_target = true;
        cfg.target_humidity = 75.0F;
        cfg.humidity_deadband = 3.0F;

        setTemp(13.0F);
        setHumidity(75.0F);
    }

    void setTemp(float v, bool valid = true) {
        in.temperature = {v, v, valid};
    }
    void setHumidity(float v, bool valid = true) {
        in.humidity = {v, v, valid};
    }

    /// Advance the clock and tick once.
    Outputs step(Seconds dt = 1.0) {
        now += dt;
        return engine.tick(in, cfg, mode, now);
    }

    /// Advance past every cold-boot gate so tests that are not about boot
    /// behaviour start from a settled controller.
    ///
    /// That includes the minimum-OFF time: a cold boot with no credited clock
    /// owes a full min-OFF on every actuator, which is the worst-case rule of
    /// docs/ARCHITECTURE.md section 3.4, not an artefact of the fixture.
    Outputs settle() {
        Seconds gate = cfg.startup_lockout + cfg.stabilisation_period;
        for (Actuator a : kAllActuators) {
            if (cfg.actuator(a).min_off > gate) gate = cfg.actuator(a).min_off;
        }
        Outputs out{};
        while (now < gate + 5.0) out = step(5.0);
        return out;
    }

    /// Tick repeatedly for `duration`, returning the last result.
    Outputs run(Seconds duration, Seconds dt = 5.0) {
        Outputs out{};
        const Seconds end = now + duration;
        while (now < end) out = step(dt);
        return out;
    }
};

}  // namespace meatpilot::test
