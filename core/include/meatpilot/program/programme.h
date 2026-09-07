// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include "meatpilot/control/config.h"
#include "meatpilot/product/registry.h"

namespace meatpilot::program {

using meatpilot::Seconds;

inline constexpr std::size_t kMaxPhases = 8;
inline constexpr std::size_t kPhaseNameLength = 24;
inline constexpr std::size_t kProgrammeNameLength = 32;

/// How a phase ends (spec §4.1).
enum class EndKind : std::uint8_t {
    Duration,        ///< elapsed time
    Manual,          ///< operator confirmation
    ReferenceLoss,   ///< the reference product reaches its target loss
    FirstOfTargets,  ///< the first product to reach its target
    LastOfTargets,   ///< every product has reached its target
};

/// One step of a recipe.
struct Phase {
    char name[kPhaseNameLength]{};

    bool  has_temp{false};
    float target_temp{13.0F};
    float temp_deadband{0.8F};

    bool  has_humidity{false};
    float target_humidity{75.0F};
    float humidity_deadband{3.0F};

    control::DutyCycle circulation{};
    control::DutyCycle fresh_air{};

    EndKind end{EndKind::Duration};
    Seconds duration{0.0};

    /// Safety fallback (spec §4.1). Mandatory on any weight-driven phase: if
    /// the scale fails or nobody enters a weigh-in, the phase must still
    /// terminate rather than hold the chamber at fermentation temperature
    /// indefinitely.
    Seconds max_duration{0.0};

    /// Phase-specific alarm boundaries, overlaid on the global limits.
    bool  has_limits{false};
    float temp_abs_min{0.0F};
    float temp_abs_max{30.0F};

    /// True while this phase is drying rather than fermenting or resting.
    /// Drives the high-temperature-during-drying alarm (AL-10).
    bool drying{false};
};

/// An ordered list of phases, versioned.
///
/// `revision` is bumped on every edit, and a Run records the revision it
/// started under, so editing a recipe never rewrites the history of a batch
/// already drying against it (spec §7, entity "Run").
struct Programme {
    char        id[kProgrammeNameLength]{};
    char        name[kProgrammeNameLength]{};
    std::uint16_t revision{1};
    Phase       phases[kMaxPhases]{};
    std::size_t phase_count{0};
    bool        hold_last_on_complete{true};
};

/// Why a programme was refused. Validation happens before a run starts, so an
/// unsafe recipe can never be the thing that is executing (UI-05).
enum class Validation : std::uint8_t {
    Ok,
    NoPhases,
    TooManyPhases,
    NoTargets,               ///< a phase that regulates nothing
    ZeroDuration,            ///< a duration phase that ends immediately
    WeightPhaseWithoutCap,   ///< no max_duration on a weight-driven phase
    InvalidDeadband,
    InvalidLimits,
};

Validation validate(const Programme& p) noexcept;
const char* name(Validation v) noexcept;
const char* name(EndKind k) noexcept;

}  // namespace meatpilot::program
