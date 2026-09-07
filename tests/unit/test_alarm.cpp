// SPDX-License-Identifier: MIT
// Spec §5 alarm lifecycle, and the AL-01..AL-11 catalogue.
#include <doctest/doctest.h>

#include "meatpilot/alarm/manager.h"

using namespace meatpilot;
using namespace meatpilot::alarm;

namespace {

/// A settled chamber with everything fitted and nothing wrong.
struct Rig {
    Manager                   manager{};
    control::RegulationConfig cfg{};
    Signals                   sig{};
    Event                     events[32]{};
    std::size_t               count{0};
    Seconds                   now{0.0};

    Rig() {
        for (control::Actuator a : control::kAllActuators) {
            cfg.actuators[control::index(a)].present = true;
        }
        cfg.has_temp_target = true;
        cfg.target_temp = 13.0F;
        cfg.temp_deadband = 0.8F;
        cfg.has_humidity_target = true;
        cfg.target_humidity = 75.0F;
        cfg.humidity_deadband = 3.0F;
        cfg.temp_abs_min = 2.0F;
        cfg.temp_abs_max = 25.0F;
        cfg.humidity_abs_min = 40.0F;
        cfg.humidity_abs_max = 95.0F;
        setTemp(13.0F);
        setHumidity(75.0F);
    }

    void setTemp(float v) { sig.inputs.temperature = {v, v, true, Quality::Ok}; }
    void setHumidity(float v) { sig.inputs.humidity = {v, v, true, Quality::Ok}; }

    // Transitions are counted as they happen. Inspecting only the last
    // step's buffer would miss anything that fired mid-run.
    int raised[kKeyCount]{};
    int reminded[kKeyCount]{};
    int cleared[kKeyCount]{};

    void step(Seconds dt = 10.0) {
        now += dt;
        count = manager.evaluate(sig, cfg, now, events, 32);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t k = static_cast<std::size_t>(events[i].key);
            switch (events[i].kind) {
                case EventKind::Raised:   ++raised[k]; break;
                case EventKind::Reminder: ++reminded[k]; break;
                case EventKind::Cleared:  ++cleared[k]; break;
            }
        }
    }
    void run(Seconds duration, Seconds dt = 30.0) {
        const Seconds end = now + duration;
        while (now < end) step(dt);
    }
    void resetCounts() {
        for (std::size_t i = 0; i < kKeyCount; ++i) {
            raised[i] = reminded[i] = cleared[i] = 0;
        }
    }
    int remindersFor(Key k) const { return reminded[static_cast<std::size_t>(k)]; }
    bool sawClear(Key key) const { return cleared[static_cast<std::size_t>(key)] > 0; }
};

}  // namespace

TEST_CASE("a healthy chamber raises nothing") {
    Rig r;
    r.run(4 * 3600.0);
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        const Key key = static_cast<Key>(i);
        CAPTURE(Manager::name(key));
        CHECK_FALSE(r.manager.visible(key));
    }
}

TEST_CASE("a warning waits out its delay; a critical does not") {
    Rig r;
    r.setTemp(16.0F);  // past target + 2 x deadband, inside the absolute limit
    r.step();
    CHECK_FALSE(r.manager.visible(Key::TemperatureWarnHigh));

    r.run(700.0);
    CHECK(r.manager.visible(Key::TemperatureWarnHigh));

    Rig c;
    c.setTemp(26.0F);  // past the absolute maximum
    c.step();
    CHECK(c.manager.visible(Key::TemperatureCriticalHigh));  // immediately
}

TEST_CASE("a fast excursion in the raw value is not smoothed away") {
    // Spec §4.3: the alarm layer examines raw readings too, so a spike that
    // the low-pass has not caught up with still trips the critical limit.
    Rig r;
    r.sig.inputs.temperature = {27.0F, 14.0F, true, Quality::Ok};
    r.step();
    CHECK(r.manager.visible(Key::TemperatureCriticalHigh));
}

