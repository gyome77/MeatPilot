// SPDX-License-Identifier: MIT
// Spec §4.1: phases, completion conditions and recipe revisions.
#include <doctest/doctest.h>

#include "meatpilot/program/presets.h"
#include "meatpilot/program/runner.h"

using namespace meatpilot;
using namespace meatpilot::program;

namespace {
constexpr Seconds kHour = 3600.0;
constexpr Seconds kDay = 24 * kHour;

Programme twoPhase() {
    Programme p{};
    p.phase_count = 2;
    p.phases[0].has_temp = true;
    p.phases[0].target_temp = 22.0F;
    p.phases[0].end = EndKind::Duration;
    p.phases[0].duration = 24 * kHour;
    p.phases[1].has_temp = true;
    p.phases[1].target_temp = 13.0F;
    p.phases[1].end = EndKind::ReferenceLoss;
    p.phases[1].max_duration = 30 * kDay;
    p.phases[1].drying = true;
    p.phases[1].has_limits = true;
    p.phases[1].temp_abs_min = 4.0F;
    p.phases[1].temp_abs_max = 20.0F;
    return p;
}

product::Progress atLoss(float pct, bool reached) {
    product::Progress g{};
    g.has_reference = true;
    g.reference_loss_pct = pct;
    g.reference_target_reached = reached;
    g.active_count = 1;
    return g;
}
}  // namespace

TEST_CASE("a weight-driven phase without a safety cap is refused") {
    // Spec §4.1 calls max_duration the fallback when weight data is
    // unavailable. Without it, a load cell coming unplugged during
    // fermentation leaves the chamber at 22 C indefinitely.
    Programme p = twoPhase();
    p.phases[1].max_duration = 0.0;

    CHECK(validate(p) == Validation::WeightPhaseWithoutCap);

    Runner r;
    CHECK(r.start(p, 0.0) == Validation::WeightPhaseWithoutCap);
    CHECK(r.status() == RunStatus::Idle);   // and it does not run
}

TEST_CASE("other invalid recipes are refused before they can execute") {
    Programme p{};
    CHECK(validate(p) == Validation::NoPhases);

    p = twoPhase();
    p.phases[0].has_temp = false;
    p.phases[0].has_humidity = false;
    CHECK(validate(p) == Validation::NoTargets);

    p = twoPhase();
    p.phases[0].duration = 0.0;
    CHECK(validate(p) == Validation::ZeroDuration);

    p = twoPhase();
    p.phases[0].temp_deadband = 0.0F;
    CHECK(validate(p) == Validation::InvalidDeadband);
}

TEST_CASE("a duration phase advances on time") {
    Runner r;
    REQUIRE(r.start(twoPhase(), 0.0) == Validation::Ok);
    const product::Progress none{};

    CHECK_FALSE(r.tick(23 * kHour, none).phase_changed);
    CHECK(r.phaseIndex() == 0);

    const TickResult t = r.tick(24 * kHour, none);
    CHECK(t.phase_changed);
    CHECK(t.advance == Advance::Duration);
    CHECK(r.phaseIndex() == 1);
}

TEST_CASE("a weight-driven phase advances on the reference product") {
    Runner r;
    REQUIRE(r.start(twoPhase(), 0.0) == Validation::Ok);
    r.tick(24 * kHour, product::Progress{});
    REQUIRE(r.phaseIndex() == 1);

    CHECK_FALSE(r.tick(10 * kDay, atLoss(20.0F, false)).phase_changed);

    const TickResult t = r.tick(25 * kDay, atLoss(35.0F, true));
    CHECK(t.programme_completed);
    CHECK(t.advance == Advance::WeightTarget);
    CHECK(r.status() == RunStatus::Completed);
}

TEST_CASE("the safety cap ends a weight phase, and says that it did") {
    // The distinction matters: finishing on the cap means the weight data
    // never arrived, which is a different event from reaching the target.
    Runner r;
    REQUIRE(r.start(twoPhase(), 0.0) == Validation::Ok);
    r.tick(24 * kHour, product::Progress{});
    REQUIRE(r.phaseIndex() == 1);

    const TickResult t = r.tick(24 * kHour + 30 * kDay, product::Progress{});
    CHECK(t.programme_completed);
    CHECK(t.advance == Advance::MaxDuration);
}

TEST_CASE("the intended condition is reported in preference to the cap") {
    Runner r;
    REQUIRE(r.start(twoPhase(), 0.0) == Validation::Ok);
    r.tick(24 * kHour, product::Progress{});

    // Both conditions true on the same tick.
    const TickResult t = r.tick(24 * kHour + 30 * kDay, atLoss(35.0F, true));
    CHECK(t.advance == Advance::WeightTarget);
}

