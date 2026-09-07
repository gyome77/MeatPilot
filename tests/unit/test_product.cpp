// SPDX-License-Identifier: MIT
// FR-W-01 to FR-W-09: products, weigh-ins, loss, rate and ETA.
#include <doctest/doctest.h>

#include "meatpilot/product/registry.h"

using namespace meatpilot;
using namespace meatpilot::product;

namespace {
constexpr Seconds kDay = 24 * 3600.0;
}

TEST_CASE("15: weight loss is exact to within 0.1 percentage point") {
    Registry r;
    const ProductId id = r.create("Coppa", "B1", 1200.0F, 35.0F, 0.0);
    REQUIRE(r.recordWeighIn(id, 1080.0F, kDay) == Accept::Ok);

    const Stats s = r.stats(id, kDay);
    CHECK(s.absolute_loss == doctest::Approx(120.0F));
    CHECK(s.loss_pct == doctest::Approx(10.0F).epsilon(0.001));  // 0.1 pp
}

TEST_CASE("the initial weight seeds the curve") {
    // Without it a product has no history until its second weigh-in.
    Registry r;
    const ProductId id = r.create("Lonzo", "", 900.0F, 35.0F, 0.0);
    const Stats s = r.stats(id, 0.0);
    CHECK(s.valid);
    CHECK(s.loss_pct == doctest::Approx(0.0F));
}

TEST_CASE("FR-W-07: a new reading never overwrites an earlier one") {
    Registry r;
    const ProductId id = r.create("Saucisson", "", 500.0F, 35.0F, 0.0);
    r.recordWeighIn(id, 480.0F, kDay);
    r.recordWeighIn(id, 460.0F, 2 * kDay);

    const Product* p = r.find(id);
    REQUIRE(p != nullptr);
    CHECK(p->weigh_in_count == 3);          // seed + two
    CHECK(p->history[0].weight == doctest::Approx(500.0F));
    CHECK(p->history[1].weight == doctest::Approx(480.0F));
    CHECK(p->history[2].weight == doctest::Approx(460.0F));
}

TEST_CASE("drying rate and ETA follow a steady curve") {
    Registry r;
    const ProductId id = r.create("Bresaola", "", 1000.0F, 30.0F, 0.0);
    // A clean 1 %/day loss.
    for (int day = 1; day <= 10; ++day) {
        const float w = 1000.0F * (1.0F - 0.01F * static_cast<float>(day));
        REQUIRE(r.recordWeighIn(id, w, day * kDay) == Accept::Ok);
    }

    const Stats s = r.stats(id, 10 * kDay);
    CHECK(s.has_rate);
    CHECK(s.rate_pct_per_day == doctest::Approx(1.0F).epsilon(0.02));
    CHECK(s.loss_pct == doctest::Approx(10.0F).epsilon(0.01));
    REQUIRE(s.has_eta);
    // 20 points of loss left at 1 %/day.
    CHECK((s.eta - 10 * kDay) / kDay == doctest::Approx(20.0).epsilon(0.05));
}

TEST_CASE("a least-squares rate resists weigh-in noise") {
    // The reason not to use a two-point difference: a scale error on the last
    // reading would otherwise dominate the ETA.
    Registry noisy, clean;
    const ProductId a = noisy.create("A", "", 1000.0F, 30.0F, 0.0);
    const ProductId b = clean.create("B", "", 1000.0F, 30.0F, 0.0);

    const float jitter[10] = {2.0F, -3.0F, 1.0F, -1.0F, 4.0F,
                              -2.0F, 0.0F, 3.0F, -4.0F, 5.0F};
    for (int day = 1; day <= 10; ++day) {
        const float w = 1000.0F * (1.0F - 0.01F * static_cast<float>(day));
        noisy.recordWeighIn(a, w + jitter[day - 1], day * kDay);
        clean.recordWeighIn(b, w, day * kDay);
    }

    const float rate_noisy = noisy.stats(a, 10 * kDay).rate_pct_per_day;
    const float rate_clean = clean.stats(b, 10 * kDay).rate_pct_per_day;
    CHECK(rate_noisy == doctest::Approx(rate_clean).epsilon(0.10));
}

TEST_CASE("no ETA is offered when nothing is drying") {
    Registry r;
    const ProductId id = r.create("Stalled", "", 1000.0F, 30.0F, 0.0);
    for (int day = 1; day <= 5; ++day) r.recordWeighIn(id, 1000.0F, day * kDay);

    const Stats s = r.stats(id, 5 * kDay);
    CHECK_FALSE(s.has_eta);  // an infinite ETA is worse than none
}

