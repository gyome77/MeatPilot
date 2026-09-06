// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace meatpilot {

/// Seconds since an arbitrary epoch. Always injected, never read from a clock,
/// so every timing rule in the system is testable by advancing a number.
using Seconds = double;

/// Why a reading is or is not usable. Reported alongside the value so the UI
/// can distinguish "no probe" from "probe settling" from "probe stuck" --
/// three conditions that look identical if you only publish a validity bit.
enum class Quality : std::uint8_t {
    Ok,
    Unavailable,   ///< the driver returned nothing
    OutOfRange,    ///< physically implausible for this channel
    Frozen,        ///< unchanged for longer than the channel's stale timeout
};

/// A conditioned sensor reading.
///
/// `raw` is calibrated but unfiltered; `filtered` has been through median and
/// low-pass. Both are carried because spec 4.3 requires the alarm layer to
/// examine raw readings, so a rapid excursion is not smoothed away by the
/// filter before anything can react to it.
struct Reading {
    float   raw{0.0F};
    float   filtered{0.0F};
    bool    valid{false};
    Quality quality{Quality::Unavailable};
};

const char* name(Quality q) noexcept;

}  // namespace meatpilot
