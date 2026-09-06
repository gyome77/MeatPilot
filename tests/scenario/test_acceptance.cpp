// SPDX-License-Identifier: MIT
//
// One test per row of the specification's section 15 acceptance table that can
// be decided at the control-engine level. Rows needing sensor conditioning,
// storage or networking are named here and marked for the milestone that
// implements them, so the table stays visibly complete.
#include <doctest/doctest.h>

#include "fixture.h"

using namespace meatpilot::test;

TEST_CASE("15: sensor disconnection produces a safe output state") {
    Rig r;
    r.settle();
    r.setTemp(20.0F);
    r.setHumidity(60.0F);
    r.run(60.0);
    REQUIRE(r.engine.state(Actuator::Cool));

    // Both probes removed.
    r.setTemp(0.0F, false);
    r.setHumidity(0.0F, false);
    const auto out = r.step();

    CHECK_FALSE(out.on(Actuator::Cool));
    CHECK_FALSE(out.on(Actuator::Heat));
    CHECK_FALSE(out.on(Actuator::Humidify));
    CHECK_FALSE(out.on(Actuator::Dehumidify));
    CHECK(out.decision(Actuator::Cool).reason == Reason::SensorInvalid);
}

TEST_CASE("15: no compressor start occurs before the minimum OFF time") {
    Rig r;
    r.settle();

    r.setTemp(20.0F);
    REQUIRE(r.step().on(Actuator::Cool));
    r.run(4 * 60.0);
    r.setTemp(10.0F);
    r.run(30.0);
    REQUIRE_FALSE(r.engine.state(Actuator::Cool));

    // Take the stop time from the engine, not from the loop granularity.
    const Seconds stopped_at = r.engine.lastChange(Actuator::Cool);
    r.setTemp(20.0F);
    // Guard on the time the tick will land at, so the last assertion is still
    // strictly inside the window rather than one step past it.
    constexpr Seconds kMinOff = 7 * 60.0;
    while (r.now + 10.0 < stopped_at + kMinOff) {
        CHECK_FALSE(r.step(10.0).on(Actuator::Cool));
    }
    CHECK(r.now < stopped_at + kMinOff);  // the window really was exercised

    r.run(30.0);
    CHECK(r.engine.state(Actuator::Cool));
}

TEST_CASE("15: no command violates the minimum ON time") {
    Rig r;
    r.settle();
    r.setTemp(20.0F);
    REQUIRE(r.step().on(Actuator::Cool));

    const Seconds started_at = r.now;
    r.setTemp(5.0F);  // demand vanishes at once
    while (r.now - started_at < 3 * 60.0 - 10.0) {
        CHECK(r.step(10.0).on(Actuator::Cool));
    }
}

TEST_CASE("15: mutual exclusion holds through a restart") {
    // The engine comes up with outputs off regardless of what the journal
    // says, so no restart can produce an overlapping pair. The remaining risk
    // is the GPIO level during the first milliseconds after reset, which is a
    // board-level requirement -- see docs/ARCHITECTURE.md section 3.5.
    Rig r;
    r.settle();
    r.setTemp(20.0F);
    r.run(60.0);
    REQUIRE(r.engine.state(Actuator::Cool));

    Engine rebooted{r.now};
    rebooted.restore(Actuator::Cool, true, r.now - 60.0);  // journal says ON
    rebooted.restore(Actuator::Heat, true, r.now - 60.0);  // and so does this

    Inputs in = r.in;
    const auto out = rebooted.tick(in, r.cfg, Mode::Auto, r.now + 1.0);
    CHECK_FALSE((out.on(Actuator::Cool) && out.on(Actuator::Heat)));
}

TEST_CASE("15: 100 power cycles produce no unsafe output pulse") {
    Rig warm;
    warm.settle();
    warm.setTemp(25.0F);   // maximum demand throughout
    warm.setHumidity(50.0F);
    warm.run(120.0);

    Seconds clock = warm.now;
    for (int cycle = 0; cycle < 100; ++cycle) {
        Engine e{clock};
        // Worst case: no valid clock across the cut, so nothing is credited.
        for (Actuator a : kAllActuators) e.restore(a, false, clock);

        // The very first tick after boot must command nothing.
        const auto first = e.tick(warm.in, warm.cfg, Mode::Auto, clock);
        for (Actuator a : kAllActuators) {
            REQUIRE_FALSE(first.on(a));
        }

        // And nothing may start before the power-up lockout expires.
        Seconds t = clock;
        while (t + 5.0 < clock + warm.cfg.startup_lockout) {
            t += 5.0;
            const auto out = e.tick(warm.in, warm.cfg, Mode::Auto, t);
            for (Actuator a : kAllActuators) {
                REQUIRE_FALSE(out.on(a));
            }
        }
        clock = t + 30.0;  // next power cut
    }
}

TEST_CASE("15: control continues with no network") {
    // The engine has no network dependency to remove -- it takes no clock, no
    // socket and no broker. This test exists to assert that structurally: a
    // full regulation cycle runs with nothing but injected values.
    Rig r;
    r.settle();
    r.setTemp(20.0F);
    CHECK(r.step().on(Actuator::Cool));
    r.run(24 * 3600.0, 60.0);
    CHECK(r.engine.state(Actuator::Cool) == true);
}

// Rows deferred to later milestones, listed so the table stays complete:
//
//   Frozen sensor          -> M1, core/sensor: freeze detection sets valid=false,
//                             which this suite already proves is handled.
//   Backup sensor divergence -> M1, core/sensor.
//   Weight calculation     -> M1, core/product.
//   Multi-product tracking -> M1, core/product + persistence.
//   Alarm delivery         -> M4, net/.
//   OTA rollback           -> M3, hardware.
TEST_CASE("15: deferred rows are tracked" * doctest::skip()) {}
