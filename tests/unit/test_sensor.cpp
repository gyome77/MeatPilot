// SPDX-License-Identifier: MIT
// Spec 4.3 filtering, AL-02 sensor health, FR-T-06 divergence, FR-H-06 calibration.
#include <doctest/doctest.h>

#include "meatpilot/sensor/conditioner.h"

using namespace meatpilot;
using namespace meatpilot::sensor;

namespace {
Conditioner::Config chamberProbe() {
    Conditioner::Config c;
    c.min_valid = -20.0F;
    c.max_valid = 60.0F;
    c.median_window = 5;
    c.lowpass_alpha = 0.4F;
    c.stale_seconds = 1800.0;
    c.resolution = 0.005F;
    return c;
}
}  // namespace

TEST_CASE("a single spike does not move the filtered value") {
    // The reason for a median rather than a mean: one bad sample from a long
    // I2C run must not reach the control path at all.
    Conditioner c{chamberProbe()};
    Seconds t = 0.0;
    for (int i = 0; i < 5; ++i) c.update(true, 13.0F, t += 5.0);
    const float before = c.update(true, 13.0F, t += 5.0).filtered;

    const Reading spiked = c.update(true, 47.0F, t += 5.0);

    CHECK(spiked.filtered == doctest::Approx(before).epsilon(0.01));
    CHECK(spiked.raw == doctest::Approx(47.0F));  // but the alarm layer sees it
    CHECK(spiked.valid);
}

TEST_CASE("a sustained change does reach the filtered value") {
    // The counterpart: rejecting spikes must not mean rejecting reality.
    Conditioner c{chamberProbe()};
    Seconds t = 0.0;
    for (int i = 0; i < 6; ++i) c.update(true, 13.0F, t += 5.0);

    Reading r{};
    for (int i = 0; i < 20; ++i) r = c.update(true, 18.0F, t += 5.0);
    CHECK(r.filtered == doctest::Approx(18.0F).epsilon(0.01));
}

TEST_CASE("an unplugged probe is unavailable, not stale") {
    Conditioner c{chamberProbe()};
    c.update(true, 13.0F, 0.0);
    const Reading r = c.update(false, 0.0F, 5.0);

    CHECK_FALSE(r.valid);
    CHECK(r.quality == Quality::Unavailable);
}

TEST_CASE("a reconnected probe re-earns validity from scratch") {
    // History from before the disconnection must not carry over: it describes
    // a probe that is no longer necessarily the one now connected.
    Conditioner c{chamberProbe()};
    Seconds t = 0.0;
    for (int i = 0; i < 5; ++i) c.update(true, 13.0F, t += 5.0);
    c.update(false, 0.0F, t += 5.0);

    const Reading r = c.update(true, 20.0F, t += 5.0);
    CHECK(r.valid);
    CHECK(r.filtered == doctest::Approx(20.0F));  // not blended with the old 13
}

TEST_CASE("an implausible value reports itself rather than hiding") {
    Conditioner c{chamberProbe()};
    const Reading r = c.update(true, 142.0F, 0.0);

    CHECK_FALSE(r.valid);
    CHECK(r.quality == Quality::OutOfRange);
    CHECK(r.raw == doctest::Approx(142.0F));  // "142 C" diagnoses a short
}

TEST_CASE("15: a frozen probe is detected within the stale period") {
    Conditioner c{chamberProbe()};
    Seconds t = 0.0;

    // A probe that answers, plausibly, and never changes.
    Reading r{};
    while (t < 1700.0) r = c.update(true, 13.0F, t += 10.0);
    CHECK(r.valid);
    CHECK(r.quality == Quality::Ok);

    while (t < 1900.0) r = c.update(true, 13.0F, t += 10.0);
    CHECK_FALSE(r.valid);
    CHECK(r.quality == Quality::Frozen);
}

TEST_CASE("movement below the channel resolution does not count as change") {
    // Quantisation dither must not reset the freeze timer, or a stuck probe
    // whose last bit rattles would never be detected.
    Conditioner c{chamberProbe()};
    Seconds t = 0.0;
    Reading r{};
    bool up = false;
    while (t < 1900.0) {
        up = !up;
        r = c.update(true, 13.0F + (up ? 0.001F : 0.0F), t += 10.0);
    }
    CHECK(r.quality == Quality::Frozen);
}

TEST_CASE("a slowly drifting probe is never called frozen") {
    Conditioner c{chamberProbe()};
    Seconds t = 0.0;
    Reading r{};
    float v = 13.0F;
    while (t < 7200.0) {
        v += 0.01F;
        r = c.update(true, v, t += 10.0);
    }
    CHECK(r.valid);
    CHECK(r.quality == Quality::Ok);
}

TEST_CASE("calibration is applied before everything else") {
    // FR-H-06: coefficients from a comparison against a reference instrument.
    Conditioner::Config cfg = chamberProbe();
    cfg.offset = -1.5F;
    cfg.gain = 1.02F;
    Conditioner c{cfg};

    Seconds t = 0.0;
    Reading r{};
    for (int i = 0; i < 10; ++i) r = c.update(true, 13.0F, t += 5.0);

    CHECK(r.raw == doctest::Approx(13.0F * 1.02F - 1.5F));
    CHECK(r.filtered == doctest::Approx(13.0F * 1.02F - 1.5F).epsilon(0.01));
}

TEST_CASE("15: injected probe divergence is detected") {
    // FR-T-06: SHT45 against the DS18B20 cross-check.
    Conditioner primary{chamberProbe()};
    Conditioner backup{chamberProbe()};
    Seconds t = 0.0;

    Reading p{}, b{};
    for (int i = 0; i < 10; ++i) {
        p = primary.update(true, 13.0F, t + 5.0);
        b = backup.update(true, 13.2F, t + 5.0);
        t += 5.0;
    }
    CHECK_FALSE(compare(p, b, 2.0F).divergent);

    for (int i = 0; i < 30; ++i) {
        p = primary.update(true, 13.0F, t + 5.0);
        b = backup.update(true, 16.5F, t + 5.0);
        t += 5.0;
    }
    const Divergence d = compare(p, b, 2.0F);
    CHECK(d.divergent);
    CHECK(d.delta == doctest::Approx(-3.5F).epsilon(0.05));
}

TEST_CASE("the control source is recorded, and does not switch on disagreement") {
    Reading primary{13.0F, 13.0F, true, Quality::Ok};
    Reading backup{16.5F, 16.5F, true, Quality::Ok};

    Selection s = select(primary, backup);
    CHECK(s.source == Source::Primary);   // diverging, but still believed
    CHECK(s.reading.filtered == doctest::Approx(13.0F));

    primary.valid = false;
    primary.quality = Quality::Frozen;
    s = select(primary, backup);
    CHECK(s.source == Source::Backup);    // only once the primary is unusable

    backup.valid = false;
    s = select(primary, backup);
    CHECK(s.source == Source::None);
}
