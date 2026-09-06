// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include "meatpilot/model/types.h"

namespace meatpilot::sensor {

/// Conditions one sensor channel: calibration, plausibility, filtering and
/// health. Pure and time-injected, like everything else under core/.
///
/// Pipeline, per spec 4.3:
///
///     raw -> calibrate -> range check -> rolling median -> low-pass -> filtered
///                                    \                              /
///                                     `--- both reach the alarm layer
///
/// The median comes before the low-pass because a mean is not robust to the
/// single-sample spikes an I2C bus over a metre of cable in an 85 %RH chamber
/// actually produces: one bad sample shifts a 3-sample mean by a third of its
/// error, and a 5-sample median not at all.
class Conditioner {
public:
    static constexpr std::size_t kMaxWindow = 9;

    struct Config {
        /// Physical plausibility for this channel. Outside this, the reading is
        /// not a measurement, it is a wiring fault (AL-02).
        float min_valid{-40.0F};
        float max_valid{125.0F};

        /// Field calibration against a reference instrument (FR-H-06),
        /// applied before everything else: value = raw * gain + offset.
        float offset{0.0F};
        float gain{1.0F};

        std::size_t median_window{5};   ///< clamped to [1, kMaxWindow], odd
        float       lowpass_alpha{0.3F};///< EMA weight of the newest sample

        /// How long a channel may report an unchanged value before it is
        /// treated as stuck.
        ///
        /// This is per channel by necessity, not by preference. An SHT45
        /// reports to 0.01 C and will essentially never repeat a value, so a
        /// long timeout is safe. A DS18B20 at 0.0625 C resolution can sit
        /// genuinely still in a stable chamber, so it needs a longer one. A
        /// single global timeout would produce nuisance alarms on one probe or
        /// miss a real freeze on the other.
        Seconds stale_seconds{1800.0};

        /// Changes smaller than this are quantisation, not movement. Set to
        /// the channel's resolution.
        float resolution{0.005F};
    };

    explicit Conditioner(const Config& cfg) noexcept;

    /// Feed one reading. `available` is false when the driver returned nothing.
    Reading update(bool available, float raw, Seconds now) noexcept;

    /// Forget all history, e.g. after a probe is replaced.
    void reset() noexcept;

    constexpr Quality quality() const noexcept { return quality_; }

private:
    float median() const noexcept;

    Config      cfg_;
    float       window_[kMaxWindow]{};
    std::size_t count_{0};
    std::size_t next_{0};
    float       ema_{0.0F};
    bool        ema_primed_{false};
    float       last_significant_{0.0F};
    bool        have_significant_{false};
    Seconds     last_change_{0.0};
    Quality     quality_{Quality::Unavailable};
};

/// Result of comparing a primary probe against its backup (FR-T-06).
struct Divergence {
    bool  divergent{false};
    float delta{0.0F};
};

Divergence compare(const Reading& primary, const Reading& backup,
                   float threshold) noexcept;

/// Which probe the control path is using.
enum class Source : std::uint8_t { None, Primary, Backup };

struct Selection {
    Reading reading{};
    Source  source{Source::None};
};

/// Choose the control source.
///
/// The primary is kept whenever it is valid, even while diverging from the
/// backup: silently switching source on disagreement would mean the controller
/// changes behaviour without anyone being told which probe it believes. The
/// disagreement is raised as an alarm and the chosen source is recorded --
/// which is precisely what the spec's section 15 divergence test asks for.
Selection select(const Reading& primary, const Reading& backup) noexcept;

const char* name(Source s) noexcept;

}  // namespace meatpilot::sensor
