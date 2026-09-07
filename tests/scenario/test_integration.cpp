// SPDX-License-Identifier: MIT
//
// The whole core wired together: programme engine steering the control engine,
// products driving phase completion, alarms watching all of it, against a
// simulated chamber.
//
// The wiring lives here rather than in a core/ facade because app/ owns it on
// the target -- these tests are how that wiring is specified.
#include <doctest/doctest.h>

#include <cmath>

#include "meatpilot/alarm/manager.h"
#include "meatpilot/control/engine.h"
#include "meatpilot/control/profiles.h"
#include "meatpilot/product/registry.h"
#include "meatpilot/program/presets.h"
#include "meatpilot/program/runner.h"
#include "meatpilot/sensor/conditioner.h"
#include "meatpilot/sim/chamber.h"

using namespace meatpilot;

namespace {

constexpr Seconds kHour = 3600.0;
constexpr Seconds kDay = 24 * kHour;

/// A complete controller: sensors, programme, products, engine, alarms.
struct System {
    sim::Chamber::Params sim_params{};
    sim::Chamber         chamber{sim_params, 20.0F, 55.0F};
    control::Engine      engine{0.0};
    control::RegulationConfig cfg{};
    program::Runner      runner{};
    product::Registry    products{};
    alarm::Manager       alarms{};
    sensor::Conditioner  temp_probe;
    sensor::Conditioner  hum_probe;

    control::Outputs last{};
    alarm::Event     events[32]{};
    int              raised[alarm::kKeyCount]{};
    Seconds          now{0.0};

    // Extremes over a window, so a test can distinguish "never got there" from
    // "got there and is cycling around it".
    float min_temp{1e9F};
    float max_temp{-1e9F};
    Seconds cool_on_time{0.0};
    void resetTrace() {
        min_temp = 1e9F;
        max_temp = -1e9F;
        cool_on_time = 0.0;
    }

    static sensor::Conditioner::Config probe(float lo, float hi) {
        sensor::Conditioner::Config c;
        c.min_valid = lo;
        c.max_valid = hi;
        c.median_window = 5;
        c.lowpass_alpha = 0.4F;
        return c;
    }

    /// Parameters for a wine cooler rather than a chest freezer.
    ///
    /// The default sim cool_rate of -0.010 degC/s is 36 K/hour, which is a
    /// strong compressor fridge. A wine cooler in a small chamber manages
    /// closer to -0.003. The distinction matters more than it looks: a
    /// minimum-ON time sets a floor on overshoot of (cool_rate x min_on), so
    /// with the 5-minute min-ON that whole-appliance switching requires, a
    /// -0.010 unit overshoots by 3 K and cannot hold a 0.8 K deadband at all.
    /// See docs/ARCHITECTURE.md 3.4a.
    static sim::Chamber::Params wineCooler() {
        sim::Chamber::Params p{};
        p.cool_rate = -0.0035F;
        p.heat_rate = 0.004F;
        return p;
    }

    System() : temp_probe(probe(-20.0F, 60.0F)), hum_probe(probe(0.0F, 100.0F)) {
        chamber = sim::Chamber{wineCooler(), 20.0F, 55.0F};
        for (control::Actuator a : control::kAllActuators) {
            cfg.actuators[control::index(a)].present = true;
        }
        // The chamber is a wine fridge switched whole at the mains.
        control::applyCoolingProfile(cfg, control::CoolingKind::ApplianceWithThermostat);
        cfg.stabilisation_period = 60.0;
        cfg.temp_abs_min = 2.0F;
        cfg.temp_abs_max = 28.0F;
        cfg.humidity_abs_min = 40.0F;
        cfg.humidity_abs_max = 98.0F;
    }

