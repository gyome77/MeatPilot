// SPDX-License-Identifier: MIT
// FR-T-02, FR-T-05: setpoint, deadband and absolute limits.
#include <doctest/doctest.h>

#include "fixture.h"

using namespace meatpilot::test;

TEST_CASE("cooling starts above the band and stops on return to target") {
    Rig r;
    r.settle();

    r.setTemp(13.5F);  // inside the band (13.0 +/- 0.8)
    CHECK_FALSE(r.step().on(Actuator::Cool));

    r.setTemp(13.9F);  // above target + deadband
    CHECK(r.step().on(Actuator::Cool));

    r.setTemp(13.4F);  // still above target: hold, do not chatter
    r.run(4 * 60.0);
    CHECK(r.engine.state(Actuator::Cool));

    r.setTemp(12.9F);  // back at target: stop
    CHECK_FALSE(r.step().on(Actuator::Cool));
}

TEST_CASE("heating starts below the band and stops on return to target") {
    Rig r;
    r.settle();

    r.setTemp(12.5F);
    CHECK_FALSE(r.step().on(Actuator::Heat));

    r.setTemp(12.1F);
    CHECK(r.step().on(Actuator::Heat));

    r.setTemp(13.1F);
    CHECK_FALSE(r.step().on(Actuator::Heat));
}

TEST_CASE("an absolute limit inhibits the aggravating actuator only") {
    Rig r;
    r.settle();
    r.cfg.temp_abs_max = 20.0F;

    // Hot enough to want cooling, and past the absolute maximum.
    r.setTemp(21.0F);
    const auto out = r.step();

    CHECK(out.on(Actuator::Cool));                    // corrective: allowed
    CHECK_FALSE(out.on(Actuator::Heat));              // aggravating: inhibited
    CHECK(out.decision(Actuator::Heat).reason == Reason::AbsoluteLimitHigh);
}

TEST_CASE("an invalid probe stops the actuators that act on it") {
    Rig r;
    r.settle();
    r.setTemp(14.0F);
    REQUIRE(r.step().on(Actuator::Cool));

    r.setTemp(14.0F, /*valid=*/false);
    const auto out = r.step();

    CHECK_FALSE(out.on(Actuator::Cool));
    CHECK(out.decision(Actuator::Cool).reason == Reason::SensorInvalid);
    // Circulation is harmless and keeps running while a probe is replaced.
    CHECK(out.decision(Actuator::Circulate).reason != Reason::SensorInvalid);
}

TEST_CASE("no target means no regulation, not a default setpoint") {
    Rig r;
    r.settle();
    r.cfg.has_temp_target = false;
    r.setTemp(25.0F);

    const auto out = r.step();
    CHECK_FALSE(out.on(Actuator::Cool));
    CHECK_FALSE(out.on(Actuator::Heat));
}
