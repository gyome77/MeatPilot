// SPDX-License-Identifier: MIT
#include "meatpilot/product/registry.h"

#include <cmath>

namespace meatpilot::product {
namespace {

constexpr Seconds kDay = 24 * 3600.0;

void copyText(char* dst, std::size_t cap, const char* src) noexcept {
    std::size_t i = 0;
    if (src != nullptr) {
        for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
    }
    dst[i] = '\0';
}

float lossPercent(float initial, float current) noexcept {
    if (initial <= 0.0F) return 0.0F;
    return 100.0F * (initial - current) / initial;  // FR-W-05
}

}  // namespace

Registry::Registry(const Limits& limits) noexcept : limits_(limits) {}

const Product* Registry::find(ProductId id) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (products_[i].id == id) return &products_[i];
    }
    return nullptr;
}

Product* Registry::findMutable(ProductId id) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (products_[i].id == id) return &products_[i];
    }
    return nullptr;
}

ProductId Registry::referenceId() const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (products_[i].reference && products_[i].status == Status::Active) {
            return products_[i].id;
        }
    }
    return kNoProduct;
}

ProductId Registry::create(const char* name, const char* batch,
                           float initial_weight, float target_loss_pct,
                           Seconds now) noexcept {
    if (count_ >= kMaxActive) return kNoProduct;
    if (!(initial_weight > 0.0F)) return kNoProduct;

    Product& p = products_[count_];
    p = Product{};
    p.id = next_id_++;
    copyText(p.name, kNameLength, name);
    copyText(p.batch, kBatchLength, batch);
    p.started_at = now;
    p.initial_weight = initial_weight;
    p.target_loss_pct = target_loss_pct;
    p.status = Status::Active;

    // The initial weight is itself the first point of the drying curve;
    // without it a product has no history until its second weigh-in and no
    // rate until its third.
    p.history[0] = WeighIn{now, initial_weight, false, true, false};
    p.weigh_in_count = 1;

    p.reference = (referenceId() == kNoProduct);
    ++count_;
    return p.id;
}

bool Registry::remove(ProductId id) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        if (products_[i].id != id) continue;
        const bool was_reference = products_[i].reference;
        for (std::size_t j = i; j + 1 < count_; ++j) products_[j] = products_[j + 1];
        products_[--count_] = Product{};
        if (was_reference && count_ > 0 && referenceId() == kNoProduct) {
            products_[0].reference = true;
        }
        return true;
    }
    return false;
}

bool Registry::setReference(ProductId id) noexcept {
    Product* target = findMutable(id);
    if (target == nullptr || target->status != Status::Active) return false;
    for (std::size_t i = 0; i < count_; ++i) products_[i].reference = false;
    target->reference = true;
    return true;
}

bool Registry::setStatus(ProductId id, Status status) noexcept {
    Product* p = findMutable(id);
    if (p == nullptr) return false;
    p->status = status;
    if (status != Status::Active && p->reference) {
        // The reference must stay on something that is still drying.
        p->reference = false;
        for (std::size_t i = 0; i < count_; ++i) {
            if (products_[i].status == Status::Active) {
                products_[i].reference = true;
                break;
            }
        }
    }
    return true;
}

