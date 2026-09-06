// SPDX-License-Identifier: MIT
// FR-H-01 derived quantities.
#include <doctest/doctest.h>

#include "meatpilot/sensor/derived.h"

using namespace meatpilot::sensor;

TEST_CASE("dew point matches published values") {
    // Reference points from a psychrometric table.
    CHECK(dewPoint(20.0F, 50.0F) == doctest::Approx(9.26F).epsilon(0.02));
    CHECK(dewPoint(30.0F, 80.0F) == doctest::Approx(26.16F).epsilon(0.02));
    CHECK(dewPoint(13.0F, 100.0F) == doctest::Approx(13.0F).epsilon(0.01));
}

TEST_CASE("a curing chamber sits close to its dew point") {
    // 13 C at 78 %RH is a normal drying phase, and it is only about 2.5 C from
    // condensation -- which is why AL-09 exists and why the probe needs a
    // membrane cap.
    const float dp = dewPoint(13.0F, 78.0F);
    CHECK(dp == doctest::Approx(9.25F).epsilon(0.01));
    CHECK(13.0F - dp < 4.0F);  // 3.7 C of margin, and that is a normal phase
}

TEST_CASE("absolute humidity tracks water content, not relative humidity") {
    const float ah_warm = absoluteHumidity(20.0F, 50.0F);
    CHECK(ah_warm == doctest::Approx(8.62F).epsilon(0.01));

    // Cool that air to its dew point without removing any water: RH goes from
    // 50 % to 100 % while the water content is unchanged.
    //
    // g/m3 is nonetheless not invariant, because the air contracts as it
    // cools. It rises by exactly the absolute-temperature ratio -- the
    // quantity conserved here is vapour pressure, not density. Asserting
    // equality (as an earlier draft of this test did) asserts wrong physics.
    const float dp = dewPoint(20.0F, 50.0F);
    const float expected = ah_warm * (273.15F + 20.0F) / (273.15F + dp);
    CHECK(absoluteHumidity(dp, 100.0F) == doctest::Approx(expected).epsilon(0.005));
}
