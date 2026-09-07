// SPDX-License-Identifier: MIT
// Cooling connection profiles: what "Cool ON" means depends on the wiring.
#include <doctest/doctest.h>

#include "meatpilot/control/profiles.h"

using namespace meatpilot::control;

TEST_CASE("switching a whole appliance needs longer guards than a compressor") {
    // When we cut power to a fridge we cannot know whether its compressor was
    // running at that instant, so our relay must cover the compressor's full
    // recovery, not the remainder of it.
    RegulationConfig direct{}, appliance{};
    applyCoolingProfile(direct, CoolingKind::DirectCompressor);
    applyCoolingProfile(appliance, CoolingKind::ApplianceWithThermostat);

    CHECK(appliance.actuator(Actuator::Cool).min_off >
          direct.actuator(Actuator::Cool).min_off);
    CHECK(appliance.actuator(Actuator::Cool).min_on >
          direct.actuator(Actuator::Cool).min_on);
    CHECK(direct.actuator(Actuator::Cool).min_off == doctest::Approx(420.0));
}

TEST_CASE("a Peltier cooler has no compressor to protect") {
    RegulationConfig cfg{};
    applyCoolingProfile(cfg, CoolingKind::Thermoelectric);
    CHECK(cfg.actuator(Actuator::Cool).min_off <= 60.0);
    CHECK(cfg.actuator(Actuator::Cool).min_off > 0.0);  // still no chattering
}

TEST_CASE("a profile leaves the rest of the configuration alone") {
    RegulationConfig cfg{};
    cfg.target_temp = 12.5F;
    cfg.actuators[index(Actuator::Humidify)].min_on = 42.0;
    applyCoolingProfile(cfg, CoolingKind::ApplianceWithThermostat);

    CHECK(cfg.target_temp == doctest::Approx(12.5F));
    CHECK(cfg.actuator(Actuator::Humidify).min_on == doctest::Approx(42.0));
}
