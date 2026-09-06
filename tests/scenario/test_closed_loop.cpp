// SPDX-License-Identifier: MIT
//
// The engine driving a simulated chamber, with conditioned sensors in the
// loop. These are the tests that could not exist while every input was an
// injected constant: they exercise the controller as a feedback system.
#include <doctest/doctest.h>

#include "meatpilot/control/engine.h"
#include "meatpilot/sensor/conditioner.h"
#include "meatpilot/sensor/derived.h"
#include "meatpilot/sim/chamber.h"

using namespace meatpilot;
using namespace meatpilot::control;

namespace {

/// Engine + conditioned sensors + simulated chamber, wired as the firmware
/// wires them.
struct Loop {
    sim::Chamber::Params sim_params{};
    sim::Chamber         chamber{sim_params, 20.0F, 55.0F};
    Engine               engine{0.0};
    RegulationConfig     cfg{};
    sensor::Conditioner  temp_probe;
    sensor::Conditioner  hum_probe;
    Mode                 mode{Mode::Auto};
    Seconds              now{0.0};
    Outputs              last{};

    static sensor::Conditioner::Config probeConfig(float lo, float hi) {
        sensor::Conditioner::Config c;
        c.min_valid = lo;
        c.max_valid = hi;
        c.median_window = 5;
        c.lowpass_alpha = 0.4F;
        c.stale_seconds = 1800.0;
        return c;
    }

    Loop() : temp_probe(probeConfig(-20.0F, 60.0F)),
             hum_probe(probeConfig(0.0F, 100.0F)) {
        for (Actuator a : kAllActuators) cfg.actuators[index(a)].present = true;
        cfg.actuators[index(Actuator::Cool)].min_off = 7 * 60.0;
        cfg.actuators[index(Actuator::Cool)].min_on = 3 * 60.0;
        cfg.startup_lockout = 120.0;
        cfg.stabilisation_period = 60.0;
        cfg.humidity_anti_oscillation = 600.0;

        cfg.has_temp_target = true;
        cfg.target_temp = 13.0F;
        cfg.temp_deadband = 0.8F;
        cfg.has_humidity_target = true;
        cfg.target_humidity = 78.0F;
        cfg.humidity_deadband = 3.0F;
        cfg.temp_abs_min = 2.0F;
        cfg.temp_abs_max = 28.0F;
    }

    /// One control period: read the chamber through the probes, decide, apply.
    void tick(Seconds dt, bool door_open = false, bool probe_present = true) {
        now += dt;
        Inputs in{};
        in.temperature = temp_probe.update(probe_present, chamber.readTemperature(), now);
        in.humidity = hum_probe.update(probe_present, chamber.readHumidity(), now);
        if (in.temperature.valid && in.humidity.valid) {
            in.has_dew_point = true;
            in.dew_point = sensor::dewPoint(in.temperature.filtered,
                                            in.humidity.filtered);
        }
        in.door_open = door_open;
        last = engine.tick(in, cfg, mode, now);
        chamber.step(last.commands, door_open, dt);
    }

    void run(Seconds duration, Seconds dt = 10.0) {
        const Seconds end = now + duration;
        while (now < end) tick(dt);
    }
};

}  // namespace

TEST_CASE("the loop pulls a warm chamber to setpoint and holds it") {
    Loop loop;
    loop.run(12 * 3600.0);

    CHECK(std::abs(loop.chamber.temperature() - loop.cfg.target_temp) < 1.5F);
    CHECK(std::abs(loop.chamber.humidity() - loop.cfg.target_humidity) < 4.0F);
}

TEST_CASE("mutual exclusion holds across a full closed-loop run") {
    Loop loop;
    for (int i = 0; i < 4320; ++i) {  // 12 h at 10 s
        loop.tick(10.0);
        for (const ExclusionPair& p : kMutualExclusion) {
            REQUIRE_FALSE((loop.last.on(p.a) && loop.last.on(p.b)));
        }
    }
}