Accept Registry::recordWeighIn(ProductId id, float weight, Seconds now,
                               bool automatic, bool stable) noexcept {
    Product* p = findMutable(id);
    if (p == nullptr) return Accept::RejectedUnknown;

    // Physically impossible: a disconnected load cell reads zero or negative,
    // and no cure gains an order of magnitude (FR-W-08).
    if (!std::isfinite(weight) || weight <= 0.0F ||
        weight > p->initial_weight * 10.0F) {
        return Accept::RejectedImpossible;
    }

    const WeighIn* previous =
        p->weigh_in_count > 0 ? &p->history[p->weigh_in_count - 1] : nullptr;
    if (previous != nullptr && now < previous->timestamp) {
        // Out-of-order samples would corrupt the least-squares slope and the
        // history is append-only, so there is nowhere sensible to put them.
        return Accept::RejectedOutOfOrder;
    }

    bool suspicious = false;
    if (previous != nullptr && previous->weight > 0.0F) {
        const float step = std::fabs(weight - previous->weight) / previous->weight;
        if (step > limits_.max_step_fraction) suspicious = true;
    }
    if (weight > p->initial_weight * (1.0F + limits_.max_gain_fraction)) {
        suspicious = true;  // negative loss: FR-W-08
    }
    if (!stable) suspicious = true;

    if (p->weigh_in_count == kMaxWeighIns) {
        // The RAM window is full. Drop the oldest, never the newest: the
        // recent slope is what the ETA depends on. The full history is
        // preserved on storage, which is what FR-W-07 actually requires.
        for (std::size_t i = 0; i + 1 < kMaxWeighIns; ++i) {
            p->history[i] = p->history[i + 1];
        }
        p->weigh_in_count = kMaxWeighIns - 1;
    }

    p->history[p->weigh_in_count++] =
        WeighIn{now, weight, automatic, stable, suspicious};
    return suspicious ? Accept::Suspicious : Accept::Ok;
}

Stats Registry::stats(ProductId id, Seconds now) const noexcept {
    Stats s{};
    const Product* p = find(id);
    if (p == nullptr || p->weigh_in_count == 0) return s;

    const WeighIn& latest = p->history[p->weigh_in_count - 1];
    s.valid = true;
    s.current_weight = latest.weight;
    s.absolute_loss = p->initial_weight - latest.weight;
    s.loss_pct = lossPercent(p->initial_weight, latest.weight);
    s.last_weigh_in = latest.timestamp;
    s.age_of_last_weigh_in = now - latest.timestamp;
    s.target_reached = s.loss_pct >= p->target_loss_pct;

    // Drying rate as a least-squares slope of loss% against days, not a
    // two-point difference. Two points are dominated by weigh-in noise -- a
    // 5 g scale error on a 1 kg product is half a percent, which on a
    // three-day gap is a rate error larger than the rate -- and that error
    // propagates straight into the ETA.
    if (p->weigh_in_count < 3) return s;

    double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;
    const double n = static_cast<double>(p->weigh_in_count);
    for (std::size_t i = 0; i < p->weigh_in_count; ++i) {
        const double x = (p->history[i].timestamp - p->started_at) / kDay;
        const double y = lossPercent(p->initial_weight, p->history[i].weight);
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }
    const double denominator = n * sum_xx - sum_x * sum_x;
    if (std::fabs(denominator) < 1e-9) return s;

    const double slope = (n * sum_xy - sum_x * sum_y) / denominator;
    s.rate_pct_per_day = static_cast<float>(slope);
    s.has_rate = true;

    // FR-W-06: projected completion. Only meaningful while actually drying.
    const float remaining = p->target_loss_pct - s.loss_pct;
    if (slope > 1e-4 && remaining > 0.0F) {
        s.has_eta = true;
        s.eta = now + (static_cast<double>(remaining) / slope) * kDay;
    }
    return s;
}

Progress Registry::progress(Seconds now) const noexcept {
    Progress g{};
    bool all = true;
    for (std::size_t i = 0; i < count_; ++i) {
        const Product& p = products_[i];
        if (p.status != Status::Active) continue;
        ++g.active_count;

        const Stats s = stats(p.id, now);
        if (s.target_reached) g.any_target_reached = true;
        else all = false;

        if (s.valid && s.age_of_last_weigh_in > limits_.weigh_in_interval) {
            g.any_weigh_in_overdue = true;
        }
        if (p.reference) {
            g.has_reference = true;
            g.reference_loss_pct = s.loss_pct;
            g.reference_target_reached = s.target_reached;
        }
    }
    g.all_targets_reached = (g.active_count > 0) && all;
    return g;
}

}  // namespace meatpilot::product