    void tick(Seconds dt) {
        now += dt;

        // 1. Acquisition.
        control::Inputs in{};
        in.temperature = temp_probe.update(true, chamber.readTemperature(), now);
        in.humidity = hum_probe.update(true, chamber.readHumidity(), now);

        // 2. The programme decides the setpoints for this moment.
        const product::Progress progress = products.progress(now);
        runner.tick(now, progress);
        runner.applyTo(cfg);

        // 3. Regulate.
        last = engine.tick(in, cfg, control::Mode::Programme, now);

        // 4. Watch.
        alarm::Signals sig{};
        sig.inputs = in;
        sig.outputs = last;
        sig.progress = progress;
        sig.drying_phase = runner.dryingPhase();
        const product::ProductId ref = products.referenceId();
        if (ref != product::kNoProduct) {
            const product::Stats s = products.stats(ref, now);
            sig.has_drying_rate = s.has_rate;
            sig.drying_rate_pct_per_day = s.rate_pct_per_day;
        }
        const std::size_t n = alarms.evaluate(sig, cfg, now, events, 32);
        for (std::size_t i = 0; i < n; ++i) {
            if (events[i].kind == alarm::EventKind::Raised) {
                ++raised[static_cast<std::size_t>(events[i].key)];
            }
        }

        // 5. Apply.
        chamber.step(last.commands, false, dt);

        if (chamber.temperature() < min_temp) min_temp = chamber.temperature();
        if (chamber.temperature() > max_temp) max_temp = chamber.temperature();
        if (last.on(control::Actuator::Cool)) cool_on_time += dt;
    }

    void run(Seconds duration, Seconds dt = 30.0) {
        const Seconds end = now + duration;
        while (now < end) tick(dt);
    }

    int raisedCount(alarm::Key k) const {
        return raised[static_cast<std::size_t>(k)];
    }
};

}  // namespace

TEST_CASE("AL-03 distinguishes a dead actuator from a slow one") {
    // The pair of runs the simulator exists for. An alarm that fires on both
    // is useless, and one that fires on neither is worse.

    SUBCASE("dead cooling raises it") {
        System s;
        s.chamber.faults().cooling_dead = true;
        program::Programme p{};
        program::load(program::Preset::CellarHold, p);
        REQUIRE(s.runner.start(p, 0.0) == program::Validation::Ok);

        s.run(6 * kHour, 60.0);
        CHECK(s.raisedCount(alarm::Key::NoResponseCooling) > 0);
        CHECK(s.chamber.temperature() > 18.0F);
    }

    SUBCASE("cooling that is merely slow does not") {
        System s;
        s.chamber.params().cool_rate = -0.0025F;  // a quarter of the power
        program::Programme p{};
        program::load(program::Preset::CellarHold, p);
        REQUIRE(s.runner.start(p, 0.0) == program::Validation::Ok);

        s.run(6 * kHour, 60.0);
        CHECK(s.raisedCount(alarm::Key::NoResponseCooling) == 0);
    }

    SUBCASE("healthy cooling does not") {
        System s;
        program::Programme p{};
        program::load(program::Preset::CellarHold, p);
        REQUIRE(s.runner.start(p, 0.0) == program::Validation::Ok);

        s.run(6 * kHour, 60.0);
        CHECK(s.raisedCount(alarm::Key::NoResponseCooling) == 0);
    }
}

TEST_CASE("a coppa runs its recipe from rest to target weight loss") {
    System s;
    program::Programme p{};
    program::load(program::Preset::Coppa, p);
    REQUIRE(s.runner.start(p, 0.0) == program::Validation::Ok);

    const product::ProductId coppa =
        s.products.create("Coppa", "2026-09", 1200.0F, 35.0F, 0.0);
    REQUIRE(coppa != product::kNoProduct);

    // Phase 1: the warm rest, 24 h at 22 C.
    s.run(12 * kHour, 60.0);
    CHECK(s.runner.phaseIndex() == 0);
    CHECK_FALSE(s.runner.dryingPhase());
    CHECK(s.chamber.temperature() > 19.0F);  // deliberately warm

    // Phase 2 begins: the long cool dry.
    s.run(14 * kHour, 60.0);
    CHECK(s.runner.phaseIndex() == 1);
    CHECK(s.runner.dryingPhase());

    // Cooling is a whole appliance on a 10-minute minimum-OFF, so the chamber
    // cycles around setpoint on a long period rather than sitting on it.
    // Assert that it genuinely reaches the band, and record the swing.
    s.resetTrace();
    s.run(2 * kDay, 120.0);
    CAPTURE(s.min_temp);
    CAPTURE(s.max_temp);
    CAPTURE(s.cool_on_time / (2 * kDay));
    CHECK(s.min_temp <= 13.8F);          // reaches the band
    CHECK(s.max_temp < 16.0F);           // and never drifts far above it

    // Weigh-ins over the cure. Drying is not simulated, so weights are
    // supplied: this test is about the recipe reacting to them.
    for (int day = 3; day <= 40; ++day) {
        const float loss = 0.9F * static_cast<float>(day);  // %/day
        const float w = 1200.0F * (1.0F - loss / 100.0F);
        s.products.recordWeighIn(coppa, w, static_cast<Seconds>(day) * kDay);
    }
    s.run(2 * kDay, 300.0);

    // 35 % reached around day 39, so the programme should have completed.
    CHECK(s.runner.status() == program::RunStatus::Completed);
    const product::Stats st = s.products.stats(coppa, s.now);
    CHECK(st.loss_pct >= 35.0F);
    CHECK(st.has_rate);
    CHECK(st.rate_pct_per_day == doctest::Approx(0.9F).epsilon(0.05));
}

