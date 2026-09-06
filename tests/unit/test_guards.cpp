// SPDX-License-Identifier: MIT
// FR-T-04 and spec 13.1: compressor protection, including across a restart.
#include <doctest/doctest.h>

#include "fixture.h"

using namespace meatpilot::test;

TEST_CASE("nothing starts during the power-up lockout") {
    Rig r;
    // Isolate the lockout from the cold-boot min-OFF, exercised separately.
    r.cfg.actuators[index(Actuator::Cool)].min_off = 0.0;
    r.setTemp(20.0F);  // strong demand for cooling from the first tick

    r.run(r.cfg.startup_lockout - 10.0, 1.0);
    const auto blocked = r.engine.state(Actuator::Cool);
    CHECK_FALSE(blocked);

    // Past both the lockout and the stabilisation window, cooling may start.
    r.run(r.cfg.stabilisation_period + 30.0, 1.0);
    CHECK(r.engine.state(Actuator::Cool));
}

TEST_CASE("each boot gate reports its own remaining time") {
    // DP-05: a refusal without a countdown is not an explanation.
    Rig r;
    r.setTemp(20.0F);

    r.step(1.0);  // sensors first read valid here, starting the window
    const auto during_stabilisation = r.step(20.0);
    CHECK(during_stabilisation.decision(Actuator::Cool).blocked_by ==
          Blocker::Stabilising);
    CHECK(during_stabilisation.decision(Actuator::Cool).blocked_for_s > 0);

    // Past stabilisation (60 s) but still inside the lockout (120 s).
    r.run(50.0, 5.0);
    const auto during_lockout = r.step(0.0);
    CHECK(during_lockout.decision(Actuator::Cool).blocked_by ==
          Blocker::StartupLockout);
    CHECK(during_lockout.decision(Actuator::Cool).blocked_for_s > 0);
}

TEST_CASE("minimum OFF time is honoured before a restart") {
    Rig r;
    r.settle();

    r.setTemp(20.0F);
    REQUIRE(r.step().on(Actuator::Cool));

    // Satisfy min-ON, then let it stop.
    r.run(4 * 60.0);
    r.setTemp(12.0F);
    r.run(60.0);
    REQUIRE_FALSE(r.engine.state(Actuator::Cool));

    // Demand cooling again immediately: refused, with the timer reported.
    r.setTemp(20.0F);
    const auto out = r.step();
    CHECK_FALSE(out.on(Actuator::Cool));
    CHECK(out.decision(Actuator::Cool).blocked_by == Blocker::MinOffTimer);
    CHECK(out.decision(Actuator::Cool).blocked_for_s > 5 * 60);

    // Once 7 minutes have passed it starts.
    r.run(7 * 60.0);
    CHECK(r.engine.state(Actuator::Cool));
}

TEST_CASE("minimum ON time is honoured before stopping") {
    Rig r;
    r.settle();
    r.setTemp(20.0F);
    REQUIRE(r.step().on(Actuator::Cool));

    r.setTemp(10.0F);  // demand disappears immediately
    const auto out = r.step();
    CHECK(out.on(Actuator::Cool));
    CHECK(out.decision(Actuator::Cool).blocked_by == Blocker::MinOnTimer);

    r.run(3 * 60.0 + 10.0);
    CHECK_FALSE(r.engine.state(Actuator::Cool));
}

TEST_CASE("a safety inhibition overrides minimum ON time") {
    // Stopping a compressor early may shorten its life; ignoring an absolute
    // limit risks the product and the equipment. Safety wins.
    Rig r;
    r.settle();
    r.setTemp(12.0F);
    r.cfg.temp_abs_min = 11.0F;
    r.setTemp(12.1F);
    r.cfg.target_temp = 5.0F;   // force a cooling demand
    REQUIRE(r.step().on(Actuator::Cool));

    r.setTemp(10.5F);           // now below the absolute minimum
    const auto out = r.step();  // well inside min-ON
    CHECK_FALSE(out.on(Actuator::Cool));
    CHECK(out.decision(Actuator::Cool).reason == Reason::AbsoluteLimitLow);
}

TEST_CASE("cold boot with no valid clock costs a full minimum-OFF") {
    // Spec 13.1 requires the lockout to survive a restart. With no RTC the
    // controller cannot know how long the compressor has been off, so it must
    // assume the worst. See docs/ARCHITECTURE.md section 3.4.
    Rig r;
    r.engine.restore(Actuator::Cool, /*on=*/false, /*since=*/0.0);
    r.setTemp(20.0F);

    r.run(r.cfg.startup_lockout + r.cfg.stabilisation_period + 30.0, 1.0);
    CHECK_FALSE(r.engine.state(Actuator::Cool));  // min-OFF, not just lockout

    r.run(7 * 60.0, 1.0);
    CHECK(r.engine.state(Actuator::Cool));
}

TEST_CASE("a valid clock across the restart credits elapsed OFF time") {
    Rig r;
    // The journal says the compressor stopped 10 minutes before boot.
    r.engine.restore(Actuator::Cool, /*on=*/false, /*since=*/-600.0);
    r.setTemp(20.0F);

    r.run(r.cfg.startup_lockout + r.cfg.stabilisation_period + 30.0, 1.0);
    CHECK(r.engine.state(Actuator::Cool));  // min-OFF already satisfied
}