TEST_CASE("§5: a critical stays visible after the condition clears") {
    Rig r;
    r.setTemp(26.0F);
    r.step();
    REQUIRE(r.manager.visible(Key::TemperatureCriticalHigh));

    r.setTemp(13.0F);  // excursion self-corrects
    r.step();
    CHECK(r.manager.visible(Key::TemperatureCriticalHigh));  // latched
    CHECK_FALSE(r.sawClear(Key::TemperatureCriticalHigh));

    r.manager.acknowledge(Key::TemperatureCriticalHigh);
    r.step();
    CHECK_FALSE(r.manager.visible(Key::TemperatureCriticalHigh));
    CHECK(r.sawClear(Key::TemperatureCriticalHigh));
}

TEST_CASE("a warning resolves itself without an acknowledgement") {
    Rig r;
    r.setTemp(16.0F);
    r.run(700.0);
    REQUIRE(r.manager.visible(Key::TemperatureWarnHigh));

    r.setTemp(13.0F);
    r.step();
    CHECK_FALSE(r.manager.visible(Key::TemperatureWarnHigh));
    CHECK(r.sawClear(Key::TemperatureWarnHigh));
}

TEST_CASE("reminders repeat while unacknowledged, and stop once acknowledged") {
    Rig r;
    r.setTemp(16.0F);
    r.run(700.0);
    REQUIRE(r.manager.visible(Key::TemperatureWarnHigh));

    r.resetCounts();
    r.run(12 * 3600.0);
    CHECK(r.remindersFor(Key::TemperatureWarnHigh) >= 2);  // every 2 h over 12 h
    CHECK(r.remindersFor(Key::TemperatureWarnHigh) <= 8);  // not one per tick

    r.manager.acknowledge(Key::TemperatureWarnHigh);
    r.resetCounts();
    r.run(6 * 3600.0);
    CHECK(r.remindersFor(Key::TemperatureWarnHigh) == 0);
}

TEST_CASE("AL-02: an invalid probe is critical immediately") {
    Rig r;
    r.sig.inputs.temperature.valid = false;
    r.step();
    CHECK(r.manager.visible(Key::TempSensorFault));
    CHECK(Manager::severity(Key::TempSensorFault) == Severity::Critical);
}

TEST_CASE("losing both probes demands SAFE, losing one does not") {
    // Spec §4.2 rank 2. A single failed probe stops the actuators that depend
    // on it; losing both means the controller cannot know what it is doing.
    Rig r;
    r.sig.inputs.temperature.valid = false;
    r.step();
    CHECK_FALSE(r.manager.requiresSafeState());

    r.sig.inputs.humidity.valid = false;
    r.step();
    CHECK(r.manager.requiresSafeState());
}

TEST_CASE("AL-04: feedback disagreeing with the command is critical") {
    // A welded relay reports ON while we command OFF, and no environmental
    // rule catches it.
    Rig r;
    const std::size_t i = control::index(control::Actuator::Cool);
    r.sig.inputs.feedback[i] = {true, true};
    r.sig.outputs.commands[i] = false;
    r.step();
    CHECK(r.manager.visible(Key::ActuatorFeedbackMismatch));
}

TEST_CASE("AL-05: the door alarm waits for the configured delay") {
    Rig r;
    r.sig.inputs.door_open = true;
    r.step();
    CHECK_FALSE(r.manager.visible(Key::DoorOpenTooLong));
    r.run(320.0);
    CHECK(r.manager.visible(Key::DoorOpenTooLong));

    r.sig.inputs.door_open = false;
    r.step();
    CHECK_FALSE(r.manager.visible(Key::DoorOpenTooLong));
}

TEST_CASE("AL-05: too many compressor starts per hour is caught") {
    Rig r;
    const std::size_t cool = control::index(control::Actuator::Cool);
    for (int i = 0; i < 10; ++i) {
        r.sig.outputs.commands[cool] = true;
        r.step(60.0);
        r.sig.outputs.commands[cool] = false;
        r.step(60.0);
    }
    CHECK(r.manager.visible(Key::ExcessiveStarts));
}