TEST_CASE("the warm rest phase does not trip the drying-temperature alarm") {
    // AL-10 exists precisely because a fixed limit cannot tell a 22 C
    // fermentation from a 22 C drying failure. The programme supplies the
    // context that makes the distinction possible.
    System s;
    program::Programme p{};
    program::load(program::Preset::SaucissonSec, p);
    REQUIRE(s.runner.start(p, 0.0) == program::Validation::Ok);

    s.run(20 * kHour, 60.0);
    REQUIRE(s.runner.phaseIndex() == 0);
    REQUIRE(s.chamber.temperature() > 19.0F);
    CHECK(s.raisedCount(alarm::Key::HighTempDrying) == 0);
}

TEST_CASE("case hardening is caught from the weigh-in curve alone") {
    // The chamber can be holding both setpoints perfectly and still be ruining
    // the product. Nothing in the environmental readings shows this.
    System s;
    program::Programme p{};
    program::load(program::Preset::Coppa, p);
    REQUIRE(s.runner.start(p, 0.0) == program::Validation::Ok);

    const product::ProductId id =
        s.products.create("Coppa", "", 1000.0F, 35.0F, 0.0);
    s.run(26 * kHour, 120.0);
    REQUIRE(s.runner.dryingPhase());

    // Losing 2.5 %/day: far too fast, and the surface will crust.
    for (int day = 2; day <= 6; ++day) {
        const float w = 1000.0F * (1.0F - 0.025F * static_cast<float>(day));
        s.products.recordWeighIn(id, w, static_cast<Seconds>(day) * kDay);
    }
    s.run(2 * kDay, 300.0);

    CHECK(s.raisedCount(alarm::Key::CaseHardening) > 0);
}

TEST_CASE("a run survives a restart with its journal") {
    // Spec DP-04 and §13.1: configuration and progress outlive a power cut,
    // and outputs come back off.
    System s;
    program::Programme p{};
    program::load(program::Preset::Coppa, p);
    REQUIRE(s.runner.start(p, 0.0) == program::Validation::Ok);
    const product::ProductId id =
        s.products.create("Coppa", "", 1000.0F, 35.0F, 0.0);
    s.products.recordWeighIn(id, 950.0F, kDay);

    s.run(30 * kHour, 120.0);
    const std::size_t phase_before = s.runner.phaseIndex();
    const float loss_before = s.products.stats(id, s.now).loss_pct;

    // Power cut: the engine is rebuilt from nothing, everything else is
    // restored from the journal.
    control::Engine rebooted{s.now};
    for (control::Actuator a : control::kAllActuators) {
        rebooted.restore(a, false, s.now);
    }
    control::Inputs in{};
    in.temperature = {s.chamber.temperature(), s.chamber.temperature(), true,
                      Quality::Ok};
    in.humidity = {s.chamber.humidity(), s.chamber.humidity(), true, Quality::Ok};
    const control::Outputs first =
        rebooted.tick(in, s.cfg, control::Mode::Programme, s.now + 1.0);

    for (control::Actuator a : control::kAllActuators) {
        CHECK_FALSE(first.on(a));  // no unsafe pulse
    }
    CHECK(s.runner.phaseIndex() == phase_before);
    CHECK(s.products.stats(id, s.now).loss_pct == doctest::Approx(loss_before));
}
