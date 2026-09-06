// SPDX-License-Identifier: MIT
// FR-H-02/03/04: humidity control and its interaction with temperature.
#include <doctest/doctest.h>

#include "fixture.h"

using namespace meatpilot::test;

TEST_CASE("humidity below the band starts the humidifier") {
    Rig r;
    r.settle();
    r.setHumidity(70.0F);
    const auto out = r.step();
    CHECK(out.on(Actuator::Humidify));
    CHECK_FALSE(out.on(Actuator::Dehumidify));
}

TEST_CASE("cooling makes the dehumidifier redundant") {
    // FR-H-04: cooling already dries the air, so running a dehumidifier as
    // well wastes energy and overshoots.
    Rig r;
    r.settle();
    r.setTemp(20.0F);     // cooling demand
    r.setHumidity(80.0F); // would otherwise call for dehumidification

    const auto out = r.step();
    CHECK(out.on(Actuator::Cool));
    CHECK_FALSE(out.on(Actuator::Dehumidify));
    CHECK(out.decision(Actuator::Dehumidify).reason == Reason::RedundantWithClimate);
}

TEST_CASE("the humidifier may compensate while cooling runs") {
    Rig r;
    r.settle();
    r.setTemp(20.0F);
    r.setHumidity(70.0F);

    const auto out = r.step();
    CHECK(out.on(Actuator::Cool));
    CHECK(out.on(Actuator::Humidify));
    CHECK(out.decision(Actuator::Humidify).reason == Reason::HumidityCompensation);
}

TEST_CASE("humidity actuators cannot flip inside the anti-oscillation window") {
    Rig r;
    r.settle();
    r.setHumidity(70.0F);
    REQUIRE(r.step().on(Actuator::Humidify));

    r.setHumidity(80.0F);   // swing hard the other way
    r.run(60.0);
    CHECK_FALSE(r.engine.state(Actuator::Humidify));   // stops freely

    const auto out = r.step();
    CHECK_FALSE(out.on(Actuator::Dehumidify));         // but cannot start yet
    CHECK(out.decision(Actuator::Dehumidify).blocked_by == Blocker::AntiOscillation);

    r.run(r.cfg.humidity_anti_oscillation);
    CHECK(r.engine.state(Actuator::Dehumidify));
}

TEST_CASE("an absent actuator is reported, not silently ignored") {
    Rig r;
    r.cfg.actuators[index(Actuator::Humidify)].present = false;
    r.settle();
    r.setHumidity(60.0F);

    const auto out = r.step();
    CHECK_FALSE(out.on(Actuator::Humidify));
    CHECK(out.decision(Actuator::Humidify).reason == Reason::NotConfigured);
}