TEST_CASE("AL-08: case hardening fires on rate as well as on dry air") {
    SUBCASE("drying too fast, even at the humidity setpoint") {
        Rig r;
        r.sig.has_drying_rate = true;
        r.sig.drying_rate_pct_per_day = 2.4F;   // above 1.5
        r.run(1000.0);
        CHECK(r.manager.visible(Key::CaseHardening));
    }
    SUBCASE("air durably below the humidity band") {
        Rig r;
        r.setHumidity(70.0F);
        r.run(1000.0);
        CHECK(r.manager.visible(Key::CaseHardening));
    }
    SUBCASE("a normal rate at the setpoint does not") {
        Rig r;
        r.sig.has_drying_rate = true;
        r.sig.drying_rate_pct_per_day = 0.8F;
        r.run(4 * 3600.0);
        CHECK_FALSE(r.manager.visible(Key::CaseHardening));
    }
}

TEST_CASE("AL-09: condensation risk tracks the dew-point margin") {
    Rig r;
    r.setTemp(13.0F);
    r.setHumidity(78.0F);   // about 3.7 C of margin
    r.step();
    CHECK_FALSE(r.manager.visible(Key::CondensationRisk));

    r.setHumidity(96.0F);   // now within a degree of the dew point
    r.step();
    CHECK(r.manager.visible(Key::CondensationRisk));
}

TEST_CASE("AL-10: warmth is only an alarm while drying") {
    // A rest phase at 22 C is the recipe. The same temperature in a drying
    // phase is a food-safety problem, and no fixed limit tells them apart.
    Rig rest;
    rest.sig.drying_phase = false;
    rest.setTemp(22.0F);
    rest.run(3 * 3600.0);
    CHECK_FALSE(rest.manager.visible(Key::HighTempDrying));

    Rig drying;
    drying.sig.drying_phase = true;
    drying.setTemp(22.0F);
    drying.run(3 * 3600.0);
    CHECK(drying.manager.visible(Key::HighTempDrying));
    CHECK(Manager::severity(Key::HighTempDrying) == Severity::Critical);
}

TEST_CASE("AL-11: degraded mode names what to do by hand") {
    SUBCASE("no humidifier fitted and the air is dry") {
        Rig r;
        r.cfg.actuators[control::index(control::Actuator::Humidify)].present = false;
        r.setHumidity(66.0F);
        r.run(1000.0);
        CHECK(r.manager.visible(Key::DegradedMode));
        CHECK(r.manager.manualAction() == ManualAction::AddHumidity);
    }
    SUBCASE("no cooling fitted and the chamber is warm") {
        Rig r;
        r.cfg.actuators[control::index(control::Actuator::Cool)].present = false;
        r.setTemp(16.0F);
        r.run(1000.0);
        CHECK(r.manager.visible(Key::DegradedMode));
        CHECK(r.manager.manualAction() == ManualAction::AddCooling);
    }
    SUBCASE("an actuator that exists is not a degraded condition") {
        Rig r;
        r.setHumidity(66.0F);   // humidifier is fitted and will deal with it
        r.run(1000.0);
        CHECK_FALSE(r.manager.visible(Key::DegradedMode));
    }
    SUBCASE("temperature outranks humidity when both are degraded") {
        Rig r;
        r.cfg.actuators[control::index(control::Actuator::Cool)].present = false;
        r.cfg.actuators[control::index(control::Actuator::Humidify)].present = false;
        r.setTemp(16.0F);
        r.setHumidity(66.0F);
        r.run(1000.0);
        CHECK(r.manager.manualAction() == ManualAction::AddCooling);
    }
}

TEST_CASE("the event buffer never overflows, and says when it dropped") {
    Rig r;
    r.sig.inputs.temperature.valid = false;
    r.sig.inputs.humidity.valid = false;
    r.sig.storage_ok = false;
    r.sig.network_ok = false;
    r.sig.supply_ok = false;
    r.sig.clock_valid = false;

    Event small[2]{};
    const std::size_t written = r.manager.evaluate(r.sig, r.cfg, 10.0, small, 2);
    CHECK(written == 2);
    CHECK(r.manager.dropped() > 0);
}