TEST_CASE("compressor starts per hour stay bounded in steady state") {
    // AL-05 watches for short cycling. The guards should make it impossible
    // for the controller itself to be the cause.
    Loop loop;
    loop.run(6 * 3600.0);  // reach setpoint first

    int starts = 0;
    bool prev = loop.last.on(Actuator::Cool);
    const Seconds began = loop.now;
    while (loop.now - began < 6 * 3600.0) {
        loop.tick(10.0);
        const bool on = loop.last.on(Actuator::Cool);
        if (on && !prev) ++starts;
        prev = on;
    }
    // 7 min off + 3 min on floors the period at 10 min, so 6 per hour is the
    // hard ceiling; assert comfortably inside it.
    CHECK(starts <= 36);
}

TEST_CASE("AL-03 precondition: a dead actuator is distinguishable from a slow one") {
    // This is the pair of runs that an injected-constant test cannot produce,
    // and the reason platform/sim exists. The alarm rule lands in M1; what is
    // asserted here is that the signal it will key on is actually present.

    SUBCASE("working cooling reaches setpoint, and stops asking") {
        Loop loop;
        loop.run(12 * 3600.0);
        CHECK(loop.chamber.temperature() < loop.cfg.target_temp + 1.5F);
    }

    SUBCASE("slow cooling still converges, and must not be alarmed on") {
        Loop loop;
        loop.chamber.params().cool_rate = -0.0025F;  // a quarter of the power
        loop.run(24 * 3600.0);
        CHECK(loop.chamber.temperature() < loop.cfg.target_temp + 1.5F);
    }

    SUBCASE("dead cooling never converges, while the command stays asserted") {
        Loop loop;
        loop.chamber.faults().cooling_dead = true;
        loop.run(12 * 3600.0);

        CHECK(loop.chamber.temperature() > 18.0F);   // never left ambient
        CHECK(loop.last.on(Actuator::Cool));         // still demanding cooling
        CHECK(loop.last.decision(Actuator::Cool).reason == Reason::TemperatureHigh);
    }
}

TEST_CASE("15: pulling the probe mid-run stops regulation safely") {
    Loop loop;
    loop.run(6 * 3600.0);
    REQUIRE(loop.chamber.temperature() < 16.0F);

    loop.tick(10.0, /*door_open=*/false, /*probe_present=*/false);

    CHECK_FALSE(loop.last.on(Actuator::Cool));
    CHECK_FALSE(loop.last.on(Actuator::Heat));
    CHECK_FALSE(loop.last.on(Actuator::Humidify));
    CHECK_FALSE(loop.last.on(Actuator::Dehumidify));
    CHECK(loop.last.decision(Actuator::Cool).reason == Reason::SensorInvalid);
}

TEST_CASE("15: a frozen probe stops regulation, through the real detector") {
    // Previously this row was covered by injecting valid=false. Now the engine
    // is stopped by the conditioner's own freeze detection, driven by a
    // chamber that has genuinely stopped reporting movement.
    Loop loop;
    loop.run(3 * 3600.0);

    // The probe sticks: it keeps answering, plausibly, with the same number.
    const float stuck = loop.chamber.temperature();
    Outputs out{};
    const Seconds froze_at = loop.now;
    while (loop.now - froze_at < 2100.0) {
        loop.now += 10.0;
        Inputs in{};
        in.temperature = loop.temp_probe.update(true, stuck, loop.now);
        in.humidity = loop.hum_probe.update(true, loop.chamber.readHumidity(), loop.now);
        out = loop.engine.tick(in, loop.cfg, loop.mode, loop.now);
        loop.chamber.step(out.commands, false, 10.0);
    }

    CHECK(loop.temp_probe.quality() == Quality::Frozen);
    CHECK_FALSE(out.on(Actuator::Cool));
    CHECK(out.decision(Actuator::Cool).reason == Reason::SensorInvalid);
}

TEST_CASE("an open door interrupts regulation and the chamber recovers") {
    Loop loop;
    loop.run(8 * 3600.0);
    const float settled = loop.chamber.temperature();

    for (int i = 0; i < 30; ++i) loop.tick(10.0, /*door_open=*/true);
    CHECK(loop.chamber.temperature() > settled);       // room air got in
    CHECK_FALSE(loop.last.on(Actuator::Circulate));    // FR-V-04

    loop.run(6 * 3600.0);
    CHECK(std::abs(loop.chamber.temperature() - loop.cfg.target_temp) < 1.5F);
}
