// SPDX-License-Identifier: MIT
// FR-V-*, FR-A-*: circulation and fresh-air exchange.
#include <doctest/doctest.h>

#include "fixture.h"

using namespace meatpilot::test;

TEST_CASE("circulation follows its interval schedule") {
    Rig r;
    r.cfg.circulation.period = 1800.0;  // 5 min every 30 min
    r.cfg.circulation.run = 300.0;
    r.settle();

    // Land inside a run window.
    r.now = 3600.0;
    CHECK(r.step(0.0).on(Actuator::Circulate));

    // And inside a rest window.
    r.now = 3600.0 + 600.0;
    CHECK_FALSE(r.step(0.0).on(Actuator::Circulate));
}

TEST_CASE("continuous circulation ignores the interval") {
    Rig r;
    r.cfg.circulation.continuous = true;
    r.settle();
    CHECK(r.step().on(Actuator::Circulate));
}

TEST_CASE("an open door pauses circulation and fresh air") {
    Rig r;
    r.cfg.circulation.continuous = true;
    r.cfg.fresh_air.continuous = true;
    r.settle();
    REQUIRE(r.step().on(Actuator::Circulate));

    r.in.door_open = true;
    const auto out = r.step();
    CHECK_FALSE(out.on(Actuator::Circulate));
    CHECK_FALSE(out.on(Actuator::FreshAir));
    CHECK(out.decision(Actuator::Circulate).reason == Reason::DoorOpen);
}

TEST_CASE("closing the door triggers a post-door circulation cycle") {
    Rig r;
    r.cfg.post_door_circulation = 300.0;
    r.settle();
    r.in.door_open = true;
    r.step();

    r.in.door_open = false;
    const auto out = r.step();
    CHECK(out.on(Actuator::Circulate));
    CHECK(out.decision(Actuator::Circulate).reason == Reason::DoorOpen);

    r.run(360.0);
    CHECK_FALSE(r.engine.state(Actuator::Circulate));
}

TEST_CASE("fresh air is withheld while an absolute limit is breached") {
    // FR-A-03: drawing outside air would deepen the excursion.
    Rig r;
    r.cfg.fresh_air.continuous = true;
    r.cfg.temp_abs_max = 20.0F;
    r.settle();
    REQUIRE(r.step().on(Actuator::FreshAir));

    r.setTemp(21.0F);
    CHECK_FALSE(r.step().on(Actuator::FreshAir));
}

TEST_CASE("CO2 above threshold demands fresh air outside the schedule") {
    Rig r;
    r.cfg.co2_demand_enabled = true;
    r.cfg.co2_threshold = 1500.0F;
    r.settle();
    CHECK_FALSE(r.step().on(Actuator::FreshAir));

    r.in.has_co2 = true;
    r.in.co2 = 1800.0F;
    const auto out = r.step();
    CHECK(out.on(Actuator::FreshAir));
    CHECK(out.decision(Actuator::FreshAir).reason == Reason::Co2Demand);
}