TEST_CASE("FR-W-08: implausible readings are caught") {
    Registry r;
    const ProductId id = r.create("Coppa", "", 1000.0F, 35.0F, 0.0);

    SUBCASE("a disconnected load cell reads zero and is rejected") {
        CHECK(r.recordWeighIn(id, 0.0F, kDay) == Accept::RejectedImpossible);
        CHECK(r.find(id)->weigh_in_count == 1);  // not stored
    }
    SUBCASE("an absurd value is rejected") {
        CHECK(r.recordWeighIn(id, 50000.0F, kDay) == Accept::RejectedImpossible);
    }
    SUBCASE("a large step is stored but flagged") {
        CHECK(r.recordWeighIn(id, 700.0F, kDay) == Accept::Suspicious);
        CHECK(r.find(id)->history[1].suspicious);
    }
    SUBCASE("weight gain is flagged as negative loss") {
        CHECK(r.recordWeighIn(id, 1050.0F, kDay) == Accept::Suspicious);
    }
    SUBCASE("an unstable load-cell reading is flagged") {
        CHECK(r.recordWeighIn(id, 980.0F, kDay, true, /*stable=*/false) ==
              Accept::Suspicious);
    }
    SUBCASE("an out-of-order reading is refused, not silently reordered") {
        r.recordWeighIn(id, 980.0F, 5 * kDay);
        CHECK(r.recordWeighIn(id, 970.0F, 2 * kDay) == Accept::RejectedOutOfOrder);
    }
}

TEST_CASE("an overdue weigh-in is visible in progress") {
    Registry r;
    const ProductId id = r.create("Forgotten", "", 1000.0F, 35.0F, 0.0);
    r.recordWeighIn(id, 950.0F, kDay);

    CHECK_FALSE(r.progress(3 * kDay).any_weigh_in_overdue);
    CHECK(r.progress(20 * kDay).any_weigh_in_overdue);
}

TEST_CASE("15: 20 products keep independent histories") {
    // FR-W-01 and FR-W-09.
    Registry r;
    ProductId ids[kMaxActive];
    for (std::size_t i = 0; i < kMaxActive; ++i) {
        ids[i] = r.create("P", "", 1000.0F + static_cast<float>(i) * 10.0F,
                          30.0F, 0.0);
        REQUIRE(ids[i] != kNoProduct);
    }
    CHECK(r.create("overflow", "", 1000.0F, 30.0F, 0.0) == kNoProduct);

    for (std::size_t i = 0; i < kMaxActive; ++i) {
        const float initial = 1000.0F + static_cast<float>(i) * 10.0F;
        r.recordWeighIn(ids[i], initial * (1.0F - 0.01F * static_cast<float>(i + 1)),
                        kDay);
    }
    for (std::size_t i = 0; i < kMaxActive; ++i) {
        const Stats s = r.stats(ids[i], kDay);
        CHECK(s.loss_pct ==
              doctest::Approx(static_cast<float>(i + 1)).epsilon(0.01));
    }
}

TEST_CASE("the reference product drives programme progress") {
    Registry r;
    const ProductId a = r.create("A", "", 1000.0F, 30.0F, 0.0);
    const ProductId b = r.create("B", "", 1000.0F, 30.0F, 0.0);
    CHECK(r.referenceId() == a);  // first created becomes the reference

    r.recordWeighIn(a, 900.0F, kDay);   // 10 %
    r.recordWeighIn(b, 600.0F, kDay);   // 40 %, past its target

    Progress g = r.progress(kDay);
    CHECK(g.has_reference);
    CHECK(g.reference_loss_pct == doctest::Approx(10.0F));
    CHECK_FALSE(g.reference_target_reached);
    CHECK(g.any_target_reached);        // B is done
    CHECK_FALSE(g.all_targets_reached); // A is not

    REQUIRE(r.setReference(b));
    g = r.progress(kDay);
    CHECK(g.reference_target_reached);
}

TEST_CASE("completing the reference moves it to something still drying") {
    Registry r;
    const ProductId a = r.create("A", "", 1000.0F, 30.0F, 0.0);
    const ProductId b = r.create("B", "", 1000.0F, 30.0F, 0.0);
    REQUIRE(r.referenceId() == a);

    r.setStatus(a, Status::Completed);
    CHECK(r.referenceId() == b);
}

TEST_CASE("the RAM window rolls without losing the newest readings") {
    Registry r;
    const ProductId id = r.create("Long", "", 1000.0F, 40.0F, 0.0);
    for (std::size_t i = 1; i <= kMaxWeighIns + 10; ++i) {
        const float w = 1000.0F - static_cast<float>(i);
        REQUIRE(r.recordWeighIn(id, w, static_cast<Seconds>(i) * kDay) != Accept::RejectedImpossible);
    }
    const Product* p = r.find(id);
    CHECK(p->weigh_in_count == kMaxWeighIns);
    CHECK(p->history[kMaxWeighIns - 1].weight ==
          doctest::Approx(1000.0F - static_cast<float>(kMaxWeighIns + 10)));
}
