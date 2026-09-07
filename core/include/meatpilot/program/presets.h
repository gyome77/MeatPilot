// SPDX-License-Identifier: MIT
#pragma once

#include "meatpilot/program/programme.h"

namespace meatpilot::program {

/// Built-in starting points.
///
/// These are **indicative** and must be presented as such: spec §13.2 forbids
/// offering them as validated food-safety advice. They encode a shape --
/// a warm rest, then a long cool dry to a target weight loss -- not a
/// guarantee. Duplicate and adapt to your own recipes, salt levels and cuts.
enum class Preset : std::uint8_t {
    SaucissonSec,
    Coppa,
    Bresaola,
    PancettaRoulee,
    Lonzo,
    CellarHold,
};

inline constexpr std::size_t kPresetCount = 6;

/// Fill `out` with the named preset. Always produces a programme that passes
/// validate().
void load(Preset preset, Programme& out) noexcept;

const char* name(Preset p) noexcept;

}  // namespace meatpilot::program
