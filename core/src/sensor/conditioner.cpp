// SPDX-License-Identifier: MIT
#include "meatpilot/sensor/conditioner.h"

#include <cmath>

namespace meatpilot::sensor {

Conditioner::Conditioner(const Config& cfg) noexcept : cfg_(cfg) {
    if (cfg_.median_window < 1) cfg_.median_window = 1;
    if (cfg_.median_window > kMaxWindow) cfg_.median_window = kMaxWindow;
    if (cfg_.lowpass_alpha <= 0.0F || cfg_.lowpass_alpha > 1.0F) {
        cfg_.lowpass_alpha = 1.0F;
    }
}

void Conditioner::reset() noexcept {
    count_ = 0;
    next_ = 0;
    ema_primed_ = false;
    have_significant_ = false;
    quality_ = Quality::Unavailable;
}

float Conditioner::median() const noexcept {
    // Insertion sort over a fixed buffer: no allocation, and at n <= 9 it beats
    // anything cleverer.
    float sorted[kMaxWindow];
    for (std::size_t i = 0; i < count_; ++i) sorted[i] = window_[i];
    for (std::size_t i = 1; i < count_; ++i) {
        const float key = sorted[i];
        std::size_t j = i;
        while (j > 0 && sorted[j - 1] > key) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = key;
    }
    return sorted[count_ / 2];
}

Reading Conditioner::update(bool available, float raw, Seconds now) noexcept {
    if (!available) {
        // A missing probe is not a stale probe. Drop the history so that a
        // reconnected probe re-earns validity from scratch rather than
        // inheriting a window recorded before it was unplugged.
        reset();
        return Reading{0.0F, 0.0F, false, Quality::Unavailable};
    }

    const float value = raw * cfg_.gain + cfg_.offset;

    if (!std::isfinite(value) || value < cfg_.min_valid || value > cfg_.max_valid) {
        reset();
        quality_ = Quality::OutOfRange;
        // Report the offending value: "142 C" tells the operator far more about
        // a shorted probe than "out of range" alone.
        return Reading{value, value, false, Quality::OutOfRange};
    }

    window_[next_] = value;
    next_ = (next_ + 1) % cfg_.median_window;
    if (count_ < cfg_.median_window) ++count_;

    const float med = median();
    if (!ema_primed_) {
        ema_ = med;
        ema_primed_ = true;
    } else {
        ema_ = cfg_.lowpass_alpha * med + (1.0F - cfg_.lowpass_alpha) * ema_;
    }

    // Freeze detection runs on the calibrated raw value, not the filtered one:
    // the low-pass keeps drifting towards a constant input for several samples
    // after the input stops moving, which would mask the very condition we are
    // looking for.
    if (!have_significant_ ||
        std::fabs(value - last_significant_) > cfg_.resolution) {
        last_significant_ = value;
        last_change_ = now;
        have_significant_ = true;
    }
    const bool frozen = (now - last_change_) >= cfg_.stale_seconds;

    quality_ = frozen ? Quality::Frozen : Quality::Ok;
    return Reading{value, ema_, !frozen, quality_};
}

Divergence compare(const Reading& primary, const Reading& backup,
                   float threshold) noexcept {
    if (!primary.valid || !backup.valid) return Divergence{};
    const float delta = primary.filtered - backup.filtered;
    return Divergence{std::fabs(delta) > threshold, delta};
}

Selection select(const Reading& primary, const Reading& backup) noexcept {
    if (primary.valid) return Selection{primary, Source::Primary};
    if (backup.valid) return Selection{backup, Source::Backup};
    return Selection{Reading{}, Source::None};
}

const char* name(Source s) noexcept {
    switch (s) {
        case Source::None:    return "none";
        case Source::Primary: return "primary";
        case Source::Backup:  return "backup";
    }
    return "unknown";
}

}  // namespace meatpilot::sensor

namespace meatpilot {

const char* name(Quality q) noexcept {
    switch (q) {
        case Quality::Ok:          return "ok";
        case Quality::Unavailable: return "unavailable";
        case Quality::OutOfRange:  return "out_of_range";
        case Quality::Frozen:      return "frozen";
    }
    return "unknown";
}

}  // namespace meatpilot
