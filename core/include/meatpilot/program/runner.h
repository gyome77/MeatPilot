// SPDX-License-Identifier: MIT
#pragma once

#include "meatpilot/program/programme.h"

namespace meatpilot::program {

enum class RunStatus : std::uint8_t { Idle, Running, Paused, Completed };

/// Why a phase ended, for the run log.
enum class Advance : std::uint8_t {
    None,
    Duration,
    Manual,
    WeightTarget,
    MaxDuration,  ///< the safety cap fired, not the intended condition
};

struct TickResult {
    bool        phase_changed{false};
    bool        programme_completed{false};
    Advance     advance{Advance::None};
    std::size_t phase_index{0};
};

/// Executes a programme: tracks the current phase, decides when it ends, and
/// overlays its setpoints onto the regulation configuration.
///
/// Pure and time-injected. It knows nothing about products beyond the
/// aggregate `product::Progress` handed to it, so a recipe's completion rules
/// stay decoupled from how weights are recorded.
class Runner {
public:
    /// Returns the validation result; the run only starts on Ok.
    Validation start(const Programme& p, Seconds now) noexcept;
    void stop() noexcept;
    void pause(Seconds now) noexcept;
    void resume(Seconds now) noexcept;

    /// Operator confirmation, for a Manual phase or an early advance.
    void requestNextPhase() noexcept;

    TickResult tick(Seconds now, const product::Progress& progress) noexcept;

    /// Overlay the current phase's setpoints and ventilation rules onto `cfg`.
    /// Leaves actuator presence, guards and absolute limits untouched unless
    /// the phase carries its own.
    void applyTo(control::RegulationConfig& cfg) const noexcept;

    constexpr RunStatus status() const noexcept { return status_; }
    constexpr std::size_t phaseIndex() const noexcept { return phase_index_; }
    constexpr std::uint16_t revision() const noexcept { return revision_; }
    constexpr Seconds phaseStartedAt() const noexcept { return phase_started_at_; }

    /// Seconds remaining on a duration phase, or on the safety cap of a
    /// weight-driven one. Zero when neither applies.
    Seconds phaseTimeRemaining(Seconds now) const noexcept;

    const Phase* currentPhase() const noexcept;
    bool dryingPhase() const noexcept;

private:
    bool phaseComplete(Seconds now, const product::Progress& progress,
                       Advance& why) const noexcept;

    Programme     programme_{};
    RunStatus     status_{RunStatus::Idle};
    std::size_t   phase_index_{0};
    Seconds       phase_started_at_{0.0};
    Seconds       paused_at_{0.0};
    std::uint16_t revision_{0};
    bool          next_requested_{false};
};

}  // namespace meatpilot::program
