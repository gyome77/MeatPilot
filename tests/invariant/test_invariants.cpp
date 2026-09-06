// SPDX-License-Identifier: MIT
//
// Properties that must hold for *every* input sequence, not just the ones we
// thought to write down. Each runs the engine over randomised inputs with a
// fixed seed, so a failure is reproducible.
#include <doctest/doctest.h>

#include <cstdint>

#include "fixture.h"

using namespace meatpilot::test;

namespace {

/// Small reproducible PRNG. Deliberately not <random>: the generators there
/// are not required to produce identical sequences across implementations,
/// and a property test that cannot be reproduced is not much of a test.
struct Lcg {
    std::uint64_t s;
    std::uint32_t next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(s >> 33);
    }
    bool  chance(std::uint32_t pct) { return next() % 100 < pct; }
    float range(float lo, float hi) {
        return lo + (hi - lo) * (static_cast<float>(next() % 10001) / 10000.0F);
    }
};

/// Drive a rig through `steps` of randomised inputs, calling `check` on every
/// emitted command set.
template <typename F>
void fuzz(std::uint64_t seed, int steps, F&& check) {
    Lcg rng{seed};
    Rig r;
    r.settle();

    for (int i = 0; i < steps; ++i) {
        r.setTemp(rng.range(-5.0F, 35.0F), rng.chance(90));
        r.setHumidity(rng.range(30.0F, 100.0F), rng.chance(90));
        r.in.door_open = rng.chance(10);
        r.in.safety_trip = rng.chance(3);
        r.in.has_co2 = rng.chance(50);
        r.in.co2 = rng.range(400.0F, 3000.0F);

        // Occasionally jump into a different mode, including MANUAL with
        // deliberately contradictory operator requests.
        switch (rng.next() % 10) {
            case 0: r.mode = Mode::Off; break;
            case 1: r.mode = Mode::Maintenance; break;
            case 2: r.mode = Mode::Safe; break;
            case 3:
                r.mode = Mode::Manual;
                for (Actuator a : kAllActuators) {
                    r.in.manual_request[index(a)] = rng.chance(50);
                }
                break;
            default: r.mode = Mode::Auto; break;
        }

        check(r.step(rng.range(1.0F, 120.0F)), r);
    }
}

}  // namespace

TEST_CASE("mutual exclusion is never violated") {
    // FR-T-03, FR-H-03 and the section 15 acceptance criterion.
    fuzz(0xC0FFEEULL, 5000, [](const Outputs& out, const Rig&) {
        for (const ExclusionPair& p : kMutualExclusion) {
            REQUIRE_FALSE((out.on(p.a) && out.on(p.b)));
        }
    });
}

TEST_CASE("minimum OFF time is never violated") {
    // FR-T-04. Checked by watching for OFF->ON transitions and measuring the
    // gap since the actuator last stopped.
    Seconds last_off[kActuatorCount]{};
    bool    prev[kActuatorCount]{};
    bool    seen_off[kActuatorCount]{};

    fuzz(0xBEEF01ULL, 5000, [&](const Outputs& out, const Rig& r) {
        for (Actuator a : kAllActuators) {
            const std::size_t i = index(a);
            const bool on = out.on(a);
            if (prev[i] && !on) {
                last_off[i] = r.now;
                seen_off[i] = true;
            } else if (!prev[i] && on && seen_off[i]) {
                const Seconds gap = r.now - last_off[i];
                REQUIRE(gap >= r.cfg.actuator(a).min_off - 1e-6);
            }
            prev[i] = on;
        }
    });
}

TEST_CASE("a safety trip switches everything off, in every mode") {
    // Spec 4.2 rank 1. No mode, no manual request and no timer may keep an
    // actuator energised through a trip.
    fuzz(0x5AFE01ULL, 5000, [](const Outputs& out, const Rig& r) {
        if (!r.in.safety_trip) return;
        for (Actuator a : kAllActuators) {
            REQUIRE_FALSE(out.on(a));
        }
        REQUIRE(out.mode == Mode::Safe);
        REQUIRE(out.safety_active);
    });
}

TEST_CASE("every energised actuator carries a reason") {
    // DP-05: the operator never sees an unexplained output.
    fuzz(0xD00D05ULL, 5000, [](const Outputs& out, const Rig&) {
        for (Actuator a : kAllActuators) {
            if (out.on(a)) REQUIRE(out.decision(a).reason != Reason::None);
        }
    });
}

TEST_CASE("a blocked decision always reports how long it is blocked for") {
    fuzz(0x71DEULL, 5000, [](const Outputs& out, const Rig&) {
        for (Actuator a : kAllActuators) {
            const Decision& d = out.decision(a);
            const bool timer = d.blocked_by == Blocker::MinOnTimer ||
                               d.blocked_by == Blocker::MinOffTimer ||
                               d.blocked_by == Blocker::StartupLockout ||
                               d.blocked_by == Blocker::AntiOscillation;
            if (timer) REQUIRE(d.blocked_for_s > 0);
        }
    });
}

TEST_CASE("an unconfigured actuator is never commanded") {
    Lcg rng{0xABCDEFULL};
    Rig r;
    r.cfg.actuators[index(Actuator::Heat)].present = false;
    r.cfg.actuators[index(Actuator::Dehumidify)].present = false;
    r.settle();

    for (int i = 0; i < 2000; ++i) {
        r.setTemp(rng.range(-5.0F, 35.0F));
        r.setHumidity(rng.range(30.0F, 100.0F));
        const auto out = r.step(rng.range(1.0F, 60.0F));
        REQUIRE_FALSE(out.on(Actuator::Heat));
        REQUIRE_FALSE(out.on(Actuator::Dehumidify));
    }
}