TEST_CASE("first-of and last-of targets differ with several products") {
    Programme p = twoPhase();
    p.phases[1].end = EndKind::FirstOfTargets;

    product::Progress g{};
    g.active_count = 3;
    g.any_target_reached = true;
    g.all_targets_reached = false;

    Runner first;
    REQUIRE(first.start(p, 0.0) == Validation::Ok);
    first.tick(24 * kHour, product::Progress{});
    CHECK(first.tick(10 * kDay, g).programme_completed);

    p.phases[1].end = EndKind::LastOfTargets;
    Runner last;
    REQUIRE(last.start(p, 0.0) == Validation::Ok);
    last.tick(24 * kHour, product::Progress{});
    CHECK_FALSE(last.tick(10 * kDay, g).programme_completed);

    g.all_targets_reached = true;
    CHECK(last.tick(11 * kDay, g).programme_completed);
}

TEST_CASE("a manual phase waits for the operator") {
    Programme p = twoPhase();
    p.phases[0].end = EndKind::Manual;
    Runner r;
    REQUIRE(r.start(p, 0.0) == Validation::Ok);

    CHECK_FALSE(r.tick(100 * kDay, product::Progress{}).phase_changed);

    r.requestNextPhase();
    const TickResult t = r.tick(100 * kDay, product::Progress{});
    CHECK(t.phase_changed);
    CHECK(t.advance == Advance::Manual);
}

TEST_CASE("pausing does not consume the recipe") {
    // A pause is for opening the chamber or changing a probe. Charging that
    // time to the phase would quietly shorten the cure.
    Runner r;
    REQUIRE(r.start(twoPhase(), 0.0) == Validation::Ok);

    r.pause(10 * kHour);
    CHECK_FALSE(r.tick(30 * kHour, product::Progress{}).phase_changed);
    r.resume(20 * kHour);   // 10 h paused

    CHECK_FALSE(r.tick(33 * kHour, product::Progress{}).phase_changed);
    CHECK(r.tick(34 * kHour, product::Progress{}).phase_changed);
}

TEST_CASE("phase setpoints are overlaid onto the regulation config") {
    Runner r;
    REQUIRE(r.start(twoPhase(), 0.0) == Validation::Ok);

    control::RegulationConfig cfg{};
    cfg.actuators[control::index(control::Actuator::Cool)].present = true;
    cfg.actuators[control::index(control::Actuator::Cool)].min_off = 600.0;

    r.applyTo(cfg);
    CHECK(cfg.has_temp_target);
    CHECK(cfg.target_temp == doctest::Approx(22.0F));
    // Hardware configuration is not the recipe's business.
    CHECK(cfg.actuator(control::Actuator::Cool).present);
    CHECK(cfg.actuator(control::Actuator::Cool).min_off == doctest::Approx(600.0));

    r.tick(24 * kHour, product::Progress{});
    r.applyTo(cfg);
    CHECK(cfg.target_temp == doctest::Approx(13.0F));
    CHECK(cfg.temp_abs_max == doctest::Approx(20.0F));  // phase-specific limit
}

TEST_CASE("a run records the revision it started under") {
    // Editing a recipe must not rewrite the history of a batch already drying
    // against it (spec §7).
    Programme p = twoPhase();
    p.revision = 4;
    Runner r;
    REQUIRE(r.start(p, 0.0) == Validation::Ok);
    CHECK(r.revision() == 4);

    p.revision = 5;
    p.phases[0].target_temp = 25.0F;   // the recipe is edited mid-run
    control::RegulationConfig cfg{};
    r.applyTo(cfg);
    CHECK(cfg.target_temp == doctest::Approx(22.0F));  // the run is unaffected
    CHECK(r.revision() == 4);
}

TEST_CASE("phase time remaining is reported for both kinds of deadline") {
    Runner r;
    REQUIRE(r.start(twoPhase(), 0.0) == Validation::Ok);
    CHECK(r.phaseTimeRemaining(6 * kHour) == doctest::Approx(18 * kHour));

    r.tick(24 * kHour, product::Progress{});
    // A weight phase has no known end, so the cap is what can be shown.
    CHECK(r.phaseTimeRemaining(24 * kHour + kDay) ==
          doctest::Approx(29 * kDay));
}

TEST_CASE("every built-in preset validates") {
    const Preset all[kPresetCount] = {
        Preset::SaucissonSec, Preset::Coppa, Preset::Bresaola,
        Preset::PancettaRoulee, Preset::Lonzo, Preset::CellarHold};

    for (Preset preset : all) {
        Programme p{};
        load(preset, p);
        CAPTURE(name(preset));
        CHECK(validate(p) == Validation::Ok);
        CHECK(p.phase_count > 0);
    }
}

TEST_CASE("presets mark their drying phase, for the high-temperature alarm") {
    Programme p{};
    load(Preset::Coppa, p);
    CHECK_FALSE(p.phases[0].drying);  // the warm rest is deliberately warm
    CHECK(p.phases[1].drying);        // and the dry phase is not
}
